// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Independent-process tests.
//
// Every assertion here is about behaviour between real operating-system
// processes, started by this test binary and observed through files they write.
// The registry's writer lock, its generation fence, and its crash recovery are
// all claimed at process granularity, so they are proved at process
// granularity.
//
// The helper binary is located through the DCR_TEST_CHILD environment variable,
// which the test runner sets to the built helper. When it is not set the suite
// reports that plainly rather than passing silently.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "dcr/data_center_registry.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace dcr;

namespace {

std::filesystem::path child_program() {
  return dcrtest::helper_executable("DCR_TEST_CHILD");
}

/// Runs the helper and returns its exit code, recording a failure when the
/// helper is unavailable.
int run_child(const std::vector<std::string>& arguments) {
  const std::filesystem::path program = child_program();
  if (program.empty()) {
    dcrtest::record_failure(__FILE__, __LINE__,
                            "DCR_TEST_CHILD is not set, so independent-process behaviour was "
                            "not exercised");
    return -1;
  }
  return dcrtest::run_child_process(program, arguments);
}

/// Reads a child's result file as a single line.
std::string child_result(const std::filesystem::path& path) {
  std::string contents;
  if (!dcrtest::read_text_file(path, contents)) {
    dcrtest::record_failure(__FILE__, __LINE__, "the child wrote no result to " + path.string());
    return {};
  }
  while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r')) {
    contents.pop_back();
  }
  return contents;
}

bool starts_with(const std::string& text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

DCR_TEST(multiprocess, the_helper_is_available) {
  const std::filesystem::path program = child_program();
  if (program.empty()) {
    dcrtest::record_failure(__FILE__, __LINE__,
                            "DCR_TEST_CHILD is not set; the independent-process suite cannot run");
    return;
  }
  DCR_CHECK(std::filesystem::exists(program));
}

DCR_TEST(multiprocess, a_live_writer_fences_other_processes) {
  dcrtest::Fixture fixture("mp-fence");
  auto holder = fixture.open(StoreOpenMode::open_or_create);
  Registry& registry = *holder.registry;
  DCR_REQUIRE_OK(const MutationResult created,
                 registry.register_data_center(fixture.register_command(
                     "dc-holder", "Holder", LifecycleState::registered, "site-east")));
  DCR_CHECK(created.status == MutationStatus::created);

  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-holder")));
  DCR_CHECK_EQ(std::string("Holder"), record.display_name);

  const std::filesystem::path result = fixture.root() / "child-result.txt";
  const int status = run_child({"try-open-writer", fixture.root().string(), result.string()});
  DCR_CHECK_EQ(0, status);
  const std::string outcome = child_result(result);
  DCR_CHECK(starts_with(outcome, "refused store_locked"));

  // A read-only observer in another process is allowed while a writer holds the
  // lock: published generations are immutable.
  const std::filesystem::path read_result = fixture.root() / "child-read.txt";
  DCR_CHECK_EQ(0, run_child({"read", fixture.root().string(), read_result.string()}));
  DCR_CHECK(starts_with(child_result(read_result), "read generation=1 records=1"));

  // The authoritative state is untouched by the refused writer.
  DCR_CHECK_EQ(std::uint64_t{1}, registry.generation().value());
  DCR_CHECK_EQ(std::uint64_t{1}, registry.record_count());
}

DCR_TEST(multiprocess, work_committed_by_one_process_is_visible_to_the_next) {
  dcrtest::Fixture fixture("mp-handover");
  Sha256Digest digest;
  {
    auto first = fixture.open(StoreOpenMode::open_or_create);
    DCR_REQUIRE_OK(const MutationResult created,
                   first.registry->register_data_center(fixture.register_command(
                       "dc-first", "First", LifecycleState::registered, "site-east")));
    DCR_CHECK(created.status == MutationStatus::created);
    digest = first.registry->snapshot_digest();
    DCR_CHECK(first.registry->close().has_value());
  }

  // A second process commits three more generations.
  const std::filesystem::path result = fixture.root() / "child-result.txt";
  DCR_CHECK_EQ(0, run_child({"register", fixture.root().string(), result.string(), "child-actor",
                             "3"}));
  const std::string outcome = child_result(result);
  DCR_CHECK(starts_with(outcome, "registered generation=4 records=4"));

  // A third process reads it.
  const std::filesystem::path read_result = fixture.root() / "child-read.txt";
  DCR_CHECK_EQ(0, run_child({"read", fixture.root().string(), read_result.string()}));
  const std::string read_outcome = child_result(read_result);
  DCR_CHECK(starts_with(read_outcome, "read generation=4 records=4"));

  // The parent reopens and sees exactly the same thing, and the digest differs
  // from the state it wrote itself.
  auto reopened = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{4}, reopened.registry->generation().value());
  DCR_CHECK_EQ(std::uint64_t{4}, reopened.registry->record_count());
  DCR_CHECK(!(reopened.registry->snapshot_digest() == digest));
  DCR_REQUIRE_OK(const DataCenterRecord child_record,
                 reopened.registry->get(fixture.id("dc-child-2")));
  DCR_CHECK_EQ(std::string("Child Facility 2"), child_record.display_name);
  DCR_CHECK(child_record.last_provenance.source() == ProvenanceSource::cli);
  DCR_CHECK_EQ(std::string("child-actor"), child_record.last_provenance.principal().value());
}

DCR_TEST(multiprocess, a_stale_writer_cannot_overwrite_a_newer_generation) {
  // The claim under test: after one process restarts or hands over, a writer
  // holding an older view of the store can neither commit nor corrupt the
  // newer committed generation.
  dcrtest::Fixture fixture("mp-stale");
  std::uint64_t observed_generation = 0;
  Sha256Digest committed_digest;
  {
    auto first = fixture.open(StoreOpenMode::open_or_create);
    DCR_REQUIRE_OK(const MutationResult created,
                   first.registry->register_data_center(fixture.register_command(
                       "dc-parent", "From Parent", LifecycleState::registered, "site-east")));
    DCR_CHECK(created.status == MutationStatus::created);
    observed_generation = first.registry->generation().value();
    DCR_CHECK(first.registry->close().has_value());
  }

  // Another process advances the store twice.
  const std::filesystem::path register_result = fixture.root() / "child-register.txt";
  DCR_CHECK_EQ(0, run_child({"register", fixture.root().string(), register_result.string(),
                             "child-actor", "2"}));
  DCR_CHECK(starts_with(child_result(register_result), "registered generation=3 records=3"));

  auto fresh = fixture.open(StoreOpenMode::open_existing);
  committed_digest = fresh.registry->snapshot_digest();
  DCR_CHECK_EQ(std::uint64_t{3}, fresh.registry->generation().value());
  DCR_CHECK(fresh.registry->close().has_value());

  // A third process asserts the generation it saw before the handover. It is
  // refused, and the store is unchanged.
  const std::filesystem::path mutate_result = fixture.root() / "child-mutate.txt";
  DCR_CHECK_EQ(0, run_child({"mutate-at", fixture.root().string(), mutate_result.string(),
                             "stale-actor", std::to_string(observed_generation)}));
  const std::string outcome = child_result(mutate_result);
  DCR_CHECK(starts_with(outcome, "rejected stale_generation"));

  auto final_state = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{3}, final_state.registry->generation().value());
  DCR_CHECK(final_state.registry->snapshot_digest() == committed_digest);
  DCR_REQUIRE_OK(const DataCenterRecord record, final_state.registry->get(fixture.id("dc-child-0")));
  DCR_CHECK_EQ(std::string("Child Facility 0"), record.display_name);
  DCR_REQUIRE_OK(const DataCenterRecord parent,
                 final_state.registry->get(fixture.id("dc-parent")));
  DCR_CHECK_EQ(std::string("From Parent"), parent.display_name);

  // With the current generation the same mutation does commit, which shows the
  // refusal was about staleness and not about the actor.
  DCR_CHECK(final_state.registry->close().has_value());
  const std::filesystem::path fresh_result = fixture.root() / "child-mutate2.txt";
  DCR_CHECK_EQ(0, run_child({"mutate-at", fixture.root().string(), fresh_result.string(),
                             "fresh-actor", "3"}));
  DCR_CHECK(starts_with(child_result(fresh_result), "applied updated generation=4"));
}

DCR_TEST(multiprocess, a_crashed_writer_leaves_the_last_committed_generation_authoritative) {
  dcrtest::Fixture fixture("mp-crash");
  Sha256Digest committed_digest;
  {
    auto opened = fixture.open(StoreOpenMode::open_or_create);
    DCR_REQUIRE_OK(const MutationResult created,
                   opened.registry->register_data_center(fixture.register_command(
                       "dc-keep", "Keep", LifecycleState::registered, "site-east")));
    DCR_CHECK(created.status == MutationStatus::created);
    committed_digest = opened.registry->snapshot_digest();
    DCR_CHECK(opened.registry->close().has_value());
  }

  // A separate process leaves a truncated generation file and staging residue
  // behind and dies.
  const std::filesystem::path result = fixture.root() / "child-crash.txt";
  const int status = run_child({"crash-mid-write", fixture.root().string(), result.string()});
  DCR_CHECK_EQ(3, status);
  DCR_CHECK(starts_with(child_result(result), "crashed with gen-00000000000000000002.dcrs"));
  DCR_CHECK(std::filesystem::exists(fixture.root() / "tmp" / "snapshot.staging-crash"));

  // The committed generation two does not exist; the truncated file claims it.
  // Recovery keeps generation one authoritative, sets the unusable file aside,
  // clears the staging residue, and makes the store writable again.
  auto recovered = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{1}, recovered.registry->generation().value());
  DCR_CHECK(recovered.registry->snapshot_digest() == committed_digest);
  DCR_REQUIRE_OK(const DataCenterRecord record, recovered.registry->get(fixture.id("dc-keep")));
  DCR_CHECK_EQ(std::string("Keep"), record.display_name);
  DCR_CHECK(!std::filesystem::exists(fixture.root() / "tmp" / "snapshot.staging-crash"));

  // The truncated file was reported as unusable and set aside, never adopted.
  DCR_CHECK(!recovered.report.unusable_generations.empty());
  DCR_CHECK(!recovered.report.quarantined_generations.empty());
  DCR_CHECK_EQ(std::uint64_t{2}, recovered.report.quarantined_generations.front().value());
  DCR_CHECK(std::filesystem::exists(fixture.root() / "uncommitted" /
                                    "gen-00000000000000000002.dcrs"));

  // Writing continues, and the generation number the crash claimed is free
  // again.
  DCR_REQUIRE_OK(const MutationResult created,
                 recovered.registry->register_data_center(fixture.register_command(
                     "dc-next", "Next", LifecycleState::registered, "site-east")));
  DCR_CHECK(created.status == MutationStatus::created);
  DCR_CHECK_EQ(std::uint64_t{2}, recovered.registry->generation().value());

  // A read-only observer in another process agrees.
  const std::filesystem::path read_result = fixture.root() / "child-read.txt";
  DCR_CHECK_EQ(0, run_child({"read", fixture.root().string(), read_result.string()}));
  DCR_CHECK(starts_with(child_result(read_result), "read generation=2 records=2"));
}

DCR_TEST(multiprocess, the_writer_lock_is_released_when_a_process_dies) {
  dcrtest::Fixture fixture("mp-death");
  {
    auto opened = fixture.open(StoreOpenMode::open_or_create);
    DCR_REQUIRE_OK(const MutationResult created,
                   opened.registry->register_data_center(fixture.register_command(
                       "dc-alpha", "Alpha", LifecycleState::registered, "site-east")));
    DCR_CHECK(created.status == MutationStatus::created);
    DCR_CHECK(opened.registry->close().has_value());
  }

  // The child takes the writer lock and terminates without releasing it.
  const std::filesystem::path result = fixture.root() / "child-lock.txt";
  const int status = run_child({"abort-while-locked", fixture.root().string(), result.string()});
  DCR_CHECK_EQ(4, status);
  DCR_CHECK(starts_with(child_result(result), "locked generation=1"));

  // The operating system released the lock with the process, so the store is
  // immediately writable again: a crash cannot leave a store permanently
  // locked, and no stale lock file blocks the next writer.
  auto reopened = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{1}, reopened.registry->generation().value());
  DCR_REQUIRE_OK(const MutationResult created,
                 reopened.registry->register_data_center(fixture.register_command(
                     "dc-beta", "Beta", LifecycleState::registered, "site-east")));
  DCR_CHECK(created.status == MutationStatus::created);
  DCR_CHECK_EQ(std::uint64_t{2}, reopened.registry->generation().value());
}

DCR_TEST(multiprocess, the_store_level_fence_stops_a_writer_that_ignores_the_lock) {
  // The registry's earliest line of defence is the writer lock. This test
  // removes it: a registry is opened read-only while another process writes,
  // and the store's own publication fence is exercised directly. Even a writer
  // that never took the lock cannot replace a newer committed generation.
  dcrtest::Fixture fixture("mp-fence2");
  {
    auto opened = fixture.open(StoreOpenMode::open_or_create);
    DCR_REQUIRE_OK(const MutationResult created,
                   opened.registry->register_data_center(fixture.register_command(
                       "dc-alpha", "Alpha", LifecycleState::registered, "site-east")));
    DCR_CHECK(created.status == MutationStatus::created);
    DCR_CHECK(opened.registry->close().has_value());
  }

  const std::filesystem::path register_result = fixture.root() / "child-register.txt";
  DCR_CHECK_EQ(0, run_child({"register", fixture.root().string(), register_result.string(),
                             "child-actor", "1"}));
  DCR_CHECK(starts_with(child_result(register_result), "registered generation=2"));

  // The parent now holds a view from generation 1 and asks to publish
  // generation 2 itself. The store refuses: generation 2 is already committed
  // by someone else.
  auto stale = fixture.open(StoreOpenMode::read_only);
  DCR_CHECK_EQ(std::uint64_t{2}, stale.registry->generation().value());
  DCR_CHECK(stale.registry->close().has_value());

  const std::filesystem::path mutate_result = fixture.root() / "child-mutate.txt";
  DCR_CHECK_EQ(0, run_child({"mutate-at", fixture.root().string(), mutate_result.string(),
                             "stale-actor", "1"}));
  DCR_CHECK(starts_with(child_result(mutate_result), "rejected stale_generation"));

  auto final_state = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{2}, final_state.registry->generation().value());
  DCR_CHECK_EQ(std::uint64_t{2}, final_state.registry->record_count());
}
