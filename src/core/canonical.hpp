// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The canonical byte encoding used by the snapshot codec and by command
// digests.
//
// Rules that make the encoding canonical, and therefore make two states with
// the same meaning produce the same bytes on every platform:
//
//   * fixed-width integers are little-endian, never native-endian;
//   * lengths and counts are unsigned LEB128 varints;
//   * text is a varint byte length followed by raw bytes, with no terminator
//     and no padding;
//   * booleans are one byte, 0 or 1, and any other value is rejected;
//   * nothing is written by pointer value, address, or platform type size.
//
// The reader never allocates on the strength of a length it has not first
// checked against the bytes that actually remain, so a truncated or hostile
// payload cannot cause a large allocation.

#ifndef DCR_INTERNAL_CANONICAL_HPP
#define DCR_INTERNAL_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "dcr/error.hpp"
#include "dcr/result.hpp"

namespace dcr::internal {

class CanonicalWriter {
 public:
  CanonicalWriter() = default;

  void u8(std::uint8_t value) { buffer_.push_back(static_cast<char>(value)); }

  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xFFU));
    u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }

  void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  void varint(std::uint64_t value) {
    while (value >= 0x80U) {
      u8(static_cast<std::uint8_t>((value & 0x7FU) | 0x80U));
      value >>= 7U;
    }
    u8(static_cast<std::uint8_t>(value));
  }

  void boolean(bool value) { u8(value ? 1U : 0U); }

  void text(std::string_view value) {
    varint(static_cast<std::uint64_t>(value.size()));
    if (!value.empty()) {
      bytes(value.data(), value.size());
    }
  }

  void bytes(const void* data, std::size_t size) {
    const auto* first = static_cast<const char*>(data);
    buffer_.append(first, size);
  }

  [[nodiscard]] const std::string& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::string take() { return std::move(buffer_); }

 private:
  std::string buffer_;
};

class CanonicalReader {
 public:
  explicit CanonicalReader(std::string_view data) noexcept : data_(data) {}

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }
  [[nodiscard]] bool at_end() const noexcept { return position_ == data_.size(); }

  [[nodiscard]] Result<std::uint8_t> u8() {
    if (remaining() < 1) {
      return truncated();
    }
    return static_cast<std::uint8_t>(data_[position_++]);
  }

  [[nodiscard]] Result<std::uint16_t> u16() {
    DCR_TRY_ASSIGN(const std::uint8_t low, u8());
    DCR_TRY_ASSIGN(const std::uint8_t high, u8());
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(low) |
                                      static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U));
  }

  [[nodiscard]] Result<std::uint32_t> u32() {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4U; ++index) {
      DCR_TRY_ASSIGN(const std::uint8_t byte, u8());
      value |= static_cast<std::uint32_t>(byte) << (index * 8U);
    }
    return value;
  }

  [[nodiscard]] Result<std::uint64_t> u64() {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8U; ++index) {
      DCR_TRY_ASSIGN(const std::uint8_t byte, u8());
      value |= static_cast<std::uint64_t>(byte) << (index * 8U);
    }
    return value;
  }

  [[nodiscard]] Result<std::int32_t> i32() {
    DCR_TRY_ASSIGN(const std::uint32_t value, u32());
    return static_cast<std::int32_t>(value);
  }

  [[nodiscard]] Result<std::int64_t> i64() {
    DCR_TRY_ASSIGN(const std::uint64_t value, u64());
    return static_cast<std::int64_t>(value);
  }

  [[nodiscard]] Result<std::uint64_t> varint() {
    std::uint64_t value = 0;
    unsigned shift = 0;
    for (unsigned index = 0; index < 10U; ++index) {
      DCR_TRY_ASSIGN(const std::uint8_t byte, u8());
      const std::uint64_t payload = static_cast<std::uint64_t>(byte & 0x7FU);
      if (shift == 63U && payload > 1U) {
        return Error(ErrorCode::store_corrupt, "varint overflows 64 bits in the payload",
                     "payload");
      }
      value |= payload << shift;
      if ((byte & 0x80U) == 0U) {
        return value;
      }
      shift += 7U;
    }
    return Error(ErrorCode::store_corrupt, "varint is longer than 10 bytes in the payload",
                 "payload");
  }

  [[nodiscard]] Result<bool> boolean() {
    DCR_TRY_ASSIGN(const std::uint8_t value, u8());
    if (value > 1U) {
      return Error(ErrorCode::store_corrupt,
                   "boolean field holds " + std::to_string(value) + ", which is neither 0 nor 1",
                   "payload");
    }
    return value == 1U;
  }

  /// A view of the next length-prefixed text field. No allocation happens, and
  /// the length is checked against the bytes that remain before it is used.
  [[nodiscard]] Result<std::string_view> text() {
    DCR_TRY_ASSIGN(const std::uint64_t length, varint());
    if (length > static_cast<std::uint64_t>(remaining())) {
      return Error(ErrorCode::store_corrupt,
                   "text field claims " + std::to_string(length) +
                       " bytes but only " + std::to_string(remaining()) + " remain",
                   "payload");
    }
    const auto size = static_cast<std::size_t>(length);
    const std::string_view view = data_.substr(position_, size);
    position_ += size;
    return view;
  }

  /// A length-prefixed run of raw bytes.
  [[nodiscard]] Result<std::string_view> raw() { return text(); }

  /// Checks a declared element count before anything is allocated for it. Every
  /// element of every encoded container occupies at least one byte, so a count
  /// larger than the remaining byte count is impossible.
  [[nodiscard]] Result<std::size_t> count(std::string_view what, std::size_t maximum) {
    DCR_TRY_ASSIGN(const std::uint64_t declared, varint());
    if (declared > static_cast<std::uint64_t>(maximum)) {
      return Error(ErrorCode::store_limit_exceeded,
                   std::string(what) + " declares " + std::to_string(declared) +
                       " entries, which exceeds the accepted maximum of " +
                       std::to_string(maximum),
                   "payload");
    }
    if (declared > static_cast<std::uint64_t>(remaining())) {
      return Error(ErrorCode::store_corrupt,
                   std::string(what) + " declares " + std::to_string(declared) +
                       " entries but only " + std::to_string(remaining()) + " bytes remain",
                   "payload");
    }
    return static_cast<std::size_t>(declared);
  }

  [[nodiscard]] Result<void> expect_end() const {
    if (!at_end()) {
      return Error(ErrorCode::store_corrupt,
                   "payload has " + std::to_string(remaining()) +
                       " unread bytes after the last field",
                   "payload");
    }
    return {};
  }

  [[nodiscard]] Result<void> expect_byte(std::uint8_t expected, std::string_view what) {
    DCR_TRY_ASSIGN(const std::uint8_t actual, u8());
    if (actual != expected) {
      return Error(ErrorCode::store_corrupt,
                   std::string(what) + " must be " + std::to_string(expected) + ", found " +
                       std::to_string(actual),
                   "payload");
    }
    return {};
  }

 private:
  [[nodiscard]] static Result<std::uint8_t> truncated() {
    return Error(ErrorCode::store_corrupt, "payload ends in the middle of a field", "payload");
  }

  std::string_view data_;
  std::size_t position_ = 0;
};

}  // namespace dcr::internal

#endif  // DCR_INTERNAL_CANONICAL_HPP
