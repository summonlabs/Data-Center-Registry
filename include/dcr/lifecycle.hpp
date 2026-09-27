// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Administrative lifecycle state and its transition table.
//
// These states classify where a data center stands administratively. They are
// not incident states: `degraded` and `maintenance` here mean that an
// authorised caller has classified the facility that way, and the reason code
// that accompanies the transition is opaque to this library. Incident
// detection, response, maintenance orchestration and observability belong to
// other systems; the registry records the classification it is given and
// enforces which classifications may follow which.
//
// The table below is the whole state machine. It is data, not code paths, so
// it can be enumerated and tested exhaustively.

#ifndef DCR_LIFECYCLE_HPP
#define DCR_LIFECYCLE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/result.hpp"

namespace dcr {

enum class LifecycleState : std::uint8_t {
  /// The identity has been proposed but is not yet a registered facility.
  proposed = 1,
  /// The facility is registered and administratively ready for service.
  registered = 2,
  /// The facility is in service.
  active = 3,
  /// The facility is in service but the operator has classified it as
  /// degraded.
  degraded = 4,
  /// The facility is administratively withdrawn from service.
  maintenance = 5,
  /// The record is retired. Terminal: nothing may revive it.
  retired = 6,
  /// The record was retired by a replacement. Terminal: nothing may revive it.
  replaced = 7,
};

inline constexpr std::size_t kLifecycleStateCount = 7;

/// Every state, in declaration order. Used for exhaustive enumeration and for
/// statistics arrays.
inline constexpr std::array<LifecycleState, kLifecycleStateCount> kAllLifecycleStates = {
    LifecycleState::proposed, LifecycleState::registered, LifecycleState::active,
    LifecycleState::degraded, LifecycleState::maintenance, LifecycleState::retired,
    LifecycleState::replaced};

/// Stable machine-readable token: "proposed", "registered", ...
[[nodiscard]] std::string_view to_string(LifecycleState state) noexcept;

/// Parses the token produced by to_string(). Strict: unknown tokens are
/// rejected, never mapped to a default.
[[nodiscard]] Result<LifecycleState> parse_lifecycle_state(std::string_view token);

/// Index into kAllLifecycleStates, for statistics arrays.
[[nodiscard]] constexpr std::size_t lifecycle_state_index(LifecycleState state) noexcept {
  return static_cast<std::size_t>(state) - 1U;
}

/// The outcome of asking whether one state may follow another.
enum class TransitionVerdict : std::uint8_t {
  /// from == to. Nothing changes; the mutation is an idempotent no-op.
  unchanged = 1,
  /// The transition is legal and changes the state.
  legal = 2,
  /// The transition is not legal. The registry rejects it and explains why.
  illegal = 3,
};

/// The transition table, expressed as data.
[[nodiscard]] TransitionVerdict classify_transition(LifecycleState from,
                                                    LifecycleState to) noexcept;

/// True when the transition is legal and changes the state.
[[nodiscard]] bool is_legal_transition(LifecycleState from, LifecycleState to) noexcept;

/// True for `retired` and `replaced`. A terminal record accepts no further
/// metadata, lifecycle or membership mutation: retirement must not be a route
/// to reviving authority that was withdrawn.
[[nodiscard]] bool is_terminal(LifecycleState state) noexcept;

/// True for the states in which a facility is administratively present:
/// registered, active, degraded and maintenance. A live record must have
/// exactly one primary site membership, and its canonical identity cannot be
/// reused.
[[nodiscard]] bool is_live(LifecycleState state) noexcept;

/// The states that may legally follow `state`, in kAllLifecycleStates order.
/// Terminal states produce an empty list.
[[nodiscard]] std::vector<LifecycleState> legal_successors(LifecycleState state);

}  // namespace dcr

#endif  // DCR_LIFECYCLE_HPP
