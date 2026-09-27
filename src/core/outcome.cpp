// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/outcome.hpp"

namespace dcr {

std::string_view to_string(MutationStatus status) noexcept {
  switch (status) {
    case MutationStatus::created:
      return "created";
    case MutationStatus::updated:
      return "updated";
    case MutationStatus::unchanged:
      return "unchanged";
    case MutationStatus::replayed:
      return "replayed";
  }
  return "unknown";
}

Result<MutationStatus> parse_mutation_status(std::string_view token) {
  if (token == "created") {
    return MutationStatus::created;
  }
  if (token == "updated") {
    return MutationStatus::updated;
  }
  if (token == "unchanged") {
    return MutationStatus::unchanged;
  }
  if (token == "replayed") {
    return MutationStatus::replayed;
  }
  return Error(ErrorCode::invalid_argument,
               "'" + std::string(token) + "' is not a mutation status", "MutationStatus");
}

}  // namespace dcr
