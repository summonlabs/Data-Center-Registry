// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Administrative and facility metadata, and deterministic extension metadata.
//
// These are descriptive facts about a data center: where it is, what it is
// called administratively, how it is classified, and which external objects it
// refers to. They are not capacity, placement, power or thermal models; those
// belong to other repositories in the control plane. Anything this library
// cannot describe with a typed field goes into extension metadata, which is
// carried verbatim, bounded, and never interpreted.

#ifndef DCR_METADATA_HPP
#define DCR_METADATA_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/identity.hpp"
#include "dcr/limits.hpp"
#include "dcr/result.hpp"

namespace dcr {

/// A caller-supplied facility classification. The registry records the
/// classification and never derives it.
enum class FacilityTier : std::uint8_t {
  tier_i = 1,
  tier_ii = 2,
  tier_iii = 3,
  tier_iv = 4,
};

[[nodiscard]] std::string_view to_string(FacilityTier tier) noexcept;
[[nodiscard]] Result<FacilityTier> parse_facility_tier(std::string_view token);

/// A postal address. Every part is optional except that a non-empty part must
/// be valid text; an address with no parts is not an address (use an empty
/// optional instead).
class PostalAddress {
 public:
  PostalAddress() = delete;
  PostalAddress(const PostalAddress&) = default;
  PostalAddress(PostalAddress&&) noexcept = default;
  PostalAddress& operator=(const PostalAddress&) = default;
  PostalAddress& operator=(PostalAddress&&) noexcept = default;
  ~PostalAddress() = default;

  /// Validates and builds. Rejects more than StructuralLimits::kMaxAddressLines
  /// lines, lines longer than kMaxAddressLineBytes, invalid UTF-8, control
  /// characters, and an address whose parts are all empty.
  [[nodiscard]] static Result<PostalAddress> make(std::vector<std::string> lines,
                                                  std::string_view locality,
                                                  std::string_view administrative_area,
                                                  std::string_view postal_code);

  [[nodiscard]] const std::vector<std::string>& lines() const noexcept { return lines_; }
  [[nodiscard]] const std::string& locality() const noexcept { return locality_; }
  [[nodiscard]] const std::string& administrative_area() const noexcept {
    return administrative_area_;
  }
  [[nodiscard]] const std::string& postal_code() const noexcept { return postal_code_; }
  [[nodiscard]] bool is_empty() const noexcept;

  friend bool operator==(const PostalAddress& left, const PostalAddress& right) {
    return left.lines_ == right.lines_ && left.locality_ == right.locality_ &&
           left.administrative_area_ == right.administrative_area_ &&
           left.postal_code_ == right.postal_code_;
  }
  friend bool operator!=(const PostalAddress& left, const PostalAddress& right) {
    return !(left == right);
  }

 private:
  PostalAddress(std::vector<std::string> lines, std::string locality,
                std::string administrative_area, std::string postal_code)
      : lines_(std::move(lines)),
        locality_(std::move(locality)),
        administrative_area_(std::move(administrative_area)),
        postal_code_(std::move(postal_code)) {}

  std::vector<std::string> lines_;
  std::string locality_;
  std::string administrative_area_;
  std::string postal_code_;
};

/// WGS-84 coordinates in ten-millionths of a degree. Integers, not floating
/// point: coordinates are part of authoritative state and must serialise
/// exactly.
class GeoCoordinates {
 public:
  GeoCoordinates() = delete;
  GeoCoordinates(const GeoCoordinates&) = default;
  GeoCoordinates(GeoCoordinates&&) noexcept = default;
  GeoCoordinates& operator=(const GeoCoordinates&) = default;
  GeoCoordinates& operator=(GeoCoordinates&&) noexcept = default;
  ~GeoCoordinates() = default;

  static constexpr std::int32_t kLatitudeMin = -900000000;
  static constexpr std::int32_t kLatitudeMax = 900000000;
  static constexpr std::int32_t kLongitudeMin = -1800000000;
  static constexpr std::int32_t kLongitudeMax = 1800000000;

  [[nodiscard]] static Result<GeoCoordinates> make(std::int32_t latitude_e7,
                                                   std::int32_t longitude_e7);

  [[nodiscard]] std::int32_t latitude_e7() const noexcept { return latitude_e7_; }
  [[nodiscard]] std::int32_t longitude_e7() const noexcept { return longitude_e7_; }

  friend bool operator==(const GeoCoordinates& left, const GeoCoordinates& right) noexcept {
    return left.latitude_e7_ == right.latitude_e7_ &&
           left.longitude_e7_ == right.longitude_e7_;
  }
  friend bool operator!=(const GeoCoordinates& left, const GeoCoordinates& right) noexcept {
    return !(left == right);
  }

 private:
  GeoCoordinates(std::int32_t latitude_e7, std::int32_t longitude_e7) noexcept
      : latitude_e7_(latitude_e7), longitude_e7_(longitude_e7) {}

  std::int32_t latitude_e7_;
  std::int32_t longitude_e7_;
};

/// One extension metadata entry.
struct MetadataEntry {
  MetadataKey key;
  std::string value;

  friend bool operator==(const MetadataEntry& left, const MetadataEntry& right) {
    return left.key == right.key && left.value == right.value;
  }
  friend bool operator!=(const MetadataEntry& left, const MetadataEntry& right) {
    return !(left == right);
  }
};

/// A deterministic key/value map.
///
/// Entries are a set: the map stores them in canonical key order, so two maps
/// with the same entries are equal and serialise identically regardless of the
/// order they were supplied in. Duplicate keys are rejected, never merged,
/// because silently keeping one of two conflicting values would be a silent
/// reinterpretation of authoritative metadata.
///
/// Keys beginning with `dcr.` are reserved to the registry and rejected from
/// caller input, so registry-reserved metadata cannot be spoofed by a client.
class MetadataMap {
 public:
  MetadataMap() = default;
  MetadataMap(const MetadataMap&) = default;
  MetadataMap(MetadataMap&&) noexcept = default;
  MetadataMap& operator=(const MetadataMap&) = default;
  MetadataMap& operator=(MetadataMap&&) noexcept = default;
  ~MetadataMap() = default;

  /// Validates, sorts into canonical order and builds.
  /// `field` names the input in any rejection ("extensions", "external_refs").
  [[nodiscard]] static Result<MetadataMap> make(std::vector<MetadataEntry> entries,
                                                std::string_view field);

  /// Re-checks an existing map against configured limits. Used when loading
  /// durable state and before accepting a mutation.
  [[nodiscard]] Result<void> validate(const RegistryLimits& limits,
                                      std::string_view field) const;

  [[nodiscard]] const std::vector<MetadataEntry>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  /// The value for a key, or nullptr when absent.
  [[nodiscard]] const std::string* find(const MetadataKey& key) const noexcept;

  friend bool operator==(const MetadataMap& left, const MetadataMap& right) {
    return left.entries_ == right.entries_;
  }
  friend bool operator!=(const MetadataMap& left, const MetadataMap& right) {
    return !(left == right);
  }

 private:
  std::vector<MetadataEntry> entries_;
};

/// The administrative and facility facts the registry holds about a data
/// center. Every field is optional: a record can be registered before its
/// address is known, and metadata can be filled in later under a precondition.
struct FacilityMetadata {
  std::optional<CountryCode> country;
  std::optional<RegionCode> region;
  std::optional<MetroCode> metro;
  std::optional<PostalAddress> address;
  std::optional<GeoCoordinates> coordinates;
  std::optional<FacilityTier> tier;
  /// The organisation that operates the facility. An opaque reference; the
  /// registry does not own operator records.
  std::optional<PrincipalId> facility_operator;
  /// References to external objects this facility depends on or is described
  /// by, as deterministic key/value pairs.
  MetadataMap external_refs;

  /// Validates the whole structure against configured limits.
  [[nodiscard]] Result<void> validate(const RegistryLimits& limits,
                                      std::string_view field) const;

  friend bool operator==(const FacilityMetadata& left, const FacilityMetadata& right) {
    return left.country == right.country && left.region == right.region &&
           left.metro == right.metro && left.address == right.address &&
           left.coordinates == right.coordinates && left.tier == right.tier &&
           left.facility_operator == right.facility_operator &&
           left.external_refs == right.external_refs;
  }
  friend bool operator!=(const FacilityMetadata& left, const FacilityMetadata& right) {
    return !(left == right);
  }
};

}  // namespace dcr

#endif  // DCR_METADATA_HPP
