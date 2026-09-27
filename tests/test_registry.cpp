// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Registry behaviour: registration, metadata updates, preconditions,
// idempotency, queries, enumeration, history, statistics and provenance.

#include <algorithm>
#include <string>
#include <vector>

#include "dcr/data_center_registry.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace dcr;

namespace {

std::vector<std::string> ids_of(const std::vector<DataCenterRecord>& records) {
  std::vector<std::string> ids;
  ids.reserve(records.size());
  for (const auto& record : records) {
    ids.push_back(record.id.value());
  }
  return ids;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

DCR_TEST(registration, creates_a_canonical_record) {
  dcrtest::Fixture fixture("reg-create");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  DCR_CHECK_EQ(std::uint64_t{0}, registry.generation().value());
  DCR_CHECK_EQ(std::uint64_t{0}, registry.record_count());

  auto command = fixture.register_command("dc-ashburn-01", "Ashburn One",
                                          LifecycleState::registered, "site-east");
  command.draft.aliases.push_back(fixture.alias("legacy-ashburn"));
  command.draft.extensions =
      *MetadataMap::make({{*MetadataKey::parse("rack.count"), "42"}}, "extensions");

  DCR_REQUIRE_OK(const MutationResult result, registry.register_data_center(command));
  DCR_CHECK(result.status == MutationStatus::created);
  DCR_CHECK(result.id == fixture.id("dc-ashburn-01"));
  DCR_CHECK_EQ(std::uint64_t{1}, result.generation.value());
  DCR_CHECK_EQ(std::uint64_t{1}, result.revision.value());
  DCR_CHECK(result.sequence.has_value());
  DCR_CHECK_EQ(std::uint64_t{1}, result.sequence->value());

  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-ashburn-01")));
  DCR_CHECK_EQ(std::string("Ashburn One"), record.display_name);
  DCR_CHECK(record.state == LifecycleState::registered);
  DCR_CHECK_EQ(std::uint64_t{1}, record.created_generation.value());
  DCR_CHECK(!record.retired_generation.has_value());
  DCR_CHECK(!record.replaces.has_value());
  DCR_CHECK_EQ(std::size_t{1}, record.memberships.size());
  DCR_CHECK(record.primary_membership().has_value());
  DCR_CHECK_EQ(std::string("site-east"), record.primary_membership()->site.value());
  DCR_CHECK_EQ(std::uint64_t{1}, record.last_sequence.value());
  DCR_CHECK(record.last_provenance.source() == ProvenanceSource::api);
  DCR_CHECK_EQ(std::string("operator"), record.last_provenance.principal().value());
  DCR_CHECK_EQ(std::int64_t{0}, record.last_provenance.recorded_at().unix_millis());

  // The alias resolves the same record.
  DCR_REQUIRE_OK(const DataCenterRecord by_alias, registry.find_by_alias(fixture.alias("legacy-ashburn")));
  DCR_CHECK(by_alias.id == record.id);
}

DCR_TEST(registration, rejects_duplicate_identity_and_alias_conflicts) {
  dcrtest::Fixture fixture("reg-dupes");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  auto first = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                        "site-east");
  first.draft.aliases.push_back(fixture.alias("shared-name"));
  DCR_REQUIRE_OK(const MutationResult unused, registry.register_data_center(first));

  auto second = fixture.register_command("dc-alpha", "Alpha Again", LifecycleState::registered,
                                         "site-east");
  DCR_REQUIRE_ERROR(ErrorCode::duplicate_identity, registry.register_data_center(second));
  DCR_CHECK_EQ(std::uint64_t{1}, registry.generation().value());
  DCR_CHECK_EQ(std::uint64_t{1}, registry.record_count());

  auto third = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                        "site-east");
  third.draft.aliases.push_back(fixture.alias("shared-name"));
  DCR_REQUIRE_ERROR(ErrorCode::alias_conflict, registry.register_data_center(third));
  DCR_CHECK_EQ(std::uint64_t{1}, registry.generation().value());
  DCR_CHECK_EQ(std::uint64_t{1}, registry.record_count());

  // A malformed command never reaches the store either.
  auto bad = fixture.register_command("dc-gamma", "", LifecycleState::registered, "site-east");
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.register_data_center(bad));
  auto active = fixture.register_command("dc-gamma", "Gamma", LifecycleState::active, "site-east");
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.register_data_center(active));
  auto with_precondition =
      fixture.register_command("dc-delta", "Delta", LifecycleState::proposed, "");
  with_precondition.context.precondition.expected_generation = registry.generation();
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument,
                    registry.register_data_center(with_precondition));
  DCR_CHECK_EQ(std::uint64_t{1}, registry.generation().value());
}

DCR_TEST(registration, live_states_require_exactly_one_primary_site) {
  dcrtest::Fixture fixture("reg-membership");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  auto no_site = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered, "");
  DCR_REQUIRE_ERROR(ErrorCode::membership_violation, registry.register_data_center(no_site));

  auto two_primaries = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                                "site-east");
  two_primaries.draft.memberships.push_back(
      SiteMembership{fixture.site("site-west"), MembershipRole::primary});
  DCR_REQUIRE_ERROR(ErrorCode::membership_violation,
                    registry.register_data_center(two_primaries));

  // A proposal needs no site at all, and may carry a secondary membership.
  auto proposed = fixture.register_command("dc-gamma", "Gamma", LifecycleState::proposed, "");
  proposed.draft.memberships.push_back(
      SiteMembership{fixture.site("site-west"), MembershipRole::secondary});
  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(proposed));
  DCR_CHECK(created.status == MutationStatus::created);

  // A duplicate site in one command is a duplicate membership, not a merge.
  auto duplicated = fixture.register_command("dc-delta", "Delta", LifecycleState::proposed, "");
  duplicated.draft.memberships.push_back(
      SiteMembership{fixture.site("site-west"), MembershipRole::primary});
  duplicated.draft.memberships.push_back(
      SiteMembership{fixture.site("site-west"), MembershipRole::secondary});
  DCR_REQUIRE_ERROR(ErrorCode::duplicate_membership, registry.register_data_center(duplicated));
}

// ---------------------------------------------------------------------------
// Metadata updates
// ---------------------------------------------------------------------------

DCR_TEST(update, applies_field_patches_and_tracks_revisions) {
  dcrtest::Fixture fixture("upd-basic");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  DCR_REQUIRE_OK(const MutationResult unused,
                 registry.register_data_center(fixture.register_command(
                     "dc-alpha", "Alpha", LifecycleState::registered, "site-east")));

  UpdateMetadataCommand command;
  command.context = fixture.context_at(registry);
  command.id = fixture.id("dc-alpha");
  command.patch.display_name = FieldPatch<std::string>::set("Alpha Renamed");
  command.patch.facility = FieldPatch<FacilityMetadata>::set([] {
    FacilityMetadata facility;
    facility.region = *RegionCode::parse("us-east-1");
    facility.country = *CountryCode::parse("US");
    return facility;
  }());
  command.patch.aliases = FieldPatch<std::vector<Alias>>::set(
      {fixture.alias("alpha-alias"), fixture.alias("alpha-other")});

  DCR_REQUIRE_OK(const MutationResult updated, registry.update_metadata(command));
  DCR_CHECK(updated.status == MutationStatus::updated);
  DCR_CHECK_EQ(std::uint64_t{2}, updated.generation.value());
  DCR_CHECK_EQ(std::uint64_t{2}, updated.revision.value());

  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-alpha")));
  DCR_CHECK_EQ(std::string("Alpha Renamed"), record.display_name);
  DCR_CHECK_EQ(std::size_t{2}, record.aliases.size());
  // Aliases are stored in canonical order regardless of the order supplied.
  DCR_CHECK_EQ(std::string("alpha-alias"), record.aliases[0].value());
  DCR_CHECK(record.facility.region.has_value());
  DCR_CHECK_EQ(std::string("us-east-1"), record.facility.region->value());

  // The old generation is stale now, for a command that would really change
  // something.
  UpdateMetadataCommand stale = command;
  stale.patch.display_name = FieldPatch<std::string>::set("Alpha Stale");
  DCR_REQUIRE_ERROR(ErrorCode::stale_generation, registry.update_metadata(stale));

  // A command whose desired end state already holds is reported as unchanged
  // even with a stale generation: the work it describes is already done, and
  // telling the caller to retry would be misleading.
  DCR_REQUIRE_OK(const MutationResult replayed_noop, registry.update_metadata(command));
  DCR_CHECK(replayed_noop.status == MutationStatus::unchanged);

  // A revision precondition that no longer holds is rejected even when the
  // generation is current.
  UpdateMetadataCommand revision_check;
  revision_check.context = fixture.context_at(registry);
  revision_check.context.precondition.expected_revision = MetadataRevision::minimum();
  revision_check.id = fixture.id("dc-alpha");
  revision_check.patch.display_name = FieldPatch<std::string>::set("Stale Revision");
  DCR_REQUIRE_ERROR(ErrorCode::stale_revision, registry.update_metadata(revision_check));

  // A missing generation precondition is a caller error, not a stale write.
  UpdateMetadataCommand no_precondition;
  no_precondition.context = fixture.context();
  no_precondition.id = fixture.id("dc-alpha");
  no_precondition.patch.display_name = FieldPatch<std::string>::set("No Precondition");
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.update_metadata(no_precondition));

  // An empty patch is rejected rather than silently accepted.
  UpdateMetadataCommand empty_patch;
  empty_patch.context = fixture.context_at(registry);
  empty_patch.id = fixture.id("dc-alpha");
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.update_metadata(empty_patch));

  // Clearing the display name is refused: a record always has a name.
  UpdateMetadataCommand clear_name;
  clear_name.context = fixture.context_at(registry);
  clear_name.id = fixture.id("dc-alpha");
  clear_name.patch.display_name = FieldPatch<std::string>::clear();
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.update_metadata(clear_name));

  // An update that changes nothing is an idempotent no-op: no generation, no
  // revision, no history entry, no write.
  const RegistryGeneration generation_before = registry.generation();
  const Sha256Digest digest_before = registry.snapshot_digest();
  UpdateMetadataCommand no_change;
  no_change.context = fixture.context_at(registry);
  no_change.id = fixture.id("dc-alpha");
  no_change.patch.display_name = FieldPatch<std::string>::set("Alpha Renamed");
  DCR_REQUIRE_OK(const MutationResult unchanged, registry.update_metadata(no_change));
  DCR_CHECK(unchanged.status == MutationStatus::unchanged);
  DCR_CHECK_EQ(std::uint64_t{2}, unchanged.revision.value());
  DCR_CHECK(registry.generation() == generation_before);
  DCR_CHECK(registry.snapshot_digest() == digest_before);

  // Updating an unknown record is not found.
  UpdateMetadataCommand missing;
  missing.context = fixture.context_at(registry);
  missing.id = fixture.id("dc-absent");
  missing.patch.display_name = FieldPatch<std::string>::set("Absent");
  DCR_REQUIRE_ERROR(ErrorCode::not_found, registry.update_metadata(missing));

  // Clearing optional metadata is legal.
  UpdateMetadataCommand clear_optional;
  clear_optional.context = fixture.context_at(registry);
  clear_optional.id = fixture.id("dc-alpha");
  clear_optional.patch.facility = FieldPatch<FacilityMetadata>::clear();
  DCR_REQUIRE_OK(const MutationResult cleared, registry.update_metadata(clear_optional));
  DCR_CHECK(cleared.status == MutationStatus::updated);
  DCR_REQUIRE_OK(const DataCenterRecord after_clear, registry.get(fixture.id("dc-alpha")));
  DCR_CHECK(!after_clear.facility.region.has_value());
}

DCR_TEST(update, alias_conflicts_are_detected_and_aliases_are_released) {
  dcrtest::Fixture fixture("upd-alias");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  auto alpha = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                        "site-east");
  alpha.draft.aliases.push_back(fixture.alias("alpha-alias"));
  DCR_REQUIRE_OK(const MutationResult unused_a, registry.register_data_center(alpha));
  DCR_REQUIRE_OK(const MutationResult unused_b,
                 registry.register_data_center(fixture.register_command(
                     "dc-beta", "Beta", LifecycleState::registered, "site-east")));

  UpdateMetadataCommand steal;
  steal.context = fixture.context_at(registry);
  steal.id = fixture.id("dc-beta");
  steal.patch.aliases = FieldPatch<std::vector<Alias>>::set({fixture.alias("alpha-alias")});
  DCR_REQUIRE_ERROR(ErrorCode::alias_conflict, registry.update_metadata(steal));

  // Removing an alias from its owner releases it for someone else.
  UpdateMetadataCommand release;
  release.context = fixture.context_at(registry);
  release.id = fixture.id("dc-alpha");
  release.patch.aliases = FieldPatch<std::vector<Alias>>::set({});
  DCR_REQUIRE_OK(const MutationResult released, registry.update_metadata(release));
  DCR_CHECK(released.status == MutationStatus::updated);

  UpdateMetadataCommand claim;
  claim.context = fixture.context_at(registry);
  claim.id = fixture.id("dc-beta");
  claim.patch.aliases = FieldPatch<std::vector<Alias>>::set({fixture.alias("alpha-alias")});
  DCR_REQUIRE_OK(const MutationResult claimed, registry.update_metadata(claim));
  DCR_CHECK(claimed.status == MutationStatus::updated);
  DCR_REQUIRE_OK(const DataCenterRecord beta, registry.find_by_alias(fixture.alias("alpha-alias")));
  DCR_CHECK(beta.id == fixture.id("dc-beta"));

  // A duplicate alias inside one command is rejected.
  UpdateMetadataCommand duplicate;
  duplicate.context = fixture.context_at(registry);
  duplicate.id = fixture.id("dc-beta");
  duplicate.patch.aliases = FieldPatch<std::vector<Alias>>::set(
      {fixture.alias("beta-one"), fixture.alias("beta-one")});
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.update_metadata(duplicate));
}

// ---------------------------------------------------------------------------
// Idempotency
// ---------------------------------------------------------------------------

DCR_TEST(idempotency, replay_reports_the_original_outcome) {
  dcrtest::Fixture fixture("idem-replay");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  RegisterCommand command = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                                     "site-east");
  DCR_REQUIRE_OK(const IdempotencyKey key, IdempotencyKey::parse("retry-0001"));
  command.context.idempotency_key = key;
  command.context.provenance.detail = "first attempt";

  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(command));
  DCR_CHECK(created.status == MutationStatus::created);
  const RegistryGeneration generation = registry.generation();
  const Sha256Digest digest = registry.snapshot_digest();

  // The identical command, submitted again, reports the original outcome and
  // writes nothing.
  DCR_REQUIRE_OK(const MutationResult replayed, registry.register_data_center(command));
  DCR_CHECK(replayed.status == MutationStatus::replayed);
  DCR_CHECK(replayed.generation == created.generation);
  DCR_CHECK(replayed.revision == created.revision);
  DCR_CHECK(replayed.sequence == created.sequence);
  DCR_CHECK(registry.generation() == generation);
  DCR_CHECK(registry.snapshot_digest() == digest);

  // A different command under the same key is a conflict, not a replay.
  RegisterCommand different = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                                       "site-east");
  different.context.idempotency_key = key;
  different.context.provenance.detail = "first attempt";
  DCR_REQUIRE_ERROR(ErrorCode::idempotency_conflict, registry.register_data_center(different));
  DCR_CHECK(registry.snapshot_digest() == digest);

  // A different detail is different content.
  RegisterCommand other_detail = command;
  other_detail.context.provenance.detail = "second attempt";
  DCR_REQUIRE_ERROR(ErrorCode::idempotency_conflict, registry.register_data_center(other_detail));
}

DCR_TEST(idempotency, replay_survives_reopen) {
  dcrtest::Fixture fixture("idem-reopen");
  {
    auto opened = fixture.open();
    Registry& registry = *opened.registry;
    RegisterCommand command =
        fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered, "site-east");
    DCR_REQUIRE_OK(const IdempotencyKey key, IdempotencyKey::parse("retry-0002"));
    command.context.idempotency_key = key;
    DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(command));
    DCR_CHECK(created.status == MutationStatus::created);
    DCR_CHECK(registry.close().has_value());
  }
  auto reopened = fixture.open(StoreOpenMode::open_existing);
  Registry& registry = *reopened.registry;
  RegisterCommand command =
      fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered, "site-east");
  DCR_REQUIRE_OK(const IdempotencyKey key, IdempotencyKey::parse("retry-0002"));
  command.context.idempotency_key = key;
  DCR_REQUIRE_OK(const MutationResult replayed, registry.register_data_center(command));
  DCR_CHECK(replayed.status == MutationStatus::replayed);
  DCR_CHECK_EQ(std::uint64_t{1}, registry.generation().value());
  DCR_CHECK_EQ(std::uint64_t{1}, registry.record_count());
}

DCR_TEST(idempotency, table_is_bounded_and_evicts_oldest_first) {
  dcrtest::Fixture fixture("idem-bound");
  RegistryLimits limits = RegistryLimits::defaults();
  limits.max_idempotency_entries = 4;
  fixture.set_limits(limits);
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  for (std::uint64_t index = 0; index < 8; ++index) {
    RegisterCommand command =
        fixture.register_command(dcrtest::data_center_id_text(index),
                                 "Facility " + std::to_string(index), LifecycleState::registered,
                                 "site-east");
    DCR_REQUIRE_OK(const IdempotencyKey key,
                   IdempotencyKey::parse("retry-1000" + std::to_string(index)));
    command.context.idempotency_key = key;
    DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(command));
    DCR_CHECK(created.status == MutationStatus::created);
    DCR_CHECK(registry.stats().idempotency_entries <= 4);
  }
  DCR_CHECK_EQ(std::uint64_t{4}, registry.stats().idempotency_entries);

  // The two oldest keys were evicted, so they no longer replay; they are
  // treated as a fresh command and rejected because their record now exists.
  RegisterCommand first = fixture.register_command(dcrtest::data_center_id_text(0),
                                                   "Facility 0", LifecycleState::registered,
                                                   "site-east");
  DCR_REQUIRE_OK(const IdempotencyKey evicted_key, IdempotencyKey::parse("retry-10000"));
  first.context.idempotency_key = evicted_key;
  DCR_REQUIRE_ERROR(ErrorCode::duplicate_identity, registry.register_data_center(first));

  // The newest key still replays.
  RegisterCommand last = fixture.register_command(dcrtest::data_center_id_text(7), "Facility 7",
                                                  LifecycleState::registered, "site-east");
  DCR_REQUIRE_OK(const IdempotencyKey kept_key, IdempotencyKey::parse("retry-10007"));
  last.context.idempotency_key = kept_key;
  DCR_REQUIRE_OK(const MutationResult replayed, registry.register_data_center(last));
  DCR_CHECK(replayed.status == MutationStatus::replayed);
}

// ---------------------------------------------------------------------------
// External epoch assertions
// ---------------------------------------------------------------------------

DCR_TEST(epoch, stale_and_foreign_authority_is_rejected) {
  dcrtest::Fixture fixture("epoch-check");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  DCR_REQUIRE_OK(const EpochIssuerId issuer, EpochIssuerId::parse("cpe-primary"));
  DCR_REQUIRE_OK(const EpochIssuerId foreign_issuer, EpochIssuerId::parse("cpe-other"));
  DCR_REQUIRE_OK(const EpochToken epoch_five, EpochToken::make(issuer, 5));
  DCR_REQUIRE_OK(const EpochToken epoch_six, EpochToken::make(issuer, 6));
  DCR_REQUIRE_OK(const EpochToken epoch_four, EpochToken::make(issuer, 4));
  DCR_REQUIRE_OK(const EpochToken foreign, EpochToken::make(foreign_issuer, 99));

  auto first = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                        "site-east");
  first.context.precondition.asserted_epoch = epoch_five;
  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(first));
  DCR_CHECK(created.status == MutationStatus::created);
  DCR_CHECK(registry.external_epoch().has_value());
  DCR_CHECK(*registry.external_epoch() == epoch_five);
  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-alpha")));
  DCR_CHECK(record.last_provenance.epoch().has_value());
  DCR_CHECK(*record.last_provenance.epoch() == epoch_five);

  // An older token is stale authority.
  auto stale = fixture.register_command("dc-beta", "Beta", LifecycleState::registered, "site-east");
  stale.context.precondition.asserted_epoch = epoch_four;
  DCR_REQUIRE_ERROR(ErrorCode::stale_authority, registry.register_data_center(stale));

  // A token from another authority cannot be compared, and is refused.
  auto foreign_command = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                                  "site-east");
  foreign_command.context.precondition.asserted_epoch = foreign;
  DCR_REQUIRE_ERROR(ErrorCode::stale_authority, registry.register_data_center(foreign_command));

  // A newer token from the recorded authority is accepted and recorded.
  auto newer = fixture.register_command("dc-beta", "Beta", LifecycleState::registered, "site-east");
  newer.context.precondition.asserted_epoch = epoch_six;
  DCR_REQUIRE_OK(const MutationResult accepted, registry.register_data_center(newer));
  DCR_CHECK(accepted.status == MutationStatus::created);
  DCR_CHECK(*registry.external_epoch() == epoch_six);

  // A mutation with no assertion keeps the recorded epoch.
  DCR_REQUIRE_OK(const MutationResult third,
                 registry.register_data_center(fixture.register_command(
                     "dc-gamma", "Gamma", LifecycleState::registered, "site-east")));
  DCR_CHECK(third.status == MutationStatus::created);
  DCR_CHECK(*registry.external_epoch() == epoch_six);
}

// ---------------------------------------------------------------------------
// Queries and enumeration
// ---------------------------------------------------------------------------

DCR_TEST(queries, enumeration_orders_and_filters_are_deterministic) {
  dcrtest::Fixture fixture("query");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  const std::vector<std::string> names = {"Delta", "Alpha", "Charlie", "Bravo"};
  const std::vector<std::string> ids = {"dc-four", "dc-one", "dc-three", "dc-two"};
  for (std::size_t index = 0; index < ids.size(); ++index) {
    auto command = fixture.register_command(ids[index], names[index], LifecycleState::registered,
                                            index % 2 == 0 ? "site-east" : "site-west");
    command.draft.facility.region = *RegionCode::parse("us-east-1");
    DCR_REQUIRE_OK(const MutationResult unused, registry.register_data_center(command));
  }

  const EnumerationQuery canonical;
  DCR_REQUIRE_OK(const std::vector<DataCenterRecord> by_id, registry.enumerate(canonical));
  DCR_CHECK_EQ(std::size_t{4}, by_id.size());
  const std::vector<std::string> expected_ids = {"dc-four", "dc-one", "dc-three", "dc-two"};
  DCR_CHECK(ids_of(by_id) == expected_ids);

  EnumerationQuery by_name;
  by_name.order = EnumerationOrder::display_name_ascending;
  DCR_REQUIRE_OK(const std::vector<DataCenterRecord> named, registry.enumerate(by_name));
  const std::vector<std::string> expected_names = {"Alpha", "Bravo", "Charlie", "Delta"};
  std::vector<std::string> actual_names;
  for (const auto& record : named) {
    actual_names.push_back(record.display_name);
  }
  DCR_CHECK(actual_names == expected_names);

  EnumerationQuery by_revision;
  by_revision.order = EnumerationOrder::revision_descending;
  DCR_REQUIRE_OK(const std::vector<DataCenterRecord> revisions, registry.enumerate(by_revision));
  // All revisions are 1, so the tie-break is canonical identity.
  DCR_CHECK(ids_of(revisions) == expected_ids);

  EnumerationQuery by_site;
  by_site.site = fixture.site("site-east");
  DCR_REQUIRE_OK(const std::vector<DataCenterRecord> east, registry.enumerate(by_site));
  DCR_CHECK_EQ(std::size_t{2}, east.size());
  DCR_CHECK_EQ(std::string("dc-four"), east[0].id.value());
  DCR_CHECK_EQ(std::string("dc-three"), east[1].id.value());

  EnumerationQuery paged;
  paged.limit = 2;
  paged.offset = 1;
  DCR_REQUIRE_OK(const std::vector<DataCenterId> page, registry.enumerate_ids(paged));
  DCR_CHECK_EQ(std::size_t{2}, page.size());
  DCR_CHECK_EQ(std::string("dc-one"), page[0].value());
  DCR_CHECK_EQ(std::string("dc-three"), page[1].value());

  EnumerationQuery by_region;
  by_region.region = *RegionCode::parse("us-east-1");
  DCR_REQUIRE_OK(const std::vector<DataCenterRecord> regional, registry.enumerate(by_region));
  DCR_CHECK_EQ(std::size_t{4}, regional.size());

  EnumerationQuery absent_region;
  absent_region.region = *RegionCode::parse("eu-west-9");
  DCR_REQUIRE_OK(const std::vector<DataCenterRecord> nothing, registry.enumerate(absent_region));
  DCR_CHECK(nothing.empty());

  EnumerationQuery too_many;
  too_many.limit = static_cast<std::size_t>(registry.limits().max_query_results) + 1;
  DCR_REQUIRE_ERROR(ErrorCode::invalid_query, registry.enumerate(too_many));

  EnumerationQuery by_state;
  by_state.state = LifecycleState::active;
  DCR_REQUIRE_OK(const std::vector<DataCenterRecord> active, registry.enumerate(by_state));
  DCR_CHECK(active.empty());

  // Display names are not unique, so a name lookup returns every match.
  DCR_CHECK_EQ(std::size_t{1}, registry.find_by_display_name("Alpha").size());
  DCR_CHECK(registry.find_by_display_name("Nothing").empty());

  DCR_REQUIRE_ERROR(ErrorCode::not_found, registry.get(fixture.id("dc-absent")));
  DCR_REQUIRE_ERROR(ErrorCode::not_found, registry.find_by_alias(fixture.alias("no-such-alias")));
  DCR_CHECK(!registry.find(fixture.id("dc-absent")).has_value());
}

DCR_TEST(queries, snapshots_are_immutable_and_ordered) {
  dcrtest::Fixture fixture("snapshot");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  DCR_REQUIRE_OK(const MutationResult unused,
                 registry.register_data_center(fixture.register_command(
                     "dc-alpha", "Alpha", LifecycleState::registered, "site-east")));

  DCR_REQUIRE_OK(const RegistrySnapshot before, registry.snapshot());
  DCR_CHECK_EQ(std::uint64_t{1}, before.generation().value());
  DCR_CHECK_EQ(std::uint64_t{1}, before.record_count());
  DCR_CHECK(!before.digest().is_zero());

  DCR_REQUIRE_OK(const MutationResult second,
                 registry.register_data_center(fixture.register_command(
                     "dc-beta", "Beta", LifecycleState::registered, "site-east")));
  DCR_CHECK(second.status == MutationStatus::created);

  // The snapshot still describes generation 1.
  DCR_CHECK_EQ(std::uint64_t{1}, before.generation().value());
  DCR_CHECK_EQ(std::uint64_t{1}, before.record_count());
  DCR_CHECK(!before.find(fixture.id("dc-beta")).has_value());

  DCR_REQUIRE_OK(const RegistrySnapshot after, registry.snapshot());
  DCR_CHECK_EQ(std::uint64_t{2}, after.generation().value());
  DCR_CHECK_EQ(std::uint64_t{2}, after.record_count());
  DCR_CHECK(before != after);
  DCR_CHECK(before.digest() != after.digest());

  const RegistryStats stats = after.stats();
  DCR_CHECK_EQ(std::uint64_t{2}, stats.record_count);
  DCR_CHECK_EQ(std::uint64_t{2}, stats.count_of(LifecycleState::registered));
  DCR_CHECK_EQ(std::uint64_t{0}, stats.count_of(LifecycleState::retired));
  DCR_CHECK_EQ(std::uint64_t{2}, stats.membership_count);
  DCR_CHECK_EQ(std::uint64_t{1}, stats.distinct_site_count);
  DCR_CHECK_EQ(std::uint64_t{2}, stats.history_entries);
  DCR_CHECK(stats.snapshot_payload_bytes > 0);
  DCR_CHECK(stats.snapshot_digest == after.digest());
}

// ---------------------------------------------------------------------------
// History and provenance
// ---------------------------------------------------------------------------

DCR_TEST(history, records_every_committed_change_in_sequence_order) {
  dcrtest::Fixture fixture("history");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  DCR_REQUIRE_OK(const MutationResult created,
                 registry.register_data_center(fixture.register_command(
                     "dc-alpha", "Alpha", LifecycleState::registered, "site-east")));

  UpdateMetadataCommand update;
  update.context = fixture.context_at(registry, "service-account");
  update.id = fixture.id("dc-alpha");
  update.patch.display_name = FieldPatch<std::string>::set("Alpha Two");
  DCR_REQUIRE_OK(const MutationResult updated, registry.update_metadata(update));

  TransitionCommand transition;
  transition.context = fixture.context_at(registry);
  transition.id = fixture.id("dc-alpha");
  transition.target_state = LifecycleState::active;
  transition.reason = *ReasonCode::parse("commissioned");
  DCR_REQUIRE_OK(const MutationResult transitioned, registry.transition_lifecycle(transition));

  HistoryQuery query;
  const HistoryPage page = registry.history(query);
  DCR_CHECK_EQ(std::uint64_t{3}, page.matched_total);
  DCR_CHECK_EQ(std::size_t{3}, page.entries.size());
  DCR_CHECK(!page.truncated);
  DCR_CHECK_EQ(std::uint64_t{0}, page.dropped_entries);
  DCR_CHECK(page.earliest_retained_sequence.has_value());
  DCR_CHECK_EQ(std::uint64_t{1}, page.earliest_retained_sequence->value());

  DCR_CHECK(page.entries[0].action == HistoryAction::registered);
  DCR_CHECK(!page.entries[0].previous_state.has_value());
  DCR_CHECK(page.entries[0].new_state == LifecycleState::registered);
  DCR_CHECK(page.entries[1].action == HistoryAction::metadata_updated);
  DCR_CHECK_EQ(std::string("service-account"),
               page.entries[1].provenance.principal().value());
  DCR_CHECK(page.entries[2].action == HistoryAction::lifecycle_transition);
  DCR_CHECK(page.entries[2].previous_state.has_value());
  DCR_CHECK(*page.entries[2].previous_state == LifecycleState::registered);
  DCR_CHECK(page.entries[2].new_state == LifecycleState::active);
  DCR_CHECK(page.entries[2].provenance.reason().has_value());
  DCR_CHECK_EQ(std::string("commissioned"), page.entries[2].provenance.reason()->value());

  for (std::size_t index = 1; index < page.entries.size(); ++index) {
    DCR_CHECK(page.entries[index - 1].sequence < page.entries[index].sequence);
    DCR_CHECK(page.entries[index - 1].generation <= page.entries[index].generation);
  }

  HistoryQuery filtered;
  filtered.action = HistoryAction::lifecycle_transition;
  const HistoryPage actions = registry.history(filtered);
  DCR_CHECK_EQ(std::uint64_t{1}, actions.matched_total);

  HistoryQuery by_id;
  by_id.id = fixture.id("dc-alpha");
  const HistoryPage by_record = registry.history(by_id);
  DCR_CHECK_EQ(std::uint64_t{3}, by_record.matched_total);

  HistoryQuery paged;
  paged.limit = 2;
  const HistoryPage first_page = registry.history(paged);
  DCR_CHECK_EQ(std::size_t{2}, first_page.entries.size());
  DCR_CHECK(first_page.truncated);
  paged.offset = 2;
  const HistoryPage second_page = registry.history(paged);
  DCR_CHECK_EQ(std::size_t{1}, second_page.entries.size());
  DCR_CHECK(!second_page.truncated);

  HistoryQuery from_generation;
  from_generation.from_generation = *RegistryGeneration::from_value(3);
  const HistoryPage recent = registry.history(from_generation);
  DCR_CHECK_EQ(std::uint64_t{1}, recent.matched_total);
}

DCR_TEST(history, window_is_bounded_and_accounting_is_exact) {
  dcrtest::Fixture fixture("history-bound");
  RegistryLimits limits = RegistryLimits::defaults();
  limits.max_history_entries = 5;
  fixture.set_limits(limits);
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  for (std::uint64_t index = 0; index < 10; ++index) {
    DCR_REQUIRE_OK(const MutationResult created,
                   registry.register_data_center(fixture.register_command(
                       dcrtest::data_center_id_text(index), "Facility " + std::to_string(index),
                       LifecycleState::registered, "site-east")));
    DCR_CHECK(created.status == MutationStatus::created);
  }

  const RegistryStats stats = registry.stats();
  DCR_CHECK_EQ(std::uint64_t{5}, stats.history_entries);
  DCR_CHECK_EQ(std::uint64_t{5}, stats.history_dropped);

  const HistoryPage page = registry.history(HistoryQuery{});
  DCR_CHECK_EQ(std::size_t{5}, page.entries.size());
  DCR_CHECK_EQ(std::uint64_t{5}, page.dropped_entries);
  DCR_CHECK(page.earliest_retained_sequence.has_value());
  DCR_CHECK_EQ(std::uint64_t{6}, page.earliest_retained_sequence->value());
  // The registry's own sequence counter never rolls back, even though the
  // retained window starts later.
  DCR_CHECK_EQ(std::uint64_t{10}, registry.generation().value());
}

// ---------------------------------------------------------------------------
// Provenance sources
// ---------------------------------------------------------------------------

DCR_TEST(provenance, callers_cannot_claim_registry_sources) {
  dcrtest::Fixture fixture("prov-source");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  auto command = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                          "site-east");
  command.context.provenance.source = ProvenanceSource::recovery;
  DCR_REQUIRE_ERROR(ErrorCode::invalid_provenance, registry.register_data_center(command));

  auto oversize = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                           "site-east");
  oversize.context.provenance.detail =
      std::string(registry.limits().max_provenance_detail_bytes + 1, 'x');
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, registry.register_data_center(oversize));

  auto control = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                          "site-east");
  control.context.provenance.detail = std::string("bad\x01") + "detail";
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.register_data_center(control));

  auto cli = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered, "site-east");
  cli.context.provenance.source = ProvenanceSource::cli;
  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(cli));
  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-alpha")));
  DCR_CHECK(record.last_provenance.source() == ProvenanceSource::cli);
}

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

DCR_TEST(bounds, record_limit_is_enforced_before_any_write) {
  dcrtest::Fixture fixture("bounds-records");
  RegistryLimits limits = RegistryLimits::defaults();
  limits.max_records = 3;
  limits.max_query_results = 3;
  fixture.set_limits(limits);
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  for (std::uint64_t index = 0; index < 3; ++index) {
    DCR_REQUIRE_OK(const MutationResult created,
                   registry.register_data_center(fixture.register_command(
                       dcrtest::data_center_id_text(index), "Facility " + std::to_string(index),
                       LifecycleState::registered, "site-east")));
    DCR_CHECK(created.status == MutationStatus::created);
  }
  const Sha256Digest digest = registry.snapshot_digest();
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded,
                    registry.register_data_center(fixture.register_command(
                        "dc-overflow", "Overflow", LifecycleState::registered, "site-east")));
  DCR_CHECK(registry.snapshot_digest() == digest);
  DCR_CHECK_EQ(std::uint64_t{3}, registry.record_count());
}

DCR_TEST(bounds, per_record_collections_are_bounded) {
  dcrtest::Fixture fixture("bounds-collections");
  RegistryLimits limits = RegistryLimits::defaults();
  limits.max_aliases_per_record = 2;
  limits.max_memberships_per_record = 2;
  fixture.set_limits(limits);
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  auto command = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                          "site-east");
  command.draft.aliases = {fixture.alias("alias-one"), fixture.alias("alias-two")};
  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(command));
  DCR_CHECK(created.status == MutationStatus::created);

  auto too_many = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                           "site-east");
  too_many.draft.aliases = {fixture.alias("beta-one"), fixture.alias("beta-two"),
                            fixture.alias("beta-three")};
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, registry.register_data_center(too_many));

  auto memberships = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                              "site-east");
  memberships.draft.memberships.push_back(
      SiteMembership{fixture.site("site-west"), MembershipRole::secondary});
  memberships.draft.memberships.push_back(
      SiteMembership{fixture.site("site-north"), MembershipRole::standby});
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, registry.register_data_center(memberships));
}

DCR_TEST(bounds, oversized_metadata_is_rejected) {
  dcrtest::Fixture fixture("bounds-metadata");
  RegistryLimits limits = RegistryLimits::defaults();
  limits.max_metadata_value_bytes = 8;
  fixture.set_limits(limits);
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  auto command = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                          "site-east");
  command.draft.extensions = *MetadataMap::make(
      {{*MetadataKey::parse("key.one"), "123456789"}}, "extensions");
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, registry.register_data_center(command));
  DCR_CHECK_EQ(std::uint64_t{0}, registry.generation().value());
}
