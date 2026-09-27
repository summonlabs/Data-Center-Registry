// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The definition of RegistrySnapshot's private implementation.
//
// This header exists so that the registry, which constructs snapshots, and the
// snapshot implementation itself can share one definition of what a snapshot
// holds, without exposing any of it through the installed public headers.

#ifndef DCR_CORE_SNAPSHOT_IMPL_HPP
#define DCR_CORE_SNAPSHOT_IMPL_HPP

#include <memory>
#include <utility>

#include "core/state.hpp"
#include "dcr/snapshot.hpp"

namespace dcr {

struct RegistrySnapshot::Impl {
  Impl(std::shared_ptr<const internal::RegistryState> state_in, Sha256Digest digest_in)
      : state(std::move(state_in)), digest(digest_in) {}

  std::shared_ptr<const internal::RegistryState> state;
  Sha256Digest digest;
};

}  // namespace dcr

#endif  // DCR_CORE_SNAPSHOT_IMPL_HPP
