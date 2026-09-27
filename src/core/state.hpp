// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The authoritative in-memory state of one registry.
//
// This type is internal: consumers see records, snapshots and outcomes rather
// than this structure. It exists so that the mutation engine, the codec and
// the store can share one definition of what authoritative state is.
//
// Invariants held by a valid RegistryState:
//
//   * records are unique by canonical identity and sorted by it;
//   * aliases are globally unique across records;
//   * history entries are sorted by provenance sequence, with no gaps inside
//     the retained window;
//   * idempotency entries are unique and sorted by key;
//   * the registry generation is at least the creation generation of every
//     record and at least the generation of every history entry;
//   * last_sequence is the highest sequence ever recorded, including sequences
//     whose history entry has been evicted by the retention bound.

#ifndef DCR_CORE_STATE_HPP
#define DCR_CORE_STATE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dcr/digest.hpp"
#include "dcr/limits.hpp"
#include "dcr/outcome.hpp"
#include "dcr/query.hpp"
#include "dcr/record.hpp"
#include "dcr/result.hpp"

namespace dcr::internal {

/// A remembered idempotent outcome. Bounded by max_idempotency_entries and
/// evicted oldest-first by `recorded_sequence`.
struct IdempotencyEntry {
  IdempotencyKey key;
  /// SHA-256 of the canonical encoding of the command's semantic content.
  Sha256Digest command_digest;
  MutationStatus status = MutationStatus::created;
  DataCenterId id;
  RegistryGeneration generation;
  MetadataRevision revision;
  std::optional<DataCenterId> related_id;
  std::optional<SequenceNumber> sequence;
  SequenceNumber recorded_sequence;

  friend bool operator==(const IdempotencyEntry& left, const IdempotencyEntry& right) {
    return left.key == right.key && left.command_digest == right.command_digest &&
           left.status == right.status && left.id == right.id &&
           left.generation == right.generation && left.revision == right.revision &&
           left.related_id == right.related_id && left.sequence == right.sequence &&
           left.recorded_sequence == right.recorded_sequence;
  }
};

struct RegistryState {
  std::uint16_t format_major = 0;
  std::uint16_t format_minor = 0;
  RegistryGeneration generation = RegistryGeneration::minimum();
  std::optional<EpochToken> external_epoch;
  SequenceNumber last_sequence = SequenceNumber::minimum();
  std::uint64_t history_dropped = 0;
  RegistryLimits limits;
  std::vector<DataCenterRecord> records;
  std::vector<HistoryEntry> history;
  std::vector<IdempotencyEntry> idempotency;

  /// An empty registry at generation 0.
  [[nodiscard]] static RegistryState empty(RegistryLimits limits);

  [[nodiscard]] bool has_record(const DataCenterId& id) const noexcept;
  [[nodiscard]] const DataCenterRecord* find(const DataCenterId& id) const noexcept;
  [[nodiscard]] DataCenterRecord* find_mutable(const DataCenterId& id) noexcept;
  [[nodiscard]] const DataCenterRecord* find_by_alias(const Alias& alias) const noexcept;
  [[nodiscard]] const IdempotencyEntry* find_idempotency(const IdempotencyKey& key) const noexcept;

  /// Position at which a record with this identity is or would be, preserving
  /// canonical order.
  [[nodiscard]] std::size_t lower_bound_position(const DataCenterId& id) const noexcept;

  friend bool operator==(const RegistryState& left, const RegistryState& right);
  friend bool operator!=(const RegistryState& left, const RegistryState& right) {
    return !(left == right);
  }
};

/// Validates every state-level invariant, including the per-record ones.
[[nodiscard]] Result<void> validate_state(const RegistryState& state);

/// Computes statistics, including the canonical payload size and digest.
[[nodiscard]] Result<RegistryStats> compute_stats(const RegistryState& state);

}  // namespace dcr::internal

#endif  // DCR_CORE_STATE_HPP
