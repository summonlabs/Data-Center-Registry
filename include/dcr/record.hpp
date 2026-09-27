// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The canonical data-center record.
//
// A record is the authoritative statement that a data center exists, under a
// stable identity, with a lifecycle state, an owning scope, a compatibility
// contract, site memberships and provenance. It is a plain value type: records
// obtained from a registry are always valid, and the only way to change one is
// to submit a mutation command, which revalidates the whole record before
// anything is committed.

#ifndef DCR_RECORD_HPP
#define DCR_RECORD_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/compatibility.hpp"
#include "dcr/generation.hpp"
#include "dcr/identity.hpp"
#include "dcr/lifecycle.hpp"
#include "dcr/limits.hpp"
#include "dcr/membership.hpp"
#include "dcr/metadata.hpp"
#include "dcr/provenance.hpp"
#include "dcr/result.hpp"

namespace dcr {

/// Who administers a record.
///
/// Ownership here is attribution, not policy: the registry records which scope
/// a record belongs to so that later layers can scope their own decisions. It
/// does not evaluate tenancy policy, and it never lets one scope mutate another
/// scope's record on the strength of this field alone.
enum class OwnershipKind : std::uint8_t {
  /// No scope has been assigned yet.
  unassigned = 1,
  /// The facility platform itself.
  platform = 2,
  /// A tenant of the facility.
  tenant = 3,
  /// A partner organisation.
  partner = 4,
  /// An internal system scope.
  system = 5,
};

[[nodiscard]] std::string_view to_string(OwnershipKind kind) noexcept;
[[nodiscard]] Result<OwnershipKind> parse_ownership_kind(std::string_view token);

/// An ownership attribution. `unassigned` carries no scope identifier, and any
/// other kind requires one; the invariant is enforced at construction.
///
/// A default-constructed scope is `unassigned`, which is the honest starting
/// point for a record whose ownership has not been decided yet. It is not an
/// invalid placeholder: it authorises nothing and it is a legal value in
/// authoritative state.
class OwnershipScope {
 public:
  OwnershipScope() noexcept : kind_(OwnershipKind::unassigned) {}
  OwnershipScope(const OwnershipScope&) = default;
  OwnershipScope(OwnershipScope&&) noexcept = default;
  OwnershipScope& operator=(const OwnershipScope&) = default;
  OwnershipScope& operator=(OwnershipScope&&) noexcept = default;
  ~OwnershipScope() = default;

  [[nodiscard]] static OwnershipScope unassigned() noexcept { return OwnershipScope(); }

  [[nodiscard]] static Result<OwnershipScope> make(OwnershipKind kind, OwnershipScopeId scope);

  [[nodiscard]] OwnershipKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::optional<OwnershipScopeId>& scope() const noexcept { return scope_; }

  friend bool operator==(const OwnershipScope& left, const OwnershipScope& right) noexcept {
    return left.kind_ == right.kind_ && left.scope_ == right.scope_;
  }
  friend bool operator!=(const OwnershipScope& left, const OwnershipScope& right) noexcept {
    return !(left == right);
  }

 private:
  OwnershipScope(OwnershipKind kind, std::optional<OwnershipScopeId> scope) noexcept
      : kind_(kind), scope_(std::move(scope)) {}

  OwnershipKind kind_;
  std::optional<OwnershipScopeId> scope_;
};

/// Why and when a record left service, and what stands in its place if
/// anything does.
struct RetirementInfo {
  /// Caller-supplied machine-readable reason. Optional: provenance always
  /// records who retired the record and when, and not every retirement has a
  /// code from the caller's vocabulary.
  std::optional<ReasonCode> reason;
  /// The registry generation at which the retirement committed.
  RegistryGeneration generation;
  /// Set when this record was retired by a replacement, and equal to the
  /// successor's identity.
  std::optional<DataCenterId> replaced_by;

  friend bool operator==(const RetirementInfo& left, const RetirementInfo& right) {
    return left.reason == right.reason && left.generation == right.generation &&
           left.replaced_by == right.replaced_by;
  }
  friend bool operator!=(const RetirementInfo& left, const RetirementInfo& right) {
    return !(left == right);
  }
};

/// One data center, as the registry holds it.
struct DataCenterRecord {
  /// Stable canonical identity. Never changes after creation.
  DataCenterId id;
  /// Monotonic per-record revision. Starts at 1 and increases by one for every
  /// committed change to this record.
  MetadataRevision revision;
  /// Human-facing name. Not an identity and not unique across records, but
  /// unique per record, and the subject of the exact-match secondary query.
  std::string display_name;
  /// Secondary lookup keys, in canonical order, unique within the record and
  /// globally unique across the registry.
  std::vector<Alias> aliases;
  /// Site memberships, in SiteId order, at most one per site.
  std::vector<SiteMembership> memberships;
  /// Administrative and facility metadata.
  FacilityMetadata facility;
  /// Deterministic extension metadata.
  MetadataMap extensions;
  /// Who administers this record.
  OwnershipScope ownership;
  /// The compatibility contract this record satisfies.
  CompatibilityKey compatibility;
  /// Administrative lifecycle state.
  LifecycleState state = LifecycleState::proposed;
  /// The generation at which this record was created.
  RegistryGeneration created_generation;
  /// The generation at which this record left service, when it has.
  std::optional<RegistryGeneration> retired_generation;
  /// Retirement detail, present exactly when retired_generation is present.
  std::optional<RetirementInfo> retirement;
  /// The record this one replaced, when it was created by a replacement.
  std::optional<DataCenterId> replaces;
  /// Provenance of the most recent committed change to this record.
  ProvenanceRecord last_provenance;
  /// The provenance sequence of the most recent committed change.
  SequenceNumber last_sequence;

  /// The primary site membership, when the record has one.
  [[nodiscard]] std::optional<SiteMembership> primary_membership() const noexcept;
  /// The membership for a site, when present.
  [[nodiscard]] const SiteMembership* membership_for(const SiteId& site) const noexcept;
  /// Position at which a membership for this site is, or would be, preserving
  /// canonical site order.
  [[nodiscard]] std::size_t lower_bound_position_for_site(const SiteId& site) const noexcept;
  /// True when the record carries this alias.
  [[nodiscard]] bool has_alias(const Alias& alias) const noexcept;

  friend bool operator==(const DataCenterRecord& left, const DataCenterRecord& right);
  friend bool operator!=(const DataCenterRecord& left, const DataCenterRecord& right) {
    return !(left == right);
  }
};

/// Validates one record against the configured limits and against every
/// record-level invariant. This is the single gate every record passes through
/// before it becomes authoritative, on both the mutation path and the load
/// path.
[[nodiscard]] Result<void> validate_record(const DataCenterRecord& record,
                                           const RegistryLimits& limits);

/// Validates the set-level invariants of a whole registry state: canonical
/// identity uniqueness, alias uniqueness, membership consistency across
/// records, and ordering. Used before publication and after loading.
[[nodiscard]] Result<void> validate_record_set(const std::vector<DataCenterRecord>& records,
                                               const RegistryLimits& limits);

}  // namespace dcr

#endif  // DCR_RECORD_HPP
