// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Stable, machine-readable error categories.
//
// Every rejection the library can produce carries exactly one ErrorCode. The
// token returned by to_string(ErrorCode) is part of the public contract: it is
// stable across releases, it is what the CLI prints, and it is what automated
// callers are expected to branch on. The human-readable message that
// accompanies a code is explanatory only and may change.

#ifndef DCR_ERROR_HPP
#define DCR_ERROR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace dcr {

enum class ErrorCode : std::uint16_t {
  // --- input validation -------------------------------------------------
  /// A caller-supplied identifier did not match its canonical syntax.
  invalid_identity = 1,
  /// A caller-supplied facility or administrative metadata value was invalid.
  invalid_metadata = 2,
  /// A caller-supplied argument was invalid and no more specific code applies.
  invalid_argument = 3,
  /// A query or enumeration request was invalid, or asked for too much.
  invalid_query = 4,
  /// A caller-supplied provenance record was invalid or claimed a source that
  /// only the registry itself may assert.
  invalid_provenance = 5,
  /// A configured or requested bound was exceeded before any work was done.
  limit_exceeded = 6,

  // --- lookup -----------------------------------------------------------
  /// The requested object does not exist in this registry.
  not_found = 7,

  // --- conflicts --------------------------------------------------------
  /// A record with this canonical identity already exists.
  duplicate_identity = 8,
  /// A secondary key (alias) is already claimed by a different record.
  alias_conflict = 9,
  /// The same site is already attached to this record.
  duplicate_membership = 10,
  /// The requested change would break a site-membership invariant.
  membership_violation = 11,

  // --- authority and lifecycle -----------------------------------------
  /// The mutation's expected registry generation is not the current one.
  stale_generation = 12,
  /// The mutation's expected record revision is not the current one.
  stale_revision = 13,
  /// The mutation asserted an external control-plane epoch that is older than
  /// the epoch this registry has recorded.
  stale_authority = 14,
  /// The requested lifecycle transition is not legal from the current state.
  illegal_transition = 15,
  /// The record is retired or replaced; obsolete authority cannot be revived.
  terminal_state = 16,
  /// A replacement was requested whose compatibility key cannot represent the
  /// record being replaced.
  incompatible_replacement = 17,
  /// An idempotency key was reused with different command content.
  idempotency_conflict = 18,

  // --- persistence ------------------------------------------------------
  /// No registry store exists at the requested root.
  store_not_found = 19,
  /// Stored state failed validation, integrity checking or internal
  /// consistency, and was therefore not accepted as authoritative.
  store_corrupt = 20,
  /// Stored state was produced by an incompatible snapshot format version.
  store_incompatible_version = 21,
  /// Another process holds the writer lock for this store.
  store_locked = 22,
  /// The underlying filesystem operation failed.
  store_io_error = 23,
  /// The registry was opened read-only and cannot accept mutations.
  store_read_only = 24,
  /// Stored state exceeds the configured limits and was not accepted.
  store_limit_exceeded = 25,

  // --- operation lifecycle ---------------------------------------------
  /// The caller cancelled the operation before it crossed its commit point.
  cancelled = 26,
  /// The registry has been closed.
  closed = 27,
  /// An invariant the library relies on was violated. This is a defect, not a
  /// caller error, and no state was changed.
  internal_error = 28,
};

/// The stable machine-readable token for a code. Never returns an empty view.
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

/// Parses a token produced by to_string(ErrorCode). Returns false when the
/// token is not a known code; the output parameter is left untouched.
[[nodiscard]] bool parse_error_code(std::string_view token, ErrorCode& out) noexcept;

/// True when retrying the same logical operation could succeed without the
/// caller changing anything other than re-reading current state.
[[nodiscard]] bool is_retryable(ErrorCode code) noexcept;

/// A rejection: a stable category plus an explanation.
class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message);
  Error(ErrorCode code, std::string message, std::string field);

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }

  /// Explanation for a human. Empty only for a default-constructed Error,
  /// which is not a valid rejection.
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

  /// Optional name of the offending input, for callers that want to point at
  /// the argument that was rejected. Empty when not applicable.
  [[nodiscard]] const std::string& field() const noexcept { return field_; }

  /// "code: message", or "code (field): message".
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Error& left, const Error& right) {
    return left.code_ == right.code_ && left.message_ == right.message_ &&
           left.field_ == right.field_;
  }
  friend bool operator!=(const Error& left, const Error& right) { return !(left == right); }

 private:
  ErrorCode code_ = ErrorCode::internal_error;
  std::string message_;
  std::string field_;
};

/// Convenience constructor.
[[nodiscard]] inline Error make_error(ErrorCode code, std::string message) {
  return Error(code, std::move(message));
}

}  // namespace dcr

#endif  // DCR_ERROR_HPP
