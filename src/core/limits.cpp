// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/limits.hpp"

#include <string>

namespace dcr {
namespace {

[[nodiscard]] Result<void> check_range(const char* name, std::uint64_t value,
                                       std::uint64_t minimum, std::uint64_t maximum) {
  if (value < minimum) {
    return Error(ErrorCode::limit_exceeded,
                 std::string(name) + " must be at least " + std::to_string(minimum), name);
  }
  if (value > maximum) {
    return Error(ErrorCode::limit_exceeded,
                 std::string(name) + " must be at most " + std::to_string(maximum), name);
  }
  return {};
}

}  // namespace

Result<void> RegistryLimits::validate() const {
  if (auto result = check_range("max_records", max_records, 1, StructuralLimits::kMaxRecords);
      !result.has_value()) {
    return result.error();
  }
  if (max_query_results < 1 || max_query_results > max_records) {
    return Error(ErrorCode::limit_exceeded,
                 "max_query_results must be between 1 and max_records (" +
                     std::to_string(max_records) + ")",
                 "max_query_results");
  }

  struct Bounded {
    const char* name;
    std::uint32_t value;
    std::uint32_t maximum;
  };
  const Bounded bounded[] = {
      {"max_aliases_per_record", max_aliases_per_record, StructuralLimits::kMaxAliasesPerRecord},
      {"max_memberships_per_record", max_memberships_per_record,
       StructuralLimits::kMaxMembershipsPerRecord},
      {"max_metadata_entries", max_metadata_entries, StructuralLimits::kMaxMetadataEntries},
      {"max_metadata_value_bytes", max_metadata_value_bytes,
       StructuralLimits::kMaxMetadataValueBytes},
      {"max_display_name_bytes", max_display_name_bytes, StructuralLimits::kMaxDisplayNameBytes},
      {"max_address_lines", max_address_lines, StructuralLimits::kMaxAddressLines},
      {"max_address_line_bytes", max_address_line_bytes, StructuralLimits::kMaxAddressLineBytes},
      {"max_provenance_detail_bytes", max_provenance_detail_bytes,
       StructuralLimits::kMaxProvenanceDetailBytes},
  };
  for (const auto& entry : bounded) {
    if (auto result = check_range(entry.name, entry.value, 1, entry.maximum);
        !result.has_value()) {
      return result.error();
    }
  }
  if (max_history_entries < 1) {
    return Error(ErrorCode::limit_exceeded, "max_history_entries must be at least 1",
                 "max_history_entries");
  }
  if (max_idempotency_entries < 1) {
    return Error(ErrorCode::limit_exceeded, "max_idempotency_entries must be at least 1",
                 "max_idempotency_entries");
  }
  if (auto result = check_range("max_snapshot_bytes", max_snapshot_bytes,
                                StructuralLimits::kMinSnapshotBytes,
                                StructuralLimits::kMaxSnapshotBytes);
      !result.has_value()) {
    return result.error();
  }
  return {};
}

}  // namespace dcr
