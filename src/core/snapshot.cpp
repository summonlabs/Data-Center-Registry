// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/snapshot.hpp"

#include <algorithm>

#include "core/codec.hpp"
#include "core/snapshot_impl.hpp"
#include "core/state.hpp"

namespace dcr {
namespace {

using internal::RegistryState;

/// Selects the records a query matches, in the requested order, and applies
/// offset and limit. The result is a list of pointers into immutable state;
/// callers copy what they need.
[[nodiscard]] std::vector<const DataCenterRecord*> select(const RegistryState& state,
                                                          const EnumerationQuery& query) {
  std::vector<const DataCenterRecord*> selected;
  selected.reserve(state.records.size());
  for (const auto& record : state.records) {
    if (query.state.has_value() && !(record.state == *query.state)) {
      continue;
    }
    if (!query.include_terminal && is_terminal(record.state)) {
      continue;
    }
    if (query.site.has_value() && record.membership_for(*query.site) == nullptr) {
      continue;
    }
    if (query.ownership_kind.has_value() && !(record.ownership.kind() == *query.ownership_kind)) {
      continue;
    }
    if (query.ownership_scope.has_value()) {
      if (!record.ownership.scope().has_value() ||
          !(*record.ownership.scope() == *query.ownership_scope)) {
        continue;
      }
    }
    if (query.region.has_value()) {
      if (!record.facility.region.has_value() || !(*record.facility.region == *query.region)) {
        continue;
      }
    }
    if (query.country.has_value()) {
      if (!record.facility.country.has_value() || !(*record.facility.country == *query.country)) {
        continue;
      }
    }
    selected.push_back(&record);
  }

  const auto by_id = [](const DataCenterRecord* left, const DataCenterRecord* right) {
    return left->id < right->id;
  };
  switch (query.order) {
    case EnumerationOrder::canonical_id_ascending:
      std::sort(selected.begin(), selected.end(), by_id);
      break;
    case EnumerationOrder::display_name_ascending:
      std::sort(selected.begin(), selected.end(),
                [&by_id](const DataCenterRecord* left, const DataCenterRecord* right) {
                  if (left->display_name != right->display_name) {
                    return left->display_name < right->display_name;
                  }
                  return by_id(left, right);
                });
      break;
    case EnumerationOrder::revision_descending:
      std::sort(selected.begin(), selected.end(),
                [&by_id](const DataCenterRecord* left, const DataCenterRecord* right) {
                  if (!(left->revision == right->revision)) {
                    return right->revision < left->revision;
                  }
                  return by_id(left, right);
                });
      break;
    case EnumerationOrder::created_generation_descending:
      std::sort(selected.begin(), selected.end(),
                [&by_id](const DataCenterRecord* left, const DataCenterRecord* right) {
                  if (!(left->created_generation == right->created_generation)) {
                    return right->created_generation < left->created_generation;
                  }
                  return by_id(left, right);
                });
      break;
  }

  const std::size_t limit = query.limit.value_or(selected.size());
  if (query.offset >= selected.size() || limit == 0) {
    return {};
  }
  const std::size_t available = selected.size() - query.offset;
  const std::size_t take = std::min(limit, available);
  return std::vector<const DataCenterRecord*>(selected.begin() +
                                                  static_cast<std::ptrdiff_t>(query.offset),
                                              selected.begin() +
                                                  static_cast<std::ptrdiff_t>(query.offset + take));
}

}  // namespace

RegistrySnapshot::RegistrySnapshot(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

RegistrySnapshot::~RegistrySnapshot() = default;
RegistrySnapshot::RegistrySnapshot(const RegistrySnapshot&) noexcept = default;
RegistrySnapshot::RegistrySnapshot(RegistrySnapshot&&) noexcept = default;
RegistrySnapshot& RegistrySnapshot::operator=(const RegistrySnapshot&) noexcept = default;
RegistrySnapshot& RegistrySnapshot::operator=(RegistrySnapshot&&) noexcept = default;

RegistryGeneration RegistrySnapshot::generation() const { return impl_->state->generation; }

std::optional<EpochToken> RegistrySnapshot::external_epoch() const {
  return impl_->state->external_epoch;
}

std::uint64_t RegistrySnapshot::record_count() const {
  return static_cast<std::uint64_t>(impl_->state->records.size());
}

std::optional<DataCenterRecord> RegistrySnapshot::find(const DataCenterId& id) const {
  const DataCenterRecord* record = impl_->state->find(id);
  if (record == nullptr) {
    return std::nullopt;
  }
  return *record;
}

std::optional<DataCenterRecord> RegistrySnapshot::find_by_alias(const Alias& alias) const {
  const DataCenterRecord* record = impl_->state->find_by_alias(alias);
  if (record == nullptr) {
    return std::nullopt;
  }
  return *record;
}

std::vector<DataCenterRecord> RegistrySnapshot::records() const { return impl_->state->records; }

Result<std::vector<DataCenterRecord>> RegistrySnapshot::enumerate(
    const EnumerationQuery& query) const {
  if (auto result = validate_query(query, impl_->state->limits); !result.has_value()) {
    return result.error();
  }
  std::vector<DataCenterRecord> out;
  for (const DataCenterRecord* record : select(*impl_->state, query)) {
    out.push_back(*record);
  }
  return out;
}

Result<std::vector<DataCenterId>> RegistrySnapshot::enumerate_ids(
    const EnumerationQuery& query) const {
  if (auto result = validate_query(query, impl_->state->limits); !result.has_value()) {
    return result.error();
  }
  std::vector<DataCenterId> out;
  for (const DataCenterRecord* record : select(*impl_->state, query)) {
    out.push_back(record->id);
  }
  return out;
}

std::vector<DataCenterRecord> RegistrySnapshot::find_by_display_name(std::string_view name) const {
  std::vector<DataCenterRecord> out;
  for (const auto& record : impl_->state->records) {
    if (record.display_name == name) {
      out.push_back(record);
    }
  }
  return out;
}

HistoryPage RegistrySnapshot::history(const HistoryQuery& query) const {
  HistoryPage page;
  page.dropped_entries = impl_->state->history_dropped;
  if (!impl_->state->history.empty()) {
    page.earliest_retained_sequence = impl_->state->history.front().sequence;
  }
  std::vector<const HistoryEntry*> matched;
  for (const auto& entry : impl_->state->history) {
    if (query.id.has_value() && !(entry.id == *query.id)) {
      continue;
    }
    if (query.action.has_value() && !(entry.action == *query.action)) {
      continue;
    }
    if (query.from_generation.has_value() && entry.generation < *query.from_generation) {
      continue;
    }
    if (query.from_sequence.has_value() && entry.sequence < *query.from_sequence) {
      continue;
    }
    matched.push_back(&entry);
  }
  page.matched_total = static_cast<std::uint64_t>(matched.size());
  if (query.offset >= matched.size() || query.limit == 0) {
    return page;
  }
  const std::size_t available = matched.size() - query.offset;
  const std::size_t take = std::min(query.limit, available);
  page.truncated = take < available;
  for (std::size_t index = query.offset; index < query.offset + take; ++index) {
    page.entries.push_back(*matched[index]);
  }
  return page;
}

RegistryStats RegistrySnapshot::stats() const {
  auto stats = internal::compute_stats(*impl_->state);
  if (!stats.has_value()) {
    // A snapshot always holds a validated state, so this cannot happen. Rather
    // than publish numbers for a state that does not encode, the statistics
    // report the identity of the state without a digest.
    RegistryStats fallback;
    fallback.generation = impl_->state->generation;
    fallback.external_epoch = impl_->state->external_epoch;
    fallback.record_count = static_cast<std::uint64_t>(impl_->state->records.size());
    return fallback;
  }
  return stats.value();
}

Sha256Digest RegistrySnapshot::digest() const { return impl_->digest; }

bool operator==(const RegistrySnapshot& left, const RegistrySnapshot& right) {
  return *left.impl_->state == *right.impl_->state;
}

}  // namespace dcr
