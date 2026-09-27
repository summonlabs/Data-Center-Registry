// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/provenance.hpp"

namespace dcr {

std::string_view to_string(ProvenanceSource source) noexcept {
  switch (source) {
    case ProvenanceSource::api:
      return "api";
    case ProvenanceSource::cli:
      return "cli";
    case ProvenanceSource::import:
      return "import";
    case ProvenanceSource::migration:
      return "migration";
    case ProvenanceSource::recovery:
      return "recovery";
    case ProvenanceSource::internal:
      return "internal";
  }
  return "unknown";
}

Result<ProvenanceSource> parse_provenance_source(std::string_view token) {
  if (token == "api") {
    return ProvenanceSource::api;
  }
  if (token == "cli") {
    return ProvenanceSource::cli;
  }
  if (token == "import") {
    return ProvenanceSource::import;
  }
  if (token == "migration") {
    return ProvenanceSource::migration;
  }
  if (token == "recovery") {
    return ProvenanceSource::recovery;
  }
  if (token == "internal") {
    return ProvenanceSource::internal;
  }
  return Error(ErrorCode::invalid_provenance,
               "'" + std::string(token) + "' is not a provenance source", "provenance.source");
}

bool is_caller_suppliable(ProvenanceSource source) noexcept {
  switch (source) {
    case ProvenanceSource::api:
    case ProvenanceSource::cli:
    case ProvenanceSource::import:
    case ProvenanceSource::migration:
      return true;
    case ProvenanceSource::recovery:
    case ProvenanceSource::internal:
      return false;
  }
  return false;
}

Result<ProvenanceRecord> ProvenanceRecord::make(ProvenanceSource source, PrincipalId principal,
                                                SequenceNumber sequence,
                                                RegistryGeneration generation,
                                                Timestamp recorded_at,
                                                std::optional<EpochToken> epoch,
                                                std::optional<ReasonCode> reason,
                                                std::string detail) {
  return ProvenanceRecord(source, std::move(principal), sequence, generation, recorded_at,
                          std::move(epoch), std::move(reason), std::move(detail));
}

}  // namespace dcr
