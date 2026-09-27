// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Corrupt, truncated, oversized and tampered state.
//
// Every case here asserts the same property in a different way: state that
// does not verify never becomes authoritative. The registry either adopts a
// generation it has fully verified, or it refuses to open.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/codec.hpp"
#include "dcr/data_center_registry.hpp"
#include "persistence/store.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace dcr;

namespace {

std::filesystem::path generations_dir(const std::filesystem::path& root) {
  return root / "generations";
}

std::filesystem::path current_file(const std::filesystem::path& root) { return root / "CURRENT"; }

std::filesystem::path newest_generation_file(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> files;
  std::error_code code;
  for (const auto& entry : std::filesystem::directory_iterator(generations_dir(root), code)) {
    files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files.empty() ? std::filesystem::path() : files.back();
}

std::string read_all(const std::filesystem::path& path) {
  std::string contents;
  if (!dcrtest::read_text_file(path, contents)) {
    dcrtest::record_failure(__FILE__, __LINE__, "cannot read " + path.string());
  }
  return contents;
}

void write_all(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!stream) {
    dcrtest::record_failure(__FILE__, __LINE__, "cannot write " + path.string());
  }
}

/// Registers two facilities and closes, leaving a store with two generations.
void seed_store(dcrtest::Fixture& fixture) {
  auto opened = fixture.open();
  for (std::uint64_t index = 0; index < 2; ++index) {
    auto command = fixture.register_command(dcrtest::data_center_id_text(index),
                                            "Facility " + std::to_string(index),
                                            LifecycleState::registered, "site-east");
    auto created = opened.registry->register_data_center(command);
    if (!created.has_value()) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              "seed_store failed: " + created.error().to_string());
    }
  }
  if (!opened.registry->close().has_value()) {
    dcrtest::record_failure(__FILE__, __LINE__, "seed_store could not close the registry");
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Container integrity
// ---------------------------------------------------------------------------

DCR_TEST(corruption, a_flipped_payload_byte_is_detected) {
  dcrtest::Fixture fixture("corrupt-flip");
  seed_store(fixture);
  const std::filesystem::path file = newest_generation_file(fixture.root());
  std::string bytes = read_all(file);
  DCR_REQUIRE(bytes.size() > 100);

  // Flip one bit in the payload, leaving the header untouched.
  bytes[bytes.size() - 5] = static_cast<char>(bytes[bytes.size() - 5] ^ 0x01);
  write_all(file, bytes);

  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
  DCR_REQUIRE_OK(const StoreInspection inspection,
                 Registry::inspect(fixture.store_options(StoreOpenMode::open_existing)));
  DCR_CHECK(inspection.current_present);
  DCR_CHECK(!inspection.current_valid);
  DCR_CHECK(!inspection.consistent);
  // Generation 1 still verifies, so the store is recoverable by explicit
  // choice rather than by guessing.
  DCR_CHECK(inspection.newest_valid_generation.has_value());
  DCR_CHECK_EQ(std::uint64_t{1}, inspection.newest_valid_generation->value());
}

DCR_TEST(corruption, a_flipped_header_byte_is_detected) {
  dcrtest::Fixture fixture("corrupt-header");
  seed_store(fixture);
  const std::filesystem::path file = newest_generation_file(fixture.root());
  const std::string original = read_all(file);

  // A byte inside the header digest: the header no longer verifies.
  std::string damaged_digest = original;
  damaged_digest[60] = static_cast<char>(damaged_digest[60] ^ 0xFF);
  write_all(file, damaged_digest);
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  // A byte inside the declared payload length: the length no longer matches
  // the file.
  std::string damaged_length = original;
  damaged_length[16] = static_cast<char>(damaged_length[16] ^ 0xFF);
  write_all(file, damaged_length);
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  // A byte inside the declared format version is a version problem, not
  // corruption, and says so.
  std::string damaged_version = original;
  damaged_version[10] = static_cast<char>(damaged_version[10] ^ 0x01);
  write_all(file, damaged_version);
  DCR_REQUIRE_ERROR(ErrorCode::store_incompatible_version,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  write_all(file, original);
  auto restored = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
  DCR_CHECK(restored.has_value());
}

DCR_TEST(corruption, truncation_is_detected) {
  dcrtest::Fixture fixture("corrupt-truncate");
  seed_store(fixture);
  const std::filesystem::path file = newest_generation_file(fixture.root());
  const std::string bytes = read_all(file);

  for (const std::size_t keep : {std::size_t{0}, std::size_t{1}, std::size_t{40},
                                 std::size_t{87}, bytes.size() / 2, bytes.size() - 1}) {
    write_all(file, std::string_view(bytes).substr(0, keep));
    auto opened = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
    DCR_CHECK(!opened.has_value());
    if (!opened.has_value()) {
      DCR_CHECK(opened.error().code() == ErrorCode::store_corrupt ||
                opened.error().code() == ErrorCode::store_limit_exceeded);
    }
  }

  // Restoring the file makes the store usable again: nothing about the refusal
  // was sticky.
  write_all(file, bytes);
  auto opened = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
  DCR_CHECK(opened.has_value());
  if (opened.has_value()) {
    DCR_CHECK_EQ(std::uint64_t{2}, opened.value().registry->generation().value());
  }
}

DCR_TEST(corruption, an_oversized_file_is_rejected_before_it_is_read) {
  dcrtest::Fixture fixture("corrupt-oversize");
  seed_store(fixture);
  const std::filesystem::path file = newest_generation_file(fixture.root());

  // A file beyond the configured snapshot bound is refused by size, without
  // being decoded.
  {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    const std::string chunk(1024 * 1024, 'x');
    for (int index = 0; index < 4; ++index) {
      stream.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
  }
  dcr::StoreOptions tight = fixture.store_options(StoreOpenMode::open_existing);
  tight.limits.max_snapshot_bytes = 1024ULL * 1024ULL;
  dcr::OpenOptions tight_options;
  tight_options.store = tight;
  tight_options.clock = &fixture.clock();
  DCR_REQUIRE_ERROR(ErrorCode::store_limit_exceeded, Registry::open(tight_options));

  // Under a bound that admits it, the same file fails integrity instead, which
  // shows the bound is what stopped it the first time.
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
}

DCR_TEST(corruption, wrong_magic_and_endian_marker_are_rejected) {
  dcrtest::Fixture fixture("corrupt-magic");
  seed_store(fixture);
  const std::filesystem::path file = newest_generation_file(fixture.root());
  const std::string original = read_all(file);

  std::string wrong_magic = original;
  wrong_magic[0] = 'X';
  write_all(file, wrong_magic);
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  std::string wrong_endian = original;
  // The marker is 0x01020304 stored little-endian, so byte 12 is already 0x04.
  wrong_endian[13] = 0x7F;
  write_all(file, wrong_endian);
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  std::string wrong_version = original;
  wrong_version[8] = 9;
  write_all(file, wrong_version);
  DCR_REQUIRE_ERROR(ErrorCode::store_incompatible_version,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
}

// ---------------------------------------------------------------------------
// CURRENT file handling
// ---------------------------------------------------------------------------

DCR_TEST(corruption, a_damaged_current_file_is_not_silently_ignored) {
  dcrtest::Fixture fixture("corrupt-current");
  seed_store(fixture);

  write_all(current_file(fixture.root()), "not a current file\n");
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  write_all(current_file(fixture.root()), "");
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  // CURRENT naming a file that does not exist is refused.
  write_all(current_file(fixture.root()),
            "DCR-CURRENT 1\nfile=gen-00000000000000000009.dcrs\ngeneration=9\n"
            "digest=" + std::string(64, 'a') + "\n");
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  // CURRENT with a digest that does not match the file it names is refused.
  const std::filesystem::path file = newest_generation_file(fixture.root());
  write_all(current_file(fixture.root()),
            "DCR-CURRENT 1\nfile=" + file.filename().string() +
                "\ngeneration=2\ndigest=" + std::string(64, 'b') + "\n");
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
}

DCR_TEST(corruption, a_missing_current_file_is_refused_under_strict_recovery) {
  dcrtest::Fixture fixture("corrupt-no-current");
  seed_store(fixture);
  std::error_code code;
  std::filesystem::remove(current_file(fixture.root()), code);
  DCR_CHECK(!code);

  // Strict recovery refuses to guess which generation an operator meant.
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  // Last-known-good recovery adopts the newest verified generation, and says
  // so.
  dcr::OpenOptions options = fixture.open_options(StoreOpenMode::open_existing);
  options.store.recovery = RecoveryPolicy::last_known_good;
  auto opened = Registry::open(options);
  DCR_REQUIRE(opened.has_value());
  DCR_CHECK(opened.value().report.recovered);
  DCR_CHECK(opened.value().report.disposition == RecoveryDisposition::recovered_last_known_good);
  DCR_CHECK_EQ(std::uint64_t{2}, opened.value().registry->generation().value());
  DCR_CHECK_EQ(std::uint64_t{2}, opened.value().registry->record_count());
  DCR_CHECK(!opened.value().report.detail.empty());

  // The adoption repaired CURRENT, so the next strict open is clean.
  DCR_CHECK(opened.value().registry->close().has_value());
  auto strict = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
  DCR_REQUIRE(strict.has_value());
  DCR_CHECK(!strict.value().report.recovered);
  DCR_CHECK_EQ(std::uint64_t{2}, strict.value().registry->generation().value());
}

DCR_TEST(corruption, last_known_good_reports_what_it_discarded) {
  dcrtest::Fixture fixture("corrupt-lkg");
  seed_store(fixture);
  const std::filesystem::path file = newest_generation_file(fixture.root());
  std::string bytes = read_all(file);
  bytes[bytes.size() - 3] = static_cast<char>(bytes[bytes.size() - 3] ^ 0xFF);
  write_all(file, bytes);

  dcr::OpenOptions options = fixture.open_options(StoreOpenMode::open_existing);
  options.store.recovery = RecoveryPolicy::last_known_good;
  auto opened = Registry::open(options);
  DCR_REQUIRE(opened.has_value());
  DCR_CHECK(opened.value().report.recovered);
  DCR_CHECK(opened.value().report.discarded_generation.has_value());
  DCR_CHECK_EQ(std::uint64_t{2}, opened.value().report.discarded_generation->value());
  DCR_CHECK_EQ(std::uint64_t{1}, opened.value().registry->generation().value());
  DCR_CHECK_EQ(std::uint64_t{1}, opened.value().registry->record_count());

  // The damaged file is still on disk and is still reported as unusable, so
  // the decision is visible rather than hidden.
  DCR_REQUIRE_OK(const StoreInspection inspection,
                 Registry::inspect(fixture.store_options(StoreOpenMode::open_existing)));
  DCR_CHECK(!inspection.consistent);
  DCR_CHECK(inspection.newest_valid_generation.has_value());
  DCR_CHECK_EQ(std::uint64_t{1}, inspection.newest_valid_generation->value());
}

DCR_TEST(corruption, an_uncommitted_generation_is_never_adopted) {
  dcrtest::Fixture fixture("corrupt-uncommitted");
  seed_store(fixture);
  const std::filesystem::path published = newest_generation_file(fixture.root());

  // Simulate a crash between publishing the generation file and replacing
  // CURRENT: the file exists, CURRENT does not name it.
  const std::filesystem::path orphan = generations_dir(fixture.root()) /
                                       "gen-00000000000000000007.dcrs";
  std::filesystem::copy_file(published, orphan);
  const std::string orphan_bytes = read_all(orphan);
  DCR_CHECK(!orphan_bytes.empty());

  auto opened = fixture.open(StoreOpenMode::open_existing);
  Registry& registry = *opened.registry;
  // The committed generation is authoritative; the orphan never was.
  DCR_CHECK_EQ(std::uint64_t{2}, registry.generation().value());
  DCR_CHECK_EQ(std::uint64_t{2}, registry.record_count());
  DCR_CHECK(opened.report.quarantined_generations.empty() == false);
  DCR_CHECK_EQ(std::uint64_t{7}, opened.report.quarantined_generations.front().value());

  // It was moved aside, not deleted and not adopted.
  DCR_CHECK(!std::filesystem::exists(orphan));
  DCR_CHECK(std::filesystem::exists(fixture.root() / "uncommitted" /
                                    "gen-00000000000000000007.dcrs"));

  // And the registry can keep writing: the generation number is reusable
  // because the file that claimed it has been set aside.
  auto command = fixture.register_command("dc-fac-9", "Facility 9", LifecycleState::registered,
                                          "site-east");
  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(command));
  DCR_CHECK(created.status == MutationStatus::created);
  DCR_CHECK_EQ(std::uint64_t{3}, registry.generation().value());
}

DCR_TEST(corruption, a_newer_uncommitted_generation_is_set_aside) {
  dcrtest::Fixture fixture("corrupt-collision");
  seed_store(fixture);

  // A valid generation file that is newer than CURRENT is never committed
  // state, so it is moved aside rather than adopted or deleted, and the
  // generation number it claimed becomes usable again.
  const std::filesystem::path source = generations_dir(fixture.root()) /
                                       "gen-00000000000000000001.dcrs";
  const std::filesystem::path collision = generations_dir(fixture.root()) /
                                          "gen-00000000000000000003.dcrs";
  std::string bytes = read_all(source);
  DCR_REQUIRE_OK(internal::RegistryState state, internal::decode_container(bytes, fixture.limits()));
  state.generation = *RegistryGeneration::from_value(3);
  DCR_REQUIRE_OK(const std::string encoded, internal::encode_container(state));
  write_all(collision, encoded);

  auto opened = fixture.open(StoreOpenMode::open_existing);
  Registry& registry = *opened.registry;
  DCR_CHECK_EQ(std::uint64_t{2}, registry.generation().value());
  DCR_CHECK(!opened.report.quarantined_generations.empty());
  DCR_CHECK_EQ(std::uint64_t{3}, opened.report.quarantined_generations.front().value());
  DCR_CHECK(!std::filesystem::exists(collision));

  auto command = fixture.register_command("dc-fac-8", "Facility 8", LifecycleState::registered,
                                          "site-east");
  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(command));
  DCR_CHECK_EQ(std::uint64_t{3}, registry.generation().value());
}

DCR_TEST(corruption, a_generation_file_that_appears_after_open_blocks_publication) {
  // The quarantine step handles files that exist when the store is opened. A
  // file that appears afterwards is a rogue writer, and the publication path
  // refuses to write over it rather than publishing two states under one
  // generation number.
  dcrtest::Fixture fixture("corrupt-race");
  const StoreOptions options = fixture.store_options(StoreOpenMode::open_or_create);
  auto opened_result = internal::SnapshotStore::open(options);
  DCR_REQUIRE(opened_result.has_value());
  internal::StoreOpenResult opened = std::move(opened_result).value();
  internal::SnapshotStore store = std::move(opened.store);
  internal::RegistryState state = std::move(opened.state);

  // Commit generation 1 normally.
  internal::RegistryState next = state;
  DCR_REQUIRE_OK(const RegistryGeneration one, state.generation.successor());
  next.generation = one;
  DCR_REQUIRE_OK(const Sha256Digest unused, store.publish(next, state.generation));
  state = std::move(next);

  // Now drop an unrelated, internally valid generation 2 file in place.
  DCR_REQUIRE_OK(const std::string other_bytes, internal::encode_container([&] {
                   internal::RegistryState other = state;
                   other.generation = *RegistryGeneration::from_value(2);
                   for (auto& record : other.records) {
                     record.created_generation = other.generation;
                   }
                   // Different content from anything this writer would
                   // produce, so the collision is a real disagreement rather
                   // than an idempotent replay of the same state.
                   other.last_sequence = *SequenceNumber::from_value(41);
                   return other;
                 }()));
  const std::filesystem::path target = generations_dir(fixture.root()) /
                                       "gen-00000000000000000002.dcrs";
  write_all(target, other_bytes);

  internal::RegistryState mine = state;
  mine.generation = *RegistryGeneration::from_value(2);
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt, store.publish(mine, state.generation));
}

// ---------------------------------------------------------------------------
// Rejected payloads below the container
// ---------------------------------------------------------------------------

DCR_TEST(corruption, decoded_payloads_are_validated_independently_of_the_digest) {
  // The container digests only prove the bytes are the bytes that were written.
  // Everything below re-checks the decoded content, which is what makes a
  // deliberately crafted store no more trusted than a corrupt one.
  const RegistryLimits limits = RegistryLimits::defaults();

  // An empty payload is truncated.
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt, internal::decode_state("", limits));

  // A payload that declares a record count larger than the bytes that remain.
  std::string payload;
  payload.push_back(static_cast<char>(1));  // format major low byte
  payload.push_back(static_cast<char>(0));
  payload.push_back(static_cast<char>(0));  // format minor
  payload.push_back(static_cast<char>(0));
  for (int index = 0; index < 8; ++index) {
    payload.push_back(static_cast<char>(0xFF));  // generation varint, unterminated
  }
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt, internal::decode_state(payload, limits));

  // A payload whose format major is from the future is a version problem, not
  // a corruption problem, and is reported as such.
  std::string future;
  future.push_back(static_cast<char>(9));
  future.push_back(static_cast<char>(0));
  future.push_back(static_cast<char>(0));
  future.push_back(static_cast<char>(0));
  DCR_REQUIRE_ERROR(ErrorCode::store_incompatible_version, internal::decode_state(future, limits));

  // A payload with trailing bytes after the last field is refused: it is not
  // the canonical encoding of any state.
  DCR_REQUIRE_OK(const std::string canonical,
                 internal::encode_state(internal::RegistryState::empty(limits)));
  DCR_REQUIRE_OK(const internal::RegistryState parsed, internal::decode_state(canonical, limits));
  DCR_CHECK_EQ(std::uint64_t{0}, parsed.generation.value());
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    internal::decode_state(canonical + "trailing", limits));
}

DCR_TEST(corruption, decoding_enforces_the_configured_limits) {
  RegistryLimits small = RegistryLimits::defaults();
  small.max_display_name_bytes = 8;
  RegistryLimits large = RegistryLimits::defaults();

  dcrtest::Fixture fixture("corrupt-limits");
  {
    auto opened = fixture.open();
    auto command = fixture.register_command("dc-fac-1", "A Very Long Facility Name",
                                            LifecycleState::registered, "site-east");
    DCR_REQUIRE_OK(const MutationResult created,
                   opened.registry->register_data_center(command));
    DCR_CHECK(created.status == MutationStatus::created);
    DCR_CHECK(opened.registry->close().has_value());
  }

  const std::filesystem::path file = newest_generation_file(fixture.root());
  const std::string bytes = read_all(file);
  // Under the limits it was written with, it decodes.
  DCR_CHECK(internal::decode_container(bytes, large).has_value());
  // Under tighter limits it is refused rather than truncated or normalised.

  DCR_REQUIRE_ERROR(ErrorCode::store_limit_exceeded,
                    internal::decode_container(bytes, small));
}

DCR_TEST(corruption, a_store_written_with_a_future_format_is_refused) {
  dcrtest::Fixture fixture("corrupt-future");
  seed_store(fixture);
  const std::filesystem::path file = newest_generation_file(fixture.root());
  std::string bytes = read_all(file);

  // Claim a newer minor version inside the container header.
  bytes[10] = static_cast<char>(kSnapshotFormatMinor + 1);
  write_all(file, bytes);
  DCR_REQUIRE_ERROR(ErrorCode::store_incompatible_version,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
}

// ---------------------------------------------------------------------------
// Staging residue
// ---------------------------------------------------------------------------

DCR_TEST(corruption, staging_residue_is_cleared_and_never_authoritative) {
  dcrtest::Fixture fixture("corrupt-staging");
  seed_store(fixture);
  const std::filesystem::path staging = fixture.root() / "tmp";
  std::filesystem::create_directories(staging);
  write_all(staging / "snapshot.staging-9999-0-0", "partial write that was interrupted");
  write_all(staging / "CURRENT.staging-9999-0-1", "DCR-CURRENT 1\nfile=garbage\n");

  auto opened = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{2}, opened.registry->generation().value());
  DCR_CHECK_EQ(std::uint64_t{2}, opened.registry->record_count());

  std::vector<std::string> remaining;
  std::error_code code;
  for (const auto& entry : std::filesystem::directory_iterator(staging, code)) {
    remaining.push_back(entry.path().filename().string());
  }
  DCR_CHECK(remaining.empty());
}

DCR_TEST(corruption, foreign_files_in_the_generations_directory_are_ignored) {
  dcrtest::Fixture fixture("corrupt-foreign");
  seed_store(fixture);
  const std::filesystem::path directory = generations_dir(fixture.root());
  write_all(directory / "notes.txt", "operator notes");
  write_all(directory / "gen-0000000000000000000.dcrs", "too few digits");
  write_all(directory / "gen-0000000000000000000a.dcrs", "not a number");
  write_all(directory / "gen-99999999999999999999.dcrs", "beyond the counter");

  auto opened = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{2}, opened.registry->generation().value());
  DCR_CHECK_EQ(std::uint64_t{2}, opened.registry->record_count());
  DCR_CHECK(opened.report.unusable_generations.empty());
}

// ---------------------------------------------------------------------------
// Path handling
// ---------------------------------------------------------------------------

DCR_TEST(corruption, a_store_root_that_is_not_a_directory_is_refused) {
  dcrtest::Fixture fixture("corrupt-root");
  const std::filesystem::path file = fixture.root() / "not-a-directory";
  write_all(file, "a file where a store root should be");

  dcr::OpenOptions options = fixture.open_options(StoreOpenMode::open_or_create);
  options.store.root = file;
  DCR_REQUIRE_ERROR(ErrorCode::store_io_error, Registry::open(options));

  dcr::OpenOptions empty_root = fixture.open_options(StoreOpenMode::open_or_create);
  empty_root.store.root = std::filesystem::path();
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Registry::open(empty_root));
}

DCR_TEST(corruption, a_symlinked_generation_file_is_never_followed) {
  dcrtest::Fixture fixture("corrupt-symlink");
  seed_store(fixture);
  const std::filesystem::path file = newest_generation_file(fixture.root());
  const std::filesystem::path target = fixture.root() / "real-generation.dcrs";
  std::filesystem::copy_file(file, target);
  std::error_code code;
  std::filesystem::remove(file, code);
  std::filesystem::create_symlink(target, file, code);
  if (code) {
    // Creating a symlink needs a privilege this process may not hold on
    // Windows. The check below is then not claimed to have run.
    std::printf("     note: symlink creation is not permitted here; "
                "the symlink rejection path was not exercised\n");
    return;
  }
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
}
