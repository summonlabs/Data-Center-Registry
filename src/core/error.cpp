// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/error.hpp"

namespace dcr {
namespace {

struct ErrorCodeToken {
  ErrorCode code;
  std::string_view token;
};

// The token table is the public contract. Order does not matter; the tokens do.
// A test asserts that every enumerator appears exactly once and parses back.
constexpr ErrorCodeToken kTokens[] = {
    {ErrorCode::invalid_identity, "invalid_identity"},
    {ErrorCode::invalid_metadata, "invalid_metadata"},
    {ErrorCode::invalid_argument, "invalid_argument"},
    {ErrorCode::invalid_query, "invalid_query"},
    {ErrorCode::invalid_provenance, "invalid_provenance"},
    {ErrorCode::limit_exceeded, "limit_exceeded"},
    {ErrorCode::not_found, "not_found"},
    {ErrorCode::duplicate_identity, "duplicate_identity"},
    {ErrorCode::alias_conflict, "alias_conflict"},
    {ErrorCode::duplicate_membership, "duplicate_membership"},
    {ErrorCode::membership_violation, "membership_violation"},
    {ErrorCode::stale_generation, "stale_generation"},
    {ErrorCode::stale_revision, "stale_revision"},
    {ErrorCode::stale_authority, "stale_authority"},
    {ErrorCode::illegal_transition, "illegal_transition"},
    {ErrorCode::terminal_state, "terminal_state"},
    {ErrorCode::incompatible_replacement, "incompatible_replacement"},
    {ErrorCode::idempotency_conflict, "idempotency_conflict"},
    {ErrorCode::store_not_found, "store_not_found"},
    {ErrorCode::store_corrupt, "store_corrupt"},
    {ErrorCode::store_incompatible_version, "store_incompatible_version"},
    {ErrorCode::store_locked, "store_locked"},
    {ErrorCode::store_io_error, "store_io_error"},
    {ErrorCode::store_read_only, "store_read_only"},
    {ErrorCode::store_limit_exceeded, "store_limit_exceeded"},
    {ErrorCode::cancelled, "cancelled"},
    {ErrorCode::closed, "closed"},
    {ErrorCode::internal_error, "internal_error"},
};

}  // namespace

std::string_view to_string(ErrorCode code) noexcept {
  for (const auto& entry : kTokens) {
    if (entry.code == code) {
      return entry.token;
    }
  }
  return "unknown_error_code";
}

bool parse_error_code(std::string_view token, ErrorCode& out) noexcept {
  for (const auto& entry : kTokens) {
    if (entry.token == token) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

bool is_retryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::stale_generation:
    case ErrorCode::stale_revision:
    case ErrorCode::store_locked:
    case ErrorCode::cancelled:
      return true;
    default:
      return false;
  }
}

Error::Error(ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Error::Error(ErrorCode code, std::string message, std::string field)
    : code_(code), message_(std::move(message)), field_(std::move(field)) {}

std::string Error::to_string() const {
  std::string out(::dcr::to_string(code_));
  if (!field_.empty()) {
    out += " (";
    out += field_;
    out += ")";
  }
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace dcr
