// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Record and record-set validation.
//
// validate_record() is the single gate that every record passes through before
// it can become authoritative, whether it arrived from a mutation command or
// from a decoded snapshot. validate_record_set() adds the invariants that only
// make sense across the whole registry.

#include "dcr/record.hpp"

#include <algorithm>
#include <set>
#include <string>

#include "internal/text.hpp"

namespace dcr {
namespace {

[[nodiscard]] Result<void> fail(ErrorCode code, std::string message, const char* field) {
  return Error(code, std::move(message), field);
}

}  // namespace

std::string_view to_string(OwnershipKind kind) noexcept {
  switch (kind) {
    case OwnershipKind::unassigned:
      return "unassigned";
    case OwnershipKind::platform:
      return "platform";
    case OwnershipKind::tenant:
      return "tenant";
    case OwnershipKind::partner:
      return "partner";
    case OwnershipKind::system:
      return "system";
  }
  return "unknown";
}

Result<OwnershipKind> parse_ownership_kind(std::string_view token) {
  if (token == "unassigned") {
    return OwnershipKind::unassigned;
  }
  if (token == "platform") {
    return OwnershipKind::platform;
  }
  if (token == "tenant") {
    return OwnershipKind::tenant;
  }
  if (token == "partner") {
    return OwnershipKind::partner;
  }
  if (token == "system") {
    return OwnershipKind::system;
  }
  return Error(ErrorCode::invalid_metadata,
               "'" + std::string(token) + "' is not an ownership kind", "OwnershipKind");
}

Result<OwnershipScope> OwnershipScope::make(OwnershipKind kind, OwnershipScopeId scope) {
  if (kind == OwnershipKind::unassigned) {
    return Error(ErrorCode::invalid_metadata,
                 "an unassigned ownership scope carries no scope identifier", "ownership");
  }
  return OwnershipScope(kind, std::optional<OwnershipScopeId>(std::move(scope)));
}

std::optional<SiteMembership> DataCenterRecord::primary_membership() const noexcept {
  for (const auto& membership : memberships) {
    if (membership.role == MembershipRole::primary) {
      return membership;
    }
  }
  return std::nullopt;
}

const SiteMembership* DataCenterRecord::membership_for(const SiteId& site) const noexcept {
  const std::size_t position = lower_bound_position_for_site(site);
  if (position >= memberships.size() || !(memberships[position].site == site)) {
    return nullptr;
  }
  return &memberships[position];
}

std::size_t DataCenterRecord::lower_bound_position_for_site(const SiteId& site) const noexcept {
  const auto position =
      std::lower_bound(memberships.begin(), memberships.end(), site,
                       [](const SiteMembership& membership, const SiteId& wanted) {
                         return membership.site < wanted;
                       });
  return static_cast<std::size_t>(position - memberships.begin());
}

bool DataCenterRecord::has_alias(const Alias& alias) const noexcept {
  return std::binary_search(aliases.begin(), aliases.end(), alias);
}

bool operator==(const DataCenterRecord& left, const DataCenterRecord& right) {
  return left.id == right.id && left.revision == right.revision &&
         left.display_name == right.display_name && left.aliases == right.aliases &&
         left.memberships == right.memberships && left.facility == right.facility &&
         left.extensions == right.extensions && left.ownership == right.ownership &&
         left.compatibility == right.compatibility && left.state == right.state &&
         left.created_generation == right.created_generation &&
         left.retired_generation == right.retired_generation &&
         left.retirement == right.retirement && left.replaces == right.replaces &&
         left.last_provenance == right.last_provenance &&
         left.last_sequence == right.last_sequence;
}

Result<void> validate_record(const DataCenterRecord& record, const RegistryLimits& limits) {
  // --- display name -----------------------------------------------------
  if (auto result = internal::validate_text(record.display_name, limits.max_display_name_bytes,
                                            "display_name", false);
      !result.has_value()) {
    return result.error();
  }

  // --- aliases ----------------------------------------------------------
  if (record.aliases.size() > limits.max_aliases_per_record) {
    return fail(ErrorCode::limit_exceeded,
                "a record may carry at most " + std::to_string(limits.max_aliases_per_record) +
                    " aliases",
                "aliases");
  }
  for (std::size_t index = 0; index < record.aliases.size(); ++index) {
    if (index > 0 && !(record.aliases[index - 1] < record.aliases[index])) {
      return fail(ErrorCode::internal_error,
                  "aliases must be unique and in canonical order", "aliases");
    }
  }

  // --- memberships ------------------------------------------------------
  if (record.memberships.size() > limits.max_memberships_per_record) {
    return fail(ErrorCode::limit_exceeded,
                "a record may carry at most " +
                    std::to_string(limits.max_memberships_per_record) + " site memberships",
                "memberships");
  }
  std::size_t primary_count = 0;
  for (std::size_t index = 0; index < record.memberships.size(); ++index) {
    if (index > 0 && !(record.memberships[index - 1].site < record.memberships[index].site)) {
      return fail(ErrorCode::internal_error,
                  "site memberships must be unique per site and in canonical order",
                  "memberships");
    }
    if (record.memberships[index].role == MembershipRole::primary) {
      ++primary_count;
    }
  }
  if (primary_count > 1) {
    return fail(ErrorCode::membership_violation,
                "a record may have at most one primary site membership, found " +
                    std::to_string(primary_count),
                "memberships");
  }
  if (is_live(record.state) && primary_count != 1) {
    return fail(ErrorCode::membership_violation,
                std::string("a record in state '") + std::string(to_string(record.state)) +
                    "' must have exactly one primary site membership",
                "memberships");
  }

  // --- metadata ---------------------------------------------------------
  if (auto result = record.facility.validate(limits, "facility"); !result.has_value()) {
    return result.error();
  }
  if (auto result = record.extensions.validate(limits, "extensions"); !result.has_value()) {
    return result.error();
  }

  // --- revision and generations ----------------------------------------
  if (record.revision.value() < MetadataRevision::kMinValue) {
    return fail(ErrorCode::internal_error, "a record revision starts at 1", "revision");
  }

  // --- lifecycle and retirement coupling --------------------------------
  const bool terminal = is_terminal(record.state);
  if (terminal != record.retired_generation.has_value()) {
    return fail(ErrorCode::internal_error,
                "retired_generation is present exactly when the record is terminal",
                "retired_generation");
  }
  if (terminal != record.retirement.has_value()) {
    return fail(ErrorCode::internal_error,
                "retirement detail is present exactly when the record is terminal", "retirement");
  }
  if (record.retirement.has_value()) {
    const auto& retirement = *record.retirement;
    if (retirement.generation != *record.retired_generation) {
      return fail(ErrorCode::internal_error,
                  "retirement.generation must equal retired_generation", "retirement");
    }
    if (*record.retired_generation < record.created_generation) {
      return fail(ErrorCode::internal_error,
                  "a record cannot be retired before it was created", "retired_generation");
    }
    const bool replaced = record.state == LifecycleState::replaced;
    if (replaced != retirement.replaced_by.has_value()) {
      return fail(ErrorCode::internal_error,
                  "retirement.replaced_by is present exactly when the state is 'replaced'",
                  "retirement.replaced_by");
    }
    if (retirement.replaced_by.has_value() && *retirement.replaced_by == record.id) {
      return fail(ErrorCode::internal_error, "a record cannot replace itself",
                  "retirement.replaced_by");
    }
  }
  if (record.replaces.has_value() && *record.replaces == record.id) {
    return fail(ErrorCode::internal_error, "a record cannot replace itself", "replaces");
  }
  if (record.replaces.has_value() && terminal) {
    return fail(ErrorCode::internal_error,
                "a record created by a replacement is created live and cannot be terminal",
                "replaces");
  }

  // --- provenance -------------------------------------------------------
  if (record.last_provenance.sequence() != record.last_sequence) {
    return fail(ErrorCode::internal_error,
                "last_sequence must equal the sequence of the last provenance record",
                "last_sequence");
  }
  if (record.last_provenance.generation() < record.created_generation) {
    return fail(ErrorCode::internal_error,
                "the last provenance record cannot precede the record's creation",
                "last_provenance");
  }
  return {};
}

Result<void> validate_record_set(const std::vector<DataCenterRecord>& records,
                                 const RegistryLimits& limits) {
  if (records.size() > limits.max_records) {
    return fail(ErrorCode::limit_exceeded,
                "the registry holds at most " + std::to_string(limits.max_records) + " records",
                "records");
  }

  std::set<Alias> aliases;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    if (auto result = validate_record(record, limits); !result.has_value()) {
      return result.error();
    }
    if (index > 0 && !(records[index - 1].id < record.id)) {
      return fail(ErrorCode::internal_error,
                  "records must be unique by canonical identity and in canonical order", "records");
    }
    for (const auto& alias : record.aliases) {
      if (!aliases.insert(alias).second) {
        return fail(ErrorCode::alias_conflict,
                    "alias '" + alias.value() + "' is claimed by more than one record", "aliases");
      }
    }
  }

  // Replacement links must agree in both directions. A one-sided link would
  // let a record claim it was replaced by something that does not acknowledge
  // it, or hide a successor.
  for (const auto& record : records) {
    if (record.replaces.has_value()) {
      const auto position = std::lower_bound(
          records.begin(), records.end(), *record.replaces,
          [](const DataCenterRecord& candidate, const DataCenterId& wanted) {
            return candidate.id < wanted;
          });
      if (position == records.end() || !(position->id == *record.replaces)) {
        return fail(ErrorCode::internal_error,
                    "record '" + record.id.value() + "' claims to replace '" +
                        record.replaces->value() + "', which does not exist",
                    "replaces");
      }
      if (position->state != LifecycleState::replaced) {
        return fail(ErrorCode::internal_error,
                    "record '" + record.id.value() + "' replaced '" +
                        record.replaces->value() + "', which is not in state 'replaced'",
                    "replaces");
      }
      if (!position->retirement.has_value() ||
          !(position->retirement->replaced_by.value() == record.id)) {
        return fail(ErrorCode::internal_error,
                    "the replacement link between '" + record.id.value() + "' and '" +
                        record.replaces->value() + "' is not symmetric",
                    "replaces");
      }
      if (position->retirement->generation != record.created_generation) {
        return fail(ErrorCode::internal_error,
                    "a replacement and the record it retires share one generation",
                    "created_generation");
      }
    }
    if (record.retirement.has_value() && record.retirement->replaced_by.has_value()) {
      const auto position = std::lower_bound(
          records.begin(), records.end(), *record.retirement->replaced_by,
          [](const DataCenterRecord& candidate, const DataCenterId& wanted) {
            return candidate.id < wanted;
          });
      if (position == records.end() || !(position->id == *record.retirement->replaced_by)) {
        return fail(ErrorCode::internal_error,
                    "record '" + record.id.value() + "' names successor '" +
                        record.retirement->replaced_by->value() + "', which does not exist",
                    "retirement.replaced_by");
      }
      if (!(position->replaces.value() == record.id)) {
        return fail(ErrorCode::internal_error,
                    "the replacement link between '" + record.id.value() + "' and '" +
                        record.retirement->replaced_by->value() + "' is not symmetric",
                    "retirement.replaced_by");
      }
      if (position->created_generation != *record.retired_generation) {
        return fail(ErrorCode::internal_error,
                    "a replacement and the record it retires share one generation",
                    "retired_generation");
      }
    }
  }
  return {};
}

}  // namespace dcr
