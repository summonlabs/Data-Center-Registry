// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Generations, revisions, sequences and external epochs.
//
// Four counters with four different meanings. They are separate types because
// mixing them is the classic way to authorise the wrong write:
//
//   RegistryGeneration  registry-wide, +1 per committed mutation batch.
//                       This is the concurrency and fencing unit.
//   MetadataRevision    per record, +1 per committed change to that record.
//                       This is the lost-update detection unit.
//   SequenceNumber      registry-wide, +1 per recorded provenance entry.
//                       This is the history and attribution unit.
//   EpochToken          external, issued by the control-plane-epoch authority.
//                       The registry records it and never elects it.
//
// Each counter is monotonic and checked: incrementing the maximum value is an
// error, never a wrap.

#ifndef DCR_GENERATION_HPP
#define DCR_GENERATION_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "dcr/identity.hpp"
#include "dcr/result.hpp"

namespace dcr {

struct RegistryGenerationTag {
  static constexpr const char* kTypeName = "RegistryGeneration";
  static constexpr std::uint64_t kMinValue = 0;
};

struct MetadataRevisionTag {
  static constexpr const char* kTypeName = "MetadataRevision";
  static constexpr std::uint64_t kMinValue = 1;
};

struct SequenceNumberTag {
  static constexpr const char* kTypeName = "SequenceNumber";
  static constexpr std::uint64_t kMinValue = 0;
};

/// A monotonic counter with an explicit minimum value and checked increment.
/// The counter families are a closed set: only the tags below are
/// instantiated by the library.
///
/// A default-constructed counter holds the family minimum. That is a real
/// value, not a sentinel: generation 0 is the empty registry, sequence 0 is
/// "nothing recorded yet", and revision 1 is the first revision a record ever
/// has. Holding a minimum never authorises anything, and every comparison the
/// library makes is between two real values.
template <class Tag>
class Counter {
 public:
  using Value = std::uint64_t;
  static constexpr Value kMinValue = Tag::kMinValue;
  static constexpr Value kMaxValue = UINT64_MAX;

  Counter() noexcept : value_(kMinValue) {}
  Counter(const Counter&) = default;
  Counter(Counter&&) noexcept = default;
  Counter& operator=(const Counter&) = default;
  Counter& operator=(Counter&&) noexcept = default;
  ~Counter() = default;

  /// The lowest legal value for this family.
  [[nodiscard]] static Counter minimum() noexcept { return Counter(kMinValue); }

  /// Builds a counter from a raw value, rejecting values below the family
  /// minimum.
  [[nodiscard]] static Result<Counter> from_value(Value value);

  [[nodiscard]] Value value() const noexcept { return value_; }

  /// value + 1. Fails rather than wrapping when the counter is exhausted.
  [[nodiscard]] Result<Counter> successor() const;

  [[nodiscard]] static constexpr const char* type_name() noexcept { return Tag::kTypeName; }

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

  /// Parses the decimal form produced by to_string().
  [[nodiscard]] static Result<Counter> parse(std::string_view text);

  friend bool operator==(const Counter& left, const Counter& right) noexcept {
    return left.value_ == right.value_;
  }
  friend bool operator!=(const Counter& left, const Counter& right) noexcept {
    return !(left == right);
  }
  friend std::strong_ordering operator<=>(const Counter& left,
                                          const Counter& right) noexcept {
    return left.value_ <=> right.value_;
  }
  friend std::ostream& operator<<(std::ostream& out, const Counter& counter) {
    return out << counter.value_;
  }

 private:
  explicit Counter(Value value) noexcept : value_(value) {}
  Value value_;
};

/// Registry-wide commit counter. Generation 0 is the empty registry and is
/// never published; the first committed mutation produces generation 1.
using RegistryGeneration = Counter<RegistryGenerationTag>;

/// Per-record revision. A record is created at revision 1 and every committed
/// change to it increments the revision by exactly one.
using MetadataRevision = Counter<MetadataRevisionTag>;

/// Registry-wide provenance sequence.
using SequenceNumber = Counter<SequenceNumberTag>;

/// A control-plane epoch token issued by an external authority.
///
/// The registry does not elect epochs and does not fence controllers. It
/// records the token that accompanied an accepted mutation and can reject a
/// mutation whose caller asserts a token older than the recorded one. Tokens
/// are comparable only within the issuing authority: comparing tokens from
/// different issuers has no defined order and returns no ordering rather than
/// a guess.
class EpochToken {
 public:
  EpochToken() = delete;
  EpochToken(const EpochToken&) = default;
  EpochToken(EpochToken&&) noexcept = default;
  EpochToken& operator=(const EpochToken&) = default;
  EpochToken& operator=(EpochToken&&) noexcept = default;
  ~EpochToken() = default;

  [[nodiscard]] static Result<EpochToken> make(EpochIssuerId issuer, std::uint64_t value);

  [[nodiscard]] const EpochIssuerId& issuer() const noexcept { return issuer_; }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

  /// Ordering within one issuer. Empty when the issuers differ, which is the
  /// only honest answer for tokens from unrelated authorities.
  [[nodiscard]] std::optional<std::strong_ordering> compare(const EpochToken& other) const noexcept;

  /// Canonical form: `<issuer>@<decimal value>`, e.g. `cpe-primary@42`.
  [[nodiscard]] std::string to_string() const;

  /// Parses the canonical form. Rejects a missing or malformed issuer, a
  /// missing separator, and any value that is not a plain decimal integer.
  [[nodiscard]] static Result<EpochToken> parse(std::string_view text);

  friend bool operator==(const EpochToken& left, const EpochToken& right) noexcept {
    return left.issuer_ == right.issuer_ && left.value_ == right.value_;
  }
  friend bool operator!=(const EpochToken& left, const EpochToken& right) noexcept {
    return !(left == right);
  }

 private:
  EpochToken(EpochIssuerId issuer, std::uint64_t value) : issuer_(std::move(issuer)), value_(value) {}

  EpochIssuerId issuer_;
  std::uint64_t value_;
};

}  // namespace dcr

#endif  // DCR_GENERATION_HPP
