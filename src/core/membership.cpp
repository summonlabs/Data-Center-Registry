// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Membership roles, and the recovery dispositions the persistence layer
// reports. Both are small closed sets whose tokens are part of the contract,
// so each is rendered and parsed in one place.

#include <string>

#include "dcr/membership.hpp"
#include "dcr/persistence.hpp"

namespace dcr {

std::string_view to_string(MembershipRole role) noexcept {
  switch (role) {
    case MembershipRole::primary:
      return "primary";
    case MembershipRole::secondary:
      return "secondary";
    case MembershipRole::transit:
      return "transit";
    case MembershipRole::standby:
      return "standby";
  }
  return "unknown";
}

Result<MembershipRole> parse_membership_role(std::string_view token) {
  if (token == "primary") {
    return MembershipRole::primary;
  }
  if (token == "secondary") {
    return MembershipRole::secondary;
  }
  if (token == "transit") {
    return MembershipRole::transit;
  }
  if (token == "standby") {
    return MembershipRole::standby;
  }
  return Error(ErrorCode::invalid_argument,
               "'" + std::string(token) + "' is not a membership role", "MembershipRole");
}

std::string_view to_string(RecoveryDisposition disposition) noexcept {
  switch (disposition) {
    case RecoveryDisposition::created_empty:
      return "created_empty";
    case RecoveryDisposition::opened_empty:
      return "opened_empty";
    case RecoveryDisposition::opened_current:
      return "opened_current";
    case RecoveryDisposition::recovered_last_known_good:
      return "recovered_last_known_good";
    case RecoveryDisposition::opened_read_only:
      return "opened_read_only";
  }
  return "unknown";
}

}  // namespace dcr
