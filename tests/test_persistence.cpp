// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Durable store behaviour: publication, integrity, retention, reopen
// equivalence, read-only observation and store-level writer fencing.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/codec.hpp"
#include "dcr/data_center_registry.hpp"
#include "persistence/store.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace dcr;

namespace {

struct StoreLayout {
  std::filesystem::path root;
  std::filesystem::path current;
  std::filesystem::path generations;
  std::filesystem::path staging;
  std::filesystem::path quarantine;
};

StoreLayout layout_of(const std::filesystem::path& root) {
  StoreLayout layout;
  layout.root = root;
  layout.current = root / "CURRENT";
  layout.generations = root / "generations";
  layout.staging = root / "tmp";
  layout.quarantine = root / "uncommitted";
  return layout;
}

std::vector<std::string> generation_file_names(const StoreLayout& layout) {
  std::vector<std::string> names;
  std::error_code code;
  if (!std::filesystem::is_directory(layout.generations, code)) {
    return names;
  }
  for (const auto& entry : std::filesystem::directory_iterator(layout.generations, code)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> file_names(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  std::error_code code;
  if (!std::filesystem::is_directory(directory, code)) {
    return names;
  }
  for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::uint64_t file_size(const std::filesystem::path& path) {
  std::error_code code;
  const auto size = std::filesystem::file_size(path, code);
  return code ? 0 : static_cast<std::uint64_t>(size);
}

/// Registers `count` facilities through the public API.
void register_many(dcrtest::Fixture& fixture, Registry& registry, std::uint64_t count,
                   std::uint64_t first = 0) {
  for (std::uint64_t index = first; index < first + count; ++index) {
    auto command = fixture.register_command(dcrtest::data_center_id_text(index),
                                            "Facility " + std::to_string(index),
                                            LifecycleState::registered, "site-east");
    command.draft.facility.region = *RegionCode::parse("us-east-1");
    auto created = registry.register_data_center(command);
    if (!created.has_value()) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              "register_many failed: " + created.error().to_string());
      return;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Publication layout
// ---------------------------------------------------------------------------

DCR_TEST(store, publication_produces_the_documented_layout) {
  dcrtest::Fixture fixture("store-layout");
  const StoreLayout layout = layout_of(fixture.root());
  DCR_CHECK(!std::filesystem::exists(layout.current));

  auto opened = fixture.open(StoreOpenMode::open_or_create);
  Registry& registry = *opened.registry;
  DCR_CHECK(opened.report.created);
  DCR_CHECK(opened.report.disposition == RecoveryDisposition::created_empty);
  DCR_CHECK(std::filesystem::exists(layout.root / "registry.lock"));
  // Generation 0 is never published.
  DCR_CHECK(!std::filesystem::exists(layout.current));

  register_many(fixture, registry, 3);
  DCR_CHECK_EQ(std::uint64_t{3}, registry.generation().value());
  DCR_CHECK(std::filesystem::exists(layout.current));
  const std::vector<std::string> names = generation_file_names(layout);
  DCR_CHECK_EQ(std::size_t{3}, names.size());
  DCR_CHECK_EQ(std::string("gen-00000000000000000001.dcrs"), names[0]);
  DCR_CHECK_EQ(std::string("gen-00000000000000000002.dcrs"), names[1]);
  DCR_CHECK_EQ(std::string("gen-00000000000000000003.dcrs"), names[2]);

  // Nothing is left staged after a successful publication.
  DCR_CHECK(file_names(layout.staging).empty());

  // The lock file is released on close, and the registry can be reopened.
  DCR_CHECK(registry.close().has_value());
  auto reopened = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{3}, reopened.registry->generation().value());
  DCR_CHECK(!reopened.report.created);
  DCR_CHECK(reopened.report.disposition == RecoveryDisposition::opened_current);
  DCR_CHECK_EQ(std::uint64_t{3}, reopened.report.record_count);
}

DCR_TEST(store, generation_retention_is_bounded) {
  dcrtest::Fixture fixture("store-retention");
  dcr::StoreOptions store_options = fixture.store_options(StoreOpenMode::open_or_create);
  store_options.generation_retention = 2;
  dcr::OpenOptions options;
  options.store = store_options;
  options.clock = &fixture.clock();
  auto opened_result = Registry::open(options);
  DCR_REQUIRE(opened_result.has_value());
  auto opened = std::move(opened_result).value();
  Registry& registry = *opened.registry;
  const StoreLayout layout = layout_of(fixture.root());

  register_many(fixture, registry, 6);
  DCR_CHECK_EQ(std::uint64_t{6}, registry.generation().value());
  const std::vector<std::string> names = generation_file_names(layout);
  DCR_CHECK_EQ(std::size_t{2}, names.size());
  DCR_CHECK_EQ(std::string("gen-00000000000000000005.dcrs"), names[0]);
  DCR_CHECK_EQ(std::string("gen-00000000000000000006.dcrs"), names[1]);

  // A retention below 2 would let a writer delete the file a reader is opening.
  store_options.generation_retention = 1;
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, store_options.validate());
}

DCR_TEST(store, open_modes_are_enforced) {
  dcrtest::Fixture fixture("store-modes");
  DCR_REQUIRE_ERROR(ErrorCode::store_not_found,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
  DCR_REQUIRE_ERROR(ErrorCode::store_not_found,
                    Registry::open(fixture.open_options(StoreOpenMode::read_only)));

  {
    auto opened = fixture.open(StoreOpenMode::create_new);
    DCR_CHECK(opened.report.created);
    register_many(fixture, *opened.registry, 1);
  }

  // create_new never adopts existing state.
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument,
                    Registry::open(fixture.open_options(StoreOpenMode::create_new)));

  // A read-only open reads but cannot write.
  auto read_only = fixture.open(StoreOpenMode::read_only);
  DCR_CHECK(!read_only.registry->is_writable());
  DCR_CHECK_EQ(std::uint64_t{1}, read_only.registry->generation().value());
  DCR_CHECK(read_only.report.disposition == RecoveryDisposition::opened_read_only);

  auto command = fixture.register_command("dc-fac-1", "Facility 1", LifecycleState::registered,
                                          "site-east");
  DCR_REQUIRE_ERROR(ErrorCode::store_read_only,
                    read_only.registry->register_data_center(command));
  DCR_CHECK_EQ(std::uint64_t{1}, read_only.registry->generation().value());
}

DCR_TEST(store, read_only_observers_follow_a_live_writer) {
  dcrtest::Fixture fixture("store-observer");
  auto writer = fixture.open(StoreOpenMode::open_or_create);
  register_many(fixture, *writer.registry, 2);

  auto observer = fixture.open(StoreOpenMode::read_only);
  DCR_CHECK_EQ(std::uint64_t{2}, observer.registry->generation().value());

  register_many(fixture, *writer.registry, 3, 100);
  // The observer still holds generation 2 until it refreshes.
  DCR_CHECK_EQ(std::uint64_t{2}, observer.registry->generation().value());
  DCR_REQUIRE_OK(const OpenReport report, observer.registry->refresh());
  DCR_CHECK_EQ(std::uint64_t{5}, report.generation.value());
  DCR_CHECK_EQ(std::uint64_t{5}, observer.registry->generation().value());
  DCR_CHECK_EQ(std::uint64_t{5}, observer.registry->record_count());

  // A writable registry is the writer and has nothing to refresh.
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, writer.registry->refresh());
}

// ---------------------------------------------------------------------------
// Writer fencing
// ---------------------------------------------------------------------------

DCR_TEST(store, a_second_writer_cannot_open_the_same_store) {
  dcrtest::Fixture fixture("store-lock");
  auto first = fixture.open(StoreOpenMode::open_or_create);
  register_many(fixture, *first.registry, 1);

  DCR_REQUIRE_ERROR(ErrorCode::store_locked,
                    Registry::open(fixture.open_options(StoreOpenMode::open_existing)));
  DCR_REQUIRE_ERROR(ErrorCode::store_locked,
                    Registry::open(fixture.open_options(StoreOpenMode::open_or_create)));

  // A read-only observer is always allowed: published generations are
  // immutable, so reading one cannot disturb the writer.
  auto observer = fixture.open(StoreOpenMode::read_only);
  DCR_CHECK_EQ(std::uint64_t{1}, observer.registry->generation().value());

  // Closing the writer releases the lock.
  DCR_CHECK(first.registry->close().has_value());
  auto second = fixture.open(StoreOpenMode::open_existing);
  DCR_CHECK_EQ(std::uint64_t{1}, second.registry->generation().value());
}

DCR_TEST(store, the_store_refuses_a_publication_that_is_not_the_successor) {
  // This exercises the fencing check directly, below the registry: a writer
  // whose view of the store is stale cannot overwrite a newer commit even if
  // the writer lock were bypassed entirely.
  dcrtest::Fixture fixture("store-fence");
  const StoreOptions options = fixture.store_options(StoreOpenMode::open_or_create);

  auto opened_result = internal::SnapshotStore::open(options);
  DCR_REQUIRE(opened_result.has_value());
  internal::StoreOpenResult opened = std::move(opened_result).value();
  internal::SnapshotStore store = std::move(opened.store);
  internal::RegistryState state = std::move(opened.state);

  // Two commits.
  for (int index = 0; index < 2; ++index) {
    internal::RegistryState next = state;
    auto generation = state.generation.successor();
    DCR_REQUIRE(generation.has_value());
    next.generation = generation.value();
    DCR_REQUIRE_OK(const Sha256Digest unused, store.publish(next, state.generation));
    state = std::move(next);
  }
  DCR_CHECK_EQ(std::uint64_t{2}, state.generation.value());

  // A writer holding a view from generation 0 asks to publish generation 1,
  // which is a well-formed publication on its own terms, but the store's
  // CURRENT already names generation 2. The fence refuses it.
  internal::RegistryState stale = state;
  stale.generation = *RegistryGeneration::from_value(1);
  DCR_REQUIRE_ERROR(ErrorCode::stale_generation,
                    store.publish(stale, *RegistryGeneration::from_value(0)));

  // Publishing a state that does not advance by exactly one is an internal
  // error, not a silent write.
  auto successor = state.generation.successor();
  DCR_REQUIRE(successor.has_value());
  auto two_more = successor.value().successor();
  DCR_REQUIRE(two_more.has_value());
  internal::RegistryState skip = state;
  skip.generation = two_more.value();
  DCR_REQUIRE_ERROR(ErrorCode::internal_error, store.publish(skip, state.generation));

  // The store is still exactly where it was.
  const std::vector<std::string> names = generation_file_names(layout_of(fixture.root()));
  DCR_CHECK_EQ(std::size_t{2}, names.size());
}

// ---------------------------------------------------------------------------
// Reopen equivalence
// ---------------------------------------------------------------------------

DCR_TEST(persistence, reopen_reproduces_the_exact_state) {
  dcrtest::Fixture fixture("reopen");
  Sha256Digest digest;
  RegistryStats before;
  std::vector<DataCenterRecord> records_before;

  {
    auto opened = fixture.open();
    Registry& registry = *opened.registry;
    register_many(fixture, registry, 4);

    UpdateMetadataCommand update;
    update.context = fixture.context_at(registry);
    update.id = fixture.id(dcrtest::data_center_id_text(0));
    update.patch.display_name = FieldPatch<std::string>::set("Renamed Facility");
    update.patch.aliases = FieldPatch<std::vector<Alias>>::set({fixture.alias("renamed-facility")});
    update.patch.facility = FieldPatch<FacilityMetadata>::set([&] {
      FacilityMetadata facility;
      facility.region = *RegionCode::parse("us-east-2");
      facility.country = *CountryCode::parse("US");
      facility.metro = *MetroCode::parse("ashburn");
      facility.tier = FacilityTier::tier_iii;
      facility.coordinates = GeoCoordinates::make(390439000, -774875000).value();
      facility.address = PostalAddress::make({"1 Example Way"}, "Ashburn", "VA", "20147").value();
      facility.facility_operator = *PrincipalId::parse("operator-west");
      facility.external_refs =
          *MetadataMap::make({{*MetadataKey::parse("power.feed"), "feed-a"}}, "external_refs");
      return facility;
    }());
    DCR_REQUIRE_OK(const MetadataMap extensions,
                   MetadataMap::make({{*MetadataKey::parse("zone.kind"), "hall"}}, "extensions"));
    update.patch.extensions = FieldPatch<MetadataMap>::set(extensions);
    update.patch.ownership = FieldPatch<OwnershipScope>::set(
        *OwnershipScope::make(OwnershipKind::platform, *OwnershipScopeId::parse("scope-platform")));
    DCR_REQUIRE_OK(const MutationResult unused_update, registry.update_metadata(update));

    TransitionCommand transition;
    transition.context = fixture.context_at(registry);
    transition.id = fixture.id(dcrtest::data_center_id_text(1));
    transition.target_state = LifecycleState::active;
    transition.reason = *ReasonCode::parse("commissioned");
    DCR_REQUIRE_OK(const MutationResult unused_transition, registry.transition_lifecycle(transition));

    RetireCommand retire;
    retire.context = fixture.context_at(registry);
    retire.id = fixture.id(dcrtest::data_center_id_text(2));
    retire.reason = *ReasonCode::parse("decommissioned");
    DCR_REQUIRE_OK(const MutationResult unused_retire, registry.retire_data_center(retire));

    before = registry.stats();
    digest = registry.snapshot_digest();
    DCR_REQUIRE_OK(const RegistrySnapshot snapshot, registry.snapshot());
    records_before = snapshot.records();
    DCR_CHECK(registry.close().has_value());
  }

  auto reopened = fixture.open(StoreOpenMode::open_existing);
  Registry& registry = *reopened.registry;
  DCR_CHECK(registry.snapshot_digest() == digest);
  const RegistryStats after = registry.stats();
  DCR_CHECK_EQ(before.generation.value(), after.generation.value());
  DCR_CHECK_EQ(before.record_count, after.record_count);
  DCR_CHECK(before.records_by_state == after.records_by_state);
  DCR_CHECK_EQ(before.alias_count, after.alias_count);
  DCR_CHECK_EQ(before.membership_count, after.membership_count);
  DCR_CHECK_EQ(before.distinct_site_count, after.distinct_site_count);
  DCR_CHECK_EQ(before.distinct_ownership_scope_count, after.distinct_ownership_scope_count);
  DCR_CHECK_EQ(before.history_entries, after.history_entries);
  DCR_CHECK_EQ(before.idempotency_entries, after.idempotency_entries);
  DCR_CHECK_EQ(before.snapshot_payload_bytes, after.snapshot_payload_bytes);

  DCR_REQUIRE_OK(const RegistrySnapshot snapshot, registry.snapshot());
  DCR_CHECK(snapshot.records() == records_before);

  // The retired record is still retired and still not resurrectable.
  const DataCenterId retired_id = fixture.id(dcrtest::data_center_id_text(2));
  DCR_REQUIRE_OK(const DataCenterRecord retired, registry.get(retired_id));
  DCR_CHECK(retired.state == LifecycleState::retired);
  DCR_REQUIRE_ERROR(ErrorCode::terminal_state,
                    registry.transition_lifecycle([&] {
                      TransitionCommand command;
                      command.context = fixture.context_at(registry);
                      command.id = retired_id;
                      command.target_state = LifecycleState::active;
                      command.reason = *ReasonCode::parse("operator_request");
                      return command;
                    }()));

  // Mutations continue from the reloaded generation, not from zero.
  register_many(fixture, registry, 1, 200);
  DCR_CHECK_EQ(before.generation.value() + 1, registry.generation().value());
}

DCR_TEST(persistence, the_digest_matches_the_published_payload) {
  dcrtest::Fixture fixture("digest-match");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  register_many(fixture, registry, 2);

  const Sha256Digest digest = registry.snapshot_digest();
  DCR_REQUIRE_OK(const RegistrySnapshot snapshot, registry.snapshot());
  DCR_CHECK(snapshot.digest() == digest);
  DCR_CHECK(snapshot.stats().snapshot_digest == digest);

  // The published file carries exactly that payload.
  const StoreLayout layout = layout_of(fixture.root());
  const std::string file = (layout.generations / "gen-00000000000000000002.dcrs").string();
  DCR_CHECK(file_size(file) > 88);

  // Reopening read-only over the same bytes produces the same digest, which is
  // what makes the digest usable as an external comparison point.
  auto observer = fixture.open(StoreOpenMode::read_only);
  DCR_CHECK(observer.registry->snapshot_digest() == digest);
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

DCR_TEST(inspection, reports_a_healthy_store) {
  dcrtest::Fixture fixture("inspect-healthy");
  auto opened = fixture.open();
  register_many(fixture, *opened.registry, 2);
  DCR_CHECK(opened.registry->close().has_value());

  DCR_REQUIRE_OK(const StoreInspection inspection,
                 Registry::inspect(fixture.store_options(StoreOpenMode::open_existing)));
  DCR_CHECK(inspection.root_exists);
  DCR_CHECK(inspection.current_present);
  DCR_CHECK(inspection.current_parseable);
  DCR_CHECK(inspection.current_valid);
  DCR_CHECK(inspection.consistent);
  DCR_CHECK(inspection.current_generation.has_value());
  DCR_CHECK_EQ(std::uint64_t{2}, inspection.current_generation->value());
  DCR_CHECK(inspection.newest_valid_generation.has_value());
  DCR_CHECK_EQ(std::uint64_t{2}, inspection.newest_valid_generation->value());
  DCR_CHECK(inspection.selected_generation.has_value());
  DCR_CHECK_EQ(std::uint64_t{2}, inspection.selected_generation->value());
  DCR_CHECK(inspection.disposition == RecoveryDisposition::opened_current);
  DCR_CHECK_EQ(std::size_t{2}, inspection.generation_files.size());
  for (const auto& file : inspection.generation_files) {
    DCR_CHECK(file.digest_ok);
    DCR_CHECK(file.parsed);
    DCR_CHECK(file.generation_matches_name);
    DCR_CHECK(file.detail.empty());
  }
}

DCR_TEST(inspection, reports_an_absent_store) {
  dcrtest::Fixture fixture("inspect-absent");
  // A root that has never been created, rather than the fixture's own
  // directory.
  dcr::StoreOptions options = fixture.store_options(StoreOpenMode::open_existing);
  options.root = fixture.root() / "never-created";
  DCR_REQUIRE_OK(const StoreInspection inspection, Registry::inspect(options));
  DCR_CHECK(!inspection.root_exists);
  DCR_CHECK(!inspection.current_present);
  DCR_CHECK(!inspection.consistent);
  DCR_CHECK(!inspection.newest_valid_generation.has_value());
}

// ---------------------------------------------------------------------------
// Canonical codec
// ---------------------------------------------------------------------------

DCR_TEST(codec, published_payload_round_trips_byte_for_byte) {
  dcrtest::Fixture fixture("codec-roundtrip");
  const StoreLayout layout = layout_of(fixture.root());
  {
    auto opened = fixture.open();
    register_many(fixture, *opened.registry, 3);
    DCR_CHECK(opened.registry->close().has_value());
  }

  // Decoding the published container and re-encoding it must reproduce the
  // same bytes: that is what "canonical" means here, and it is what makes two
  // registries with the same state produce the same digest.
  const std::filesystem::path file = layout.generations / "gen-00000000000000000003.dcrs";
  std::string bytes;
  DCR_REQUIRE(dcrtest::read_text_file(file, bytes));

  DCR_REQUIRE_OK(internal::RegistryState state,
                 internal::decode_container(bytes, fixture.limits()));
  DCR_CHECK_EQ(std::uint64_t{3}, state.generation.value());
  DCR_CHECK_EQ(std::size_t{3}, state.records.size());

  DCR_REQUIRE_OK(const std::string reencoded, internal::encode_container(state));
  DCR_CHECK(reencoded == bytes);

  // Re-encoding from the decoded state also reproduces the digest the registry
  // reported while it was open.
  DCR_REQUIRE_OK(const std::string payload, internal::encode_state(state));
  DCR_CHECK(sha256_text(payload) == *Sha256Digest::from_hex(
                                        [&bytes] {
                                          std::string hex;
                                          for (std::size_t index = 24; index < 56; ++index) {
                                            static constexpr char kDigits[] = "0123456789abcdef";
                                            const auto byte =
                                                static_cast<std::uint8_t>(bytes[index]);
                                            hex.push_back(kDigits[(byte >> 4U) & 0xFU]);
                                            hex.push_back(kDigits[byte & 0xFU]);
                                          }
                                          return hex;
                                        }()));
}

DCR_TEST(codec, limits_only_ever_tighten_on_load) {
  RegistryLimits left = RegistryLimits::defaults();
  RegistryLimits right = RegistryLimits::defaults();
  right.max_records = 10;
  right.max_display_name_bytes = 200;
  const RegistryLimits tightened = internal::tighten_limits(left, right);
  DCR_CHECK_EQ(std::uint64_t{10}, tightened.max_records);
  DCR_CHECK_EQ(std::uint32_t{200}, tightened.max_display_name_bytes);

  // A store cannot widen its own limits beyond the structural maximum.
  RegistryLimits wide = RegistryLimits::defaults();
  wide.max_metadata_entries = 100000;
  const RegistryLimits clamped = internal::tighten_limits(wide, RegistryLimits::defaults());
  DCR_CHECK(clamped.max_metadata_entries <= StructuralLimits::kMaxMetadataEntries);
}
