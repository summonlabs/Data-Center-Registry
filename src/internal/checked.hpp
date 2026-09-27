// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Checked arithmetic helpers.
//
// Every size, count and offset that comes from outside the library is treated
// as untrusted. These helpers make the checks explicit at the point of use
// rather than relying on the reader to notice that a conversion could wrap.

#ifndef DCR_INTERNAL_CHECKED_HPP
#define DCR_INTERNAL_CHECKED_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "dcr/error.hpp"
#include "dcr/result.hpp"

namespace dcr::internal {

[[nodiscard]] inline bool checked_add_u64(std::uint64_t left, std::uint64_t right,
                                          std::uint64_t& out) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  out = left + right;
  return true;
}

[[nodiscard]] inline bool checked_mul_u64(std::uint64_t left, std::uint64_t right,
                                          std::uint64_t& out) noexcept {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  out = left * right;
  return true;
}

/// Narrows a 64-bit value to size_t, rejecting values that do not fit. On a
/// 64-bit platform this cannot fail; it is written out anyway because the
/// library must behave predictably where size_t is 32 bits.
[[nodiscard]] inline Result<std::size_t> narrow_to_size(std::uint64_t value,
                                                        std::string_view field) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return Error(ErrorCode::limit_exceeded,
                 "value " + std::to_string(value) + " does not fit in a size on this platform",
                 std::string(field));
  }
  return static_cast<std::size_t>(value);
}

/// Rejects an unsigned value that cannot be represented as a signed value of
/// the same width, so that a conversion to a signed type is always exact.
[[nodiscard]] inline Result<std::int64_t> narrow_to_i64(std::uint64_t value,
                                                        std::string_view field) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Error(ErrorCode::limit_exceeded,
                 "value " + std::to_string(value) + " exceeds the signed 64-bit range",
                 std::string(field));
  }
  return static_cast<std::int64_t>(value);
}

}  // namespace dcr::internal

#endif  // DCR_INTERNAL_CHECKED_HPP
