// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The compatibility contract carried by every record.
//
// Replacement is the one operation that can retire a live facility and stand a
// new identity in its place. The registry refuses a replacement whose
// compatibility key cannot represent everything the predecessor claimed, so
// that a replacement cannot silently drop capability.
//
// The key is deliberately small and explicit:
//
//   model_major       breaking revision of the facility model. A replacement
//                     must keep the same major: a different major describes a
//                     different kind of object, not a successor.
//   model_minor       additive revision. A replacement must not go backwards.
//   capability_mask   the set of modelled capabilities. A replacement must be
//                     a superset of its predecessor's.
//
// This is a compatibility contract, not a feature negotiation: the registry
// compares masks and never interprets individual bits.

#ifndef DCR_COMPATIBILITY_HPP
#define DCR_COMPATIBILITY_HPP

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "dcr/result.hpp"

namespace dcr {

class CompatibilityKey {
 public:
  CompatibilityKey() = delete;
  CompatibilityKey(const CompatibilityKey&) = default;
  CompatibilityKey(CompatibilityKey&&) noexcept = default;
  CompatibilityKey& operator=(const CompatibilityKey&) = default;
  CompatibilityKey& operator=(CompatibilityKey&&) noexcept = default;
  ~CompatibilityKey() = default;

  /// Builds a key. `model_major` must be at least 1: every record carries a
  /// real compatibility contract rather than an unversioned placeholder.
  [[nodiscard]] static Result<CompatibilityKey> make(std::uint16_t model_major,
                                                     std::uint16_t model_minor,
                                                     std::uint64_t capability_mask);

  [[nodiscard]] std::uint16_t model_major() const noexcept { return model_major_; }
  [[nodiscard]] std::uint16_t model_minor() const noexcept { return model_minor_; }
  [[nodiscard]] std::uint64_t capability_mask() const noexcept { return capability_mask_; }

  /// Canonical form: `<major>.<minor>+0x<16 hex digits>`, e.g.
  /// `1.2+0x0000000000000007`.
  [[nodiscard]] std::string to_string() const;

  /// Parses the canonical form. Rejects anything that is not exactly that
  /// shape, including short hex fields and trailing characters.
  [[nodiscard]] static Result<CompatibilityKey> parse(std::string_view text);

  friend bool operator==(const CompatibilityKey& left, const CompatibilityKey& right) noexcept {
    return left.model_major_ == right.model_major_ && left.model_minor_ == right.model_minor_ &&
           left.capability_mask_ == right.capability_mask_;
  }
  friend bool operator!=(const CompatibilityKey& left, const CompatibilityKey& right) noexcept {
    return !(left == right);
  }
  friend std::strong_ordering operator<=>(const CompatibilityKey& left,
                                          const CompatibilityKey& right) noexcept {
    if (left.model_major_ != right.model_major_) {
      return left.model_major_ <=> right.model_major_;
    }
    if (left.model_minor_ != right.model_minor_) {
      return left.model_minor_ <=> right.model_minor_;
    }
    return left.capability_mask_ <=> right.capability_mask_;
  }

 private:
  CompatibilityKey(std::uint16_t major, std::uint16_t minor, std::uint64_t mask) noexcept
      : model_major_(major), model_minor_(minor), capability_mask_(mask) {}

  std::uint16_t model_major_ = 1;
  std::uint16_t model_minor_ = 0;
  std::uint64_t capability_mask_ = 0;
};

/// How a candidate successor's key relates to its predecessor's.
enum class CompatibilityRelation : std::uint8_t {
  /// Same major, same minor, same capability mask.
  identical = 1,
  /// Same major, minor not older, capability mask a superset.
  compatible_upgrade = 2,
  /// Different major, older minor, or a capability that would be lost.
  incompatible = 3,
};

[[nodiscard]] std::string_view to_string(CompatibilityRelation relation) noexcept;

/// Classifies `candidate` relative to `predecessor`.
[[nodiscard]] CompatibilityRelation classify_compatibility(const CompatibilityKey& predecessor,
                                                           const CompatibilityKey& candidate) noexcept;

/// True when a replacement with `candidate` may stand in for `predecessor`.
[[nodiscard]] bool is_acceptable_replacement(const CompatibilityKey& predecessor,
                                             const CompatibilityKey& candidate) noexcept;

}  // namespace dcr

#endif  // DCR_COMPATIBILITY_HPP
