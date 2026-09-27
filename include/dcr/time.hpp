// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Time, and the injection of time.
//
// Provenance records when a change was recorded. That timestamp is supplied by
// a Clock rather than read from the platform inside the mutation path, so that
// a sequence of commands replayed with a manual clock produces byte-identical
// authoritative state. Determinism is a property of the whole library, not
// only of the codec.

#ifndef DCR_TIME_HPP
#define DCR_TIME_HPP

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "dcr/result.hpp"

namespace dcr {

/// Milliseconds since the Unix epoch, UTC. The accepted range is
/// 1970-01-01T00:00:00.000Z through 9999-12-31T23:59:59.999Z; anything outside
/// it is rejected rather than clamped.
class Timestamp {
 public:
  static constexpr std::int64_t kMinMillis = 0;
  static constexpr std::int64_t kMaxMillis = 253402300799999LL;

  Timestamp() = delete;
  Timestamp(const Timestamp&) = default;
  Timestamp(Timestamp&&) noexcept = default;
  Timestamp& operator=(const Timestamp&) = default;
  Timestamp& operator=(Timestamp&&) noexcept = default;
  ~Timestamp() = default;

  [[nodiscard]] static Result<Timestamp> from_unix_millis(std::int64_t millis);
  [[nodiscard]] static Timestamp unix_epoch() noexcept { return Timestamp(0); }

  [[nodiscard]] std::int64_t unix_millis() const noexcept { return millis_; }

  /// RFC 3339 UTC with millisecond precision: `1970-01-01T00:00:00.000Z`.
  [[nodiscard]] std::string to_string() const;

  /// Parses the canonical form produced by to_string().
  [[nodiscard]] static Result<Timestamp> parse(std::string_view text);

  friend bool operator==(const Timestamp& left, const Timestamp& right) noexcept {
    return left.millis_ == right.millis_;
  }
  friend bool operator!=(const Timestamp& left, const Timestamp& right) noexcept {
    return !(left == right);
  }
  friend std::strong_ordering operator<=>(const Timestamp& left,
                                          const Timestamp& right) noexcept {
    return left.millis_ <=> right.millis_;
  }

 private:
  explicit Timestamp(std::int64_t millis) noexcept : millis_(millis) {}
  std::int64_t millis_;
};

/// Source of the timestamp stamped into provenance.
class Clock {
 public:
  Clock() = default;
  virtual ~Clock();
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;

  /// The current instant. Must never move backwards for a given instance; the
  /// registry does not enforce that, because a clock stepping backwards is an
  /// environment fault that provenance should record rather than hide.
  [[nodiscard]] virtual Timestamp now() const = 0;
};

/// Reads the system clock.
class SystemClock final : public Clock {
 public:
  SystemClock() = default;
  [[nodiscard]] Timestamp now() const override;
};

/// A clock the caller advances explicitly. Used by tests and by replay tools
/// that must reproduce authoritative state exactly.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(Timestamp start) : current_(start) {}

  [[nodiscard]] Timestamp now() const override { return current_; }

  void set(Timestamp value) noexcept { current_ = value; }

  /// Advances by a non-negative number of milliseconds. Rejects a negative
  /// delta or an advance past the representable range.
  [[nodiscard]] Result<void> advance_millis(std::int64_t delta);

 private:
  Timestamp current_;
};

}  // namespace dcr

#endif  // DCR_TIME_HPP
