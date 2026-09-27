// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The mutation engine.
//
// Every command is planned against a base state and produces either a
// rejection, or a complete new state plus the outcome to report. Planning is
// pure: it does not touch the store, the clock or any lock. Publication is a
// separate step owned by the registry, which is what makes the ordering
// "validate everything, then write" structural rather than a convention.
//
// Precedence, stated once and relied on everywhere:
//
//   1. command content is validated against the configured limits;
//   2. cancellation is honoured, so a cancelled mutation never even plans;
//   3. an idempotency key is resolved, so a retry reports the original outcome
//      instead of acting twice;
//   4. the target is located, so an unknown identity is `not_found`;
//   5. a command whose desired end state already holds is `unchanged`, before
//      any precondition is consulted. Reporting a stale generation for work
//      that is already complete would tell a caller to re-read state and retry
//      an operation that has nothing left to do;
//   6. preconditions are enforced for every command that would change state:
//      stale generation, stale revision and stale external authority;
//   7. the change is applied and the whole resulting state is validated.
//
// A command that reports `unchanged` writes nothing at all: no generation, no
// revision, no provenance entry. Repeating it recomputes the same deterministic
// answer, which is what makes it idempotent without storing anything.

#ifndef DCR_CORE_MUTATION_HPP
#define DCR_CORE_MUTATION_HPP

#include <optional>
#include <vector>

#include "core/state.hpp"
#include "dcr/commands.hpp"
#include "dcr/digest.hpp"
#include "dcr/outcome.hpp"
#include "dcr/result.hpp"
#include "dcr/time.hpp"

namespace dcr::internal {

/// The complete result of planning one mutation.
struct MutationPlan {
  /// True when the command asked for something that already holds. `state` is
  /// then not meaningful and must not be published.
  bool unchanged = false;
  /// The complete new authoritative state. Only meaningful when !unchanged.
  RegistryState state;
  /// What to report to the caller.
  MutationResult result;
};

/// The canonical digest of a command's semantic content. It covers every field
/// that determines what the command means, and deliberately excludes the
/// expected generation, the expected revision and the idempotency key itself,
/// so that an honest retry that re-read state, or that is identified by its
/// key, matches the original.
[[nodiscard]] Sha256Digest digest_register_command(const RegisterCommand& command);
[[nodiscard]] Sha256Digest digest_update_command(const UpdateMetadataCommand& command);
[[nodiscard]] Sha256Digest digest_transition_command(const TransitionCommand& command);
[[nodiscard]] Sha256Digest digest_attach_command(const AttachSiteCommand& command);
[[nodiscard]] Sha256Digest digest_detach_command(const DetachSiteCommand& command);
[[nodiscard]] Sha256Digest digest_retire_command(const RetireCommand& command);
[[nodiscard]] Sha256Digest digest_replace_command(const ReplaceCommand& command);

[[nodiscard]] Result<MutationPlan> plan_register(const RegistryState& base,
                                                 const RegisterCommand& command, Timestamp now);
[[nodiscard]] Result<MutationPlan> plan_update(const RegistryState& base,
                                               const UpdateMetadataCommand& command,
                                               Timestamp now);
[[nodiscard]] Result<MutationPlan> plan_transition(const RegistryState& base,
                                                   const TransitionCommand& command,
                                                   Timestamp now);
[[nodiscard]] Result<MutationPlan> plan_attach(const RegistryState& base,
                                               const AttachSiteCommand& command, Timestamp now);
[[nodiscard]] Result<MutationPlan> plan_detach(const RegistryState& base,
                                               const DetachSiteCommand& command, Timestamp now);
[[nodiscard]] Result<MutationPlan> plan_retire(const RegistryState& base,
                                               const RetireCommand& command, Timestamp now);
[[nodiscard]] Result<MutationPlan> plan_replace(const RegistryState& base,
                                                const ReplaceCommand& command, Timestamp now);

/// Sorts an alias set into canonical order, rejecting duplicates. Aliases are a
/// set, so the input order carries no meaning; duplicates would be a silent
/// merge, so they are rejected.
[[nodiscard]] Result<std::vector<Alias>> canonicalize_aliases(std::vector<Alias> aliases,
                                                              const RegistryLimits& limits);

/// Sorts site memberships by site, rejecting two entries for the same site.
[[nodiscard]] Result<std::vector<SiteMembership>> canonicalize_memberships(
    std::vector<SiteMembership> memberships, const RegistryLimits& limits);

/// Resolves the external epoch token a mutation will record. A token older than
/// the recorded one, or from a different issuing authority, is rejected as
/// stale authority.
[[nodiscard]] Result<std::optional<EpochToken>> resolve_epoch(
    const RegistryState& base, const std::optional<EpochToken>& asserted);

}  // namespace dcr::internal

#endif  // DCR_CORE_MUTATION_HPP
