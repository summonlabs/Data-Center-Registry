// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Bounded resource limits.
//
// Every unbounded resource in this library is bounded by a declared limit:
// record count, per-record collections, string lengths, retained history,
// retained idempotency entries, and the size of a snapshot that will be read
// into memory. Limits are part of the authoritative state: they are persisted
// with the snapshot and re-checked on load, so a store written under one
// configuration is not silently reinterpreted under another.
//
// Each structural maximum below is an absolute cap enforced by the value types
// themselves. A RegistryLimits setting may make a bound tighter than the
// structural maximum; it can never make it looser.

#ifndef DCR_LIMITS_HPP
#define DCR_LIMITS_HPP

#include <cstdint>
#include <string>

#include "dcr/result.hpp"

namespace dcr {

/// Absolute caps enforced by the value types. Caller configuration cannot
/// raise these, because they are what keeps an untrusted payload from turning
/// into an unbounded allocation.
struct StructuralLimits {
  static constexpr std::uint64_t kMaxRecords = 1'000'000;
  static constexpr std::uint32_t kMaxAliasesPerRecord = 64;
  static constexpr std::uint32_t kMaxMembershipsPerRecord = 32;
  static constexpr std::uint32_t kMaxMetadataEntries = 128;
  static constexpr std::uint32_t kMaxMetadataKeyBytes = 96;
  static constexpr std::uint32_t kMaxMetadataValueBytes = 1024;
  static constexpr std::uint32_t kMaxDisplayNameBytes = 256;
  static constexpr std::uint32_t kMaxAddressLines = 8;
  static constexpr std::uint32_t kMaxAddressLineBytes = 256;
  static constexpr std::uint32_t kMaxProvenanceDetailBytes = 1024;
  static constexpr std::uint64_t kMaxSnapshotBytes = 1024ULL * 1024ULL * 1024ULL;
  static constexpr std::uint64_t kMinSnapshotBytes = 4096;
};

/// The bounds in force for one registry instance.
struct RegistryLimits {
  std::uint64_t max_records = 65536;
  std::uint64_t max_query_results = 65536;
  std::uint32_t max_aliases_per_record = 32;
  std::uint32_t max_memberships_per_record = 16;
  std::uint32_t max_metadata_entries = 64;
  std::uint32_t max_metadata_value_bytes = 512;
  std::uint32_t max_display_name_bytes = 256;
  std::uint32_t max_address_lines = 8;
  std::uint32_t max_address_line_bytes = 256;
  std::uint32_t max_provenance_detail_bytes = 512;
  std::uint32_t max_history_entries = 4096;
  std::uint32_t max_idempotency_entries = 4096;
  std::uint64_t max_snapshot_bytes = 64ULL * 1024ULL * 1024ULL;

  /// The library defaults: sized for a facility registry of tens of thousands
  /// of records with a bounded history window.
  [[nodiscard]] static RegistryLimits defaults() noexcept { return RegistryLimits{}; }

  /// Checks every field against its structural maximum and its minimum.
  [[nodiscard]] Result<void> validate() const;

  friend bool operator==(const RegistryLimits& left, const RegistryLimits& right) {
    return left.max_records == right.max_records &&
           left.max_query_results == right.max_query_results &&
           left.max_aliases_per_record == right.max_aliases_per_record &&
           left.max_memberships_per_record == right.max_memberships_per_record &&
           left.max_metadata_entries == right.max_metadata_entries &&
           left.max_metadata_value_bytes == right.max_metadata_value_bytes &&
           left.max_display_name_bytes == right.max_display_name_bytes &&
           left.max_address_lines == right.max_address_lines &&
           left.max_address_line_bytes == right.max_address_line_bytes &&
           left.max_provenance_detail_bytes == right.max_provenance_detail_bytes &&
           left.max_history_entries == right.max_history_entries &&
           left.max_idempotency_entries == right.max_idempotency_entries &&
           left.max_snapshot_bytes == right.max_snapshot_bytes;
  }
  friend bool operator!=(const RegistryLimits& left, const RegistryLimits& right) {
    return !(left == right);
  }
};

}  // namespace dcr

#endif  // DCR_LIMITS_HPP
