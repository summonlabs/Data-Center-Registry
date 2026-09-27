// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Immutable registry snapshots.
//
// A snapshot is a consistent view of one committed registry generation. It is
// cheap to copy, it never changes, and it stays valid after the registry that
// produced it has moved on or been closed. Readers therefore never hold a lock
// while they work, and a long enumeration cannot be interleaved with a
// mutation.
//
// Snapshots are values, not handles into live state: a snapshot taken at
// generation 7 still reports generation 7 after the registry has committed
// generation 8.

#ifndef DCR_SNAPSHOT_HPP
#define DCR_SNAPSHOT_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dcr/digest.hpp"
#include "dcr/generation.hpp"
#include "dcr/identity.hpp"
#include "dcr/query.hpp"
#include "dcr/record.hpp"
#include "dcr/result.hpp"

namespace dcr {

class Registry;

class RegistrySnapshot {
 public:
  RegistrySnapshot() = delete;
  ~RegistrySnapshot();

  RegistrySnapshot(const RegistrySnapshot&) noexcept;
  RegistrySnapshot(RegistrySnapshot&&) noexcept;
  RegistrySnapshot& operator=(const RegistrySnapshot&) noexcept;
  RegistrySnapshot& operator=(RegistrySnapshot&&) noexcept;

  [[nodiscard]] RegistryGeneration generation() const;
  [[nodiscard]] std::optional<EpochToken> external_epoch() const;
  [[nodiscard]] std::uint64_t record_count() const;

  /// The record with this canonical identity, when present.
  [[nodiscard]] std::optional<DataCenterRecord> find(const DataCenterId& id) const;

  /// The record that claims this alias, when present.
  [[nodiscard]] std::optional<DataCenterRecord> find_by_alias(const Alias& alias) const;

  /// Every record, in canonical identity order.
  [[nodiscard]] std::vector<DataCenterRecord> records() const;

  /// Records matching a filter, in the requested order.
  [[nodiscard]] Result<std::vector<DataCenterRecord>> enumerate(
      const EnumerationQuery& query) const;

  /// Identities matching a filter, in the requested order.
  [[nodiscard]] Result<std::vector<DataCenterId>> enumerate_ids(
      const EnumerationQuery& query) const;

  /// Records whose display name matches exactly, in canonical identity order.
  /// Display names are not unique, so this returns all of them rather than
  /// failing ambiguously.
  [[nodiscard]] std::vector<DataCenterRecord> find_by_display_name(std::string_view name) const;

  [[nodiscard]] HistoryPage history(const HistoryQuery& query) const;

  /// Statistics for this state. Computes the canonical payload size and digest,
  /// so it is O(records).
  [[nodiscard]] RegistryStats stats() const;

  /// SHA-256 of the canonical payload of this state. Computed once and cached
  /// for the life of the snapshot.
  [[nodiscard]] Sha256Digest digest() const;

  /// Exact state equality: generation, epoch, every record, the retained
  /// history window and the idempotency table.
  friend bool operator==(const RegistrySnapshot& left, const RegistrySnapshot& right);
  friend bool operator!=(const RegistrySnapshot& left, const RegistrySnapshot& right) {
    return !(left == right);
  }

 private:
  friend class Registry;

  struct Impl;
  explicit RegistrySnapshot(std::shared_ptr<const Impl> impl) noexcept;

  std::shared_ptr<const Impl> impl_;
};

}  // namespace dcr

#endif  // DCR_SNAPSHOT_HPP
