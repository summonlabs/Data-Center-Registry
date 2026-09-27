// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Adversarial input.
//
// The tests here do not use the library the way a cooperative caller does.
// They hand it stores that lie, payloads whose digests have been recomputed
// after tampering, paths that try to escape the store, and values at and beyond
// every declared bound. Each case asserts the same property in a different
// way: nothing invalid becomes authoritative.
//
// One thing this suite deliberately does not claim: SHA-256 integrity does not
// defend against someone who can rewrite a store *and* recompute its digests.
// What defends against that is the semantic validation every payload passes
// through on load, which is what the recomputed-digest cases below exercise.

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

void seed(dcrtest::Fixture& fixture, std::uint64_t count) {
  auto opened = fixture.open();
  for (std::uint64_t index = 0; index < count; ++index) {
    RegisterCommand command;
    command.context.provenance.principal = *PrincipalId::parse("seed");
    command.id = *DataCenterId::parse(dcrtest::data_center_id_text(index));
    command.draft.display_name = "Facility " + std::to_string(index);
    command.draft.ownership = OwnershipScope::unassigned();
    command.draft.compatibility = *CompatibilityKey::make(1, 0, 0);
    command.draft.memberships = {
        SiteMembership{*SiteId::parse("site-east"), MembershipRole::primary}};
    // A distinct alias per record so that alias bytes can be swapped later.
    command.draft.aliases = {*Alias::parse("seed-alias-" + std::to_string(index))};
    command.initial_state = LifecycleState::registered;
    auto created = opened.registry->register_data_center(command);
    if (!created.has_value()) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              "seed failed: " + created.error().to_string());
      return;
    }
  }
  const auto closed = opened.registry->close();
  if (!closed.has_value()) {
    dcrtest::record_failure(__FILE__, __LINE__, "seed could not close");
  }
}

/// Recomputes the container header and payload digests after a payload edit, so
/// that the file's own integrity check passes and only the semantic validation
/// stands between the edit and authoritative state.
std::string with_recomputed_digests(std::string container) {
  constexpr std::size_t kHeaderSize = 88;
  const std::string_view payload(container.data() + kHeaderSize, container.size() - kHeaderSize);
  const Sha256Digest payload_digest = sha256_text(payload);
  std::copy(payload_digest.bytes().begin(), payload_digest.bytes().end(), container.begin() + 24);
  const Sha256Digest header_digest = sha256_bytes(container.data(), 56);
  std::copy(header_digest.bytes().begin(), header_digest.bytes().end(), container.begin() + 56);
  return container;
}

/// Rewrites the digest CURRENT records so that the pointer agrees with an
/// edited generation file. Without this an edit is caught by the pointer digest
/// before any semantic validation runs; with it, the semantic layer is the only
/// thing left between the edit and authoritative state.
void align_current_digest(const std::filesystem::path& root, const std::string& container) {
  constexpr std::size_t kHeaderSize = 88;
  const std::string_view payload(container.data() + kHeaderSize, container.size() - kHeaderSize);
  const std::string digest = sha256_text(payload).to_hex();
  std::string current = read_all(root / "CURRENT");
  const std::size_t marker = current.find("digest=");
  if (marker == std::string::npos) {
    dcrtest::record_failure(__FILE__, __LINE__, "CURRENT has no digest line");
    return;
  }
  const std::size_t line_end = current.find('\n', marker);
  current.replace(marker + 7, line_end - (marker + 7), digest);
  write_all(root / "CURRENT", current);
}

}  // namespace

// ---------------------------------------------------------------------------
// Paths and layout
// ---------------------------------------------------------------------------

DCR_TEST(adversarial, current_cannot_point_outside_the_store) {
  dcrtest::Fixture fixture("adv-traversal");
  seed(fixture, 1);
  const std::string original = read_all(fixture.root() / "CURRENT");

  // A CURRENT file that names a path rather than a generation file is refused,
  // so a store cannot be redirected at a file outside its own directory.
  const std::vector<std::string> hostile = {
      "DCR-CURRENT 1\nfile=../../escape.dcrs\ngeneration=1\ndigest=" + std::string(64, 'a') + "\n",
      "DCR-CURRENT 1\nfile=..\\..\\escape.dcrs\ngeneration=1\ndigest=" + std::string(64, 'a') +
          "\n",
      "DCR-CURRENT 1\nfile=/etc/passwd\ngeneration=1\ndigest=" + std::string(64, 'a') + "\n",
      "DCR-CURRENT 1\nfile=C:\\Windows\\win.ini\ngeneration=1\ndigest=" + std::string(64, 'a') +
          "\n",
      "DCR-CURRENT 1\nfile=gen-00000000000000000001.dcrs/../../x\ngeneration=1\ndigest=" +
          std::string(64, 'a') + "\n",
  };
  for (const auto& text : hostile) {
    write_all(fixture.root() / "CURRENT", text);
    DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                      Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
  }

  // Restoring the pointer makes the store usable again.
  write_all(fixture.root() / "CURRENT", original);
  auto restored = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
  DCR_CHECK(restored.has_value());
}

DCR_TEST(adversarial, a_store_with_too_many_generation_files_is_refused) {
  // Generation retention is what keeps the directory bounded. A store that has
  // accumulated hundreds of generation files is not examined until it is
  // resolved, because the alternative is unbounded work on open.
  dcrtest::Fixture fixture("adv-many-files");
  seed(fixture, 1);
  const std::filesystem::path directory = generations_dir(fixture.root());
  const std::string existing = read_all(directory / "gen-00000000000000000001.dcrs");
  for (int index = 0; index < 300; ++index) {
    char name[64];
    std::snprintf(name, sizeof(name), "gen-%020d.dcrs", 1000 + index);
    write_all(directory / name, existing);
  }
  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
}

DCR_TEST(adversarial, a_directory_that_is_not_a_store_is_left_untouched) {
  dcrtest::Fixture fixture("adv-unrelated");
  const std::filesystem::path root = fixture.root() / "unrelated";
  std::filesystem::create_directories(root);
  write_all(root / "operator-notes.txt", "this directory belongs to something else");

  // Neither a writable nor a read-only open may create a registry inside a
  // directory that only looks like a plausible location.
  DCR_REQUIRE_ERROR(ErrorCode::store_not_found,
                    Registry::open([&] {
                      OpenOptions options = fixture.open_options(StoreOpenMode::open_existing);
                      options.store.root = root;
                      return options;
                    }()));
  DCR_REQUIRE_ERROR(ErrorCode::store_not_found,
                    Registry::open([&] {
                      OpenOptions options = fixture.open_options(StoreOpenMode::read_only);
                      options.store.root = root;
                      return options;
                    }()));
  // create_new is an explicit request to make a store at this path, so it is
  // allowed in a directory that holds other things -- it adds the store layout
  // and leaves everything else alone. What is refused is adopting a layout
  // that is already there, which is checked separately.
  auto created = Registry::open([&] {
    OpenOptions options = fixture.open_options(StoreOpenMode::create_new);
    options.store.root = root;
    return options;
  }());
  DCR_REQUIRE(created.has_value());
  DCR_CHECK(created.value().report.created);
  DCR_CHECK(created.value().registry->close().has_value());

  std::vector<std::string> entries;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    entries.push_back(entry.path().filename().string());
  }
  std::sort(entries.begin(), entries.end());
  DCR_CHECK_EQ(std::size_t{4}, entries.size());
  DCR_CHECK(std::find(entries.begin(), entries.end(), "operator-notes.txt") != entries.end());
  // The unrelated file is untouched, byte for byte.
  DCR_CHECK_EQ(std::string("this directory belongs to something else"),
               read_all(root / "operator-notes.txt"));

  // And create_new refuses to adopt the store it just created.
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument,
                    Registry::open([&] {
                      OpenOptions options = fixture.open_options(StoreOpenMode::create_new);
                      options.store.root = root;
                      return options;
                    }()));
}

// ---------------------------------------------------------------------------
// Recomputed digests
// ---------------------------------------------------------------------------

DCR_TEST(adversarial, an_edited_payload_that_current_does_not_vouch_for_is_refused) {
  // The first line of defence after the container's own digests: CURRENT
  // records the payload digest of the generation it names, so an edit that does
  // not also repair the pointer is caught without decoding anything.
  dcrtest::Fixture fixture("adv-pointer-digest");
  seed(fixture, 1);
  const std::filesystem::path file = generations_dir(fixture.root()) /
                                     "gen-00000000000000000001.dcrs";
  std::string container = read_all(file);
  const std::string name = "Facility 0";
  const std::size_t offset = container.find(name);
  DCR_REQUIRE(offset != std::string::npos);
  container.replace(offset, name.size(), "Facility 9");
  container = with_recomputed_digests(std::move(container));
  write_all(file, container);

  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
  DCR_REQUIRE_OK(const StoreInspection inspection,
                 Registry::inspect(fixture.store_options(StoreOpenMode::open_existing)));
  DCR_CHECK(!inspection.current_valid);
  DCR_CHECK(!inspection.consistent);
}

DCR_TEST(adversarial, a_duplicate_alias_with_valid_digests_is_still_refused) {
  // The container verifies: its digests were recomputed after the edit. What
  // refuses it is the set-level invariant that an alias has exactly one owner.
  dcrtest::Fixture fixture("adv-duplicate-alias");
  seed(fixture, 2);
  const std::filesystem::path file = generations_dir(fixture.root()) /
                                     "gen-00000000000000000002.dcrs";
  std::string container = read_all(file);

  const std::string victim = "seed-alias-1";
  const std::string replacement = "seed-alias-0";
  const std::size_t offset = container.find(victim);
  DCR_REQUIRE(offset != std::string::npos);
  // Same length, so the rest of the payload keeps its shape.
  DCR_CHECK_EQ(victim.size(), replacement.size());
  container.replace(offset, victim.size(), replacement);
  container = with_recomputed_digests(std::move(container));
  write_all(file, container);
  // The pointer digest is repaired too, so the container and the pointer agree
  // and only the set-level invariant stands between the edit and the state.
  align_current_digest(fixture.root(), container);

  DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));

  // The inspection tool reports the same conclusion without opening.
  DCR_REQUIRE_OK(const StoreInspection inspection,
                 Registry::inspect(fixture.store_options(StoreOpenMode::open_existing)));
  DCR_CHECK(!inspection.current_valid);
  DCR_CHECK(!inspection.consistent);
}

DCR_TEST(adversarial, an_out_of_domain_or_inconsistent_enum_is_refused) {
  // The lifecycle state is one byte inside the payload. This test finds that
  // byte by encoding the same state twice with one field changed, which is a
  // reliable way to locate it without hard-coding a payload offset.
  dcrtest::Fixture fixture("adv-enum");
  seed(fixture, 1);
  const std::filesystem::path file = generations_dir(fixture.root()) /
                                     "gen-00000000000000000001.dcrs";
  const std::string original = read_all(file);
  const std::string original_current = read_all(fixture.root() / "CURRENT");
  constexpr std::size_t kHeaderSize = 88;

  const std::string_view payload(original.data() + kHeaderSize, original.size() - kHeaderSize);
  DCR_REQUIRE_OK(internal::RegistryState state, internal::decode_state(payload, fixture.limits()));
  DCR_REQUIRE(!state.records.empty());
  DCR_CHECK(state.records[0].state == LifecycleState::registered);

  internal::RegistryState other = state;
  other.records[0].state = LifecycleState::degraded;
  DCR_REQUIRE_OK(const std::string encoded_registered, internal::encode_state(state));
  DCR_REQUIRE_OK(const std::string encoded_degraded, internal::encode_state(other));
  DCR_CHECK_EQ(payload.size(), encoded_registered.size());
  DCR_CHECK_EQ(encoded_registered.size(), encoded_degraded.size());
  DCR_CHECK(payload == std::string_view(encoded_registered));

  std::size_t state_offset = std::string::npos;
  for (std::size_t index = 0; index < encoded_registered.size(); ++index) {
    if (encoded_registered[index] != encoded_degraded[index]) {
      DCR_CHECK(state_offset == std::string::npos);
      state_offset = index;
    }
  }
  DCR_REQUIRE(state_offset != std::string::npos);
  DCR_CHECK_EQ(static_cast<char>(static_cast<std::uint8_t>(LifecycleState::registered)),
               encoded_registered[state_offset]);
  DCR_CHECK_EQ(static_cast<char>(static_cast<std::uint8_t>(LifecycleState::degraded)),
               encoded_degraded[state_offset]);

  // Every value outside the declared domain is refused, even though the
  // container's digests are recomputed and therefore verify.
  std::size_t refused = 0;
  for (std::uint32_t value = 8; value <= 255; ++value) {
    std::string container = original;
    container[kHeaderSize + state_offset] = static_cast<char>(static_cast<std::uint8_t>(value));
    container = with_recomputed_digests(std::move(container));
    write_all(file, container);
    align_current_digest(fixture.root(), container);
    auto opened = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
    if (!opened.has_value()) {
      ++refused;
      DCR_CHECK(opened.error().code() == ErrorCode::store_corrupt ||
                opened.error().code() == ErrorCode::store_incompatible_version);
    } else {
      DCR_CHECK(opened.value().registry->close().has_value());
    }
  }
  DCR_CHECK_EQ(std::size_t{248}, refused);

  // A value inside the domain that the rest of the record contradicts is
  // refused too: `retired` without retirement detail is not a state, it is an
  // inconsistency.
  {
    std::string container = original;
    container[kHeaderSize + state_offset] =
        static_cast<char>(static_cast<std::uint8_t>(LifecycleState::retired));
    container = with_recomputed_digests(std::move(container));
    write_all(file, container);
    align_current_digest(fixture.root(), container);
    DCR_REQUIRE_ERROR(ErrorCode::store_corrupt,
                      Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
  }

  // A value inside the domain that the record does support is accepted, which
  // shows the refusals above came from the domain and the invariants rather
  // than from the edit itself.
  {
    std::string container = original;
    container[kHeaderSize + state_offset] =
        static_cast<char>(static_cast<std::uint8_t>(LifecycleState::degraded));
    container = with_recomputed_digests(std::move(container));
    write_all(file, container);
    align_current_digest(fixture.root(), container);
    auto opened = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
    if (!opened.has_value()) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              "the degraded variant was refused: " +
                                  opened.error().to_string());
    } else {
      DCR_REQUIRE_OK(const DataCenterRecord record,
                     opened.value().registry->get(*DataCenterId::parse("dc-fac-0")));
      DCR_CHECK(record.state == LifecycleState::degraded);
    }
  }

  write_all(file, original);
  write_all(fixture.root() / "CURRENT", original_current);
  auto restored = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
  DCR_CHECK(restored.has_value());
}

DCR_TEST(adversarial, an_edited_but_valid_payload_is_accepted_and_that_is_documented) {
  // This is the honest boundary: a party who can rewrite a store and recompute
  // its digests can change authoritative state. Integrity checking protects
  // against corruption and accidental damage. What such a party cannot do is
  // make the registry accept a state that violates an invariant, which the two
  // tests above show.
  dcrtest::Fixture fixture("adv-edit");
  seed(fixture, 1);
  const std::filesystem::path file = generations_dir(fixture.root()) /
                                     "gen-00000000000000000001.dcrs";
  std::string container = read_all(file);
  const std::string name = "Facility 0";
  const std::size_t offset = container.find(name);
  DCR_REQUIRE(offset != std::string::npos);
  container.replace(offset, name.size(), "Facility 9");
  container = with_recomputed_digests(std::move(container));
  write_all(file, container);
  align_current_digest(fixture.root(), container);

  auto opened = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
  if (!opened.has_value()) {
    dcrtest::record_failure(__FILE__, __LINE__,
                            "the edited payload was refused: " + opened.error().to_string());
  } else {
    DCR_REQUIRE_OK(const DataCenterRecord record,
                   opened.value().registry->get(*DataCenterId::parse("dc-fac-0")));
    DCR_CHECK_EQ(std::string("Facility 9"), record.display_name);
  }
}

// ---------------------------------------------------------------------------
// Record-level invariants, exercised directly
// ---------------------------------------------------------------------------

namespace {

/// Builds a real, fully stamped record by registering one and reading it back,
/// so that the invariant tests below vary exactly the field they mean to vary.
std::optional<DataCenterRecord> make_record(std::string_view id_text, std::string_view name) {
  dcrtest::Fixture fixture("adv-record-builder");
  auto opened = fixture.open();
  auto command = fixture.register_command(id_text, name, LifecycleState::registered, "site-east");
  auto created = opened.registry->register_data_center(command);
  if (!created.has_value()) {
    dcrtest::record_failure(__FILE__, __LINE__,
                            "record builder could not register: " +
                                created.error().to_string());
    return std::nullopt;
  }
  auto record = opened.registry->get(fixture.id(id_text));
  if (!record.has_value()) {
    dcrtest::record_failure(__FILE__, __LINE__, "record builder could not read back");
    return std::nullopt;
  }
  return record.value();
}

}  // namespace

DCR_TEST(adversarial, the_record_invariant_gate_refuses_inconsistent_records) {
  const RegistryLimits limits = RegistryLimits::defaults();
  auto built = make_record("dc-adv", "Adversarial Facility");
  DCR_REQUIRE(built.has_value());
  const DataCenterRecord record = built.value();
  DCR_CHECK_OK(validate_record(record, limits));

  // A live record with no primary site.
  DataCenterRecord no_site = record;
  no_site.memberships.clear();
  DCR_REQUIRE_ERROR(ErrorCode::membership_violation, validate_record(no_site, limits));

  // Two primaries.
  DataCenterRecord two_primaries = record;
  two_primaries.memberships.push_back(
      SiteMembership{*SiteId::parse("site-west"), MembershipRole::primary});
  DCR_REQUIRE_ERROR(ErrorCode::membership_violation, validate_record(two_primaries, limits));

  // Memberships out of canonical order.
  DataCenterRecord unsorted = record;
  unsorted.memberships.insert(unsorted.memberships.begin(),
                              SiteMembership{*SiteId::parse("site-west"),
                                             MembershipRole::secondary});
  DCR_REQUIRE_ERROR(ErrorCode::internal_error, validate_record(unsorted, limits));

  // An empty display name.
  DataCenterRecord unnamed = record;
  unnamed.display_name.clear();
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, validate_record(unnamed, limits));

  // Text that is not valid UTF-8.
  DataCenterRecord broken_utf8 = record;
  broken_utf8.display_name = std::string("bad\xC3\x28name");
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, validate_record(broken_utf8, limits));

  // A name containing a control character.
  DataCenterRecord control = record;
  control.display_name = std::string("bad\x1bname");
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, validate_record(control, limits));

  // Terminal state without retirement detail, and retirement without a
  // terminal state.
  DataCenterRecord terminal_only = record;
  terminal_only.state = LifecycleState::retired;
  DCR_REQUIRE_ERROR(ErrorCode::internal_error, validate_record(terminal_only, limits));

  DataCenterRecord retirement_only = record;
  retirement_only.retirement =
      RetirementInfo{*ReasonCode::parse("why"), *RegistryGeneration::from_value(1), std::nullopt};
  retirement_only.retired_generation = *RegistryGeneration::from_value(1);
  DCR_REQUIRE_ERROR(ErrorCode::internal_error, validate_record(retirement_only, limits));

  // A revision cannot be below one at all: the counter type refuses to hold
  // such a value, so there is no record-level case to construct here.
  // Validation still checks the invariant, because a payload could claim it.
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, MetadataRevision::from_value(0));

  // More aliases than the configured bound allows.
  RegistryLimits tight = RegistryLimits::defaults();
  tight.max_aliases_per_record = 1;
  DataCenterRecord many_aliases = record;
  many_aliases.aliases = {*Alias::parse("alias-one"), *Alias::parse("alias-two")};
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, validate_record(many_aliases, tight));

  // Aliases out of canonical order.
  DataCenterRecord unsorted_aliases = record;
  unsorted_aliases.aliases = {*Alias::parse("alias-two"), *Alias::parse("alias-one")};
  DCR_REQUIRE_ERROR(ErrorCode::internal_error, validate_record(unsorted_aliases, limits));

  // Prose that is far longer than the configured bound.
  RegistryLimits small_names = RegistryLimits::defaults();
  small_names.max_display_name_bytes = 4;
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, validate_record(record, small_names));
}

DCR_TEST(adversarial, the_record_set_gate_refuses_broken_links) {
  const RegistryLimits limits = RegistryLimits::defaults();
  auto first_built = make_record("dc-adv-a", "A");
  auto second_built = make_record("dc-adv-b", "B");
  DCR_REQUIRE(first_built.has_value());
  DCR_REQUIRE(second_built.has_value());
  DataCenterRecord first = first_built.value();
  DataCenterRecord second = second_built.value();

  DCR_CHECK_OK(validate_record_set({first, second}, limits));

  // Records out of canonical order.
  DCR_REQUIRE_ERROR(ErrorCode::internal_error, validate_record_set({second, first}, limits));

  // The same identity twice.
  DCR_REQUIRE_ERROR(ErrorCode::internal_error, validate_record_set({first, first}, limits));

  // A replacement link with no predecessor.
  DataCenterRecord orphan = second;
  orphan.replaces = *DataCenterId::parse("dc-absent");
  DCR_REQUIRE_ERROR(ErrorCode::internal_error, validate_record_set({first, orphan}, limits));

  // A one-sided replacement: the successor claims a predecessor that does not
  // acknowledge it.
  DataCenterRecord successor = second;
  successor.replaces = first.id;
  DCR_REQUIRE_ERROR(ErrorCode::internal_error, validate_record_set({first, successor}, limits));

  // The same alias claimed twice.
  DataCenterRecord shared = second;
  shared.aliases = {*Alias::parse("alias-shared")};
  DataCenterRecord claimant = first;
  claimant.aliases = {*Alias::parse("alias-shared")};
  DCR_REQUIRE_ERROR(ErrorCode::alias_conflict, validate_record_set({claimant, shared}, limits));
}

// ---------------------------------------------------------------------------
// Bounds at the edges
// ---------------------------------------------------------------------------

DCR_TEST(adversarial, text_fields_at_and_beyond_their_bounds) {
  dcrtest::Fixture fixture("adv-bounds");
  RegistryLimits limits = RegistryLimits::defaults();
  limits.max_display_name_bytes = 16;
  fixture.set_limits(limits);
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  // Exactly at the bound.
  auto at_bound = fixture.register_command("dc-exact", std::string(16, 'a'),
                                           LifecycleState::registered, "site-east");
  DCR_REQUIRE_OK(const MutationResult accepted, registry.register_data_center(at_bound));
  DCR_CHECK(accepted.status == MutationStatus::created);

  // One over.
  auto over_bound = fixture.register_command("dc-over", std::string(17, 'a'),
                                             LifecycleState::registered, "site-east");
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, registry.register_data_center(over_bound));

  // Whitespace only: not empty, so it is accepted, and that is deliberate.
  // A registry stores the name it is given; deciding that "   " means "no
  // name" would be a silent reinterpretation.
  auto whitespace = fixture.register_command("dc-space", "   ", LifecycleState::registered,
                                             "site-east");
  DCR_REQUIRE_OK(const MutationResult spaced, registry.register_data_center(whitespace));
  DCR_CHECK(spaced.status == MutationStatus::created);

  // A name that is valid UTF-8 beyond ASCII.
  auto unicode = fixture.register_command("dc-uni", "Ünïcödé", LifecycleState::registered,
                                          "site-east");
  DCR_REQUIRE_OK(const MutationResult named, registry.register_data_center(unicode));
  DCR_CHECK(named.status == MutationStatus::created);

  // An overlong UTF-8 encoding of '/' must be refused rather than normalised.
  auto overlong = fixture.register_command("dc-overlong", std::string("\xC0\xAF"), 
                                           LifecycleState::registered, "site-east");
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.register_data_center(overlong));

  // A surrogate code point encoded in UTF-8 must be refused.
  auto surrogate = fixture.register_command("dc-surrogate", std::string("\xED\xA0\x80"),
                                            LifecycleState::registered, "site-east");
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.register_data_center(surrogate));

  DCR_CHECK_EQ(std::uint64_t{3}, registry.generation().value());
}

DCR_TEST(adversarial, collection_bounds_are_exact) {
  dcrtest::Fixture fixture("adv-collections");
  RegistryLimits limits = RegistryLimits::defaults();
  limits.max_aliases_per_record = 3;
  fixture.set_limits(limits);
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  auto command = fixture.register_command("dc-exact", "Exact", LifecycleState::registered,
                                          "site-east");
  command.draft.aliases = {fixture.alias("alias-a"), fixture.alias("alias-b"),
                           fixture.alias("alias-c")};
  DCR_REQUIRE_OK(const MutationResult accepted, registry.register_data_center(command));
  DCR_CHECK(accepted.status == MutationStatus::created);

  auto over = fixture.register_command("dc-over", "Over", LifecycleState::registered, "site-east");
  over.draft.aliases = {fixture.alias("alias-d"), fixture.alias("alias-e"), fixture.alias("alias-f"),
                        fixture.alias("alias-g")};
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, registry.register_data_center(over));

  // Metadata at the entry bound and one over.
  RegistryLimits metadata_limits = RegistryLimits::defaults();
  metadata_limits.max_metadata_entries = 2;
  dcrtest::Fixture metadata_fixture("adv-metadata");
  metadata_fixture.set_limits(metadata_limits);
  auto metadata_opened = metadata_fixture.open();
  Registry& metadata_registry = *metadata_opened.registry;

  auto two_entries = metadata_fixture.register_command("dc-two", "Two", LifecycleState::registered,
                                                       "site-east");
  two_entries.draft.extensions = *MetadataMap::make(
      {{*MetadataKey::parse("one.key"), "1"}, {*MetadataKey::parse("two.key"), "2"}}, "extensions");
  DCR_REQUIRE_OK(const MutationResult two, metadata_registry.register_data_center(two_entries));
  DCR_CHECK(two.status == MutationStatus::created);

  auto three_entries = metadata_fixture.register_command("dc-three", "Three",
                                                         LifecycleState::registered, "site-east");
  three_entries.draft.extensions =
      *MetadataMap::make({{*MetadataKey::parse("one.key"), "1"},
                          {*MetadataKey::parse("two.key"), "2"},
                          {*MetadataKey::parse("three.key"), "3"}},
                         "extensions");
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded,
                    metadata_registry.register_data_center(three_entries));
}

DCR_TEST(adversarial, structurally_impossible_values_are_refused) {
  // Counters refuse to wrap rather than rolling over.
  DCR_REQUIRE_OK(const RegistryGeneration maximum, RegistryGeneration::from_value(UINT64_MAX));
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, maximum.successor());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, MetadataRevision::from_value(0));

  // Ranges that cannot be represented.
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Timestamp::from_unix_millis(INT64_MIN));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata, GeoCoordinates::make(INT32_MIN, 0));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata, CompatibilityKey::make(0, 0, 0));

  // A digest that is not a digest.
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Sha256Digest::from_hex(std::string(63, 'a')));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Sha256Digest::from_hex(std::string(65, 'a')));

  // Limits that could not bound anything.
  RegistryLimits impossible = RegistryLimits::defaults();
  impossible.max_records = 0;
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, impossible.validate());
}
