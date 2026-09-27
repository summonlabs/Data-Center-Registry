// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Mutation commands.
//
// Commands are the only way to change authoritative state. A command is a
// value: it can be built, inspected, serialised by a caller and replayed. It
// carries its own preconditions, its own provenance and optionally an
// idempotency key.
//
// Preconditions are the caller's statement about the state it believes it is
// mutating. A command that targets an existing record must carry an expected
// registry generation; without it the registry cannot tell a fresh caller from
// a stale one, so the mutation is rejected rather than guessed at.

#ifndef DCR_COMMANDS_HPP
#define DCR_COMMANDS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/cancellation.hpp"
#include "dcr/compatibility.hpp"
#include "dcr/generation.hpp"
#include "dcr/identity.hpp"
#include "dcr/lifecycle.hpp"
#include "dcr/limits.hpp"
#include "dcr/membership.hpp"
#include "dcr/metadata.hpp"
#include "dcr/provenance.hpp"
#include "dcr/record.hpp"
#include "dcr/result.hpp"

namespace dcr {

/// The state a caller believes it is mutating.
struct MutationPrecondition {
  /// The registry generation the caller last observed. Required for every
  /// command that targets an existing record.
  std::optional<RegistryGeneration> expected_generation;
  /// The revision of the target record the caller last observed. Optional;
  /// when present it is checked as well, which catches a caller that re-read
  /// the generation but not the record.
  std::optional<MetadataRevision> expected_revision;
  /// An external control-plane epoch token the caller is asserting.
  ///
  /// The registry compares it against the epoch token it has recorded from the
  /// same issuing authority. A token older than the recorded one is rejected
  /// as stale authority; a token from an unknown authority is rejected; a
  /// newer token from the recorded authority is accepted and becomes the
  /// recorded token. The registry never elects an epoch and never fences a
  /// controller: it only refuses to act on an assertion it can already see is
  /// out of date.
  std::optional<EpochToken> asserted_epoch;
};

/// The parts of a mutation that every command shares.
///
/// Commands are requests built by a caller, not authoritative state, so a
/// required field is modelled as an absent optional that the registry rejects
/// with a specific category rather than as a type-level impossibility. That
/// keeps every command default-constructible and assemblable field by field,
/// while the record and provenance types the registry stores keep their
/// non-optional strong identities.
struct CommandContext {
  MutationPrecondition precondition;
  ProvenanceInput provenance;
  std::optional<IdempotencyKey> idempotency_key;
  /// Checked at the start of the mutation, before the new generation is
  /// reserved, and again after the new generation has been written and
  /// verified but before it is published.
  CancellationToken cancellation;
};

/// A field update that separates "leave it alone" from "set it" from "clear
/// it". An absent optional would collapse the first two, which is how partial
/// updates silently erase data.
template <class T>
class FieldPatch {
 public:
  enum class Action : std::uint8_t { keep = 1, set = 2, clear = 3 };

  FieldPatch() = default;

  [[nodiscard]] static FieldPatch keep() noexcept { return FieldPatch(Action::keep, std::nullopt); }
  [[nodiscard]] static FieldPatch set(T value) {
    return FieldPatch(Action::set, std::optional<T>(std::move(value)));
  }
  [[nodiscard]] static FieldPatch clear() noexcept {
    return FieldPatch(Action::clear, std::nullopt);
  }

  [[nodiscard]] Action action() const noexcept { return action_; }
  [[nodiscard]] bool is_keep() const noexcept { return action_ == Action::keep; }
  [[nodiscard]] bool is_set() const noexcept { return action_ == Action::set; }
  [[nodiscard]] bool is_clear() const noexcept { return action_ == Action::clear; }
  /// The value. Only valid when is_set().
  [[nodiscard]] const T& value() const noexcept { return *value_; }

  friend bool operator==(const FieldPatch& left, const FieldPatch& right) {
    if (left.action_ != right.action_) {
      return false;
    }
    if (left.action_ != Action::set) {
      return true;
    }
    return *left.value_ == *right.value_;
  }
  friend bool operator!=(const FieldPatch& left, const FieldPatch& right) {
    return !(left == right);
  }

 private:
  FieldPatch(Action action, std::optional<T> value) : action_(action), value_(std::move(value)) {}

  Action action_ = Action::keep;
  std::optional<T> value_;
};

/// The descriptive fields of a data center. Shared by registration and by
/// replacement so that both validate identically.
///
/// Every field is optional in the type and checked by the registry: a
/// compatibility key is required, and everything else has a meaningful
/// default.
struct RecordDraft {
  std::string display_name;
  std::vector<Alias> aliases;
  std::vector<SiteMembership> memberships;
  FacilityMetadata facility;
  MetadataMap extensions;
  /// Defaults to `unassigned`.
  OwnershipScope ownership;
  /// The compatibility contract the record will carry. Required.
  std::optional<CompatibilityKey> compatibility;
};

/// The fields an update command can change.
///
/// Identity, creation generation, retirement and provenance are not patchable:
/// they are either immutable or owned by a dedicated operation.
struct MetadataPatch {
  /// `clear` is not accepted: a record always has a display name.
  FieldPatch<std::string> display_name;
  /// Replaces the whole alias set. Aliases removed here are released.
  FieldPatch<std::vector<Alias>> aliases;
  FieldPatch<std::vector<SiteMembership>> memberships;
  FieldPatch<FacilityMetadata> facility;
  FieldPatch<MetadataMap> extensions;
  FieldPatch<OwnershipScope> ownership;
  FieldPatch<CompatibilityKey> compatibility;

  /// True when no field would change.
  [[nodiscard]] bool is_empty() const noexcept {
    return display_name.is_keep() && aliases.is_keep() && memberships.is_keep() &&
           facility.is_keep() && extensions.is_keep() && ownership.is_keep() &&
           compatibility.is_keep();
  }
};

/// Creates a new data center.
struct RegisterCommand {
  CommandContext context;
  /// The identity to create. Required.
  std::optional<DataCenterId> id;
  RecordDraft draft;
  /// A new record may start `proposed` or `registered` only. Anything else
  /// would claim operational history the registry never observed.
  LifecycleState initial_state = LifecycleState::proposed;
  /// `expected_generation` in the context is ignored for registration: the
  /// registry rejects a registration only when the identity already exists.
};

/// Changes descriptive fields of an existing record.
struct UpdateMetadataCommand {
  CommandContext context;
  /// The record to change. Required.
  std::optional<DataCenterId> id;
  MetadataPatch patch;
};

/// Moves a record to another lifecycle state.
struct TransitionCommand {
  CommandContext context;
  /// The record to move. Required.
  std::optional<DataCenterId> id;
  LifecycleState target_state = LifecycleState::registered;
  /// Recorded in provenance. A transition without one is legal and carries no
  /// reason.
  std::optional<ReasonCode> reason;
};

/// Adds a site membership, or changes the role of an existing one.
struct AttachSiteCommand {
  CommandContext context;
  /// The record to change. Required.
  std::optional<DataCenterId> id;
  /// The membership to attach. Required.
  std::optional<SiteMembership> membership;
};

/// Removes a site membership.
struct DetachSiteCommand {
  CommandContext context;
  /// The record to change. Required.
  std::optional<DataCenterId> id;
  /// The site to detach. Required.
  std::optional<SiteId> site;
};

/// Retires a record with no successor.
struct RetireCommand {
  CommandContext context;
  /// The record to retire. Required.
  std::optional<DataCenterId> id;
  /// Recorded in provenance and in the record's retirement detail.
  std::optional<ReasonCode> reason;
};

/// Retires a record and creates its successor in one atomic generation, or
/// does neither.
struct ReplaceCommand {
  CommandContext context;
  /// The record being retired. Required.
  std::optional<DataCenterId> retired_id;
  /// The identity of the successor. Required, and different from the
  /// predecessor.
  std::optional<DataCenterId> new_id;
  RecordDraft draft;
  /// A successor must be live when it is created: `registered` or `active`.
  /// A replacement that stands in for a working facility cannot itself be a
  /// proposal.
  LifecycleState initial_state = LifecycleState::registered;
  /// Recorded in provenance and in the predecessor's retirement detail.
  /// Required: a replacement must say why the predecessor left service.
  std::optional<ReasonCode> reason;
};

/// Validates a command's own content against the configured limits, without
/// touching registry state. Used by the mutation path and by tools that want
/// to reject bad input before opening a store.
///
/// validate_command_context() validates the parts every command shares:
/// provenance a caller is allowed to assert, and a detail string within the
/// configured bound.
[[nodiscard]] Result<void> validate_command_context(const CommandContext& context,
                                                    const RegistryLimits& limits);
[[nodiscard]] Result<void> validate_register_command(const RegisterCommand& command,
                                                     const RegistryLimits& limits);
[[nodiscard]] Result<void> validate_update_command(const UpdateMetadataCommand& command,
                                                   const RegistryLimits& limits);
[[nodiscard]] Result<void> validate_transition_command(const TransitionCommand& command);
[[nodiscard]] Result<void> validate_attach_command(const AttachSiteCommand& command);
[[nodiscard]] Result<void> validate_detach_command(const DetachSiteCommand& command);
[[nodiscard]] Result<void> validate_retire_command(const RetireCommand& command);
[[nodiscard]] Result<void> validate_replace_command(const ReplaceCommand& command,
                                                    const RegistryLimits& limits);

}  // namespace dcr

#endif  // DCR_COMMANDS_HPP
