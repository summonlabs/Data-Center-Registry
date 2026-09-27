// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Enumeration, history queries and statistics.
//
// Every ordering this library publishes is a total order with an explicit
// tie-break, so an enumeration is reproducible byte for byte across processes,
// platforms and runs. `canonical_id_ascending` is the order used by canonical
// serialisation and by the on-disk state.

#ifndef DCR_QUERY_HPP
#define DCR_QUERY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/digest.hpp"
#include "dcr/generation.hpp"
#include "dcr/identity.hpp"
#include "dcr/lifecycle.hpp"
#include "dcr/limits.hpp"
#include "dcr/provenance.hpp"
#include "dcr/record.hpp"
#include "dcr/result.hpp"

namespace dcr {

/// Public enumeration order. Every order ends with a canonical-identity
/// ascending tie-break, so no two distinct records compare equal.
enum class EnumerationOrder : std::uint8_t {
  /// Byte-wise ascending canonical identity.
  canonical_id_ascending = 1,
  /// Display name ascending, then canonical identity ascending.
  display_name_ascending = 2,
  /// Metadata revision descending, then canonical identity ascending. The most
  /// recently changed records come first.
  revision_descending = 3,
  /// Creation generation descending, then canonical identity ascending.
  created_generation_descending = 4,
};

[[nodiscard]] std::string_view to_string(EnumerationOrder order) noexcept;
[[nodiscard]] Result<EnumerationOrder> parse_enumeration_order(std::string_view token);

/// A filter over the records of one registry.
///
/// All fields are optional and combine conjunctively. A query never changes
/// state and never allocates more than the configured result bound.
struct EnumerationQuery {
  std::optional<LifecycleState> state;
  std::optional<SiteId> site;
  std::optional<OwnershipKind> ownership_kind;
  std::optional<OwnershipScopeId> ownership_scope;
  std::optional<RegionCode> region;
  std::optional<CountryCode> country;
  /// When false, retired and replaced records are excluded.
  bool include_terminal = true;
  EnumerationOrder order = EnumerationOrder::canonical_id_ascending;
  /// Maximum number of records to return. Defaults to the configured
  /// max_query_results.
  std::optional<std::size_t> limit;
  /// Number of leading matches to skip, applied after ordering.
  std::size_t offset = 0;
};

[[nodiscard]] Result<void> validate_query(const EnumerationQuery& query,
                                          const RegistryLimits& limits);

enum class HistoryAction : std::uint8_t {
  registered = 1,
  metadata_updated = 2,
  lifecycle_transition = 3,
  site_attached = 4,
  site_detached = 5,
  retired = 6,
  /// The predecessor of a replacement.
  replacement_retired = 7,
  /// The successor of a replacement.
  replacement_created = 8,
};

[[nodiscard]] std::string_view to_string(HistoryAction action) noexcept;
[[nodiscard]] Result<HistoryAction> parse_history_action(std::string_view token);

/// One committed change, as recorded in the bounded history window.
struct HistoryEntry {
  SequenceNumber sequence;
  RegistryGeneration generation;
  DataCenterId id;
  HistoryAction action = HistoryAction::registered;
  /// The revision of the record after the change.
  MetadataRevision revision;
  /// The state before the change. Empty when the record was created.
  std::optional<LifecycleState> previous_state;
  LifecycleState new_state = LifecycleState::proposed;
  ProvenanceRecord provenance;

  friend bool operator==(const HistoryEntry& left, const HistoryEntry& right);
  friend bool operator!=(const HistoryEntry& left, const HistoryEntry& right) {
    return !(left == right);
  }
};

/// A filter over the recorded history. Entries are always returned in
/// ascending sequence order.
struct HistoryQuery {
  std::optional<DataCenterId> id;
  std::optional<HistoryAction> action;
  /// Only entries committed at or after this generation.
  std::optional<RegistryGeneration> from_generation;
  /// Only entries recorded at or after this sequence.
  std::optional<SequenceNumber> from_sequence;
  std::size_t limit = 256;
  std::size_t offset = 0;
};

/// A page of history plus the accounting needed to interpret it honestly.
struct HistoryPage {
  std::vector<HistoryEntry> entries;
  /// Number of entries in the retained window that matched the filter before
  /// limit and offset were applied.
  std::uint64_t matched_total = 0;
  /// Total entries evicted from the bounded window since this registry was
  /// created. Non-zero means older entries exist but are gone.
  std::uint64_t dropped_entries = 0;
  /// The earliest sequence still retained, when anything is retained. A query
  /// for an earlier sequence is answered from what remains, and this field is
  /// how a caller can tell.
  std::optional<SequenceNumber> earliest_retained_sequence;
  /// True when the page was cut short by `limit`.
  bool truncated = false;
};

/// A consistent picture of one registry state.
struct RegistryStats {
  RegistryGeneration generation;
  std::optional<EpochToken> external_epoch;
  std::uint64_t record_count = 0;
  /// Record counts indexed by lifecycle_state_index(), so the array always has
  /// value for every state and sum(records_by_state) == record_count.
  std::array<std::uint64_t, kLifecycleStateCount> records_by_state{};
  std::uint64_t alias_count = 0;
  std::uint64_t membership_count = 0;
  std::uint64_t distinct_site_count = 0;
  std::uint64_t distinct_ownership_scope_count = 0;
  std::uint64_t history_entries = 0;
  std::uint64_t history_dropped = 0;
  std::uint64_t idempotency_entries = 0;
  /// Size in bytes of the canonical payload of this state.
  std::uint64_t snapshot_payload_bytes = 0;
  /// SHA-256 of the canonical payload of this state.
  Sha256Digest snapshot_digest;

  [[nodiscard]] std::uint64_t count_of(LifecycleState state) const noexcept {
    return records_by_state[lifecycle_state_index(state)];
  }
};

}  // namespace dcr

#endif  // DCR_QUERY_HPP
