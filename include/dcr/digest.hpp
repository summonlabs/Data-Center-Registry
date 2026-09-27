// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SHA-256 digests.
//
// The registry uses SHA-256 as its integrity primitive: a digest covers the
// canonical payload bytes of a published snapshot, and a short digest covers
// the semantic content of a mutation command for idempotency matching. The
// implementation is self-contained so that the library has no third-party
// dependency and no platform crypto provider is required.

#ifndef DCR_DIGEST_HPP
#define DCR_DIGEST_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dcr/result.hpp"

namespace dcr {

/// A 256-bit digest. Ordering is the lexicographic order of the raw bytes in
/// index order, which makes digest-keyed containers deterministic.
class Sha256Digest {
 public:
  using Bytes = std::array<std::uint8_t, 32>;
  static constexpr std::size_t kSize = 32;
  static constexpr std::size_t kHexSize = 64;

  Sha256Digest() = default;
  explicit Sha256Digest(Bytes bytes) noexcept : bytes_(bytes) {}

  /// Parses exactly 64 hexadecimal characters, upper or lower case.
  [[nodiscard]] static Result<Sha256Digest> from_hex(std::string_view text);

  [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }

  [[nodiscard]] std::string to_hex() const;

  /// True when every byte is zero, i.e. the digest has not been computed.
  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const Sha256Digest& left, const Sha256Digest& right) noexcept {
    return left.bytes_ == right.bytes_;
  }
  friend bool operator!=(const Sha256Digest& left, const Sha256Digest& right) noexcept {
    return !(left == right);
  }
  friend bool operator<(const Sha256Digest& left, const Sha256Digest& right) noexcept {
    return left.bytes_ < right.bytes_;
  }
  friend std::strong_ordering operator<=>(const Sha256Digest& left,
                                          const Sha256Digest& right) noexcept {
    if (left.bytes_ < right.bytes_) {
      return std::strong_ordering::less;
    }
    if (right.bytes_ < left.bytes_) {
      return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
  }

 private:
  Bytes bytes_{};
};

/// Streaming SHA-256.
class Sha256 {
 public:
  static constexpr std::size_t kBlockSize = 64;

  Sha256() noexcept;

  Sha256(const Sha256&) = default;
  Sha256& operator=(const Sha256&) = default;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  void update(std::uint8_t byte) noexcept;

  /// Completes the hash. The object may be reused only after reset().
  [[nodiscard]] Sha256Digest finish() noexcept;

  void reset() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, kBlockSize> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_size_ = 0;
};

/// One-shot digest of a byte range.
[[nodiscard]] Sha256Digest sha256_bytes(const void* data, std::size_t size) noexcept;
[[nodiscard]] inline Sha256Digest sha256_text(std::string_view text) noexcept {
  return sha256_bytes(text.data(), text.size());
}

}  // namespace dcr

#endif  // DCR_DIGEST_HPP
