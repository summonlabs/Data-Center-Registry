// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "internal/text.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <vector>

namespace dcr::internal {
namespace {

/// Decodes one UTF-8 sequence starting at `index`. On success returns the
/// length in bytes and sets `code_point`. On failure returns 0.
[[nodiscard]] std::size_t decode_utf8(std::string_view text, std::size_t index,
                                      std::uint32_t& code_point) noexcept {
  const auto byte = [&text](std::size_t position) {
    return static_cast<std::uint8_t>(text[position]);
  };
  const std::size_t remaining = text.size() - index;
  const std::uint8_t first = byte(index);

  if (first < 0x80U) {
    code_point = first;
    return 1;
  }
  if (first >= 0xC2U && first <= 0xDFU) {
    if (remaining < 2) {
      return 0;
    }
    const std::uint8_t second = byte(index + 1);
    if ((second & 0xC0U) != 0x80U) {
      return 0;
    }
    code_point = (static_cast<std::uint32_t>(first & 0x1FU) << 6U) |
                 static_cast<std::uint32_t>(second & 0x3FU);
    return 2;
  }
  if (first >= 0xE0U && first <= 0xEFU) {
    if (remaining < 3) {
      return 0;
    }
    const std::uint8_t second = byte(index + 1);
    const std::uint8_t third = byte(index + 2);
    if ((second & 0xC0U) != 0x80U || (third & 0xC0U) != 0x80U) {
      return 0;
    }
    // Reject overlong encodings and UTF-16 surrogates.
    if (first == 0xE0U && second < 0xA0U) {
      return 0;
    }
    if (first == 0xEDU && second > 0x9FU) {
      return 0;
    }
    code_point = (static_cast<std::uint32_t>(first & 0x0FU) << 12U) |
                 (static_cast<std::uint32_t>(second & 0x3FU) << 6U) |
                 static_cast<std::uint32_t>(third & 0x3FU);
    return 3;
  }
  if (first >= 0xF0U && first <= 0xF4U) {
    if (remaining < 4) {
      return 0;
    }
    const std::uint8_t second = byte(index + 1);
    const std::uint8_t third = byte(index + 2);
    const std::uint8_t fourth = byte(index + 3);
    if ((second & 0xC0U) != 0x80U || (third & 0xC0U) != 0x80U || (fourth & 0xC0U) != 0x80U) {
      return 0;
    }
    if (first == 0xF0U && second < 0x90U) {
      return 0;
    }
    if (first == 0xF4U && second > 0x8FU) {
      return 0;
    }
    code_point = (static_cast<std::uint32_t>(first & 0x07U) << 18U) |
                 (static_cast<std::uint32_t>(second & 0x3FU) << 12U) |
                 (static_cast<std::uint32_t>(third & 0x3FU) << 6U) |
                 static_cast<std::uint32_t>(fourth & 0x3FU);
    return 4;
  }
  return 0;
}

[[nodiscard]] bool is_slug_body_char(char value) noexcept {
  return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-';
}

[[nodiscard]] bool is_slug_edge_char(char value) noexcept {
  return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

[[nodiscard]] bool valid_slug(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  if (!is_slug_edge_char(text.front()) || !is_slug_edge_char(text.back())) {
    return false;
  }
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (!is_slug_body_char(text[index])) {
      return false;
    }
    if (text[index] == '-' && index + 1 < text.size() && text[index + 1] == '-') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool valid_snake(std::string_view text) noexcept {
  if (text.empty() || text.front() < 'a' || text.front() > 'z') {
    return false;
  }
  for (const char value : text) {
    const bool ok = (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
                    value == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool is_opaque_char(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '.' || value == '_' || value == ':' ||
         value == '-';
}

[[nodiscard]] bool valid_opaque(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  const char first = text.front();
  const bool first_ok = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
                        (first >= '0' && first <= '9');
  if (!first_ok) {
    return false;
  }
  for (const char value : text) {
    if (!is_opaque_char(value)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool valid_dotted(std::string_view text) noexcept {
  if (text.empty() || text.front() == '.' || text.back() == '.') {
    return false;
  }
  std::size_t segment_start = 0;
  while (segment_start <= text.size()) {
    const std::size_t dot = text.find('.', segment_start);
    const std::size_t segment_end = dot == std::string_view::npos ? text.size() : dot;
    const std::string_view segment = text.substr(segment_start, segment_end - segment_start);
    if (segment.empty() || segment.front() < 'a' || segment.front() > 'z') {
      return false;
    }
    for (const char value : segment) {
      const bool ok = (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
                      value == '_';
      if (!ok) {
        return false;
      }
    }
    if (dot == std::string_view::npos) {
      break;
    }
    segment_start = dot + 1;
  }
  return true;
}

[[nodiscard]] bool is_principal_char(char value) noexcept {
  return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' ||
         value == '_' || value == ':' || value == '@' || value == '/' || value == '+' ||
         value == '~' || value == '-';
}

[[nodiscard]] bool valid_principal(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  const auto edge_ok = [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
  };
  if (!edge_ok(text.front()) || !edge_ok(text.back())) {
    return false;
  }
  for (const char value : text) {
    if (!is_principal_char(value)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool valid_body(TokenSyntax syntax, std::string_view body) noexcept {
  switch (syntax) {
    case TokenSyntax::slug:
    case TokenSyntax::prefixed_slug:
      return valid_slug(body);
    case TokenSyntax::snake_case:
      return valid_snake(body);
    case TokenSyntax::opaque:
      return valid_opaque(body);
    case TokenSyntax::dotted:
      return valid_dotted(body);
    case TokenSyntax::principal:
      return valid_principal(body);
  }
  return false;
}

/// Days from civil date, from Howard Hinnant's public-domain algorithms.
/// Used instead of <chrono> calendar support so that the conversion is
/// identical on every platform and C++ library version.
[[nodiscard]] std::int64_t days_from_civil(std::int64_t year, unsigned month,
                                           unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const int shifted_month = static_cast<int>(month) + (month > 2 ? -3 : 9);
  const unsigned day_of_year =
      (153U * static_cast<unsigned>(shifted_month) + 2U) / 5U + day - 1U;
  const unsigned day_of_era =
      year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

struct CivilDate {
  std::int64_t year;
  unsigned month;
  unsigned day;
};

[[nodiscard]] CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
  std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year =
      day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
  const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
  const unsigned day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
  const unsigned month = static_cast<unsigned>(static_cast<int>(month_prime) +
                                               (month_prime < 10U ? 3 : -9));
  year += month <= 2U ? 1 : 0;
  return CivilDate{year, month, day};
}

[[nodiscard]] bool is_leap_year(std::int64_t year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] unsigned days_in_month(std::int64_t year, unsigned month) noexcept {
  static constexpr std::array<unsigned, 12> kMonthLengths{31, 28, 31, 30, 31, 30,
                                                          31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return kMonthLengths[month - 1];
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    std::uint32_t code_point = 0;
    const std::size_t length = decode_utf8(text, index, code_point);
    if (length == 0) {
      return false;
    }
    index += length;
  }
  return true;
}

bool has_disallowed_control(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    std::uint32_t code_point = 0;
    const std::size_t length = decode_utf8(text, index, code_point);
    if (length == 0) {
      // Invalid UTF-8 is reported by is_valid_utf8; treat every byte as a
      // potential control character so that callers cannot skip both checks.
      return true;
    }
    if (code_point < 0x20U || code_point == 0x7FU) {
      return true;
    }
    if (code_point >= 0x80U && code_point <= 0x9FU) {
      return true;
    }
    index += length;
  }
  return false;
}

Result<std::string> validate_text(std::string_view text, std::size_t max_bytes,
                                  std::string_view field, bool allow_empty) {
  if (text.size() > max_bytes) {
    return Error(ErrorCode::limit_exceeded,
                 "text of " + std::to_string(text.size()) + " bytes exceeds the limit of " +
                     std::to_string(max_bytes) + " bytes",
                 std::string(field));
  }
  if (text.empty()) {
    if (!allow_empty) {
      return Error(ErrorCode::invalid_argument, "text must not be empty", std::string(field));
    }
    return std::string();
  }
  if (!is_valid_utf8(text)) {
    return Error(ErrorCode::invalid_argument, "text is not valid UTF-8", std::string(field));
  }
  if (has_disallowed_control(text)) {
    return Error(ErrorCode::invalid_argument,
                 "text contains a control character, which is not accepted", std::string(field));
  }
  return std::string(text);
}

Result<std::string> validate_token(const TokenPolicy& policy, std::string_view text) {
  const std::string field_name(policy.type_name);
  if (text.size() < policy.min_length || text.size() > policy.max_length) {
    return Error(ErrorCode::invalid_identity,
                 std::string(policy.type_name) + " must be between " +
                     std::to_string(policy.min_length) + " and " +
                     std::to_string(policy.max_length) + " characters, got " +
                     std::to_string(text.size()),
                 field_name);
  }
  std::string_view body = text;
  const std::string_view prefix(policy.prefix);
  if (!prefix.empty()) {
    if (!starts_with(text, prefix)) {
      return Error(ErrorCode::invalid_identity,
                   std::string(policy.type_name) + " must start with '" + std::string(prefix) + "'",
                   field_name);
    }
    body = text.substr(prefix.size());
    if (body.empty()) {
      return Error(ErrorCode::invalid_identity,
                   std::string(policy.type_name) + " has no content after its prefix",
                   field_name);
    }
  }
  if (!valid_body(policy.syntax, body)) {
    return Error(ErrorCode::invalid_identity,
                 std::string(policy.type_name) + " '" + std::string(text) +
                     "' does not match its canonical syntax",
                 field_name);
  }
  return std::string(text);
}

Result<std::uint64_t> parse_u64(std::string_view text, std::string_view field) {
  if (text.empty()) {
    return Error(ErrorCode::invalid_argument, "expected a decimal integer, got an empty value",
                 std::string(field));
  }
  if (text.size() > 1 && text.front() == '0') {
    return Error(ErrorCode::invalid_argument,
                 "decimal integers must not carry leading zeros", std::string(field));
  }
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return Error(ErrorCode::invalid_argument,
                   "expected a decimal integer, found '" + std::string(1, digit) + "'",
                   std::string(field));
    }
    const std::uint64_t digit_value = static_cast<std::uint64_t>(digit - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit_value) / 10U) {
      return Error(ErrorCode::invalid_argument, "decimal integer overflows 64 bits",
                   std::string(field));
    }
    value = value * 10U + digit_value;
  }
  return value;
}

Result<std::int64_t> parse_i64(std::string_view text, std::string_view field) {
  bool negative = false;
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  if (text.size() > 1 && text.front() == '0') {
    return Error(ErrorCode::invalid_argument,
                 "decimal integers must not carry leading zeros", std::string(field));
  }
  DCR_TRY_ASSIGN(const std::uint64_t magnitude, parse_u64(text, field));
  if (negative) {
    const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
    if (magnitude > limit) {
      return Error(ErrorCode::invalid_argument, "decimal integer underflows 64 bits",
                   std::string(field));
    }
    if (magnitude == limit) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
  }
  if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Error(ErrorCode::invalid_argument, "decimal integer overflows 64 bits",
                 std::string(field));
  }
  return static_cast<std::int64_t>(magnitude);
}

Result<std::uint64_t> parse_hex_u64(std::string_view text, std::size_t digits,
                                    std::string_view field) {
  if (text.size() != digits) {
    return Error(ErrorCode::invalid_argument,
                 "expected exactly " + std::to_string(digits) + " hexadecimal characters, got " +
                     std::to_string(text.size()),
                 std::string(field));
  }
  if (digits > 16) {
    return Error(ErrorCode::invalid_argument, "hexadecimal field wider than 64 bits",
                 std::string(field));
  }
  std::uint64_t value = 0;
  for (const char digit : text) {
    std::uint64_t nibble = 0;
    if (digit >= '0' && digit <= '9') {
      nibble = static_cast<std::uint64_t>(digit - '0');
    } else if (digit >= 'a' && digit <= 'f') {
      nibble = static_cast<std::uint64_t>(digit - 'a') + 10U;
    } else if (digit >= 'A' && digit <= 'F') {
      nibble = static_cast<std::uint64_t>(digit - 'A') + 10U;
    } else {
      return Error(ErrorCode::invalid_argument,
                   "expected a hexadecimal character, found '" + std::string(1, digit) + "'",
                   std::string(field));
    }
    value = (value << 4U) | nibble;
  }
  return value;
}

std::string format_hex_u64(std::uint64_t value, std::size_t digits) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out(digits, '0');
  for (std::size_t index = 0; index < digits; ++index) {
    const std::size_t shift = (digits - 1 - index) * 4U;
    out[index] = kHexDigits[(value >> shift) & 0xFU];
  }
  return out;
}

std::string format_decimal_padded(std::uint64_t value, std::size_t digits) {
  std::string out(digits, '0');
  std::size_t index = digits;
  while (index > 0) {
    --index;
    out[index] = static_cast<char>('0' + static_cast<char>(value % 10U));
    value /= 10U;
    if (value == 0) {
      break;
    }
  }
  return out;
}

Result<std::uint64_t> parse_decimal_fixed(std::string_view text, std::size_t digits,
                                          std::string_view field) {
  if (text.size() != digits) {
    return Error(ErrorCode::invalid_argument,
                 "expected exactly " + std::to_string(digits) + " decimal characters, got " +
                     std::to_string(text.size()),
                 std::string(field));
  }
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return Error(ErrorCode::invalid_argument,
                   "expected a decimal character, found '" + std::string(1, digit) + "'",
                   std::string(field));
    }
    const std::uint64_t digit_value = static_cast<std::uint64_t>(digit - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit_value) / 10U) {
      return Error(ErrorCode::invalid_argument, "decimal value overflows 64 bits",
                   std::string(field));
    }
    value = value * 10U + digit_value;
  }
  return value;
}

std::string format_rfc3339_millis(std::int64_t millis) {
  const std::int64_t day_millis = 86400000LL;
  std::int64_t days = millis / day_millis;
  std::int64_t remainder = millis % day_millis;
  if (remainder < 0) {
    remainder += day_millis;
    --days;
  }
  const CivilDate date = civil_from_days(days);
  const std::int64_t hour = remainder / 3600000LL;
  remainder %= 3600000LL;
  const std::int64_t minute = remainder / 60000LL;
  remainder %= 60000LL;
  const std::int64_t second = remainder / 1000LL;
  const std::int64_t milli = remainder % 1000LL;

  std::string out;
  out.reserve(24);
  out += format_decimal_padded(static_cast<std::uint64_t>(date.year), 4);
  out += '-';
  out += format_decimal_padded(date.month, 2);
  out += '-';
  out += format_decimal_padded(date.day, 2);
  out += 'T';
  out += format_decimal_padded(static_cast<std::uint64_t>(hour), 2);
  out += ':';
  out += format_decimal_padded(static_cast<std::uint64_t>(minute), 2);
  out += ':';
  out += format_decimal_padded(static_cast<std::uint64_t>(second), 2);
  out += '.';
  out += format_decimal_padded(static_cast<std::uint64_t>(milli), 3);
  out += 'Z';
  return out;
}

Result<std::int64_t> parse_rfc3339_millis(std::string_view text) {
  constexpr std::string_view kField = "timestamp";
  if (text.size() != 24 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':' || text[19] != '.' || text[23] != 'Z') {
    return Error(ErrorCode::invalid_argument,
                 "timestamp must have the form 1970-01-01T00:00:00.000Z", std::string(kField));
  }
  DCR_TRY_ASSIGN(const std::uint64_t year,
                 parse_decimal_fixed(text.substr(0, 4), 4, "timestamp.year"));
  DCR_TRY_ASSIGN(const std::uint64_t month,
                 parse_decimal_fixed(text.substr(5, 2), 2, "timestamp.month"));
  DCR_TRY_ASSIGN(const std::uint64_t day,
                 parse_decimal_fixed(text.substr(8, 2), 2, "timestamp.day"));
  DCR_TRY_ASSIGN(const std::uint64_t hour,
                 parse_decimal_fixed(text.substr(11, 2), 2, "timestamp.hour"));
  DCR_TRY_ASSIGN(const std::uint64_t minute,
                 parse_decimal_fixed(text.substr(14, 2), 2, "timestamp.minute"));
  DCR_TRY_ASSIGN(const std::uint64_t second,
                 parse_decimal_fixed(text.substr(17, 2), 2, "timestamp.second"));
  DCR_TRY_ASSIGN(const std::uint64_t milli,
                 parse_decimal_fixed(text.substr(20, 3), 3, "timestamp.millisecond"));

  if (month < 1 || month > 12) {
    return Error(ErrorCode::invalid_argument, "timestamp month is out of range",
                 std::string(kField));
  }
  const auto year_value = static_cast<std::int64_t>(year);
  const auto month_value = static_cast<unsigned>(month);
  if (day < 1 || day > days_in_month(year_value, month_value)) {
    return Error(ErrorCode::invalid_argument, "timestamp day is out of range",
                 std::string(kField));
  }
  if (hour > 23 || minute > 59 || second > 59) {
    return Error(ErrorCode::invalid_argument, "timestamp time of day is out of range",
                 std::string(kField));
  }
  const std::int64_t days = days_from_civil(year_value, month_value, static_cast<unsigned>(day));
  const std::int64_t total =
      days * 86400000LL + static_cast<std::int64_t>(hour) * 3600000LL +
      static_cast<std::int64_t>(minute) * 60000LL + static_cast<std::int64_t>(second) * 1000LL +
      static_cast<std::int64_t>(milli);
  return total;
}

Result<std::vector<std::string>> split(std::string_view text, char separator, bool allow_empty,
                                       std::string_view field) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t position = text.find(separator, start);
    const std::size_t end = position == std::string_view::npos ? text.size() : position;
    std::string_view part = text.substr(start, end - start);
    if (part.empty() && !allow_empty) {
      return Error(ErrorCode::invalid_argument, "value contains an empty field",
                   std::string(field));
    }
    parts.emplace_back(part);
    if (position == std::string_view::npos) {
      break;
    }
    start = position + 1;
  }
  return parts;
}

}  // namespace dcr::internal
