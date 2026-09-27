// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "dcr/registry.hpp"

#include <mutex>
#include <shared_mutex>
#include <utility>

#include "core/mutation.hpp"
#include "core/snapshot_impl.hpp"
#include "core/state.hpp"
#include "persistence/store.hpp"

namespace dcr {

namespace {

using internal::MutationPlan;
using internal::RegistryState;

[[nodiscard]] Error closed_error() {
  return Error(ErrorCode::closed, "the registry has been closed", "registry");
}

}  // namespace

struct Registry::Impl {
  OpenOptions options;
  SystemClock default_clock;
  const Clock* clock = nullptr;
  RegistryLimits limits;

  /// Serialises mutations. Held across planning, publication and the state
  /// swap, so at most one mutation per instance is ever in flight.
  std::mutex write_mutex;

  /// Guards the state pointer and its digest, and nothing else. It is held for
  /// the few instructions it takes to copy or replace a shared pointer, never
  /// across planning, I/O or a callback, so readers are not blocked by a
  /// writer's fsync.
  ///
  /// Lock order, everywhere: write_mutex before state_mutex, never the
  /// reverse. Readers take only state_mutex. No code path takes write_mutex
  /// while holding state_mutex.
  mutable std::shared_mutex state_mutex;
  std::shared_ptr<const RegistryState> state;
  Sha256Digest digest;

  std::unique_ptr<internal::SnapshotStore> store;
  bool writable = false;
  bool closed = false;

  [[nodiscard]] std::shared_ptr<const RegistryState> load_state() const {
    std::shared_lock guard(state_mutex);
    return state;
  }

  [[nodiscard]] Sha256Digest load_digest() const {
    std::shared_lock guard(state_mutex);
    return digest;
  }

  void store_state(std::shared_ptr<const RegistryState> next, Sha256Digest next_digest) {
    std::unique_lock guard(state_mutex);
    state = std::move(next);
    digest = next_digest;
  }
};

Registry::Registry(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Registry::~Registry() {
  if (impl_) {
    const auto result = close();
    (void)result;
  }
}

Result<OpenedRegistry> Registry::open(const OpenOptions& options) {
  DCR_TRY(options.store.limits.validate());
  DCR_TRY_ASSIGN(internal::StoreOpenResult opened, internal::SnapshotStore::open(options.store));

  auto impl = std::make_unique<Impl>();
  impl->options = options;
  impl->clock = options.clock != nullptr ? options.clock : &impl->default_clock;
  impl->limits = options.store.limits;
  impl->writable = opened.store.writable();
  impl->store = std::make_unique<internal::SnapshotStore>(std::move(opened.store));
  impl->state = std::make_shared<const RegistryState>(std::move(opened.state));
  impl->digest = opened.report.snapshot_digest;

  std::unique_ptr<Registry> registry(new Registry(std::move(impl)));
  return OpenedRegistry{std::move(registry), std::move(opened.report)};
}

Result<StoreInspection> Registry::inspect(const StoreOptions& options) {
  return dcr::inspect_store(options);
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

namespace {

/// The shared mutation path: one writer lock, plan, publish, swap.
template <class Command, class Planner>
[[nodiscard]] Result<MutationResult> run_mutation(Registry::Impl& impl, const Command& command,
                                                  Planner planner) {
  std::lock_guard<std::mutex> writer(impl.write_mutex);
  if (impl.closed) {
    return closed_error();
  }
  if (!impl.writable) {
    return Error(ErrorCode::store_read_only,
                 "this registry was opened read-only and cannot accept mutations", "store.mode");
  }
  const std::shared_ptr<const RegistryState> base = impl.load_state();
  const Timestamp now = impl.clock->now();
  DCR_TRY_ASSIGN(MutationPlan plan, planner(*base, command, now));
  if (plan.unchanged) {
    return plan.result;
  }
  // The last cancellation checkpoint sits after the new generation has been
  // planned and before it is published, so a cancelled mutation can never
  // publish.
  if (command.context.cancellation.is_cancelled()) {
    return Error(ErrorCode::cancelled,
                 "the mutation was cancelled before it was published; nothing was written",
                 "cancellation");
  }
  DCR_TRY_ASSIGN(const Sha256Digest published_digest,
                 impl.store->publish(plan.state, base->generation));
  impl.store_state(std::make_shared<const RegistryState>(std::move(plan.state)),
                   published_digest);
  return plan.result;
}

}  // namespace

Result<MutationResult> Registry::register_data_center(const RegisterCommand& command) {
  return run_mutation(*impl_, command,
                      [](const RegistryState& base, const RegisterCommand& value, Timestamp now) {
                        return internal::plan_register(base, value, now);
                      });
}

Result<MutationResult> Registry::update_metadata(const UpdateMetadataCommand& command) {
  return run_mutation(
      *impl_, command,
      [](const RegistryState& base, const UpdateMetadataCommand& value, Timestamp now) {
        return internal::plan_update(base, value, now);
      });
}

Result<MutationResult> Registry::transition_lifecycle(const TransitionCommand& command) {
  return run_mutation(
      *impl_, command,
      [](const RegistryState& base, const TransitionCommand& value, Timestamp now) {
        return internal::plan_transition(base, value, now);
      });
}

Result<MutationResult> Registry::attach_site(const AttachSiteCommand& command) {
  return run_mutation(*impl_, command,
                      [](const RegistryState& base, const AttachSiteCommand& value, Timestamp now) {
                        return internal::plan_attach(base, value, now);
                      });
}

Result<MutationResult> Registry::detach_site(const DetachSiteCommand& command) {
  return run_mutation(*impl_, command,
                      [](const RegistryState& base, const DetachSiteCommand& value, Timestamp now) {
                        return internal::plan_detach(base, value, now);
                      });
}

Result<MutationResult> Registry::retire_data_center(const RetireCommand& command) {
  return run_mutation(*impl_, command,
                      [](const RegistryState& base, const RetireCommand& value, Timestamp now) {
                        return internal::plan_retire(base, value, now);
                      });
}

Result<MutationResult> Registry::replace_data_center(const ReplaceCommand& command) {
  return run_mutation(*impl_, command,
                      [](const RegistryState& base, const ReplaceCommand& value, Timestamp now) {
                        return internal::plan_replace(base, value, now);
                      });
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Result<RegistrySnapshot> Registry::snapshot() const {
  std::shared_ptr<const RegistryState> state;
  Sha256Digest digest;
  {
    std::shared_lock guard(impl_->state_mutex);
    state = impl_->state;
    digest = impl_->digest;
  }
  // A registry that has been closed still holds its last immutable state, and
  // reading it stays valid: closing stops writes and releases the writer lock,
  // it does not make the last known state unknowable.
  return RegistrySnapshot(std::make_shared<RegistrySnapshot::Impl>(std::move(state), digest));
}
Result<DataCenterRecord> Registry::get(const DataCenterId& id) const {
  DCR_TRY_ASSIGN(RegistrySnapshot view, snapshot());
  auto record = view.find(id);
  if (!record.has_value()) {
    return Error(ErrorCode::not_found, "no record with identity '" + id.value() + "' exists",
                 "id");
  }
  return std::move(record).value();
}

std::optional<DataCenterRecord> Registry::find(const DataCenterId& id) const {
  auto view = snapshot();
  if (!view.has_value()) {
    return std::nullopt;
  }
  return view.value().find(id);
}

Result<DataCenterRecord> Registry::find_by_alias(const Alias& alias) const {
  DCR_TRY_ASSIGN(RegistrySnapshot view, snapshot());
  auto record = view.find_by_alias(alias);
  if (!record.has_value()) {
    return Error(ErrorCode::not_found,
                 "no record claims the alias '" + alias.value() + "'", "alias");
  }
  return std::move(record).value();
}

Result<std::vector<DataCenterRecord>> Registry::enumerate(const EnumerationQuery& query) const {
  DCR_TRY_ASSIGN(RegistrySnapshot view, snapshot());
  return view.enumerate(query);
}

Result<std::vector<DataCenterId>> Registry::enumerate_ids(const EnumerationQuery& query) const {
  DCR_TRY_ASSIGN(RegistrySnapshot view, snapshot());
  return view.enumerate_ids(query);
}

std::vector<DataCenterRecord> Registry::find_by_display_name(std::string_view name) const {
  auto view = snapshot();
  if (!view.has_value()) {
    return {};
  }
  return view.value().find_by_display_name(name);
}

HistoryPage Registry::history(const HistoryQuery& query) const {
  auto view = snapshot();
  if (!view.has_value()) {
    return HistoryPage{};
  }
  return view.value().history(query);
}

RegistryStats Registry::stats() const {
  auto view = snapshot();
  if (!view.has_value()) {
    RegistryStats empty_stats;
    empty_stats.generation = RegistryGeneration::minimum();
    return empty_stats;
  }
  return view.value().stats();
}

RegistryGeneration Registry::generation() const {
  const auto state = impl_->load_state();
  return state == nullptr ? RegistryGeneration::minimum() : state->generation;
}

std::optional<EpochToken> Registry::external_epoch() const {
  const auto state = impl_->load_state();
  if (state == nullptr) {
    return std::nullopt;
  }
  return state->external_epoch;
}

std::uint64_t Registry::record_count() const {
  const auto state = impl_->load_state();
  return state == nullptr ? 0 : static_cast<std::uint64_t>(state->records.size());
}

Sha256Digest Registry::snapshot_digest() const { return impl_->load_digest(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Registry::is_open() const noexcept {
  std::shared_lock guard(impl_->state_mutex);
  return !impl_->closed;
}

bool Registry::is_writable() const noexcept {
  std::shared_lock guard(impl_->state_mutex);
  return !impl_->closed && impl_->writable;
}

RegistryLimits Registry::limits() const { return impl_->limits; }

const std::filesystem::path& Registry::root() const noexcept { return impl_->options.store.root; }

Result<OpenReport> Registry::refresh() {
  std::lock_guard<std::mutex> writer(impl_->write_mutex);
  if (impl_->closed) {
    return closed_error();
  }
  if (impl_->writable) {
    return Error(ErrorCode::invalid_argument,
                 "refresh is for read-only observers; a writable registry is the writer and "
                 "already holds the newest generation",
                 "store.mode");
  }
  OpenReport report;
  report.mode = impl_->options.store.mode;
  // A refresh reads a store that already exists by definition.
  DCR_TRY_ASSIGN(RegistryState reloaded,
                 internal::load_state(impl_->options.store, report, true));
  impl_->store_state(std::make_shared<const RegistryState>(std::move(reloaded)),
                     report.snapshot_digest);
  return report;
}
Result<void> Registry::close() {
  std::lock_guard<std::mutex> writer(impl_->write_mutex);
  if (impl_->closed) {
    return {};
  }
  {
    std::unique_lock guard(impl_->state_mutex);
    impl_->closed = true;
  }
  impl_->store->close();
  return {};
}
}  // namespace dcr
