// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Provenance: who asserted a change, under what authority, and when.
//
// Every committed mutation carries a provenance record. The caller supplies
// the source, the principal and a bounded free-text detail; the registry stamps
// the sequence number, the generation the change was committed at, the clock
// reading, and the external epoch token that was asserted.
//
// A caller may not claim a registry-internal source. `recovery` and `internal`
// are produced only by the library itself, so a client cannot disguise an
// ordinary API mutation as recovery from durable state.

#ifndef DCR_PROVENANCE_HPP
#define DCR_PROVENANCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dcr/generation.hpp"
#include "dcr/identity.hpp"
#include "dcr/result.hpp"
#include "dcr/time.hpp"

namespace dcr {

enum class ProvenanceSource : std::uint8_t {
  /// Submitted through the in-process library API.
  api = 1,
  /// Submitted through an operator tool.
  cli = 2,
  /// Created while importing externally supplied records.
  import = 3,
  /// Created while migrating state from another registry incarnation.
  migration = 4,
  /// Reconstructed by the registry during recovery from durable state. Never
  /// supplied by a caller.
  recovery = 5,
  /// Produced by the registry itself. Never supplied by a caller.
  internal = 6,
};

[[nodiscard]] std::string_view to_string(ProvenanceSource source) noexcept;
[[nodiscard]] Result<ProvenanceSource> parse_provenance_source(std::string_view token);

/// True for the sources a caller is allowed to assert.
[[nodiscard]] bool is_caller_suppliable(ProvenanceSource source) noexcept;

/// The caller-supplied part of a provenance record, before the registry stamps
/// it.
///
/// The principal is optional in the type and required by the registry: a
/// mutation that does not say who is making it is rejected with
/// `invalid_provenance`. Modelling it as an absent value rather than an invalid
/// placeholder identity keeps "no principal" representable without inventing a
/// fake one, and lets a command be assembled field by field. The detail field
/// is validated against the configured limit when the mutation is submitted,
/// not when this struct is built, because the limit belongs to the registry.
struct ProvenanceInput {
  ProvenanceSource source = ProvenanceSource::api;
  std::optional<PrincipalId> principal;
  std::string detail;

  friend bool operator==(const ProvenanceInput& left, const ProvenanceInput& right) {
    return left.source == right.source && left.principal == right.principal &&
           left.detail == right.detail;
  }
  friend bool operator!=(const ProvenanceInput& left, const ProvenanceInput& right) {
    return !(left == right);
  }
};

/// The stamped, authoritative provenance attached to a committed change.
class ProvenanceRecord {
 public:
  ProvenanceRecord() = delete;
  ProvenanceRecord(const ProvenanceRecord&) = default;
  ProvenanceRecord(ProvenanceRecord&&) noexcept = default;
  ProvenanceRecord& operator=(const ProvenanceRecord&) = default;
  ProvenanceRecord& operator=(ProvenanceRecord&&) noexcept = default;
  ~ProvenanceRecord() = default;

  [[nodiscard]] static Result<ProvenanceRecord> make(ProvenanceSource source,
                                                     PrincipalId principal,
                                                     SequenceNumber sequence,
                                                     RegistryGeneration generation,
                                                     Timestamp recorded_at,
                                                     std::optional<EpochToken> epoch,
                                                     std::optional<ReasonCode> reason,
                                                     std::string detail);

  [[nodiscard]] ProvenanceSource source() const noexcept { return source_; }
  [[nodiscard]] const PrincipalId& principal() const noexcept { return principal_; }
  [[nodiscard]] SequenceNumber sequence() const noexcept { return sequence_; }
  [[nodiscard]] RegistryGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] Timestamp recorded_at() const noexcept { return recorded_at_; }
  [[nodiscard]] const std::optional<EpochToken>& epoch() const noexcept { return epoch_; }
  [[nodiscard]] const std::optional<ReasonCode>& reason() const noexcept { return reason_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  friend bool operator==(const ProvenanceRecord& left, const ProvenanceRecord& right) {
    return left.source_ == right.source_ && left.principal_ == right.principal_ &&
           left.sequence_ == right.sequence_ && left.generation_ == right.generation_ &&
           left.recorded_at_ == right.recorded_at_ && left.epoch_ == right.epoch_ &&
           left.reason_ == right.reason_ && left.detail_ == right.detail_;
  }
  friend bool operator!=(const ProvenanceRecord& left, const ProvenanceRecord& right) {
    return !(left == right);
  }

 private:
  ProvenanceRecord(ProvenanceSource source, PrincipalId principal, SequenceNumber sequence,
                   RegistryGeneration generation, Timestamp recorded_at,
                   std::optional<EpochToken> epoch, std::optional<ReasonCode> reason,
                   std::string detail)
      : source_(source),
        principal_(std::move(principal)),
        sequence_(sequence),
        generation_(generation),
        recorded_at_(recorded_at),
        epoch_(std::move(epoch)),
        reason_(std::move(reason)),
        detail_(std::move(detail)) {}

  ProvenanceSource source_ = ProvenanceSource::api;
  PrincipalId principal_;
  SequenceNumber sequence_;
  RegistryGeneration generation_;
  Timestamp recorded_at_;
  std::optional<EpochToken> epoch_;
  std::optional<ReasonCode> reason_;
  std::string detail_;
};

}  // namespace dcr

#endif  // DCR_PROVENANCE_HPP
