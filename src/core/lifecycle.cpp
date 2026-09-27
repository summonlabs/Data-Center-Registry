// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The lifecycle transition table.
//
// Legal transitions
// -----------------
//   proposed     -> registered, retired
//   registered   -> active, degraded, maintenance, retired
//   active       -> degraded, maintenance, retired
//   degraded     -> active, maintenance, retired
//   maintenance  -> active, degraded, retired
//   retired      -> (none)
//   replaced     -> (none)
//
// The reasoning behind the shape of the table:
//
//   * A proposal may be abandoned, which is a retirement.
//   * A facility cannot become `active` without first being `registered`: the
//     registry never observes a facility that skipped registration.
//   * `degraded` and `maintenance` are reachable from any live state, including
//     directly from `registered`, because an authorised caller may classify a
//     facility either way at any point.
//   * `replaced` is unreachable through this table. It is set only by the
//     replacement operation, which retires the predecessor and creates the
//     successor in one generation.
//   * Both terminal states have no successors, and `replaced` is not a state a
//     caller can request at all.
//
// A transition to the current state is classified `unchanged`, not illegal:
// repeating a transition is a no-op rather than an error, which is what makes
// it idempotent without any stored state.

#include "dcr/lifecycle.hpp"

#include <algorithm>

namespace dcr {
namespace {

struct TransitionRule {
  LifecycleState from;
  LifecycleState to;
};

// The complete legal set, as data. Pairs are unique and are checked by a test
// that enumerates every (from, to) combination.
constexpr TransitionRule kLegalTransitions[] = {
    {LifecycleState::proposed, LifecycleState::registered},
    {LifecycleState::proposed, LifecycleState::retired},
    {LifecycleState::registered, LifecycleState::active},
    {LifecycleState::registered, LifecycleState::degraded},
    {LifecycleState::registered, LifecycleState::maintenance},
    {LifecycleState::registered, LifecycleState::retired},
    {LifecycleState::active, LifecycleState::degraded},
    {LifecycleState::active, LifecycleState::maintenance},
    {LifecycleState::active, LifecycleState::retired},
    {LifecycleState::degraded, LifecycleState::active},
    {LifecycleState::degraded, LifecycleState::maintenance},
    {LifecycleState::degraded, LifecycleState::retired},
    {LifecycleState::maintenance, LifecycleState::active},
    {LifecycleState::maintenance, LifecycleState::degraded},
    {LifecycleState::maintenance, LifecycleState::retired},
};

}  // namespace

std::string_view to_string(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::proposed:
      return "proposed";
    case LifecycleState::registered:
      return "registered";
    case LifecycleState::active:
      return "active";
    case LifecycleState::degraded:
      return "degraded";
    case LifecycleState::maintenance:
      return "maintenance";
    case LifecycleState::retired:
      return "retired";
    case LifecycleState::replaced:
      return "replaced";
  }
  return "unknown";
}

Result<LifecycleState> parse_lifecycle_state(std::string_view token) {
  for (const LifecycleState state : kAllLifecycleStates) {
    if (to_string(state) == token) {
      return state;
    }
  }
  return Error(ErrorCode::invalid_argument,
               "'" + std::string(token) + "' is not a lifecycle state", "LifecycleState");
}

TransitionVerdict classify_transition(LifecycleState from, LifecycleState to) noexcept {
  if (from == to) {
    return TransitionVerdict::unchanged;
  }
  for (const auto& rule : kLegalTransitions) {
    if (rule.from == from && rule.to == to) {
      return TransitionVerdict::legal;
    }
  }
  return TransitionVerdict::illegal;
}

bool is_legal_transition(LifecycleState from, LifecycleState to) noexcept {
  return classify_transition(from, to) == TransitionVerdict::legal;
}

bool is_terminal(LifecycleState state) noexcept {
  return state == LifecycleState::retired || state == LifecycleState::replaced;
}

bool is_live(LifecycleState state) noexcept {
  return state == LifecycleState::registered || state == LifecycleState::active ||
         state == LifecycleState::degraded || state == LifecycleState::maintenance;
}

std::vector<LifecycleState> legal_successors(LifecycleState state) {
  std::vector<LifecycleState> successors;
  for (const auto& rule : kLegalTransitions) {
    if (rule.from == state) {
      successors.push_back(rule.to);
    }
  }
  std::sort(successors.begin(), successors.end());
  return successors;
}

}  // namespace dcr
