// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Lifecycle state machine, site membership rules, retirement and replacement.

#include <string>
#include <vector>

#include "dcr/data_center_registry.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace dcr;

namespace {

/// Registers a live facility and returns its identity.
DataCenterId register_live(dcrtest::Fixture& fixture, Registry& registry,
                           std::string_view id_text, std::string_view site_text) {
  auto command = fixture.register_command(id_text, "Facility " + std::string(id_text),
                                          LifecycleState::registered, site_text);
  auto created = registry.register_data_center(command);
  if (!created.has_value()) {
    dcrtest::record_failure(__FILE__, __LINE__,
                            "register_live failed: " + created.error().to_string());
  }
  return fixture.id(id_text);
}

TransitionCommand transition_command(dcrtest::Fixture& fixture, Registry& registry,
                                     std::string_view id_text, LifecycleState target) {
  TransitionCommand command;
  command.context = fixture.context_at(registry);
  command.id = fixture.id(id_text);
  command.target_state = target;
  command.reason = *ReasonCode::parse("operator_request");
  return command;
}

}  // namespace

// ---------------------------------------------------------------------------
// Transitions
// ---------------------------------------------------------------------------

DCR_TEST(transitions, legal_paths_are_accepted_and_illegal_paths_are_rejected) {
  dcrtest::Fixture fixture("lifecycle-legal");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  const DataCenterId id = register_live(fixture, registry, "dc-alpha", "site-east");

  // registered -> active -> degraded -> maintenance -> active -> retired.
  const std::vector<LifecycleState> path = {
      LifecycleState::active, LifecycleState::degraded, LifecycleState::maintenance,
      LifecycleState::active, LifecycleState::retired};
  MetadataRevision expected_revision = MetadataRevision::minimum();
  for (const LifecycleState target : path) {
    DCR_REQUIRE_OK(const MutationResult result,
                   registry.transition_lifecycle(
                       transition_command(fixture, registry, "dc-alpha", target)));
    DCR_CHECK(result.status == MutationStatus::updated);
    expected_revision = *expected_revision.successor();
    DCR_CHECK(result.revision == expected_revision);
    DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(id));
    DCR_CHECK(record.state == target);
  }

  // retired is terminal.
  DCR_REQUIRE_ERROR(ErrorCode::terminal_state,
                    registry.transition_lifecycle(
                        transition_command(fixture, registry, "dc-alpha", LifecycleState::active)));
  // Repeating the current state is a no-op, not an error, even when terminal.
  DCR_REQUIRE_OK(const MutationResult unchanged,
                 registry.transition_lifecycle(
                     transition_command(fixture, registry, "dc-alpha", LifecycleState::retired)));
  DCR_CHECK(unchanged.status == MutationStatus::unchanged);
}

DCR_TEST(transitions, illegal_targets_report_legal_successors) {
  dcrtest::Fixture fixture("lifecycle-illegal");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  // A proposal cannot become active without being registered first.
  auto proposed = fixture.register_command("dc-alpha", "Alpha", LifecycleState::proposed, "");
  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(proposed));
  DCR_CHECK(created.status == MutationStatus::created);

  DCR_REQUIRE_ERROR(ErrorCode::illegal_transition,
                    registry.transition_lifecycle(
                        transition_command(fixture, registry, "dc-alpha", LifecycleState::active)));

  // `replaced` is unreachable through a transition, whatever the current state.
  DCR_REQUIRE_ERROR(ErrorCode::illegal_transition,
                    registry.transition_lifecycle(transition_command(
                        fixture, registry, "dc-alpha", LifecycleState::replaced)));

  // An unknown record is not found.
  DCR_REQUIRE_ERROR(ErrorCode::not_found,
                    registry.transition_lifecycle(transition_command(
                        fixture, registry, "dc-absent", LifecycleState::registered)));
}

DCR_TEST(transitions, becoming_live_requires_a_primary_site) {
  dcrtest::Fixture fixture("lifecycle-membership");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  auto proposed = fixture.register_command("dc-alpha", "Alpha", LifecycleState::proposed, "");
  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(proposed));
  DCR_CHECK(created.status == MutationStatus::created);

  // No primary membership yet, so registering would produce an inconsistent
  // live record and is refused.
  DCR_REQUIRE_ERROR(ErrorCode::membership_violation,
                    registry.transition_lifecycle(transition_command(
                        fixture, registry, "dc-alpha", LifecycleState::registered)));

  DCR_REQUIRE_OK(const DataCenterRecord before, registry.get(fixture.id("dc-alpha")));
  DCR_CHECK(before.state == LifecycleState::proposed);

  // Attaching a primary site makes it possible.
  AttachSiteCommand attach;
  attach.context = fixture.context_at(registry);
  attach.id = fixture.id("dc-alpha");
  attach.membership = SiteMembership{fixture.site("site-east"), MembershipRole::primary};
  DCR_REQUIRE_OK(const MutationResult attached, registry.attach_site(attach));
  DCR_CHECK(attached.status == MutationStatus::updated);

  DCR_REQUIRE_OK(const MutationResult registered,
                 registry.transition_lifecycle(transition_command(
                     fixture, registry, "dc-alpha", LifecycleState::registered)));
  DCR_CHECK(registered.status == MutationStatus::updated);
}

DCR_TEST(transitions, stale_generation_is_rejected) {
  dcrtest::Fixture fixture("lifecycle-stale");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  static_cast<void>(register_live(fixture, registry, "dc-alpha", "site-east"));

  TransitionCommand command = transition_command(fixture, registry, "dc-alpha",
                                                 LifecycleState::active);
  DCR_REQUIRE_OK(const MutationResult moved, registry.transition_lifecycle(command));
  DCR_CHECK(moved.status == MutationStatus::updated);

  // The same precondition, now overtaken, is stale for a command that would
  // really change something.
  TransitionCommand stale = transition_command(fixture, registry, "dc-alpha",
                                               LifecycleState::degraded);
  stale.context.precondition.expected_generation = *RegistryGeneration::from_value(1);
  DCR_REQUIRE_ERROR(ErrorCode::stale_generation, registry.transition_lifecycle(stale));

  // Repeating the state the record is already in is a no-op, whatever the
  // caller's generation.
  DCR_REQUIRE_OK(const MutationResult unchanged, registry.transition_lifecycle(command));
  DCR_CHECK(unchanged.status == MutationStatus::unchanged);
}

// ---------------------------------------------------------------------------
// Site membership
// ---------------------------------------------------------------------------

DCR_TEST(membership, attach_detach_rules) {
  dcrtest::Fixture fixture("membership-attach");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  const DataCenterId id = register_live(fixture, registry, "dc-alpha", "site-east");

  AttachSiteCommand secondary;
  secondary.context = fixture.context_at(registry);
  secondary.id = id;
  secondary.membership = SiteMembership{fixture.site("site-west"), MembershipRole::secondary};
  DCR_REQUIRE_OK(const MutationResult attached, registry.attach_site(secondary));
  DCR_CHECK(attached.status == MutationStatus::updated);

  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(id));
  DCR_CHECK_EQ(std::size_t{2}, record.memberships.size());
  // Canonical site order.
  DCR_CHECK_EQ(std::string("site-east"), record.memberships[0].site.value());
  DCR_CHECK_EQ(std::string("site-west"), record.memberships[1].site.value());

  // Attaching the same membership again changes nothing.
  AttachSiteCommand repeat = secondary;
  repeat.context = fixture.context_at(registry);
  DCR_REQUIRE_OK(const MutationResult unchanged, registry.attach_site(repeat));
  DCR_CHECK(unchanged.status == MutationStatus::unchanged);

  // Changing the role of an existing membership is an update.
  AttachSiteCommand promote = secondary;
  promote.context = fixture.context_at(registry);
  promote.membership->role = MembershipRole::standby;
  DCR_REQUIRE_OK(const MutationResult promoted, registry.attach_site(promote));
  DCR_CHECK(promoted.status == MutationStatus::updated);
  DCR_REQUIRE_OK(const DataCenterRecord after_promote, registry.get(id));
  DCR_CHECK(after_promote.membership_for(fixture.site("site-west")) != nullptr);
  DCR_CHECK(after_promote.membership_for(fixture.site("site-west"))->role ==
            MembershipRole::standby);

  // A second primary is refused while the record is live.
  AttachSiteCommand second_primary;
  second_primary.context = fixture.context_at(registry);
  second_primary.id = id;
  second_primary.membership = SiteMembership{fixture.site("site-north"), MembershipRole::primary};
  DCR_REQUIRE_ERROR(ErrorCode::membership_violation, registry.attach_site(second_primary));

  // Detaching a secondary is allowed.
  DetachSiteCommand detach_secondary;
  detach_secondary.context = fixture.context_at(registry);
  detach_secondary.id = id;
  detach_secondary.site = fixture.site("site-west");
  DCR_REQUIRE_OK(const MutationResult detached, registry.detach_site(detach_secondary));
  DCR_CHECK(detached.status == MutationStatus::updated);

  // Detaching the primary of a live record is refused.
  DetachSiteCommand detach_primary;
  detach_primary.context = fixture.context_at(registry);
  detach_primary.id = id;
  detach_primary.site = fixture.site("site-east");
  DCR_REQUIRE_ERROR(ErrorCode::membership_violation, registry.detach_site(detach_primary));

  // Detaching something that is not attached is not found.
  DetachSiteCommand detach_absent;
  detach_absent.context = fixture.context_at(registry);
  detach_absent.id = id;
  detach_absent.site = fixture.site("site-south");
  DCR_REQUIRE_ERROR(ErrorCode::not_found, registry.detach_site(detach_absent));

  // Detaching the primary is allowed once the record is retired, because a
  // retired record's history is not an operating claim.
  DCR_REQUIRE_OK(const MutationResult retired,
                 registry.transition_lifecycle(
                     transition_command(fixture, registry, "dc-alpha", LifecycleState::retired)));
  DCR_CHECK(retired.status == MutationStatus::updated);
  DetachSiteCommand detach_after_retirement;
  detach_after_retirement.context = fixture.context_at(registry);
  detach_after_retirement.id = id;
  detach_after_retirement.site = fixture.site("site-east");
  DCR_REQUIRE_ERROR(ErrorCode::terminal_state, registry.detach_site(detach_after_retirement));
}

DCR_TEST(membership, terminal_records_reject_membership_changes) {
  dcrtest::Fixture fixture("membership-terminal");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  const DataCenterId id = register_live(fixture, registry, "dc-alpha", "site-east");

  RetireCommand retire;
  retire.context = fixture.context_at(registry);
  retire.id = id;
  retire.reason = *ReasonCode::parse("decommissioned");
  DCR_REQUIRE_OK(const MutationResult retired, registry.retire_data_center(retire));
  DCR_CHECK(retired.status == MutationStatus::updated);

  AttachSiteCommand attach;
  attach.context = fixture.context_at(registry);
  attach.id = id;
  attach.membership = SiteMembership{fixture.site("site-west"), MembershipRole::secondary};
  DCR_REQUIRE_ERROR(ErrorCode::terminal_state, registry.attach_site(attach));

  UpdateMetadataCommand update;
  update.context = fixture.context_at(registry);
  update.id = id;
  update.patch.display_name = FieldPatch<std::string>::set("Renamed After Retirement");
  DCR_REQUIRE_ERROR(ErrorCode::terminal_state, registry.update_metadata(update));
}

// ---------------------------------------------------------------------------
// Retirement
// ---------------------------------------------------------------------------

DCR_TEST(retirement, is_idempotent_and_terminal) {
  dcrtest::Fixture fixture("retire");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  const DataCenterId id = register_live(fixture, registry, "dc-alpha", "site-east");

  RetireCommand retire;
  retire.context = fixture.context_at(registry);
  retire.id = id;
  retire.reason = *ReasonCode::parse("decommissioned");
  DCR_REQUIRE_OK(const MutationResult retired, registry.retire_data_center(retire));
  DCR_CHECK(retired.status == MutationStatus::updated);

  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(id));
  DCR_CHECK(record.state == LifecycleState::retired);
  DCR_CHECK(record.retired_generation.has_value());
  DCR_CHECK(record.retirement.has_value());
  DCR_CHECK(record.retirement->reason.has_value());
  DCR_CHECK_EQ(std::string("decommissioned"), record.retirement->reason->value());
  DCR_CHECK(!record.retirement->replaced_by.has_value());
  DCR_CHECK(!record.replaces.has_value());

  const RegistryGeneration generation = registry.generation();
  const MetadataRevision revision = record.revision;

  // Retiring again is a no-op, even with a stale generation: the desired end
  // state already holds.
  RetireCommand again = retire;
  DCR_REQUIRE_OK(const MutationResult unchanged, registry.retire_data_center(again));
  DCR_CHECK(unchanged.status == MutationStatus::unchanged);
  DCR_CHECK(registry.generation() == generation);

  // The retired identity cannot be reused, and the record cannot come back.
  DCR_REQUIRE_ERROR(ErrorCode::duplicate_identity,
                    registry.register_data_center(fixture.register_command(
                        "dc-alpha", "Alpha Again", LifecycleState::registered, "site-east")));
  DCR_REQUIRE_ERROR(ErrorCode::terminal_state,
                    registry.transition_lifecycle(
                        transition_command(fixture, registry, "dc-alpha", LifecycleState::active)));
  DCR_REQUIRE_OK(const DataCenterRecord still, registry.get(id));
  DCR_CHECK(still.revision == revision);
}

DCR_TEST(retirement, a_proposal_can_be_abandoned) {
  dcrtest::Fixture fixture("retire-proposal");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  DCR_REQUIRE_OK(const MutationResult created,
                 registry.register_data_center(fixture.register_command(
                     "dc-alpha", "Alpha", LifecycleState::proposed, "")));
  DCR_CHECK(created.status == MutationStatus::created);

  RetireCommand retire;
  retire.context = fixture.context_at(registry);
  retire.id = fixture.id("dc-alpha");
  retire.reason = *ReasonCode::parse("proposal_withdrawn");
  DCR_REQUIRE_OK(const MutationResult retired, registry.retire_data_center(retire));
  DCR_CHECK(retired.status == MutationStatus::updated);
  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-alpha")));
  DCR_CHECK(record.state == LifecycleState::retired);
}

// ---------------------------------------------------------------------------
// Replacement
// ---------------------------------------------------------------------------

DCR_TEST(replacement, retires_and_creates_in_one_generation) {
  dcrtest::Fixture fixture("replace");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  const DataCenterId old_id = register_live(fixture, registry, "dc-old", "site-east");

  // Give the predecessor an alias and a richer compatibility key.
  UpdateMetadataCommand enrich;
  enrich.context = fixture.context_at(registry);
  enrich.id = old_id;
  enrich.patch.aliases = FieldPatch<std::vector<Alias>>::set({fixture.alias("legacy-name")});
  enrich.patch.compatibility = FieldPatch<CompatibilityKey>::set(
      dcrtest::Fixture::compatibility(1, 0, 0x1));
  DCR_REQUIRE_OK(const MutationResult enriched, registry.update_metadata(enrich));
  DCR_CHECK(enriched.status == MutationStatus::updated);

  const RegistryGeneration generation_before = registry.generation();
  const std::uint64_t history_before = registry.stats().history_entries;

  ReplaceCommand command;
  command.context = fixture.context_at(registry);
  command.retired_id = old_id;
  command.new_id = fixture.id("dc-new");
  command.draft = fixture.draft("New Facility", "site-east");
  // Claims the predecessor's alias, which transfers, plus a capability the
  // predecessor did not have.
  command.draft.aliases = {fixture.alias("legacy-name"), fixture.alias("fresh-name")};
  command.draft.compatibility = dcrtest::Fixture::compatibility(1, 3, 0x7);
  command.initial_state = LifecycleState::active;
  command.reason = *ReasonCode::parse("facility_rebuilt");

  DCR_REQUIRE_OK(const MutationResult result, registry.replace_data_center(command));
  DCR_CHECK(result.status == MutationStatus::created);
  DCR_CHECK(result.id == command.new_id);
  DCR_CHECK(result.related_id.has_value());
  DCR_CHECK(*result.related_id == old_id);
  DCR_CHECK_EQ(std::uint64_t{1}, result.revision.value());
  DCR_CHECK(result.generation.value() == generation_before.value() + 1);

  // Both records changed, in one generation.
  DCR_REQUIRE_OK(const DataCenterRecord predecessor, registry.get(old_id));
  DCR_CHECK(predecessor.state == LifecycleState::replaced);
  DCR_CHECK(predecessor.retirement.has_value());
  DCR_CHECK(predecessor.retirement->replaced_by.has_value());
  DCR_CHECK(*predecessor.retirement->replaced_by == command.new_id);
  DCR_CHECK(predecessor.retired_generation.has_value());
  DCR_CHECK(*predecessor.retired_generation == result.generation);
  // The transferred alias is gone from the predecessor and the unclaimed
  // aliases are untouched.
  DCR_CHECK(!predecessor.has_alias(fixture.alias("legacy-name")));

  DCR_REQUIRE_OK(const DataCenterRecord successor, registry.get(command.new_id.value()));
  DCR_CHECK(successor.state == LifecycleState::active);
  DCR_CHECK(successor.replaces.has_value());
  DCR_CHECK(*successor.replaces == old_id);
  DCR_CHECK(successor.created_generation == result.generation);
  DCR_CHECK_EQ(std::size_t{2}, successor.aliases.size());

  // The transferred alias now resolves to the successor.
  DCR_REQUIRE_OK(const DataCenterRecord by_alias,
                 registry.find_by_alias(fixture.alias("legacy-name")));
  DCR_CHECK(by_alias.id == command.new_id);

  // Both changes share one generation: two history entries, one generation.
  const HistoryPage history = registry.history(HistoryQuery{});
  DCR_CHECK_EQ(history_before + 2, history.matched_total);
  DCR_CHECK(history.entries[history.entries.size() - 2].action ==
            HistoryAction::replacement_retired);
  DCR_CHECK(history.entries[history.entries.size() - 1].action ==
            HistoryAction::replacement_created);
  DCR_CHECK(history.entries[history.entries.size() - 2].generation == result.generation);
  DCR_CHECK(history.entries[history.entries.size() - 1].generation == result.generation);
  DCR_CHECK(history.entries[history.entries.size() - 2].sequence <
            history.entries[history.entries.size() - 1].sequence);

  // Neither identity is reusable and neither state can move.
  DCR_REQUIRE_ERROR(ErrorCode::duplicate_identity,
                    registry.register_data_center(fixture.register_command(
                        "dc-new", "Duplicate", LifecycleState::registered, "site-east")));
  DCR_REQUIRE_ERROR(ErrorCode::terminal_state,
                    registry.retire_data_center([&] {
                      RetireCommand retire;
                      retire.context = fixture.context_at(registry);
                      retire.id = old_id;
                      retire.reason = *ReasonCode::parse("decommissioned");
                      return retire;
                    }()));
  DCR_REQUIRE_ERROR(ErrorCode::terminal_state,
                    registry.replace_data_center([&] {
                      ReplaceCommand second;
                      second.context = fixture.context_at(registry);
                      second.retired_id = old_id;
                      second.new_id = fixture.id("dc-third");
                      second.draft = fixture.draft("Third", "site-east");
                      second.reason = *ReasonCode::parse("facility_rebuilt");
                      return second;
                    }()));
}

DCR_TEST(replacement, refuses_incompatible_or_stale_commands) {
  dcrtest::Fixture fixture("replace-guards");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  const DataCenterId old_id = register_live(fixture, registry, "dc-old", "site-east");

  // The predecessor claims two capabilities, so a successor that drops one is
  // an incompatible replacement.
  UpdateMetadataCommand enrich;
  enrich.context = fixture.context_at(registry);
  enrich.id = old_id;
  enrich.patch.compatibility =
      FieldPatch<CompatibilityKey>::set(dcrtest::Fixture::compatibility(1, 0, 0x3));
  DCR_REQUIRE_OK(const MutationResult enriched, registry.update_metadata(enrich));
  DCR_CHECK(enriched.status == MutationStatus::updated);

  ReplaceCommand base;
  base.context = fixture.context_at(registry);
  base.retired_id = old_id;
  base.new_id = fixture.id("dc-new");
  base.draft = fixture.draft("New Facility", "site-east");
  base.draft.compatibility = dcrtest::Fixture::compatibility(2, 0, 0x3);
  base.reason = *ReasonCode::parse("facility_rebuilt");

  // A different major version cannot represent the predecessor.
  DCR_REQUIRE_ERROR(ErrorCode::incompatible_replacement, registry.replace_data_center(base));

  // Dropping a capability is refused too.
  ReplaceCommand weaker = base;
  weaker.draft.compatibility = dcrtest::Fixture::compatibility(1, 0, 0x1);
  DCR_REQUIRE_ERROR(ErrorCode::incompatible_replacement, registry.replace_data_center(weaker));

  // Replacing a record with itself is nonsense.
  ReplaceCommand itself = base;
  itself.draft.compatibility = dcrtest::Fixture::compatibility(1, 1, 0x3);
  itself.new_id = old_id;
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, registry.replace_data_center(itself));

  // Replacing an unknown record is not found.
  ReplaceCommand absent = base;
  absent.draft.compatibility = dcrtest::Fixture::compatibility(1, 1, 0x3);
  absent.retired_id = fixture.id("dc-absent");
  DCR_REQUIRE_ERROR(ErrorCode::not_found, registry.replace_data_center(absent));

  // A replacement is preconditioned on the predecessor, so a stale caller
  // cannot retire a facility that has moved on.
  ReplaceCommand compatible = base;
  compatible.draft.compatibility = dcrtest::Fixture::compatibility(1, 1, 0x3);
  DCR_REQUIRE_OK(const MutationResult done, registry.replace_data_center(compatible));
  DCR_CHECK(done.status == MutationStatus::created);

  ReplaceCommand stale = base;
  stale.draft.compatibility = dcrtest::Fixture::compatibility(1, 1, 0x3);
  stale.new_id = fixture.id("dc-later");
  DCR_REQUIRE_ERROR(ErrorCode::terminal_state, registry.replace_data_center(stale));

  // Nothing was written by any of the rejections above beyond the one success:
  // two records (the enrichment changed one of them) and one generation per
  // committed mutation.
  DCR_CHECK_EQ(std::uint64_t{2}, registry.record_count());
  DCR_CHECK_EQ(std::uint64_t{3}, registry.generation().value());
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

DCR_TEST(cancellation, a_cancelled_mutation_never_publishes) {
  dcrtest::Fixture fixture("cancel");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;
  static_cast<void>(register_live(fixture, registry, "dc-alpha", "site-east"));

  const RegistryGeneration generation = registry.generation();
  const Sha256Digest digest = registry.snapshot_digest();
  const std::uint64_t records = registry.record_count();

  CancellationSource source;
  source.cancel();
  auto command = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                          "site-east");
  command.context.cancellation = source.token();
  DCR_REQUIRE_ERROR(ErrorCode::cancelled, registry.register_data_center(command));

  DCR_CHECK(registry.generation() == generation);
  DCR_CHECK(registry.snapshot_digest() == digest);
  DCR_CHECK_EQ(records, registry.record_count());

  // A token from a source that has not cancelled anything is inert.
  CancellationSource live;
  auto permitted = fixture.register_command("dc-beta", "Beta", LifecycleState::registered,
                                            "site-east");
  permitted.context.cancellation = live.token();
  DCR_REQUIRE_OK(const MutationResult created, registry.register_data_center(permitted));
  DCR_CHECK(created.status == MutationStatus::created);

  // A default token can never be cancelled and never blocks.
  auto default_token = fixture.register_command("dc-gamma", "Gamma", LifecycleState::registered,
                                                "site-east");
  DCR_CHECK(!default_token.context.cancellation.can_be_cancelled());
  DCR_REQUIRE_OK(const MutationResult third, registry.register_data_center(default_token));
  DCR_CHECK(third.status == MutationStatus::created);
}
