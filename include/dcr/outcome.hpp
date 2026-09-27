// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Mutation outcomes.
//
// A mutation either commits and reports what it did, or it is rejected with an
// Error and changes nothing. There is no third state and no partial commit:
// the outcome describes exactly what happened to authoritative state, and the
// generation and revision it reports are the values a caller must use as its
// next precondition.

#ifndef DCR_OUTCOME_HPP
#define DCR_OUTCOME_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "dcr/generation.hpp"
#include "dcr/identity.hpp"
#include "dcr/result.hpp"

namespace dcr {

enum class MutationStatus : std::uint8_t {
  /// A new record was created.
  created = 1,
  /// An existing record changed.
  updated = 2,
  /// The command was valid and the registry already matched it exactly.
  /// Nothing was written: the generation did not move, no provenance entry was
  /// recorded, and no revision was consumed. Repeating the same command
  /// produces the same answer, which is what makes an explicit no-op
  /// idempotent without storing anything.
  unchanged = 3,
  /// The command carried an idempotency key that matched a previously
  /// committed command, so the original outcome is reported again and nothing
  /// was written.
  replayed = 4,
};

[[nodiscard]] std::string_view to_string(MutationStatus status) noexcept;
[[nodiscard]] Result<MutationStatus> parse_mutation_status(std::string_view token);

/// What a committed mutation did.
struct MutationResult {
  MutationStatus status = MutationStatus::unchanged;
  /// The record the command acted on. For a replacement this is the successor.
  DataCenterId id;
  /// The registry generation after the mutation. For `unchanged` and
  /// `replayed` this is the generation the registry is at, not a new one.
  RegistryGeneration generation;
  /// The revision of the affected record after the mutation.
  MetadataRevision revision;
  /// For a replacement, the predecessor that was retired.
  std::optional<DataCenterId> related_id;
  /// The provenance sequence recorded for the committed change. Empty for
  /// `unchanged` and for a `replayed` result whose original commit recorded no
  /// sequence.
  std::optional<SequenceNumber> sequence;

  friend bool operator==(const MutationResult& left, const MutationResult& right) {
    return left.status == right.status && left.id == right.id &&
           left.generation == right.generation && left.revision == right.revision &&
           left.related_id == right.related_id && left.sequence == right.sequence;
  }
  friend bool operator!=(const MutationResult& left, const MutationResult& right) {
    return !(left == right);
  }
};

}  // namespace dcr

#endif  // DCR_OUTCOME_HPP
