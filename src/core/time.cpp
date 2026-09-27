// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/time.hpp"

#include <chrono>
#include <limits>

#include "internal/checked.hpp"
#include "internal/text.hpp"

namespace dcr {

Clock::~Clock() = default;

Timestamp SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
  if (millis < Timestamp::kMinMillis) {
    return Timestamp::unix_epoch();
  }
  if (millis > Timestamp::kMaxMillis) {
    return *Timestamp::from_unix_millis(Timestamp::kMaxMillis);
  }
  return *Timestamp::from_unix_millis(millis);
}

Result<Timestamp> Timestamp::from_unix_millis(std::int64_t millis) {
  if (millis < kMinMillis || millis > kMaxMillis) {
    return Error(ErrorCode::invalid_argument,
                 "timestamp " + std::to_string(millis) +
                     " is outside the accepted range of 1970-01-01T00:00:00.000Z through "
                     "9999-12-31T23:59:59.999Z",
                 "Timestamp");
  }
  return Timestamp(millis);
}

std::string Timestamp::to_string() const { return internal::format_rfc3339_millis(millis_); }

Result<Timestamp> Timestamp::parse(std::string_view text) {
  DCR_TRY_ASSIGN(const std::int64_t millis, internal::parse_rfc3339_millis(text));
  return from_unix_millis(millis);
}

Result<void> ManualClock::advance_millis(std::int64_t delta) {
  if (delta < 0) {
    return Error(ErrorCode::invalid_argument,
                 "a manual clock cannot be advanced by a negative amount", "ManualClock.delta");
  }
  std::int64_t next = 0;
  if (delta > 0 && current_.unix_millis() > std::numeric_limits<std::int64_t>::max() - delta) {
    return Error(ErrorCode::limit_exceeded, "advancing the manual clock overflows 64 bits",
                 "ManualClock.delta");
  }
  next = current_.unix_millis() + delta;
  DCR_TRY_ASSIGN(Timestamp updated, Timestamp::from_unix_millis(next));
  current_ = updated;
  return {};
}

}  // namespace dcr
