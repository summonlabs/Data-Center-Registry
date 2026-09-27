// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/compatibility.hpp"

#include "internal/text.hpp"

namespace dcr {

Result<CompatibilityKey> CompatibilityKey::make(std::uint16_t model_major,
                                                std::uint16_t model_minor,
                                                std::uint64_t capability_mask) {
  if (model_major == 0) {
    return Error(ErrorCode::invalid_metadata,
                 "a compatibility key needs a major version of at least 1", "CompatibilityKey");
  }
  return CompatibilityKey(model_major, model_minor, capability_mask);
}

std::string CompatibilityKey::to_string() const {
  return std::to_string(model_major_) + "." + std::to_string(model_minor_) + "+0x" +
         internal::format_hex_u64(capability_mask_, 16);
}

Result<CompatibilityKey> CompatibilityKey::parse(std::string_view text) {
  constexpr std::string_view kField = "CompatibilityKey";
  const std::size_t plus = text.find('+');
  if (plus == std::string_view::npos) {
    return Error(ErrorCode::invalid_argument,
                 "a compatibility key must have the form <major>.<minor>+0x<16 hex digits>",
                 std::string(kField));
  }
  const std::string_view version = text.substr(0, plus);
  const std::string_view mask_text = text.substr(plus + 1);
  const std::size_t dot = version.find('.');
  if (dot == std::string_view::npos) {
    return Error(ErrorCode::invalid_argument,
                 "a compatibility key must have the form <major>.<minor>+0x<16 hex digits>",
                 std::string(kField));
  }
  DCR_TRY_ASSIGN(const std::uint64_t major,
                 internal::parse_u64(version.substr(0, dot), "CompatibilityKey.major"));
  DCR_TRY_ASSIGN(const std::uint64_t minor,
                 internal::parse_u64(version.substr(dot + 1), "CompatibilityKey.minor"));
  if (major > 0xFFFFU || minor > 0xFFFFU) {
    return Error(ErrorCode::invalid_argument,
                 "compatibility version components must fit in 16 bits", std::string(kField));
  }
  if (mask_text.size() != 18 || mask_text[0] != '0' || mask_text[1] != 'x') {
    return Error(ErrorCode::invalid_argument,
                 "a compatibility key mask must be exactly 0x followed by 16 hex digits",
                 std::string(kField));
  }
  DCR_TRY_ASSIGN(const std::uint64_t mask,
                 internal::parse_hex_u64(mask_text.substr(2), 16, "CompatibilityKey.mask"));
  if (major == 0) {
    return Error(ErrorCode::invalid_argument,
                 "a compatibility key needs a major version of at least 1", std::string(kField));
  }
  return CompatibilityKey(static_cast<std::uint16_t>(major), static_cast<std::uint16_t>(minor),
                          mask);
}

std::string_view to_string(CompatibilityRelation relation) noexcept {
  switch (relation) {
    case CompatibilityRelation::identical:
      return "identical";
    case CompatibilityRelation::compatible_upgrade:
      return "compatible_upgrade";
    case CompatibilityRelation::incompatible:
      return "incompatible";
  }
  return "unknown";
}

CompatibilityRelation classify_compatibility(const CompatibilityKey& predecessor,
                                             const CompatibilityKey& candidate) noexcept {
  if (predecessor.model_major() != candidate.model_major()) {
    return CompatibilityRelation::incompatible;
  }
  if (candidate.model_minor() < predecessor.model_minor()) {
    return CompatibilityRelation::incompatible;
  }
  if ((candidate.capability_mask() & predecessor.capability_mask()) !=
      predecessor.capability_mask()) {
    return CompatibilityRelation::incompatible;
  }
  if (predecessor == candidate) {
    return CompatibilityRelation::identical;
  }
  return CompatibilityRelation::compatible_upgrade;
}

bool is_acceptable_replacement(const CompatibilityKey& predecessor,
                               const CompatibilityKey& candidate) noexcept {
  return classify_compatibility(predecessor, candidate) != CompatibilityRelation::incompatible;
}

}  // namespace dcr
