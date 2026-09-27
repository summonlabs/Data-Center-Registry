// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/digest.hpp"

#include "internal/text.hpp"

namespace dcr {

Result<Sha256Digest> Sha256Digest::from_hex(std::string_view text) {
  if (text.size() != kHexSize) {
    return Error(ErrorCode::invalid_argument,
                 "a SHA-256 digest must be exactly 64 hexadecimal characters, got " +
                     std::to_string(text.size()),
                 "digest");
  }
  Bytes bytes{};
  for (std::size_t index = 0; index < kSize; ++index) {
    DCR_TRY_ASSIGN(const std::uint64_t pair,
                   internal::parse_hex_u64(text.substr(index * 2, 2), 2, "digest"));
    bytes[index] = static_cast<std::uint8_t>(pair);
  }
  return Sha256Digest(bytes);
}

std::string Sha256Digest::to_hex() const {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out(kHexSize, '0');
  for (std::size_t index = 0; index < kSize; ++index) {
    out[index * 2] = kHexDigits[(bytes_[index] >> 4U) & 0x0FU];
    out[index * 2 + 1] = kHexDigits[bytes_[index] & 0x0FU];
  }
  return out;
}

bool Sha256Digest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace dcr
