// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Text, token and numeric validation used across the library.
//
// All external input — imported records, snapshot payloads, CLI arguments,
// command fields — passes through these functions before it can influence
// authoritative state.

#ifndef DCR_INTERNAL_TEXT_HPP
#define DCR_INTERNAL_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/error.hpp"
#include "dcr/identity.hpp"
#include "dcr/result.hpp"

namespace dcr::internal {

/// Strict UTF-8 validation: rejects overlong encodings, surrogate code points,
/// values above U+10FFFF, and truncated sequences.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// True when the text contains a C0 control character, DEL, or a C1 control
/// character. Such text is rejected from authoritative fields: it cannot be
/// displayed safely and is a common carrier for log and terminal injection.
[[nodiscard]] bool has_disallowed_control(std::string_view text) noexcept;

/// Validates a free-text field and returns a copy of it.
///
/// Rejects: text longer than max_bytes (limit_exceeded); invalid UTF-8 or
/// disallowed control characters (invalid_argument); empty text when
/// allow_empty is false (invalid_argument).
[[nodiscard]] Result<std::string> validate_text(std::string_view text, std::size_t max_bytes,
                                                std::string_view field, bool allow_empty = true);

/// Validates a canonical token against its family policy.
[[nodiscard]] Result<std::string> validate_token(const TokenPolicy& policy, std::string_view text);

/// Parses a non-negative decimal integer. Rejects signs, whitespace, empty
/// input, leading zeros (except "0" itself) and overflow.
[[nodiscard]] Result<std::uint64_t> parse_u64(std::string_view text, std::string_view field);

/// Parses a signed decimal integer, used by the wire-independent text forms of
/// timestamps. Accepts an optional leading '-'.
[[nodiscard]] Result<std::int64_t> parse_i64(std::string_view text, std::string_view field);

/// Parses exactly `digits` hexadecimal characters into an unsigned value.
[[nodiscard]] Result<std::uint64_t> parse_hex_u64(std::string_view text, std::size_t digits,
                                                  std::string_view field);

/// Formats an unsigned value as exactly `digits` lower-case hexadecimal
/// characters, zero padded.
[[nodiscard]] std::string format_hex_u64(std::uint64_t value, std::size_t digits);

/// Formats an unsigned value as a decimal with exactly `digits` characters,
/// zero padded. Used for generation file names, where a fixed width keeps the
/// lexicographic order of file names equal to the numeric order of
/// generations.
[[nodiscard]] std::string format_decimal_padded(std::uint64_t value, std::size_t digits);

/// Parses exactly `digits` decimal characters. Rejects anything else.
[[nodiscard]] Result<std::uint64_t> parse_decimal_fixed(std::string_view text, std::size_t digits,
                                                        std::string_view field);

/// RFC 3339 UTC with millisecond precision.
[[nodiscard]] std::string format_rfc3339_millis(std::int64_t millis);
[[nodiscard]] Result<std::int64_t> parse_rfc3339_millis(std::string_view text);

/// True when `text` starts with `prefix`.
[[nodiscard]] inline bool starts_with(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// True when `text` ends with `suffix`.
[[nodiscard]] inline bool ends_with(std::string_view text, std::string_view suffix) noexcept {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Splits on a single character, rejecting empty fields when allow_empty is
/// false.
[[nodiscard]] Result<std::vector<std::string>> split(std::string_view text, char separator,
                                                     bool allow_empty, std::string_view field);

}  // namespace dcr::internal

#endif  // DCR_INTERNAL_TEXT_HPP
