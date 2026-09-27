// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "core/mutation.hpp"

#include <algorithm>
#include <string>

#include "core/canonical.hpp"
#include "core/codec.hpp"
#include "internal/checked.hpp"
#include "internal/text.hpp"

namespace dcr::internal {
namespace {

[[nodiscard]] Error reject(ErrorCode code, std::string message, const char* field) {
  return Error(code, std::move(message), field);
}

[[nodiscard]] bool is_cancelled(const CommandContext& context) {
  return context.cancellation.is_cancelled();
}

// ---------------------------------------------------------------------------
// Command digests
// ---------------------------------------------------------------------------

void write_optional_epoch(CanonicalWriter& writer, const std::optional<EpochToken>& epoch) {
  writer.boolean(epoch.has_value());
  if (epoch.has_value()) {
    writer.varint(epoch->value());
    writer.text(epoch->issuer().value());
  }
}

template <class Token>
[[nodiscard]] std::string token_text(const std::optional<Token>& value) {
  return value.has_value() ? value->value() : std::string();
}

void write_provenance_input(CanonicalWriter& writer, const ProvenanceInput& provenance) {
  writer.u8(static_cast<std::uint8_t>(provenance.source));
  writer.boolean(provenance.principal.has_value());
  if (provenance.principal.has_value()) {
    writer.text(provenance.principal->value());
  }
  writer.text(provenance.detail);
}

void write_metadata_map(CanonicalWriter& writer, const MetadataMap& map) {
  writer.varint(map.entries().size());
  for (const auto& entry : map.entries()) {
    writer.text(entry.key.value());
    writer.text(entry.value);
  }
}

void write_facility(CanonicalWriter& writer, const FacilityMetadata& facility) {
  writer.boolean(facility.country.has_value());
  if (facility.country.has_value()) {
    writer.text(facility.country->value());
  }
  writer.boolean(facility.region.has_value());
  if (facility.region.has_value()) {
    writer.text(facility.region->value());
  }
  writer.boolean(facility.metro.has_value());
  if (facility.metro.has_value()) {
    writer.text(facility.metro->value());
  }
  writer.boolean(facility.address.has_value());
  if (facility.address.has_value()) {
    writer.varint(facility.address->lines().size());
    for (const auto& line : facility.address->lines()) {
      writer.text(line);
    }
    writer.text(facility.address->locality());
    writer.text(facility.address->administrative_area());
    writer.text(facility.address->postal_code());
  }
  writer.boolean(facility.coordinates.has_value());
  if (facility.coordinates.has_value()) {
    writer.i32(facility.coordinates->latitude_e7());
    writer.i32(facility.coordinates->longitude_e7());
  }
  writer.boolean(facility.tier.has_value());
  if (facility.tier.has_value()) {
    writer.u8(static_cast<std::uint8_t>(*facility.tier));
  }
  writer.boolean(facility.facility_operator.has_value());
  if (facility.facility_operator.has_value()) {
    writer.text(facility.facility_operator->value());
  }
  write_metadata_map(writer, facility.external_refs);
}

void write_ownership(CanonicalWriter& writer, const OwnershipScope& ownership) {
  writer.u8(static_cast<std::uint8_t>(ownership.kind()));
  writer.boolean(ownership.scope().has_value());
  if (ownership.scope().has_value()) {
    writer.text(ownership.scope()->value());
  }
}

void write_compatibility(CanonicalWriter& writer, const CompatibilityKey& key) {
  writer.u16(key.model_major());
  writer.u16(key.model_minor());
  writer.u64(key.capability_mask());
}

void write_aliases(CanonicalWriter& writer, const std::vector<Alias>& aliases) {
  writer.varint(aliases.size());
  for (const auto& alias : aliases) {
    writer.text(alias.value());
  }
}

void write_memberships(CanonicalWriter& writer, const std::vector<SiteMembership>& memberships) {
  writer.varint(memberships.size());
  for (const auto& membership : memberships) {
    writer.text(membership.site.value());
    writer.u8(static_cast<std::uint8_t>(membership.role));
  }
}

void write_draft(CanonicalWriter& writer, const RecordDraft& draft) {
  writer.text(draft.display_name);
  write_aliases(writer, draft.aliases);
  write_memberships(writer, draft.memberships);
  write_facility(writer, draft.facility);
  write_metadata_map(writer, draft.extensions);
  write_ownership(writer, draft.ownership);
  writer.boolean(draft.compatibility.has_value());
  if (draft.compatibility.has_value()) {
    write_compatibility(writer, *draft.compatibility);
  }
}

[[nodiscard]] Sha256Digest digest_of(const CanonicalWriter& writer) {
  return sha256_bytes(writer.buffer().data(), writer.buffer().size());
}

// ---------------------------------------------------------------------------
// Planning helpers
// ---------------------------------------------------------------------------

/// One record change produced by a command.
struct RecordChange {
  DataCenterRecord record;
  HistoryAction action = HistoryAction::registered;
  std::optional<LifecycleState> previous_state;
  std::optional<ReasonCode> reason;
};

/// The set of record changes a command produces, with the outcome to report.
struct ChangeSet {
  std::vector<RecordChange> changes;
  MutationStatus status = MutationStatus::updated;
  DataCenterId primary_id;
  MetadataRevision primary_revision;
  std::optional<DataCenterId> related_id;
};

[[nodiscard]] RecordChange make_change(DataCenterRecord record, HistoryAction action,
                                       std::optional<LifecycleState> previous_state,
                                       std::optional<ReasonCode> reason) {
  return RecordChange{std::move(record), action, std::move(previous_state), std::move(reason)};
}

[[nodiscard]] ChangeSet make_change_set(MutationStatus status, DataCenterId primary_id,
                                        MetadataRevision primary_revision,
                                        std::optional<DataCenterId> related_id) {
  return ChangeSet{std::vector<RecordChange>{}, status, std::move(primary_id), primary_revision,
                   std::move(related_id)};
}

[[nodiscard]] MutationResult make_result(MutationStatus status, DataCenterId id,
                                         RegistryGeneration generation,
                                         MetadataRevision revision,
                                         std::optional<DataCenterId> related_id,
                                         std::optional<SequenceNumber> sequence) {
  return MutationResult{status,           std::move(id), generation,
                        revision,         std::move(related_id), std::move(sequence)};
}

/// Presence accessors for the required fields of a command. Validation refuses
/// a command with a missing field before planning starts; these exist so that
/// planning never dereferences an absent value even if a future caller reaches
/// it first.
[[nodiscard]] Result<DataCenterId> require_identity_value(
    const std::optional<DataCenterId>& value, const char* field) {
  if (!value.has_value()) {
    return reject(ErrorCode::invalid_identity,
                  std::string("this command must name the data center it acts on; set ") + field,
                  field);
  }
  return value.value();
}

[[nodiscard]] Result<SiteId> require_site_value(const std::optional<SiteId>& value,
                                                const char* field) {
  if (!value.has_value()) {
    return reject(ErrorCode::invalid_argument,
                  std::string("this command must name a site; set ") + field, field);
  }
  return value.value();
}

[[nodiscard]] Result<SiteMembership> require_membership_value(
    const std::optional<SiteMembership>& value, const char* field) {
  if (!value.has_value()) {
    return reject(ErrorCode::invalid_argument,
                  std::string("this command must name a membership; set ") + field, field);
  }
  return value.value();
}


[[nodiscard]] Result<CompatibilityKey> require_compatibility_value(
    const std::optional<CompatibilityKey>& value, const char* field) {
  if (!value.has_value()) {
    return reject(ErrorCode::invalid_metadata,
                  std::string("every record carries a compatibility contract; set ") + field,
                  field);
  }
  return value.value();
}

/// Looks up an idempotency key. An empty optional means the command is not a
/// replay; a value means it is, and carries the outcome to report again.
[[nodiscard]] Result<std::optional<MutationResult>> check_idempotency(
    const RegistryState& base, const CommandContext& context, const Sha256Digest& digest) {
  if (!context.idempotency_key.has_value()) {
    return std::optional<MutationResult>();
  }
  const IdempotencyEntry* entry = base.find_idempotency(*context.idempotency_key);
  if (entry == nullptr) {
    return std::optional<MutationResult>();
  }
  if (!(entry->command_digest == digest)) {
    return reject(ErrorCode::idempotency_conflict,
                  "idempotency key '" + context.idempotency_key->value() +
                      "' was already used for a different command against generation " +
                      entry->generation.to_string(),
                  "idempotency_key");
  }
  return std::optional<MutationResult>(
      make_result(MutationStatus::replayed, entry->id, entry->generation, entry->revision,
                  entry->related_id, entry->sequence));
}

[[nodiscard]] Result<MutationPlan> unchanged_plan(const RegistryState& base,
                                                  const DataCenterId& id,
                                                  MetadataRevision revision) {
  return MutationPlan{true, RegistryState::empty(base.limits),
                      make_result(MutationStatus::unchanged, id, base.generation, revision,
                                  std::nullopt, std::nullopt)};
}

[[nodiscard]] Result<MutationPlan> replayed_plan(const MutationResult& replayed) {
  return MutationPlan{true, RegistryState{}, replayed};
}

/// Enforces the preconditions that a mutating command carries.
[[nodiscard]] Result<void> check_preconditions(const RegistryState& base,
                                               const CommandContext& context,
                                               std::optional<MetadataRevision> current_revision) {
  const auto& precondition = context.precondition;
  if (precondition.expected_generation.has_value() &&
      !(*precondition.expected_generation == base.generation)) {
    return reject(ErrorCode::stale_generation,
                  "the mutation is based on generation " +
                      precondition.expected_generation->to_string() +
                      " but the registry is at generation " + base.generation.to_string(),
                  "precondition.expected_generation");
  }
  if (precondition.expected_revision.has_value() && current_revision.has_value() &&
      !(*precondition.expected_revision == *current_revision)) {
    return reject(ErrorCode::stale_revision,
                  "the mutation is based on revision " +
                      precondition.expected_revision->to_string() +
                      " but the record is at revision " + current_revision->to_string(),
                  "precondition.expected_revision");
  }
  return {};
}

/// Builds the new state: stamps provenance, applies record changes, appends
/// history, bounds the history and idempotency windows, and validates the
/// result. Nothing here touches the store.
[[nodiscard]] Result<MutationPlan> finalize(const RegistryState& base, ChangeSet changes,
                                            const CommandContext& context,
                                            const std::optional<EpochToken>& effective_epoch,
                                            const Sha256Digest& command_digest, Timestamp now) {
  auto generation = base.generation.successor();
  if (!generation.has_value()) {
    return generation.error();
  }

  if (!context.provenance.principal.has_value()) {
    return reject(ErrorCode::invalid_provenance,
                  "a mutation must say who is making it; set provenance.principal",
                  "provenance.principal");
  }
  const PrincipalId& principal = *context.provenance.principal;

  RegistryState next = base;
  next.generation = generation.value();

  SequenceNumber sequence = base.last_sequence;
  std::optional<SequenceNumber> primary_sequence;
  for (auto& change : changes.changes) {
    auto next_sequence = sequence.successor();
    if (!next_sequence.has_value()) {
      return next_sequence.error();
    }
    sequence = next_sequence.value();

    auto provenance =
        ProvenanceRecord::make(context.provenance.source, principal, sequence,
                               next.generation, now, effective_epoch, change.reason,
                               context.provenance.detail);
    if (!provenance.has_value()) {
      return provenance.error();
    }
    change.record.last_provenance = std::move(provenance).value();
    change.record.last_sequence = sequence;

    if (change.record.id == changes.primary_id) {
      primary_sequence = sequence;
    }

    HistoryEntry entry{sequence,
                       next.generation,
                       change.record.id,
                       change.action,
                       change.record.revision,
                       change.previous_state,
                       change.record.state,
                       change.record.last_provenance};
    next.history.push_back(std::move(entry));
  }
  next.last_sequence = sequence;
  next.external_epoch = effective_epoch;

  for (auto& change : changes.changes) {
    const std::size_t position = next.lower_bound_position(change.record.id);
    if (position < next.records.size() && next.records[position].id == change.record.id) {
      next.records[position] = std::move(change.record);
    } else {
      next.records.insert(next.records.begin() + static_cast<std::ptrdiff_t>(position),
                          std::move(change.record));
    }
  }

  // The history window is bounded. Dropped entries are counted so that a
  // consumer can tell that the retained window does not reach back to the
  // beginning of the registry.
  const std::size_t history_bound = static_cast<std::size_t>(next.limits.max_history_entries);
  if (next.history.size() > history_bound) {
    const std::size_t excess = next.history.size() - history_bound;
    next.history.erase(next.history.begin(),
                       next.history.begin() + static_cast<std::ptrdiff_t>(excess));
    next.history_dropped += static_cast<std::uint64_t>(excess);
  }

  MutationResult result = make_result(changes.status, changes.primary_id, generation.value(),
                                      changes.primary_revision, changes.related_id,
                                      primary_sequence);
  if (context.idempotency_key.has_value()) {
    IdempotencyEntry entry{*context.idempotency_key,
                           command_digest,
                           changes.status,
                           changes.primary_id,
                           generation.value(),
                           changes.primary_revision,
                           changes.related_id,
                           primary_sequence,
                           sequence};
    const auto position =
        std::lower_bound(next.idempotency.begin(), next.idempotency.end(), *context.idempotency_key,
                         [](const IdempotencyEntry& candidate, const IdempotencyKey& wanted) {
                           return candidate.key < wanted;
                         });
    next.idempotency.insert(position, std::move(entry));

    const std::size_t idempotency_bound =
        static_cast<std::size_t>(next.limits.max_idempotency_entries);
    while (next.idempotency.size() > idempotency_bound) {
      const auto oldest = std::min_element(
          next.idempotency.begin(), next.idempotency.end(),
          [](const IdempotencyEntry& left, const IdempotencyEntry& right) {
            return left.recorded_sequence < right.recorded_sequence;
          });
      next.idempotency.erase(oldest);
    }
  }

  if (auto validation = validate_state(next); !validation.has_value()) {
    return validation.error();
  }

  return MutationPlan{false, std::move(next), std::move(result)};
}

/// Adds a change for a record that already exists, carrying the previous state.
[[nodiscard]] RecordChange change_for_existing(DataCenterRecord record, HistoryAction action,
                                               LifecycleState previous_state,
                                               std::optional<ReasonCode> reason) {
  return make_change(std::move(record), action, std::optional<LifecycleState>(previous_state),
                     std::move(reason));
}

[[nodiscard]] Result<DataCenterRecord> with_bumped_revision(DataCenterRecord record) {
  // Revisions are monotonic per record; the engine never decrements one and
  // never wraps it.
  auto next = record.revision.successor();
  if (!next.has_value()) {
    return next.error();
  }
  record.revision = next.value();
  return record;
}
}  // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

Result<std::vector<Alias>> canonicalize_aliases(std::vector<Alias> aliases,
                                                const RegistryLimits& limits) {
  if (aliases.size() > limits.max_aliases_per_record) {
    return reject(ErrorCode::limit_exceeded,
                  "at most " + std::to_string(limits.max_aliases_per_record) +
                      " aliases are accepted per record",
                  "aliases");
  }
  std::sort(aliases.begin(), aliases.end());
  for (std::size_t index = 1; index < aliases.size(); ++index) {
    if (aliases[index - 1] == aliases[index]) {
      return reject(ErrorCode::invalid_argument,
                    "alias '" + aliases[index].value() +
                        "' appears more than once in the same command",
                    "aliases");
    }
  }
  return aliases;
}

Result<std::vector<SiteMembership>> canonicalize_memberships(
    std::vector<SiteMembership> memberships, const RegistryLimits& limits) {
  if (memberships.size() > limits.max_memberships_per_record) {
    return reject(ErrorCode::limit_exceeded,
                  "at most " + std::to_string(limits.max_memberships_per_record) +
                      " site memberships are accepted per record",
                  "memberships");
  }
  std::sort(memberships.begin(), memberships.end(),
            [](const SiteMembership& left, const SiteMembership& right) {
              return left.site < right.site;
            });
  for (std::size_t index = 1; index < memberships.size(); ++index) {
    if (memberships[index - 1].site == memberships[index].site) {
      return reject(ErrorCode::duplicate_membership,
                    "site '" + memberships[index].site.value() +
                        "' appears more than once in the same command",
                    "memberships");
    }
  }
  std::size_t primary_count = 0;
  for (const auto& membership : memberships) {
    if (membership.role == MembershipRole::primary) {
      ++primary_count;
    }
  }
  if (primary_count > 1) {
    return reject(ErrorCode::membership_violation,
                  "a record may have at most one primary site membership", "memberships");
  }
  return memberships;
}

Result<std::optional<EpochToken>> resolve_epoch(const RegistryState& base,
                                                const std::optional<EpochToken>& asserted) {
  if (!asserted.has_value()) {
    return base.external_epoch;
  }
  if (!base.external_epoch.has_value()) {
    return asserted;
  }
  const auto ordering = asserted->compare(*base.external_epoch);
  if (!ordering.has_value()) {
    return reject(ErrorCode::stale_authority,
                  "the asserted epoch token was issued by '" + asserted->issuer().value() +
                      "' but this registry records epochs from '" +
                      base.external_epoch->issuer().value() + "'",
                  "precondition.asserted_epoch");
  }
  if (*ordering == std::strong_ordering::less) {
    return reject(ErrorCode::stale_authority,
                  "the asserted epoch token " + asserted->to_string() +
                      " is older than the recorded token " + base.external_epoch->to_string(),
                  "precondition.asserted_epoch");
  }
  return asserted;
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------

Sha256Digest digest_register_command(const RegisterCommand& command) {
  CanonicalWriter writer;
  writer.text("dcr.command.register.v1");
  writer.text(token_text(command.id));
  write_draft(writer, command.draft);
  writer.u8(static_cast<std::uint8_t>(command.initial_state));
  write_provenance_input(writer, command.context.provenance);
  write_optional_epoch(writer, command.context.precondition.asserted_epoch);
  return digest_of(writer);
}

Sha256Digest digest_update_command(const UpdateMetadataCommand& command) {
  CanonicalWriter writer;
  writer.text("dcr.command.update.v1");
  writer.text(token_text(command.id));
  const auto& patch = command.patch;

  writer.u8(static_cast<std::uint8_t>(patch.display_name.action()));
  if (patch.display_name.is_set()) {
    writer.text(patch.display_name.value());
  }
  writer.u8(static_cast<std::uint8_t>(patch.aliases.action()));
  if (patch.aliases.is_set()) {
    write_aliases(writer, patch.aliases.value());
  }
  writer.u8(static_cast<std::uint8_t>(patch.memberships.action()));
  if (patch.memberships.is_set()) {
    write_memberships(writer, patch.memberships.value());
  }
  writer.u8(static_cast<std::uint8_t>(patch.facility.action()));
  if (patch.facility.is_set()) {
    write_facility(writer, patch.facility.value());
  }
  writer.u8(static_cast<std::uint8_t>(patch.extensions.action()));
  if (patch.extensions.is_set()) {
    write_metadata_map(writer, patch.extensions.value());
  }
  writer.u8(static_cast<std::uint8_t>(patch.ownership.action()));
  if (patch.ownership.is_set()) {
    write_ownership(writer, patch.ownership.value());
  }
  writer.u8(static_cast<std::uint8_t>(patch.compatibility.action()));
  if (patch.compatibility.is_set()) {
    write_compatibility(writer, patch.compatibility.value());
  }

  write_provenance_input(writer, command.context.provenance);
  write_optional_epoch(writer, command.context.precondition.asserted_epoch);
  return digest_of(writer);
}

Sha256Digest digest_transition_command(const TransitionCommand& command) {
  CanonicalWriter writer;
  writer.text("dcr.command.transition.v1");
  writer.text(token_text(command.id));
  writer.u8(static_cast<std::uint8_t>(command.target_state));
  writer.text(token_text(command.reason));
  write_provenance_input(writer, command.context.provenance);
  write_optional_epoch(writer, command.context.precondition.asserted_epoch);
  return digest_of(writer);
}

Sha256Digest digest_attach_command(const AttachSiteCommand& command) {
  CanonicalWriter writer;
  writer.text("dcr.command.attach_site.v1");
  writer.text(token_text(command.id));
  writer.boolean(command.membership.has_value());
  if (command.membership.has_value()) {
    writer.text(command.membership->site.value());
    writer.u8(static_cast<std::uint8_t>(command.membership->role));
  }
  write_provenance_input(writer, command.context.provenance);
  write_optional_epoch(writer, command.context.precondition.asserted_epoch);
  return digest_of(writer);
}

Sha256Digest digest_detach_command(const DetachSiteCommand& command) {
  CanonicalWriter writer;
  writer.text("dcr.command.detach_site.v1");
  writer.text(token_text(command.id));
  writer.text(token_text(command.site));
  write_provenance_input(writer, command.context.provenance);
  write_optional_epoch(writer, command.context.precondition.asserted_epoch);
  return digest_of(writer);
}

Sha256Digest digest_retire_command(const RetireCommand& command) {
  CanonicalWriter writer;
  writer.text("dcr.command.retire.v1");
  writer.text(token_text(command.id));
  writer.text(token_text(command.reason));
  write_provenance_input(writer, command.context.provenance);
  write_optional_epoch(writer, command.context.precondition.asserted_epoch);
  return digest_of(writer);
}

Sha256Digest digest_replace_command(const ReplaceCommand& command) {
  CanonicalWriter writer;
  writer.text("dcr.command.replace.v1");
  writer.text(token_text(command.retired_id));
  writer.text(token_text(command.new_id));
  write_draft(writer, command.draft);
  writer.u8(static_cast<std::uint8_t>(command.initial_state));
  writer.text(token_text(command.reason));
  write_provenance_input(writer, command.context.provenance);
  write_optional_epoch(writer, command.context.precondition.asserted_epoch);
  return digest_of(writer);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

Result<MutationPlan> plan_register(const RegistryState& base, const RegisterCommand& command,
                                   Timestamp now) {
  if (auto result = validate_register_command(command, base.limits); !result.has_value()) {
    return result.error();
  }
  if (is_cancelled(command.context)) {
    return reject(ErrorCode::cancelled, "the mutation was cancelled before planning", "cancellation");
  }
  DCR_TRY_ASSIGN(const DataCenterId id, require_identity_value(command.id, "id"));
  DCR_TRY_ASSIGN(const CompatibilityKey compatibility,
                 require_compatibility_value(command.draft.compatibility, "draft.compatibility"));
  const Sha256Digest digest = digest_register_command(command);
  DCR_TRY_ASSIGN(const std::optional<MutationResult> replayed,
                 check_idempotency(base, command.context, digest));
  if (replayed.has_value()) {
    return replayed_plan(replayed.value());
  }
  if (base.has_record(id)) {
    return reject(ErrorCode::duplicate_identity,
                  "a record with identity '" + id.value() + "' already exists",
                  "id");
  }
  if (base.records.size() >= static_cast<std::size_t>(base.limits.max_records)) {
    return reject(ErrorCode::limit_exceeded,
                  "the registry holds its maximum of " + std::to_string(base.limits.max_records) +
                      " records",
                  "max_records");
  }
  DCR_TRY_ASSIGN(std::vector<Alias> aliases, canonicalize_aliases(command.draft.aliases, base.limits));
  for (const auto& alias : aliases) {
    const DataCenterRecord* owner = base.find_by_alias(alias);
    if (owner != nullptr) {
      return reject(ErrorCode::alias_conflict,
                    "alias '" + alias.value() + "' is already claimed by '" + owner->id.value() +
                        "'",
                    "aliases");
    }
  }
  DCR_TRY_ASSIGN(std::vector<SiteMembership> memberships,
                 canonicalize_memberships(command.draft.memberships, base.limits));

  DCR_TRY_ASSIGN(const std::optional<EpochToken> effective_epoch,
                 resolve_epoch(base, command.context.precondition.asserted_epoch));

  // The creation generation is the generation this mutation will produce.
  auto next_generation = base.generation.successor();
  if (!next_generation.has_value()) {
    return next_generation.error();
  }

  DataCenterRecord record{id,
                          MetadataRevision::minimum(),
                          command.draft.display_name,
                          std::move(aliases),
                          std::move(memberships),
                          command.draft.facility,
                          command.draft.extensions,
                          command.draft.ownership,
                          compatibility,
                          command.initial_state,
                          next_generation.value(),
                          std::nullopt,
                          std::nullopt,
                          std::nullopt,
                          // A placeholder that the engine immediately replaces
                          // with the stamped provenance. Its generation matches
                          // the record's creation generation so that the record
                          // is valid at every intermediate step.
                          ProvenanceRecord::make(ProvenanceSource::api,
                                                 *command.context.provenance.principal,
                                                 SequenceNumber::minimum(),
                                                 next_generation.value(), now, std::nullopt,
                                                 std::nullopt, std::string())
                              .value(),
                          SequenceNumber::minimum()};

  ChangeSet changes = make_change_set(MutationStatus::created, id, record.revision,
                                      std::nullopt);
  changes.changes.push_back(
      make_change(std::move(record), HistoryAction::registered, std::nullopt, std::nullopt));
  return finalize(base, std::move(changes), command.context, effective_epoch, digest, now);
}

// ---------------------------------------------------------------------------
// Metadata update
// ---------------------------------------------------------------------------

Result<MutationPlan> plan_update(const RegistryState& base, const UpdateMetadataCommand& command,
                                 Timestamp now) {
  if (auto result = validate_update_command(command, base.limits); !result.has_value()) {
    return result.error();
  }
  if (is_cancelled(command.context)) {
    return reject(ErrorCode::cancelled, "the mutation was cancelled before planning", "cancellation");
  }
  DCR_TRY_ASSIGN(const DataCenterId id, require_identity_value(command.id, "id"));
  const Sha256Digest digest = digest_update_command(command);
  DCR_TRY_ASSIGN(const std::optional<MutationResult> replayed,
                 check_idempotency(base, command.context, digest));
  if (replayed.has_value()) {
    return replayed_plan(replayed.value());
  }
  const DataCenterRecord* existing = base.find(id);
  if (existing == nullptr) {
    return reject(ErrorCode::not_found,
                  "no record with identity '" + id.value() + "' exists", "id");
  }

  DataCenterRecord candidate = *existing;
  const auto& patch = command.patch;
  if (patch.display_name.is_set()) {
    candidate.display_name = patch.display_name.value();
  }
  // A patch either sets a field, clears it, or leaves it alone. Clearing is
  // only meaningful for a field that can be absent, and the validation step has
  // already refused to clear the ones that cannot.
  if (patch.aliases.is_set()) {
    DCR_TRY_ASSIGN(std::vector<Alias> aliases,
                   canonicalize_aliases(patch.aliases.value(), base.limits));
    for (const auto& alias : aliases) {
      const DataCenterRecord* owner = base.find_by_alias(alias);
      if (owner != nullptr && !(owner->id == id)) {
        return reject(ErrorCode::alias_conflict,
                      "alias '" + alias.value() + "' is already claimed by '" +
                          owner->id.value() + "'",
                      "patch.aliases");
      }
    }
    candidate.aliases = std::move(aliases);
  } else if (patch.aliases.is_clear()) {
    candidate.aliases.clear();
  }

  if (patch.memberships.is_set()) {
    DCR_TRY_ASSIGN(std::vector<SiteMembership> memberships,
                   canonicalize_memberships(patch.memberships.value(), base.limits));
    candidate.memberships = std::move(memberships);
  } else if (patch.memberships.is_clear()) {
    candidate.memberships.clear();
  }

  if (patch.facility.is_set()) {
    candidate.facility = patch.facility.value();
  } else if (patch.facility.is_clear()) {
    candidate.facility = FacilityMetadata{};
  }

  if (patch.extensions.is_set()) {
    candidate.extensions = patch.extensions.value();
  } else if (patch.extensions.is_clear()) {
    candidate.extensions = MetadataMap{};
  }

  if (patch.ownership.is_set()) {
    candidate.ownership = patch.ownership.value();
  } else if (patch.ownership.is_clear()) {
    candidate.ownership = OwnershipScope::unassigned();
  }

  if (patch.compatibility.is_set()) {
    candidate.compatibility = patch.compatibility.value();
  }

  // A command whose desired end state already holds is a no-op, and is decided
  // before preconditions are consulted.
  if (candidate == *existing) {
    return unchanged_plan(base, id, existing->revision);
  }
  if (is_terminal(existing->state)) {
    return reject(ErrorCode::terminal_state,
                  "record '" + id.value() + "' is '" +
                      std::string(to_string(existing->state)) +
                      "' and accepts no further mutation",
                  "id");
  }
  DCR_TRY(check_preconditions(base, command.context, existing->revision));
  DCR_TRY_ASSIGN(const std::optional<EpochToken> effective_epoch,
                 resolve_epoch(base, command.context.precondition.asserted_epoch));

  DCR_TRY_ASSIGN(candidate, with_bumped_revision(std::move(candidate)));
  if (auto result = validate_record(candidate, base.limits); !result.has_value()) {
    return result.error();
  }

  ChangeSet changes = make_change_set(MutationStatus::updated, id, candidate.revision,
                                      std::nullopt);  changes.changes.push_back(
      change_for_existing(std::move(candidate), HistoryAction::metadata_updated, existing->state,
                          std::nullopt));
  return finalize(base, std::move(changes), command.context, effective_epoch, digest, now);
}

// ---------------------------------------------------------------------------
// Lifecycle transition
// ---------------------------------------------------------------------------

Result<MutationPlan> plan_transition(const RegistryState& base, const TransitionCommand& command,
                                     Timestamp now) {
  if (auto result = validate_transition_command(command); !result.has_value()) {
    return result.error();
  }
  if (auto result = validate_command_context(command.context, base.limits);
      !result.has_value()) {
    return result.error();
  }
  if (is_cancelled(command.context)) {
    return reject(ErrorCode::cancelled, "the mutation was cancelled before planning", "cancellation");
  }
  DCR_TRY_ASSIGN(const DataCenterId id, require_identity_value(command.id, "id"));
  const Sha256Digest digest = digest_transition_command(command);
  DCR_TRY_ASSIGN(const std::optional<MutationResult> replayed,
                 check_idempotency(base, command.context, digest));
  if (replayed.has_value()) {
    return replayed_plan(replayed.value());
  }
  const DataCenterRecord* existing = base.find(id);
  if (existing == nullptr) {
    return reject(ErrorCode::not_found,
                  "no record with identity '" + id.value() + "' exists", "id");
  }

  const TransitionVerdict verdict = classify_transition(existing->state, command.target_state);
  if (verdict == TransitionVerdict::unchanged) {
    return unchanged_plan(base, id, existing->revision);
  }
  if (is_terminal(existing->state)) {
    return reject(ErrorCode::terminal_state,
                  "record '" + id.value() + "' is '" +
                      std::string(to_string(existing->state)) +
                      "' and cannot return to service",
                  "target_state");
  }
  if (verdict == TransitionVerdict::illegal) {
    std::string message = "the transition from '" + std::string(to_string(existing->state)) +
                          "' to '" + std::string(to_string(command.target_state)) +
                          "' is not legal";
    const auto successors = legal_successors(existing->state);
    if (successors.empty()) {
      message += "; no transition leaves this state";
    } else {
      message += "; legal targets are";
      for (const auto successor : successors) {
        message += " '";
        message += to_string(successor);
        message += "'";
      }
    }
    return reject(ErrorCode::illegal_transition, std::move(message), "target_state");
  }

  DCR_TRY(check_preconditions(base, command.context, existing->revision));
  DCR_TRY_ASSIGN(const std::optional<EpochToken> effective_epoch,
                 resolve_epoch(base, command.context.precondition.asserted_epoch));

  auto next_generation = base.generation.successor();
  if (!next_generation.has_value()) {
    return next_generation.error();
  }

  DCR_TRY_ASSIGN(DataCenterRecord candidate, with_bumped_revision(*existing));
  candidate.state = command.target_state;
  if (command.target_state == LifecycleState::retired) {
    // Retiring through a transition carries the same coupling a dedicated
    // retirement does: a terminal record always records when it left service,
    // and the registry generation it left at.
    candidate.retired_generation = next_generation.value();
    candidate.retirement =
        RetirementInfo{command.reason, next_generation.value(), std::nullopt};
  }
  if (auto result = validate_record(candidate, base.limits); !result.has_value()) {
    return result.error();
  }

  ChangeSet changes = make_change_set(MutationStatus::updated, id, candidate.revision,
                                      std::nullopt);
  changes.changes.push_back(change_for_existing(std::move(candidate),
                                                HistoryAction::lifecycle_transition,
                                                existing->state, command.reason));
  return finalize(base, std::move(changes), command.context, effective_epoch, digest, now);
}

// ---------------------------------------------------------------------------
// Site membership
// ---------------------------------------------------------------------------

Result<MutationPlan> plan_attach(const RegistryState& base, const AttachSiteCommand& command,
                                 Timestamp now) {
  if (auto result = validate_attach_command(command); !result.has_value()) {
    return result.error();
  }
  if (auto result = validate_command_context(command.context, base.limits);
      !result.has_value()) {
    return result.error();
  }
  if (is_cancelled(command.context)) {
    return reject(ErrorCode::cancelled, "the mutation was cancelled before planning", "cancellation");
  }
  DCR_TRY_ASSIGN(const DataCenterId id, require_identity_value(command.id, "id"));
  DCR_TRY_ASSIGN(const SiteMembership membership,
                 require_membership_value(command.membership, "membership"));
  const Sha256Digest digest = digest_attach_command(command);
  DCR_TRY_ASSIGN(const std::optional<MutationResult> replayed,
                 check_idempotency(base, command.context, digest));
  if (replayed.has_value()) {
    return replayed_plan(replayed.value());
  }
  const DataCenterRecord* existing = base.find(id);
  if (existing == nullptr) {
    return reject(ErrorCode::not_found,
                  "no record with identity '" + id.value() + "' exists", "id");
  }
  const SiteMembership* current = existing->membership_for(membership.site);
  if (current != nullptr && current->role == membership.role) {
    return unchanged_plan(base, id, existing->revision);
  }
  if (is_terminal(existing->state)) {
    return reject(ErrorCode::terminal_state,
                  "record '" + id.value() + "' is '" +
                      std::string(to_string(existing->state)) +
                      "' and its membership is historical",
                  "id");
  }
  if (membership.role == MembershipRole::primary) {
    const auto primary = existing->primary_membership();
    if (primary.has_value() && !(primary->site == membership.site)) {
      return reject(ErrorCode::membership_violation,
                    "record '" + id.value() + "' is already primary in site '" +
                        primary->site.value() + "'",
                    "membership");
    }
  }
  DCR_TRY(check_preconditions(base, command.context, existing->revision));
  DCR_TRY_ASSIGN(const std::optional<EpochToken> effective_epoch,
                 resolve_epoch(base, command.context.precondition.asserted_epoch));

  DCR_TRY_ASSIGN(DataCenterRecord candidate, with_bumped_revision(*existing));
  const std::size_t position = candidate.lower_bound_position_for_site(membership.site);
  if (position < candidate.memberships.size() &&
      candidate.memberships[position].site == membership.site) {
    candidate.memberships[position] = membership;
  } else {
    candidate.memberships.insert(
        candidate.memberships.begin() + static_cast<std::ptrdiff_t>(position),
        membership);
  }
  if (auto result = validate_record(candidate, base.limits); !result.has_value()) {
    return result.error();
  }

  ChangeSet changes = make_change_set(MutationStatus::updated, id, candidate.revision,
                                      std::nullopt);  changes.changes.push_back(change_for_existing(std::move(candidate), HistoryAction::site_attached,
                                                existing->state, std::nullopt));
  return finalize(base, std::move(changes), command.context, effective_epoch, digest, now);
}

Result<MutationPlan> plan_detach(const RegistryState& base, const DetachSiteCommand& command,
                                 Timestamp now) {
  if (auto result = validate_detach_command(command); !result.has_value()) {
    return result.error();
  }
  if (auto result = validate_command_context(command.context, base.limits);
      !result.has_value()) {
    return result.error();
  }
  if (is_cancelled(command.context)) {
    return reject(ErrorCode::cancelled, "the mutation was cancelled before planning", "cancellation");
  }
  DCR_TRY_ASSIGN(const DataCenterId id, require_identity_value(command.id, "id"));
  DCR_TRY_ASSIGN(const SiteId site, require_site_value(command.site, "site"));
  const Sha256Digest digest = digest_detach_command(command);
  DCR_TRY_ASSIGN(const std::optional<MutationResult> replayed,
                 check_idempotency(base, command.context, digest));
  if (replayed.has_value()) {
    return replayed_plan(replayed.value());
  }
  const DataCenterRecord* existing = base.find(id);
  if (existing == nullptr) {
    return reject(ErrorCode::not_found,
                  "no record with identity '" + id.value() + "' exists", "id");
  }
  const SiteMembership* current = existing->membership_for(site);
  if (current == nullptr) {
    return reject(ErrorCode::not_found,
                  "record '" + id.value() + "' is not a member of site '" +
                      site.value() + "'",
                  "site");
  }
  if (is_terminal(existing->state)) {
    return reject(ErrorCode::terminal_state,
                  "record '" + id.value() + "' is '" +
                      std::string(to_string(existing->state)) +
                      "' and its membership is historical",
                  "id");
  }
  if (current->role == MembershipRole::primary && is_live(existing->state)) {
    return reject(ErrorCode::membership_violation,
                  "record '" + id.value() + "' is '" +
                      std::string(to_string(existing->state)) +
                      "' and must keep exactly one primary site membership",
                  "site");
  }
  DCR_TRY(check_preconditions(base, command.context, existing->revision));
  DCR_TRY_ASSIGN(const std::optional<EpochToken> effective_epoch,
                 resolve_epoch(base, command.context.precondition.asserted_epoch));

  DCR_TRY_ASSIGN(DataCenterRecord candidate, with_bumped_revision(*existing));
  const std::size_t position = candidate.lower_bound_position_for_site(site);
  candidate.memberships.erase(candidate.memberships.begin() + static_cast<std::ptrdiff_t>(position));
  if (auto result = validate_record(candidate, base.limits); !result.has_value()) {
    return result.error();
  }

  ChangeSet changes = make_change_set(MutationStatus::updated, id, candidate.revision,
                                      std::nullopt);  changes.changes.push_back(change_for_existing(std::move(candidate), HistoryAction::site_detached,
                                                existing->state, std::nullopt));
  return finalize(base, std::move(changes), command.context, effective_epoch, digest, now);
}

// ---------------------------------------------------------------------------
// Retirement
// ---------------------------------------------------------------------------

Result<MutationPlan> plan_retire(const RegistryState& base, const RetireCommand& command,
                                 Timestamp now) {
  if (auto result = validate_retire_command(command); !result.has_value()) {
    return result.error();
  }
  if (auto result = validate_command_context(command.context, base.limits);
      !result.has_value()) {
    return result.error();
  }
  if (is_cancelled(command.context)) {
    return reject(ErrorCode::cancelled, "the mutation was cancelled before planning", "cancellation");
  }
  DCR_TRY_ASSIGN(const DataCenterId id, require_identity_value(command.id, "id"));
  const std::optional<ReasonCode>& reason = command.reason;
  const Sha256Digest digest = digest_retire_command(command);
  DCR_TRY_ASSIGN(const std::optional<MutationResult> replayed,
                 check_idempotency(base, command.context, digest));
  if (replayed.has_value()) {
    return replayed_plan(replayed.value());
  }
  const DataCenterRecord* existing = base.find(id);
  if (existing == nullptr) {
    return reject(ErrorCode::not_found,
                  "no record with identity '" + id.value() + "' exists", "id");
  }
  if (existing->state == LifecycleState::retired) {
    return unchanged_plan(base, id, existing->revision);
  }
  if (existing->state == LifecycleState::replaced) {
    const auto successor = existing->retirement.has_value()
                               ? existing->retirement->replaced_by
                               : std::optional<DataCenterId>();
    std::string message = "record '" + id.value() + "' was replaced";
    if (successor.has_value()) {
      message += " by '" + successor->value() + "'";
    }
    message += " and cannot be retired";
    return reject(ErrorCode::terminal_state, std::move(message), "id");
  }
  DCR_TRY(check_preconditions(base, command.context, existing->revision));
  DCR_TRY_ASSIGN(const std::optional<EpochToken> effective_epoch,
                 resolve_epoch(base, command.context.precondition.asserted_epoch));

  auto next_generation = base.generation.successor();
  if (!next_generation.has_value()) {
    return next_generation.error();
  }

  DCR_TRY_ASSIGN(DataCenterRecord candidate, with_bumped_revision(*existing));
  candidate.state = LifecycleState::retired;
  candidate.retired_generation = next_generation.value();
  candidate.retirement =
      RetirementInfo{reason, next_generation.value(), std::nullopt};
  if (auto result = validate_record(candidate, base.limits); !result.has_value()) {
    return result.error();
  }

  ChangeSet changes = make_change_set(MutationStatus::updated, id, candidate.revision,
                                      std::nullopt);  changes.changes.push_back(change_for_existing(std::move(candidate), HistoryAction::retired,
                                                existing->state, reason));
  return finalize(base, std::move(changes), command.context, effective_epoch, digest, now);
}

// ---------------------------------------------------------------------------
// Replacement
// ---------------------------------------------------------------------------

Result<MutationPlan> plan_replace(const RegistryState& base, const ReplaceCommand& command,
                                  Timestamp now) {
  if (auto result = validate_replace_command(command, base.limits); !result.has_value()) {
    return result.error();
  }
  if (is_cancelled(command.context)) {
    return reject(ErrorCode::cancelled, "the mutation was cancelled before planning", "cancellation");
  }
  DCR_TRY_ASSIGN(const DataCenterId retired_id,
                 require_identity_value(command.retired_id, "retired_id"));
  DCR_TRY_ASSIGN(const DataCenterId new_id, require_identity_value(command.new_id, "new_id"));
  DCR_TRY_ASSIGN(const CompatibilityKey compatibility,
                 require_compatibility_value(command.draft.compatibility, "draft.compatibility"));
  const std::optional<ReasonCode>& reason = command.reason;
  const Sha256Digest digest = digest_replace_command(command);
  DCR_TRY_ASSIGN(const std::optional<MutationResult> replayed,
                 check_idempotency(base, command.context, digest));
  if (replayed.has_value()) {
    return replayed_plan(replayed.value());
  }
  const DataCenterRecord* predecessor = base.find(retired_id);
  if (predecessor == nullptr) {
    return reject(ErrorCode::not_found,
                  "no record with identity '" + retired_id.value() + "' exists",
                  "retired_id");
  }
  if (is_terminal(predecessor->state)) {
    return reject(ErrorCode::terminal_state,
                  "record '" + retired_id.value() + "' is '" +
                      std::string(to_string(predecessor->state)) +
                      "' and cannot be replaced; obsolete authority is never revived",
                  "retired_id");
  }
  if (base.has_record(new_id)) {
    return reject(ErrorCode::duplicate_identity,
                  "a record with identity '" + new_id.value() + "' already exists",
                  "new_id");
  }
  if (base.records.size() >= static_cast<std::size_t>(base.limits.max_records)) {
    return reject(ErrorCode::limit_exceeded,
                  "the registry holds its maximum of " + std::to_string(base.limits.max_records) +
                      " records",
                  "max_records");
  }
  if (const auto relation =
          classify_compatibility(predecessor->compatibility, compatibility);
      relation == CompatibilityRelation::incompatible) {
    return reject(ErrorCode::incompatible_replacement,
                  "the successor's compatibility key " +
                      compatibility.to_string() +
                      " cannot represent everything '" + retired_id.value() +
                      "' claims (" + predecessor->compatibility.to_string() + ")",
                  "draft.compatibility");
  }

  DCR_TRY_ASSIGN(std::vector<Alias> aliases,
                 canonicalize_aliases(command.draft.aliases, base.limits));
  // An alias claimed by the predecessor transfers to the successor; an alias
  // claimed by any other record is a conflict. Aliases the successor does not
  // claim stay with the predecessor as history.
  std::vector<Alias> transferred;
  for (const auto& alias : aliases) {
    const DataCenterRecord* owner = base.find_by_alias(alias);
    if (owner == nullptr) {
      continue;
    }
    if (owner->id == retired_id) {
      transferred.push_back(alias);
      continue;
    }
    return reject(ErrorCode::alias_conflict,
                  "alias '" + alias.value() + "' is already claimed by '" + owner->id.value() +
                      "'",
                  "draft.aliases");
  }
  DCR_TRY_ASSIGN(std::vector<SiteMembership> memberships,
                 canonicalize_memberships(command.draft.memberships, base.limits));

  // A replacement is based on the predecessor's current state, so it carries the
  // same preconditions any other change to that record would carry.
  DCR_TRY(check_preconditions(base, command.context, predecessor->revision));
  DCR_TRY_ASSIGN(const std::optional<EpochToken> effective_epoch,
                 resolve_epoch(base, command.context.precondition.asserted_epoch));

  auto next_generation = base.generation.successor();
  if (!next_generation.has_value()) {
    return next_generation.error();
  }

  DCR_TRY_ASSIGN(DataCenterRecord retired, with_bumped_revision(*predecessor));
  retired.state = LifecycleState::replaced;
  retired.retired_generation = next_generation.value();
  retired.retirement =
      RetirementInfo{reason, next_generation.value(),
                     std::optional<DataCenterId>(new_id)};
  if (!transferred.empty()) {
    std::vector<Alias> remaining;
    remaining.reserve(retired.aliases.size());
    for (const auto& alias : retired.aliases) {
      if (std::find(transferred.begin(), transferred.end(), alias) == transferred.end()) {
        remaining.push_back(alias);
      }
    }
    retired.aliases = std::move(remaining);
  }
  if (auto result = validate_record(retired, base.limits); !result.has_value()) {
    return result.error();
  }

  DataCenterRecord successor{new_id,
                             MetadataRevision::minimum(),
                             command.draft.display_name,
                             std::move(aliases),
                             std::move(memberships),
                             command.draft.facility,
                             command.draft.extensions,
                             command.draft.ownership,
                             compatibility,
                             command.initial_state,
                             next_generation.value(),
                             std::nullopt,
                             std::nullopt,
                             std::optional<DataCenterId>(retired_id),
                             ProvenanceRecord::make(ProvenanceSource::api,
                                                    *command.context.provenance.principal,
                                                    SequenceNumber::minimum(),
                                                    next_generation.value(), now, std::nullopt,
                                                    std::nullopt, std::string())
                                 .value(),
                             SequenceNumber::minimum()};
  if (auto result = validate_record(successor, base.limits); !result.has_value()) {
    return result.error();
  }

  ChangeSet changes =
      make_change_set(MutationStatus::created, new_id, successor.revision,
                      std::optional<DataCenterId>(retired_id));
  // The predecessor is recorded first, so the history reads in the order the
  // operation happens: the old identity leaves service, then the new one
  // exists.
  changes.changes.push_back(change_for_existing(std::move(retired),
                                                HistoryAction::replacement_retired,
                                                predecessor->state, reason));
  changes.changes.push_back(make_change(std::move(successor),
                                        HistoryAction::replacement_created, std::nullopt,
                                        reason));

  return finalize(base, std::move(changes), command.context, effective_epoch, digest, now);
}

}  // namespace dcr::internal
