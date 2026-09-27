// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/query.hpp"

namespace dcr {

std::string_view to_string(EnumerationOrder order) noexcept {
  switch (order) {
    case EnumerationOrder::canonical_id_ascending:
      return "canonical_id_ascending";
    case EnumerationOrder::display_name_ascending:
      return "display_name_ascending";
    case EnumerationOrder::revision_descending:
      return "revision_descending";
    case EnumerationOrder::created_generation_descending:
      return "created_generation_descending";
  }
  return "unknown";
}

Result<EnumerationOrder> parse_enumeration_order(std::string_view token) {
  if (token == "canonical_id_ascending") {
    return EnumerationOrder::canonical_id_ascending;
  }
  if (token == "display_name_ascending") {
    return EnumerationOrder::display_name_ascending;
  }
  if (token == "revision_descending") {
    return EnumerationOrder::revision_descending;
  }
  if (token == "created_generation_descending") {
    return EnumerationOrder::created_generation_descending;
  }
  return Error(ErrorCode::invalid_query,
               "'" + std::string(token) + "' is not an enumeration order", "EnumerationOrder");
}

Result<void> validate_query(const EnumerationQuery& query, const RegistryLimits& limits) {
  const std::size_t limit = query.limit.value_or(static_cast<std::size_t>(limits.max_query_results));
  if (limit > static_cast<std::size_t>(limits.max_query_results)) {
    return Error(ErrorCode::invalid_query,
                 "the requested limit of " + std::to_string(limit) +
                     " exceeds max_query_results (" +
                     std::to_string(limits.max_query_results) + ")",
                 "limit");
  }
  if (query.offset > static_cast<std::size_t>(limits.max_records)) {
    return Error(ErrorCode::invalid_query,
                 "the requested offset of " + std::to_string(query.offset) +
                     " exceeds max_records (" + std::to_string(limits.max_records) + ")",
                 "offset");
  }
  return {};
}

std::string_view to_string(HistoryAction action) noexcept {
  switch (action) {
    case HistoryAction::registered:
      return "registered";
    case HistoryAction::metadata_updated:
      return "metadata_updated";
    case HistoryAction::lifecycle_transition:
      return "lifecycle_transition";
    case HistoryAction::site_attached:
      return "site_attached";
    case HistoryAction::site_detached:
      return "site_detached";
    case HistoryAction::retired:
      return "retired";
    case HistoryAction::replacement_retired:
      return "replacement_retired";
    case HistoryAction::replacement_created:
      return "replacement_created";
  }
  return "unknown";
}

Result<HistoryAction> parse_history_action(std::string_view token) {
  for (const HistoryAction action :
       {HistoryAction::registered, HistoryAction::metadata_updated,
        HistoryAction::lifecycle_transition, HistoryAction::site_attached,
        HistoryAction::site_detached, HistoryAction::retired,
        HistoryAction::replacement_retired, HistoryAction::replacement_created}) {
    if (to_string(action) == token) {
      return action;
    }
  }
  return Error(ErrorCode::invalid_query,
               "'" + std::string(token) + "' is not a history action", "HistoryAction");
}

bool operator==(const HistoryEntry& left, const HistoryEntry& right) {
  return left.sequence == right.sequence && left.generation == right.generation &&
         left.id == right.id && left.action == right.action &&
         left.revision == right.revision && left.previous_state == right.previous_state &&
         left.new_state == right.new_state && left.provenance == right.provenance;
}

}  // namespace dcr
