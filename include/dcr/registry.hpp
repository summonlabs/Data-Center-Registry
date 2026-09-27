// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The registry.
//
// One Registry object owns one durable store root. It is the only supported
// entry point for authoritative state: consumers submit commands, receive
// outcomes, and read immutable snapshots. They never touch persistence
// structures.
//
// Concurrency model
// -----------------
// The library creates no threads of its own. Concurrency is caller-driven and
// bounded as follows:
//
//   * Many threads may read concurrently. Reads take a short shared lock only
//     long enough to copy a shared pointer to immutable state; all subsequent
//     work happens outside every lock.
//   * Mutations are serialised by a single writer lock. There is exactly one
//     in-flight mutation per Registry instance.
//   * There are no callbacks, no observers and no subscriptions, so no
//     user code ever runs while an internal lock is held, and re-entering the
//     registry from a callback is impossible by construction.
//   * Cross-process, exactly one writable Registry may exist per store root.
//     The writer lock is held from open until close, and the store additionally
//     refuses to publish a generation that is not the direct successor of the
//     generation on disk, so a writer whose view is stale cannot overwrite a
//     newer commit even if the lock were bypassed.
//   * close() waits for an in-flight mutation to reach its commit point. A
//     mutation that has already published is committed; one that has not
//     started is rejected with `closed`.

#ifndef DCR_REGISTRY_HPP
#define DCR_REGISTRY_HPP

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/commands.hpp"
#include "dcr/limits.hpp"
#include "dcr/outcome.hpp"
#include "dcr/persistence.hpp"
#include "dcr/query.hpp"
#include "dcr/snapshot.hpp"
#include "dcr/time.hpp"

namespace dcr {

/// Everything needed to open a registry.
struct OpenOptions {
  StoreOptions store;
  /// Clock used to stamp provenance. Null selects the system clock. Supplying
  /// a clock is how a deterministic replay is made byte-identical.
  const Clock* clock = nullptr;
};

/// A registry and the report describing how it was opened. The registry owns
/// the store lock until it is closed or destroyed.
struct OpenedRegistry {
  std::unique_ptr<Registry> registry;
  OpenReport report;
};

class Registry {
 public:
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;
  Registry(Registry&&) = delete;
  Registry& operator=(Registry&&) = delete;
  ~Registry();

  /// Opens a store. Fails when the store is missing, locked by another writer,
  /// holds state this build cannot accept, or holds state that does not
  /// verify. A failed open never adopts partially valid state.
  [[nodiscard]] static Result<OpenedRegistry> open(const OpenOptions& options);

  /// Audits a store without opening it. See inspect_store().
  [[nodiscard]] static Result<StoreInspection> inspect(const StoreOptions& options);

  // --- mutations --------------------------------------------------------
  //
  // Each returns the outcome of the attempt. A rejection changes nothing: no
  // generation, no revision, no provenance entry.

  [[nodiscard]] Result<MutationResult> register_data_center(const RegisterCommand& command);
  [[nodiscard]] Result<MutationResult> update_metadata(const UpdateMetadataCommand& command);
  [[nodiscard]] Result<MutationResult> transition_lifecycle(const TransitionCommand& command);
  [[nodiscard]] Result<MutationResult> attach_site(const AttachSiteCommand& command);
  [[nodiscard]] Result<MutationResult> detach_site(const DetachSiteCommand& command);
  [[nodiscard]] Result<MutationResult> retire_data_center(const RetireCommand& command);
  /// Retires `retired_id` and creates `new_id` in one generation, or does
  /// neither. The predecessor moves to `replaced` and records `replaced_by`;
  /// the successor records `replaces`. A retired record never returns to
  /// service and its identity is never reusable.
  [[nodiscard]] Result<MutationResult> replace_data_center(const ReplaceCommand& command);

  // --- queries ----------------------------------------------------------

  /// An immutable view of the current state. Cheap: it shares storage with the
  /// registry and stays valid after later commits.
  [[nodiscard]] Result<RegistrySnapshot> snapshot() const;

  [[nodiscard]] Result<DataCenterRecord> get(const DataCenterId& id) const;
  [[nodiscard]] std::optional<DataCenterRecord> find(const DataCenterId& id) const;
  [[nodiscard]] Result<DataCenterRecord> find_by_alias(const Alias& alias) const;
  [[nodiscard]] Result<std::vector<DataCenterRecord>> enumerate(
      const EnumerationQuery& query) const;
  [[nodiscard]] Result<std::vector<DataCenterId>> enumerate_ids(
      const EnumerationQuery& query) const;
  [[nodiscard]] std::vector<DataCenterRecord> find_by_display_name(std::string_view name) const;
  [[nodiscard]] HistoryPage history(const HistoryQuery& query) const;
  [[nodiscard]] RegistryStats stats() const;

  [[nodiscard]] RegistryGeneration generation() const;
  [[nodiscard]] std::optional<EpochToken> external_epoch() const;
  [[nodiscard]] std::uint64_t record_count() const;
  [[nodiscard]] Sha256Digest snapshot_digest() const;

  // --- lifecycle --------------------------------------------------------

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] bool is_writable() const noexcept;
  [[nodiscard]] RegistryLimits limits() const;
  [[nodiscard]] const std::filesystem::path& root() const noexcept;

  /// Re-reads the newest committed generation. Only valid for a registry
  /// opened read-only, which is how an inspection tool follows a live writer.
  [[nodiscard]] Result<OpenReport> refresh();

  /// Releases the writer lock and marks the registry closed. Idempotent.
  ///
  /// After close, mutations and refresh() are rejected with `closed`, and no
  /// further publication is possible. Read operations keep working against the
  /// last committed immutable state the registry held, and every value
  /// accessor keeps reporting it, because reporting a zero generation or an
  /// empty record set for state that is still known exactly would be less
  /// truthful than reporting it. A closed writable registry lets another
  /// process open the same store for writing.
  [[nodiscard]] Result<void> close();

 private:
  struct Impl;
  explicit Registry(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace dcr

#endif  // DCR_REGISTRY_HPP
