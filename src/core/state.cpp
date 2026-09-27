// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "core/state.hpp"

#include <algorithm>
#include <set>

#include "core/codec.hpp"
#include "dcr/version.hpp"

namespace dcr::internal {
namespace {

[[nodiscard]] Result<void> fail(ErrorCode code, std::string message, const char* field) {
  return Error(code, std::move(message), field);
}

}  // namespace

RegistryState RegistryState::empty(RegistryLimits limits) {
  RegistryState state;
  state.format_major = kSnapshotFormatMajor;
  state.format_minor = kSnapshotFormatMinor;
  state.generation = RegistryGeneration::minimum();
  state.last_sequence = SequenceNumber::minimum();
  state.limits = limits;
  return state;
}

std::size_t RegistryState::lower_bound_position(const DataCenterId& id) const noexcept {
  const auto position =
      std::lower_bound(records.begin(), records.end(), id,
                       [](const DataCenterRecord& record, const DataCenterId& wanted) {
                         return record.id < wanted;
                       });
  return static_cast<std::size_t>(position - records.begin());
}

bool RegistryState::has_record(const DataCenterId& id) const noexcept {
  return find(id) != nullptr;
}

const DataCenterRecord* RegistryState::find(const DataCenterId& id) const noexcept {
  const std::size_t position = lower_bound_position(id);
  if (position >= records.size() || !(records[position].id == id)) {
    return nullptr;
  }
  return &records[position];
}

DataCenterRecord* RegistryState::find_mutable(const DataCenterId& id) noexcept {
  const std::size_t position = lower_bound_position(id);
  if (position >= records.size() || !(records[position].id == id)) {
    return nullptr;
  }
  return &records[position];
}

const DataCenterRecord* RegistryState::find_by_alias(const Alias& alias) const noexcept {
  for (const auto& record : records) {
    if (record.has_alias(alias)) {
      return &record;
    }
  }
  return nullptr;
}

const IdempotencyEntry* RegistryState::find_idempotency(const IdempotencyKey& key) const noexcept {
  const auto position =
      std::lower_bound(idempotency.begin(), idempotency.end(), key,
                       [](const IdempotencyEntry& entry, const IdempotencyKey& wanted) {
                         return entry.key < wanted;
                       });
  if (position == idempotency.end() || !(position->key == key)) {
    return nullptr;
  }
  return &*position;
}

bool operator==(const RegistryState& left, const RegistryState& right) {
  return left.format_major == right.format_major && left.format_minor == right.format_minor &&
         left.generation == right.generation && left.external_epoch == right.external_epoch &&
         left.last_sequence == right.last_sequence &&
         left.history_dropped == right.history_dropped && left.limits == right.limits &&
         left.records == right.records && left.history == right.history &&
         left.idempotency == right.idempotency;
}

Result<void> validate_state(const RegistryState& state) {
  if (auto result = validate_record_set(state.records, state.limits); !result.has_value()) {
    return result.error();
  }
  if (state.idempotency.size() >
      static_cast<std::size_t>(state.limits.max_idempotency_entries)) {
    return fail(ErrorCode::internal_error,
                "the idempotency table exceeds the configured bound", "idempotency");
  }
  if (state.history.size() > static_cast<std::size_t>(state.limits.max_history_entries)) {
    return fail(ErrorCode::internal_error, "the history window exceeds the configured bound",
                "history");
  }
  for (std::size_t index = 0; index < state.idempotency.size(); ++index) {
    if (index > 0 && !(state.idempotency[index - 1].key < state.idempotency[index].key)) {
      return fail(ErrorCode::internal_error,
                  "idempotency entries must be unique and in canonical key order",
                  "idempotency");
    }
    if (state.idempotency[index].recorded_sequence > state.last_sequence) {
      return fail(ErrorCode::internal_error,
                  "an idempotency entry cannot be recorded after the last sequence",
                  "idempotency");
    }
  }
  for (std::size_t index = 0; index < state.history.size(); ++index) {
    const auto& entry = state.history[index];
    if (entry.generation > state.generation) {
      return fail(ErrorCode::internal_error,
                  "a history entry cannot be newer than the registry generation", "history");
    }
    if (entry.sequence > state.last_sequence) {
      return fail(ErrorCode::internal_error,
                  "a history entry cannot carry a sequence beyond the last sequence", "history");
    }
    if (index > 0) {
      const auto& previous = state.history[index - 1];
      if (!(previous.sequence < entry.sequence)) {
        return fail(ErrorCode::internal_error,
                    "history entries must be unique and in ascending sequence order", "history");
      }
      if (previous.generation > entry.generation) {
        return fail(ErrorCode::internal_error,
                    "history generations must not move backwards", "history");
      }
      auto successor = previous.sequence.successor();
      if (successor.has_value() && !(successor.value() == entry.sequence)) {
        return fail(ErrorCode::internal_error,
                    "the retained history window must have no gaps", "history");
      }
    }
  }
  for (const auto& record : state.records) {
    if (record.created_generation > state.generation) {
      return fail(ErrorCode::internal_error,
                  "a record cannot be created after the registry generation", "records");
    }
    if (record.last_sequence > state.last_sequence) {
      return fail(ErrorCode::internal_error,
                  "a record cannot reference a sequence beyond the last sequence", "records");
    }
    if (record.revision.value() < 1) {
      return fail(ErrorCode::internal_error, "a record revision starts at 1", "records");
    }
  }
  if (state.format_major != kSnapshotFormatMajor) {
    return fail(ErrorCode::internal_error, "registry state carries an unexpected format version",
                "format_major");
  }
  return {};
}

Result<RegistryStats> compute_stats(const RegistryState& state) {
  RegistryStats stats;
  stats.generation = state.generation;
  stats.external_epoch = state.external_epoch;
  stats.record_count = static_cast<std::uint64_t>(state.records.size());
  std::set<SiteId> sites;
  std::set<OwnershipScopeId> scopes;
  for (const auto& record : state.records) {
    stats.records_by_state[lifecycle_state_index(record.state)] += 1;
    stats.alias_count += static_cast<std::uint64_t>(record.aliases.size());
    stats.membership_count += static_cast<std::uint64_t>(record.memberships.size());
    for (const auto& membership : record.memberships) {
      sites.insert(membership.site);
    }
    if (record.ownership.scope().has_value()) {
      scopes.insert(*record.ownership.scope());
    }
  }
  stats.distinct_site_count = static_cast<std::uint64_t>(sites.size());
  stats.distinct_ownership_scope_count = static_cast<std::uint64_t>(scopes.size());
  stats.history_entries = static_cast<std::uint64_t>(state.history.size());
  stats.history_dropped = state.history_dropped;
  stats.idempotency_entries = static_cast<std::uint64_t>(state.idempotency.size());

  DCR_TRY_ASSIGN(std::string payload, encode_state(state));
  stats.snapshot_payload_bytes = static_cast<std::uint64_t>(payload.size());
  stats.snapshot_digest = sha256_text(payload);
  return stats;
}

}  // namespace dcr::internal
