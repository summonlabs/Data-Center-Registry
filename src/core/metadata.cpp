// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/metadata.hpp"

#include <algorithm>

#include "internal/text.hpp"

namespace dcr {
namespace {

constexpr std::string_view kReservedMetadataPrefix = "dcr.";

}  // namespace

std::string_view to_string(FacilityTier tier) noexcept {
  switch (tier) {
    case FacilityTier::tier_i:
      return "tier_i";
    case FacilityTier::tier_ii:
      return "tier_ii";
    case FacilityTier::tier_iii:
      return "tier_iii";
    case FacilityTier::tier_iv:
      return "tier_iv";
  }
  return "unknown";
}

Result<FacilityTier> parse_facility_tier(std::string_view token) {
  if (token == "tier_i") {
    return FacilityTier::tier_i;
  }
  if (token == "tier_ii") {
    return FacilityTier::tier_ii;
  }
  if (token == "tier_iii") {
    return FacilityTier::tier_iii;
  }
  if (token == "tier_iv") {
    return FacilityTier::tier_iv;
  }
  return Error(ErrorCode::invalid_metadata,
               "'" + std::string(token) + "' is not a facility tier", "FacilityTier");
}

Result<PostalAddress> PostalAddress::make(std::vector<std::string> lines, std::string_view locality,
                                          std::string_view administrative_area,
                                          std::string_view postal_code) {
  if (lines.size() > StructuralLimits::kMaxAddressLines) {
    return Error(ErrorCode::limit_exceeded,
                 "an address may have at most " +
                     std::to_string(StructuralLimits::kMaxAddressLines) + " lines",
                 "address.lines");
  }
  std::vector<std::string> validated_lines;
  validated_lines.reserve(lines.size());
  for (const auto& line : lines) {
    DCR_TRY_ASSIGN(std::string value,
                   internal::validate_text(line, StructuralLimits::kMaxAddressLineBytes,
                                           "address.lines", false));
    validated_lines.push_back(std::move(value));
  }
  DCR_TRY_ASSIGN(std::string validated_locality,
                 internal::validate_text(locality, StructuralLimits::kMaxAddressLineBytes,
                                         "address.locality"));
  DCR_TRY_ASSIGN(std::string validated_area,
                 internal::validate_text(administrative_area,
                                         StructuralLimits::kMaxAddressLineBytes,
                                         "address.administrative_area"));
  DCR_TRY_ASSIGN(std::string validated_postal,
                 internal::validate_text(postal_code, StructuralLimits::kMaxAddressLineBytes,
                                         "address.postal_code"));

  PostalAddress address(std::move(validated_lines), std::move(validated_locality),
                        std::move(validated_area), std::move(validated_postal));
  if (address.is_empty()) {
    return Error(ErrorCode::invalid_metadata,
                 "an address must carry at least one component; use an absent address instead",
                 "address");
  }
  return address;
}

bool PostalAddress::is_empty() const noexcept {
  return lines_.empty() && locality_.empty() && administrative_area_.empty() &&
         postal_code_.empty();
}

Result<GeoCoordinates> GeoCoordinates::make(std::int32_t latitude_e7, std::int32_t longitude_e7) {
  if (latitude_e7 < kLatitudeMin || latitude_e7 > kLatitudeMax) {
    return Error(ErrorCode::invalid_metadata,
                 "latitude " + std::to_string(latitude_e7) +
                     " is outside [-900000000, 900000000] (ten-millionths of a degree)",
                 "coordinates.latitude");
  }
  if (longitude_e7 < kLongitudeMin || longitude_e7 > kLongitudeMax) {
    return Error(ErrorCode::invalid_metadata,
                 "longitude " + std::to_string(longitude_e7) +
                     " is outside [-1800000000, 1800000000] (ten-millionths of a degree)",
                 "coordinates.longitude");
  }
  return GeoCoordinates(latitude_e7, longitude_e7);
}

Result<MetadataMap> MetadataMap::make(std::vector<MetadataEntry> entries, std::string_view field) {
  if (entries.size() > StructuralLimits::kMaxMetadataEntries) {
    return Error(ErrorCode::limit_exceeded,
                 "at most " + std::to_string(StructuralLimits::kMaxMetadataEntries) +
                     " metadata entries are accepted",
                 std::string(field));
  }
  for (auto& entry : entries) {
    if (internal::starts_with(entry.key.value(), kReservedMetadataPrefix)) {
      return Error(ErrorCode::invalid_metadata,
                   "metadata key '" + entry.key.value() +
                       "' uses the reserved 'dcr.' namespace, which callers may not write",
                   std::string(field));
    }
    DCR_TRY_ASSIGN(std::string value,
                   internal::validate_text(entry.value, StructuralLimits::kMaxMetadataValueBytes,
                                           field));
    entry.value = std::move(value);
  }
  std::sort(entries.begin(), entries.end(),
            [](const MetadataEntry& left, const MetadataEntry& right) {
              return left.key < right.key;
            });
  for (std::size_t index = 1; index < entries.size(); ++index) {
    if (entries[index - 1].key == entries[index].key) {
      return Error(ErrorCode::invalid_metadata,
                   "metadata key '" + entries[index].key.value() +
                       "' appears more than once; conflicting values are never merged",
                   std::string(field));
    }
  }
  MetadataMap map;
  map.entries_ = std::move(entries);
  return map;
}

Result<void> MetadataMap::validate(const RegistryLimits& limits, std::string_view field) const {
  if (entries_.size() > limits.max_metadata_entries) {
    return Error(ErrorCode::limit_exceeded,
                 "at most " + std::to_string(limits.max_metadata_entries) +
                     " metadata entries are accepted",
                 std::string(field));
  }
  if (entries_.size() > StructuralLimits::kMaxMetadataEntries) {
    return Error(ErrorCode::limit_exceeded,
                 "at most " + std::to_string(StructuralLimits::kMaxMetadataEntries) +
                     " metadata entries are accepted",
                 std::string(field));
  }
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    const auto& entry = entries_[index];
    if (internal::starts_with(entry.key.value(), kReservedMetadataPrefix)) {
      return Error(ErrorCode::invalid_metadata,
                   "metadata key '" + entry.key.value() +
                       "' uses the reserved 'dcr.' namespace, which callers may not write",
                   std::string(field));
    }
    if (entry.value.size() > limits.max_metadata_value_bytes ||
        entry.value.size() > StructuralLimits::kMaxMetadataValueBytes) {
      return Error(ErrorCode::limit_exceeded,
                   "metadata value for '" + entry.key.value() + "' is " +
                       std::to_string(entry.value.size()) + " bytes, which exceeds the limit",
                   std::string(field));
    }
    auto text_result = internal::validate_text(entry.value,
                                               StructuralLimits::kMaxMetadataValueBytes,
                                               field);
    if (!text_result.has_value()) {
      return text_result.error();
    }
    if (index > 0 && !(entries_[index - 1].key < entry.key)) {
      return Error(ErrorCode::invalid_metadata,
                   "metadata entries must be unique and in canonical key order",
                   std::string(field));
    }
  }
  return {};
}

const std::string* MetadataMap::find(const MetadataKey& key) const noexcept {
  const auto position =
      std::lower_bound(entries_.begin(), entries_.end(), key,
                       [](const MetadataEntry& entry, const MetadataKey& wanted) {
                         return entry.key < wanted;
                       });
  if (position == entries_.end() || !(position->key == key)) {
    return nullptr;
  }
  return &position->value;
}

Result<void> FacilityMetadata::validate(const RegistryLimits& limits, std::string_view field) const {
  if (address.has_value()) {
    if (address->lines().size() > limits.max_address_lines) {
      return Error(ErrorCode::limit_exceeded,
                   "an address may have at most " + std::to_string(limits.max_address_lines) +
                       " lines",
                   std::string(field) + ".address");
    }
    for (const auto& line : address->lines()) {
      if (line.size() > limits.max_address_line_bytes) {
        return Error(ErrorCode::limit_exceeded,
                     "an address line exceeds the configured limit of " +
                         std::to_string(limits.max_address_line_bytes) + " bytes",
                     std::string(field) + ".address");
      }
      auto text_result = internal::validate_text(line, StructuralLimits::kMaxAddressLineBytes,
                                                 field);
      if (!text_result.has_value()) {
        return text_result.error();
      }
    }
    for (const std::string* part :
         {&address->locality(), &address->administrative_area(), &address->postal_code()}) {
      if (part->size() > limits.max_address_line_bytes) {
        return Error(ErrorCode::limit_exceeded,
                     "an address component exceeds the configured limit of " +
                         std::to_string(limits.max_address_line_bytes) + " bytes",
                     std::string(field) + ".address");
      }
      auto text_result = internal::validate_text(*part, StructuralLimits::kMaxAddressLineBytes,
                                                 field);
      if (!text_result.has_value()) {
        return text_result.error();
      }
    }
  }
  return external_refs.validate(limits, std::string(field) + ".external_refs");
}

}  // namespace dcr
