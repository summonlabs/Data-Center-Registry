// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Command validation.
//
// These functions check a command's own content against the configured limits.
// They never touch registry state: whether the target exists, whether the
// caller's generation is current and whether the transition is legal are
// decided by the mutation engine, which has the state to decide them.

#include "dcr/commands.hpp"

#include <algorithm>
#include <string>

#include "internal/checked.hpp"
#include "internal/text.hpp"

namespace dcr {
namespace {

using internal::validate_text;

[[nodiscard]] Result<void> validate_aliases(const std::vector<Alias>& aliases,
                                            const RegistryLimits& limits) {
  if (aliases.size() > limits.max_aliases_per_record) {
    return Error(ErrorCode::limit_exceeded,
                 "at most " + std::to_string(limits.max_aliases_per_record) +
                     " aliases are accepted per record",
                 "aliases");
  }
  return {};
}

[[nodiscard]] Result<void> validate_memberships(const std::vector<SiteMembership>& memberships,
                                                const RegistryLimits& limits) {
  if (memberships.size() > limits.max_memberships_per_record) {
    return Error(ErrorCode::limit_exceeded,
                 "at most " + std::to_string(limits.max_memberships_per_record) +
                     " site memberships are accepted per record",
                 "memberships");
  }
  std::size_t primary_count = 0;
  for (const auto& membership : memberships) {
    if (membership.role == MembershipRole::primary) {
      ++primary_count;
    }
  }
  if (primary_count > 1) {
    return Error(ErrorCode::membership_violation,
                 "at most one primary site membership is accepted per record", "memberships");
  }
  return {};
}

[[nodiscard]] Result<void> validate_draft(const RecordDraft& draft, const RegistryLimits& limits,
                                          const char* field) {
  if (auto result = validate_text(draft.display_name, limits.max_display_name_bytes,
                                  std::string(field) + ".display_name", false);
      !result.has_value()) {
    return result.error();
  }
  if (!draft.compatibility.has_value()) {
    return Error(ErrorCode::invalid_metadata,
                 std::string(field) +
                     ".compatibility is required: every record carries a compatibility contract",
                 std::string(field) + ".compatibility");
  }
  if (auto result = validate_aliases(draft.aliases, limits); !result.has_value()) {
    return result.error();
  }
  if (auto result = validate_memberships(draft.memberships, limits); !result.has_value()) {
    return result.error();
  }
  if (auto result = draft.facility.validate(limits, std::string(field) + ".facility");
      !result.has_value()) {
    return result.error();
  }
  if (auto result = draft.extensions.validate(limits, std::string(field) + ".extensions");
      !result.has_value()) {
    return result.error();
  }
  return {};
}

}  // namespace

Result<void> validate_command_context(const CommandContext& context,
                                      const RegistryLimits& limits) {
  if (!is_caller_suppliable(context.provenance.source)) {
    return Error(ErrorCode::invalid_provenance,
                 "provenance source '" + std::string(to_string(context.provenance.source)) +
                     "' is produced by the registry itself and cannot be asserted by a caller",
                 "provenance.source");
  }
  if (!context.provenance.principal.has_value()) {
    return Error(ErrorCode::invalid_provenance,
                 "a mutation must say who is making it; set provenance.principal",
                 "provenance.principal");
  }
  if (auto result = validate_text(context.provenance.detail, limits.max_provenance_detail_bytes,
                                  "provenance.detail");
      !result.has_value()) {
    return result.error();
  }
  return {};
}

namespace {

/// Presence checks for the required fields of a command. Each reports the
/// stable category for a missing field of that kind.
[[nodiscard]] Result<void> require_identity(const std::optional<DataCenterId>& value,
                                            const char* field) {
  if (!value.has_value()) {
    return Error(ErrorCode::invalid_identity,
                 std::string("this command must name the data center it acts on; set ") + field,
                 field);
  }
  return {};
}

}  // namespace

Result<void> validate_register_command(const RegisterCommand& command,
                                       const RegistryLimits& limits) {
  if (auto result = validate_command_context(command.context, limits); !result.has_value()) {
    return result.error();
  }
  if (auto result = require_identity(command.id, "id"); !result.has_value()) {
    return result.error();
  }
  if (command.context.precondition.expected_generation.has_value() ||
      command.context.precondition.expected_revision.has_value()) {
    return Error(ErrorCode::invalid_argument,
                 "registration creates a new record, so it cannot carry an expected generation "
                 "or an expected revision",
                 "precondition");
  }
  if (command.initial_state != LifecycleState::proposed &&
      command.initial_state != LifecycleState::registered) {
    return Error(ErrorCode::invalid_argument,
                 "a new record starts 'proposed' or 'registered'; '" +
                     std::string(to_string(command.initial_state)) +
                     "' would claim operational history the registry never observed",
                 "initial_state");
  }
  return validate_draft(command.draft, limits, "draft");
}

Result<void> validate_update_command(const UpdateMetadataCommand& command,
                                     const RegistryLimits& limits) {
  if (auto result = validate_command_context(command.context, limits); !result.has_value()) {
    return result.error();
  }
  if (auto result = require_identity(command.id, "id"); !result.has_value()) {
    return result.error();
  }
  if (!command.context.precondition.expected_generation.has_value()) {
    return Error(ErrorCode::invalid_argument,
                 "an update must carry the expected registry generation it is based on",
                 "precondition.expected_generation");
  }
  const auto& patch = command.patch;
  if (patch.is_empty()) {
    return Error(ErrorCode::invalid_argument, "an update must change at least one field", "patch");
  }
  if (patch.display_name.is_clear()) {
    return Error(ErrorCode::invalid_argument,
                 "display_name cannot be cleared; a record always has a display name",
                 "patch.display_name");
  }
  if (patch.compatibility.is_clear()) {
    return Error(ErrorCode::invalid_argument,
                 "compatibility cannot be cleared; every record carries a compatibility contract",
                 "patch.compatibility");
  }
  if (patch.display_name.is_set()) {
    if (auto result = validate_text(patch.display_name.value(), limits.max_display_name_bytes,
                                    "patch.display_name", false);
        !result.has_value()) {
      return result.error();
    }
  }
  if (patch.aliases.is_set()) {
    if (auto result = validate_aliases(patch.aliases.value(), limits); !result.has_value()) {
      return result.error();
    }
  }
  if (patch.memberships.is_set()) {
    if (auto result = validate_memberships(patch.memberships.value(), limits);
        !result.has_value()) {
      return result.error();
    }
  }
  if (patch.facility.is_set()) {
    if (auto result = patch.facility.value().validate(limits, "patch.facility");
        !result.has_value()) {
      return result.error();
    }
  }
  if (patch.extensions.is_set()) {
    if (auto result = patch.extensions.value().validate(limits, "patch.extensions");
        !result.has_value()) {
      return result.error();
    }
  }
  return {};
}

Result<void> validate_transition_command(const TransitionCommand& command) {
  if (auto result = require_identity(command.id, "id"); !result.has_value()) {
    return result.error();
  }
  if (command.target_state == LifecycleState::replaced) {
    return Error(ErrorCode::illegal_transition,
                 "'replaced' is reachable only through the replacement operation, because it "
                 "requires naming a successor",
                 "target_state");
  }
  return {};
}

Result<void> validate_attach_command(const AttachSiteCommand& command) {
  if (auto result = require_identity(command.id, "id"); !result.has_value()) {
    return result.error();
  }
  if (!command.membership.has_value()) {
    return Error(ErrorCode::invalid_argument,
                 "attach-site must say which membership to attach; set membership",
                 "membership");
  }
  return {};
}

Result<void> validate_detach_command(const DetachSiteCommand& command) {
  if (auto result = require_identity(command.id, "id"); !result.has_value()) {
    return result.error();
  }
  if (!command.site.has_value()) {
    return Error(ErrorCode::invalid_argument,
                 "detach-site must say which site to detach; set site", "site");
  }
  return {};
}

Result<void> validate_retire_command(const RetireCommand& command) {
  return require_identity(command.id, "id");
}

Result<void> validate_replace_command(const ReplaceCommand& command,
                                      const RegistryLimits& limits) {
  if (auto result = validate_command_context(command.context, limits); !result.has_value()) {
    return result.error();
  }
  if (auto result = require_identity(command.retired_id, "retired_id"); !result.has_value()) {
    return result.error();
  }
  if (auto result = require_identity(command.new_id, "new_id"); !result.has_value()) {
    return result.error();
  }
  if (!command.context.precondition.expected_generation.has_value()) {
    return Error(ErrorCode::invalid_argument,
                 "a replacement must carry the expected registry generation it is based on, "
                 "because it retires a record that another writer may already have changed",
                 "precondition.expected_generation");
  }
  if (command.new_id.value() == command.retired_id.value()) {
    return Error(ErrorCode::invalid_argument,
                 "a replacement must create a new identity; a record cannot replace itself",
                 "new_id");
  }
  if (command.initial_state != LifecycleState::registered &&
      command.initial_state != LifecycleState::active) {
    return Error(ErrorCode::invalid_argument,
                 "a replacement stands in for a facility in service, so it starts 'registered' or "
                 "'active'",
                 "initial_state");
  }
  return validate_draft(command.draft, limits, "draft");
}

}  // namespace dcr
