// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Concurrency and lifecycle.
//
// The library creates no threads of its own, so every thread here is one the
// test started. What is asserted:
//
//   * concurrent readers always observe a consistent, immutable state;
//   * concurrent writers are serialised by the registry, so a mutation whose
//     precondition has been overtaken is rejected rather than merged;
//   * exactly one writer wins a race for the same generation;
//   * no update is ever lost when writers retry on rejection;
//   * closing a registry while work is in flight never commits work that did
//     not cross the commit point, and never deadlocks.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "dcr/data_center_registry.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace dcr;

namespace {

/// Registers one facility, retrying while the precondition is overtaken.
/// Returns the number of attempts it took.
std::size_t register_with_retry(dcrtest::Fixture& fixture, Registry& registry,
                                std::string_view id_text, std::size_t max_attempts = 64) {
  std::size_t attempts = 0;
  while (attempts < max_attempts) {
    ++attempts;
    auto command = fixture.register_command(id_text, "Facility " + std::string(id_text),
                                            LifecycleState::registered, "site-east");
    auto outcome = registry.register_data_center(command);
    if (outcome.has_value()) {
      return attempts;
    }
    // Registration has no generation precondition, so the only retryable
    // rejection is a lock-order artifact; anything else is a defect in the
    // test.
    if (!is_retryable(outcome.error().code())) {
      dcrtest::record_failure(__FILE__, __LINE__, "regression rejected: " +
                                                      outcome.error().to_string());
      return attempts;
    }
  }
  dcrtest::record_failure(__FILE__, __LINE__, "registration did not succeed within the retry bound");
  return attempts;
}

/// Updates a record, retrying on a stale generation until the mutation lands.
std::size_t update_with_retry(dcrtest::Fixture& fixture, Registry& registry,
                              const DataCenterId& id, std::string_view principal,
                              std::string_view name, std::size_t max_attempts = 256) {
  std::size_t attempts = 0;
  while (attempts < max_attempts) {
    ++attempts;
    UpdateMetadataCommand command;
    command.context = fixture.context_at(registry, principal);
    command.id = id;
    command.patch.display_name = FieldPatch<std::string>::set(std::string(name));
    auto outcome = registry.update_metadata(command);
    if (outcome.has_value() && outcome.value().status == MutationStatus::updated) {
      return attempts;
    }
    if (outcome.has_value() && outcome.value().status == MutationStatus::unchanged) {
      return attempts;
    }
    if (!outcome.has_value() && outcome.error().code() == ErrorCode::stale_generation) {
      continue;
    }
    if (!outcome.has_value()) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              "update rejected: " + outcome.error().to_string());
      return attempts;
    }
  }
  dcrtest::record_failure(__FILE__, __LINE__, "update did not succeed within the retry bound");
  return attempts;
}

}  // namespace

// ---------------------------------------------------------------------------
// Concurrent writers
// ---------------------------------------------------------------------------

DCR_TEST(concurrency, every_committed_update_lands_exactly_once) {
  dcrtest::Fixture fixture("conc-updates");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  DCR_REQUIRE_OK(const MutationResult created,
                 registry.register_data_center(fixture.register_command(
                     "dc-shared", "Shared", LifecycleState::registered, "site-east")));
  DCR_CHECK(created.status == MutationStatus::created);

  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kUpdatesPerThread = 25;

  std::atomic<std::size_t> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([&, thread_index] {
      for (std::size_t update_index = 0; update_index < kUpdatesPerThread; ++update_index) {
        const std::string principal = "worker-" + std::to_string(thread_index);
        const std::string name = principal + "-" + std::to_string(update_index);
        const std::size_t attempts =
            update_with_retry(fixture, registry, fixture.id("dc-shared"), principal, name);
        if (attempts == 0) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  DCR_CHECK_EQ(std::size_t{0}, failures.load());

  // Every update committed exactly once: the revision is one for the creation
  // plus one per committed update.
  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-shared")));
  DCR_CHECK_EQ(std::uint64_t{1 + kThreads * kUpdatesPerThread}, record.revision.value());
  DCR_CHECK_EQ(std::uint64_t{1 + kThreads * kUpdatesPerThread}, registry.generation().value());
  DCR_CHECK_EQ(std::uint64_t{1 + kThreads * kUpdatesPerThread},
               registry.stats().history_entries);

  // Every worker's last write is visible in the history, so no update was lost
  // or merged away.
  const HistoryPage page = registry.history(HistoryQuery{});
  std::size_t updates = 0;
  for (const auto& entry : page.entries) {
    if (entry.action == HistoryAction::metadata_updated) {
      ++updates;
    }
  }
  DCR_CHECK_EQ(kThreads * kUpdatesPerThread, updates);
}

DCR_TEST(concurrency, exactly_one_writer_wins_a_generation_race) {
  dcrtest::Fixture fixture("conc-race");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  DCR_REQUIRE_OK(const MutationResult created,
                 registry.register_data_center(fixture.register_command(
                     "dc-shared", "Shared", LifecycleState::registered, "site-east")));

  const RegistryGeneration observed = registry.generation();
  constexpr std::size_t kThreads = 8;
  std::atomic<std::size_t> winners{0};
  std::atomic<std::size_t> stale{0};
  std::atomic<std::size_t> other{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([&, thread_index] {
      UpdateMetadataCommand command;
      command.context = fixture.context();
      command.context.precondition.expected_generation = observed;
      command.context.provenance.principal = *PrincipalId::parse(
          "racer-" + std::to_string(thread_index));
      command.id = fixture.id("dc-shared");
      command.patch.display_name =
          FieldPatch<std::string>::set("Racer " + std::to_string(thread_index));
      auto outcome = registry.update_metadata(command);
      if (outcome.has_value()) {
        winners.fetch_add(1);
      } else if (outcome.error().code() == ErrorCode::stale_generation) {
        stale.fetch_add(1);
      } else {
        other.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  DCR_CHECK_EQ(std::size_t{1}, winners.load());
  DCR_CHECK_EQ(kThreads - 1, stale.load());
  DCR_CHECK_EQ(std::size_t{0}, other.load());
  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-shared")));
  DCR_CHECK_EQ(std::uint64_t{2}, record.revision.value());
  DCR_CHECK_EQ(std::uint64_t{2}, registry.generation().value());
}

// ---------------------------------------------------------------------------
// Concurrent readers
// ---------------------------------------------------------------------------

DCR_TEST(concurrency, readers_never_observe_a_partial_state) {
  dcrtest::Fixture fixture("conc-readers");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  constexpr std::size_t kRecords = 40;
  for (std::size_t index = 0; index < kRecords; ++index) {
    DCR_REQUIRE_OK(const MutationResult created,
                   registry.register_data_center(fixture.register_command(
                       dcrtest::data_center_id_text(index), "Facility " + std::to_string(index),
                       LifecycleState::registered, "site-east")));
    static_cast<void>(created);
  }

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> observations{0};
  std::atomic<std::size_t> problems{0};

  std::vector<std::thread> readers;
  readers.reserve(4);
  for (std::size_t reader_index = 0; reader_index < 4; ++reader_index) {
    readers.emplace_back([&] {
      RegistryGeneration last_generation = RegistryGeneration::minimum();
      while (!stop.load()) {
        auto snapshot = registry.snapshot();
        if (!snapshot.has_value()) {
          problems.fetch_add(1);
          continue;
        }
        const std::uint64_t count = snapshot.value().record_count();
        const std::vector<DataCenterRecord> records = snapshot.value().records();
        if (records.size() != count) {
          problems.fetch_add(1);
        }
        if (snapshot.value().generation() < last_generation) {
          problems.fetch_add(1);
        }
        last_generation = snapshot.value().generation();
        for (const auto& record : records) {
          if (record.display_name.empty()) {
            problems.fetch_add(1);
          }
          std::size_t primaries = 0;
          for (const auto& membership : record.memberships) {
            if (membership.role == MembershipRole::primary) {
              ++primaries;
            }
          }
          if (primaries != 1) {
            problems.fetch_add(1);
          }
        }
        observations.fetch_add(1);
      }
    });
  }

  for (std::size_t index = 0; index < 60; ++index) {
    UpdateMetadataCommand command;
    command.context = fixture.context_at(registry);
    command.id = fixture.id(dcrtest::data_center_id_text(index % kRecords));
    command.patch.display_name = FieldPatch<std::string>::set("Renamed " + std::to_string(index));
    auto outcome = registry.update_metadata(command);
    if (!outcome.has_value() && outcome.error().code() != ErrorCode::stale_generation) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              "concurrent update rejected: " + outcome.error().to_string());
    }
  }
  stop.store(true);
  for (auto& reader : readers) {
    reader.join();
  }

  DCR_CHECK_EQ(std::size_t{0}, problems.load());
  DCR_CHECK(observations.load() > 0);
  DCR_CHECK_EQ(std::uint64_t{kRecords + 60}, registry.generation().value());
}

// ---------------------------------------------------------------------------
// Cancellation and shutdown
// ---------------------------------------------------------------------------

DCR_TEST(concurrency, a_cancelled_writer_never_publishes) {
  dcrtest::Fixture fixture("conc-cancel");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  DCR_REQUIRE_OK(const MutationResult created,
                 registry.register_data_center(fixture.register_command(
                     "dc-alpha", "Alpha", LifecycleState::registered, "site-east")));

  CancellationSource source;
  auto command = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                          "site-east");
  command.context.cancellation = source.token();
  source.cancel();

  for (int attempt = 0; attempt < 5; ++attempt) {
    DCR_REQUIRE_ERROR(ErrorCode::cancelled, registry.register_data_center(command));
  }
  DCR_CHECK_EQ(std::uint64_t{1}, registry.record_count());
  DCR_CHECK(!registry.find(fixture.id("dc-beta")).has_value());
}

DCR_TEST(concurrency, close_is_idempotent_and_stops_writes) {
  dcrtest::Fixture fixture("conc-close");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  DCR_REQUIRE_OK(const MutationResult created,
                 registry.register_data_center(fixture.register_command(
                     "dc-alpha", "Alpha", LifecycleState::registered, "site-east")));

  DCR_CHECK(registry.is_open());
  DCR_CHECK(registry.is_writable());
  DCR_CHECK(registry.close().has_value());
  DCR_CHECK(!registry.is_open());
  DCR_CHECK(!registry.is_writable());
  DCR_CHECK(registry.close().has_value());

  auto command = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                          "site-east");
  DCR_REQUIRE_ERROR(ErrorCode::closed, registry.register_data_center(command));
  DCR_REQUIRE_ERROR(ErrorCode::closed, registry.update_metadata([&] {
                      UpdateMetadataCommand update;
                      update.context = fixture.context_at(registry);
                      update.id = fixture.id("dc-alpha");
                      update.patch.display_name = FieldPatch<std::string>::set("Renamed");
                      return update;
                    }()));
  DCR_REQUIRE_ERROR(ErrorCode::closed, registry.refresh());

  // Reads still report the state the registry was closed at, because that
  // state is known exactly and reporting zeros would be less truthful.
  DCR_CHECK_EQ(std::uint64_t{1}, registry.generation().value());
  DCR_CHECK_EQ(std::uint64_t{1}, registry.record_count());
  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-alpha")));
  DCR_CHECK_EQ(std::string("Alpha"), record.display_name);
}

DCR_TEST(concurrency, closing_while_writers_are_in_flight_settles_cleanly) {
  dcrtest::Fixture fixture("conc-shutdown");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> committed{0};
  std::atomic<std::size_t> refused{0};
  std::atomic<std::size_t> unexpected{0};

  std::vector<std::thread> writers;
  writers.reserve(4);
  for (std::size_t thread_index = 0; thread_index < 4; ++thread_index) {
    writers.emplace_back([&, thread_index] {
      std::size_t index = 0;
      while (!stop.load()) {
        auto command = fixture.register_command(
            "dc-w" + std::to_string(thread_index) + "-" + std::to_string(index),
            "Facility " + std::to_string(index), LifecycleState::registered, "site-east");
        ++index;
        auto outcome = registry.register_data_center(command);
        if (outcome.has_value()) {
          committed.fetch_add(1);
        } else if (outcome.error().code() == ErrorCode::closed ||
                   outcome.error().code() == ErrorCode::duplicate_identity ||
                   outcome.error().code() == ErrorCode::store_locked) {
          refused.fetch_add(1);
        } else {
          unexpected.fetch_add(1);
        }
      }
    });
  }

  // Let the writers get going, then close underneath them. This is the
  // shutdown path: close waits for the in-flight mutation, and everything
  // after it is refused rather than committed into a registry that is going
  // away.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  DCR_CHECK(registry.close().has_value());
  stop.store(true);
  for (auto& writer : writers) {
    writer.join();
  }

  DCR_CHECK_EQ(std::size_t{0}, unexpected.load());
  DCR_CHECK(committed.load() > 0);
  // The generation matches the number of commits exactly: nothing was
  // half-committed and nothing was double-counted.
  DCR_CHECK_EQ(static_cast<std::uint64_t>(committed.load()), registry.generation().value());
  DCR_CHECK_EQ(static_cast<std::uint64_t>(committed.load()), registry.record_count());
  DCR_CHECK_EQ(committed.load(), registry.stats().history_entries);

  // Every writer that reaches the registry after the close is refused rather
  // than committed into a registry that is going away.
  DCR_REQUIRE_ERROR(ErrorCode::closed,
                    registry.register_data_center(fixture.register_command(
                        "dc-after-close", "After Close", LifecycleState::registered, "site-east")));
  DCR_CHECK_EQ(static_cast<std::uint64_t>(committed.load()), registry.generation().value());

  // The store is intact and reopenable after the shutdown.
  auto reopened = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(registry.generation().value(), reopened.registry->generation().value());
  DCR_CHECK_EQ(registry.record_count(), reopened.registry->record_count());
}

DCR_TEST(concurrency, a_registry_destructor_releases_the_writer_lock) {
  dcrtest::Fixture fixture("conc-destructor");
  {
    auto opened = fixture.open();
    DCR_REQUIRE_OK(const MutationResult created,
                   opened.registry->register_data_center(fixture.register_command(
                       "dc-alpha", "Alpha", LifecycleState::registered, "site-east")));
    DCR_CHECK(created.status == MutationStatus::created);
    // No explicit close: the destructor must release the lock.
  }
  auto reopened = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{1}, reopened.registry->generation().value());
}
