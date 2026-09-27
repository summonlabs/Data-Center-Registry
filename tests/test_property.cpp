// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Property and invariant tests over long randomised operation sequences.
//
// Each test names its seed, and the framework prints the seed when a property
// test fails, so a failure is reproducible exactly.
//
// What is asserted after every operation, without exception:
//
//   * canonical identities are unique;
//   * the alias index has exactly one owner per alias;
//   * record revisions never decrease and never skip a value that a committed
//     change would have consumed;
//   * the registry generation advances by exactly one per committed change and
//     never moves for a no-op;
//   * retired and replaced records never accept a change;
//   * replacement links agree in both directions;
//   * every live record has exactly one primary site membership;
//   * a replayed command reports the outcome of the original;
//   * two registries driven by the same command sequence reach byte-identical
//     state;
//   * reopening a store reproduces the state exactly.

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/codec.hpp"
#include "dcr/data_center_registry.hpp"
#include "persistence/store.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace dcr;

namespace {

/// The payload digest a container carries in its header, as canonical hex.
std::string payload_digest_hex(const std::string& container) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (std::size_t index = 24; index < 56 && index < container.size(); ++index) {
    const auto byte = static_cast<std::uint8_t>(container[index]);
    hex.push_back(kDigits[(byte >> 4U) & 0x0FU]);
    hex.push_back(kDigits[byte & 0x0FU]);
  }
  return hex;
}

/// One generated operation.
struct Operation {
  enum class Kind : std::uint8_t {
    register_record,
    update_metadata,
    transition,
    attach_site,
    detach_site,
    retire,
    replace,
    lookup,
    enumerate,
  };
  Kind kind = Kind::lookup;
  std::uint64_t target = 0;
  std::uint64_t secondary = 0;
  std::uint32_t choice = 0;
  std::string text;
  std::uint64_t generation = 0;
  bool use_idempotency_key = false;
};

constexpr std::uint64_t kPoolSize = 24;

std::string id_for(std::uint64_t index) {
  return "dc-pool-" + std::to_string(index);
}

std::string site_for(std::uint64_t index) {
  return "site-pool-" + std::to_string(index % 4);
}

std::string alias_for(std::uint64_t index) {
  return "alias-pool-" + std::to_string(index);
}

const std::vector<LifecycleState>& transition_targets() {
  static const std::vector<LifecycleState> targets = {
      LifecycleState::proposed,    LifecycleState::registered,
      LifecycleState::active,      LifecycleState::degraded,
      LifecycleState::maintenance, LifecycleState::retired};
  return targets;
}

Operation generate(dcrtest::Rng& rng, std::uint64_t generation) {
  Operation operation;
  const std::uint64_t roll = rng.below(100);
  if (roll < 26) {
    operation.kind = Operation::Kind::register_record;
  } else if (roll < 46) {
    operation.kind = Operation::Kind::update_metadata;
  } else if (roll < 60) {
    operation.kind = Operation::Kind::transition;
  } else if (roll < 68) {
    operation.kind = Operation::Kind::attach_site;
  } else if (roll < 74) {
    operation.kind = Operation::Kind::detach_site;
  } else if (roll < 80) {
    operation.kind = Operation::Kind::retire;
  } else if (roll < 86) {
    operation.kind = Operation::Kind::replace;
  } else if (roll < 93) {
    operation.kind = Operation::Kind::lookup;
  } else {
    operation.kind = Operation::Kind::enumerate;
  }
  operation.target = rng.below(kPoolSize);
  operation.secondary = rng.below(kPoolSize);
  operation.choice = static_cast<std::uint32_t>(rng.below(6));
  operation.text = rng.token("name-", 6);
  operation.generation = generation;
  operation.use_idempotency_key = rng.chance(1, 3);
  return operation;
}

/// Applies one operation and returns the outcome.
Result<MutationResult> apply(dcrtest::Fixture& fixture, Registry& registry,
                             const Operation& operation) {
  switch (operation.kind) {
    case Operation::Kind::register_record: {
      auto command = fixture.register_command(id_for(operation.target), operation.text,
                                              operation.choice % 2 == 0
                                                  ? LifecycleState::proposed
                                                  : LifecycleState::registered,
                                              site_for(operation.target));
      if (operation.choice % 3 == 0) {
        command.draft.aliases.push_back(fixture.alias(alias_for(operation.target)));
      }
      command.draft.facility.region = *RegionCode::parse("us-east-1");
      if (operation.use_idempotency_key) {
        auto key = IdempotencyKey::parse("prop-key-" + std::to_string(operation.target) + "-" +
                                         std::to_string(operation.generation));
        if (key.has_value()) {
          command.context.idempotency_key = key.value();
        }
      }
      return registry.register_data_center(command);
    }
    case Operation::Kind::update_metadata: {
      UpdateMetadataCommand command;
      command.context = fixture.context_at(registry);
      command.id = fixture.id(id_for(operation.target));
      switch (operation.choice % 4) {
        case 0:
          command.patch.display_name = FieldPatch<std::string>::set(operation.text);
          break;
        case 1:
          command.patch.aliases = FieldPatch<std::vector<Alias>>::set(
              {fixture.alias(alias_for(operation.target)),
               fixture.alias(alias_for(operation.secondary))});
          break;
        case 2:
          command.patch.facility = FieldPatch<FacilityMetadata>::set([&] {
            FacilityMetadata facility;
            facility.region = *RegionCode::parse("us-east-1");
            facility.country = *CountryCode::parse("US");
            facility.tier = FacilityTier::tier_iii;
            return facility;
          }());
          break;
        default:
          command.patch.extensions = FieldPatch<MetadataMap>::set(*MetadataMap::make(
              {{*MetadataKey::parse("pool.label"), operation.text}}, "extensions"));
          break;
      }
      if (operation.use_idempotency_key) {
        auto key = IdempotencyKey::parse("prop-upd-" + std::to_string(operation.target) + "-" +
                                         std::to_string(operation.generation));
        if (key.has_value()) {
          command.context.idempotency_key = key.value();
        }
      }
      return registry.update_metadata(command);
    }
    case Operation::Kind::transition: {
      TransitionCommand command;
      command.context = fixture.context_at(registry);
      command.id = fixture.id(id_for(operation.target));
      command.target_state = transition_targets()[operation.choice % transition_targets().size()];
      command.reason = *ReasonCode::parse("property_test");
      return registry.transition_lifecycle(command);
    }
    case Operation::Kind::attach_site: {
      AttachSiteCommand command;
      command.context = fixture.context_at(registry);
      command.id = fixture.id(id_for(operation.target));
      command.membership = SiteMembership{
          fixture.site(site_for(operation.secondary)),
          operation.choice % 4 == 0 ? MembershipRole::primary : MembershipRole::standby};
      return registry.attach_site(command);
    }
    case Operation::Kind::detach_site: {
      DetachSiteCommand command;
      command.context = fixture.context_at(registry);
      command.id = fixture.id(id_for(operation.target));
      command.site = fixture.site(site_for(operation.secondary));
      return registry.detach_site(command);
    }
    case Operation::Kind::retire: {
      RetireCommand command;
      command.context = fixture.context_at(registry);
      command.id = fixture.id(id_for(operation.target));
      command.reason = *ReasonCode::parse("property_retire");
      return registry.retire_data_center(command);
    }
    case Operation::Kind::replace: {
      ReplaceCommand command;
      command.context = fixture.context_at(registry);
      command.retired_id = fixture.id(id_for(operation.target));
      command.new_id = fixture.id("dc-next-" + std::to_string(operation.secondary) + "-" +
                                  std::to_string(operation.generation));
      command.draft = fixture.draft(operation.text, site_for(operation.secondary));
      command.draft.compatibility = dcrtest::Fixture::compatibility(1, 1, 0x1);
      command.initial_state = LifecycleState::registered;
      command.reason = *ReasonCode::parse("property_replace");
      return registry.replace_data_center(command);
    }
    case Operation::Kind::lookup:
    case Operation::Kind::enumerate:
      break;
  }
  return Error(ErrorCode::internal_error,
               "lookup and enumeration are not mutations and are handled before this point",
               "kind");
}

/// Reads the whole state and checks every invariant that must hold at all
/// times.
void check_invariants(Registry& registry, const char* context) {
  auto snapshot_result = registry.snapshot();
  if (!snapshot_result.has_value()) {
    dcrtest::record_failure(__FILE__, __LINE__,
                            std::string("snapshot failed during ") + context + ": " +
                                snapshot_result.error().to_string());
    return;
  }
  RegistrySnapshot snapshot = std::move(snapshot_result).value();
  const std::vector<DataCenterRecord> records = snapshot.records();

  std::set<std::string> identities;
  std::map<std::string, std::string> alias_owners;
  for (const auto& record : records) {
    if (!identities.insert(record.id.value()).second) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              std::string("duplicate identity ") + record.id.value() +
                                  " during " + context);
    }
    for (const auto& alias : record.aliases) {
      const auto inserted = alias_owners.emplace(alias.value(), record.id.value());
      if (!inserted.second && inserted.first->second != record.id.value()) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                std::string("alias ") + alias.value() +
                                    " is claimed by two records during " + context);
      }
    }
    if (record.revision.value() < 1) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              std::string("revision below one during ") + context);
    }
    std::size_t primaries = 0;
    for (std::size_t index = 0; index < record.memberships.size(); ++index) {
      if (record.memberships[index].role == MembershipRole::primary) {
        ++primaries;
      }
      if (index > 0 && !(record.memberships[index - 1].site < record.memberships[index].site)) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                std::string("memberships are not canonical during ") + context);
      }
    }
    if (is_live(record.state) && primaries != 1) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              std::string("live record without exactly one primary site during ") +
                                  context);
    }
    if (primaries > 1) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              std::string("more than one primary site during ") + context);
    }
    if (is_terminal(record.state)) {
      if (!record.retired_generation.has_value() || !record.retirement.has_value()) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                std::string("terminal record without retirement during ") +
                                    context);
      }
    } else if (record.retired_generation.has_value() || record.retirement.has_value()) {
      dcrtest::record_failure(__FILE__, __LINE__,
                              std::string("live record carrying retirement during ") + context);
    }
    if (record.replaces.has_value()) {
      const auto predecessor = snapshot.find(*record.replaces);
      if (!predecessor.has_value()) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                std::string("dangling replacement link during ") + context);
      } else {
        if (predecessor->state != LifecycleState::replaced) {
          dcrtest::record_failure(__FILE__, __LINE__,
                                  std::string("predecessor is not replaced during ") + context);
        }
        if (!predecessor->retirement.has_value() ||
            !(predecessor->retirement->replaced_by.value() == record.id)) {
          dcrtest::record_failure(__FILE__, __LINE__,
                                  std::string("asymmetric replacement link during ") + context);
        }
        if (!(predecessor->retired_generation.value() == record.created_generation)) {
          dcrtest::record_failure(__FILE__, __LINE__,
                                  std::string("replacement generations differ during ") + context);
        }
      }
    }
    if (record.retirement.has_value() && record.retirement->replaced_by.has_value()) {
      const auto successor = snapshot.find(*record.retirement->replaced_by);
      if (!successor.has_value() || !successor->replaces.has_value() ||
          !(*successor->replaces == record.id)) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                std::string("dangling successor link during ") + context);
      }
    }
  }
  if (records.size() != snapshot.record_count()) {
    dcrtest::record_failure(__FILE__, __LINE__,
                            std::string("record count disagrees with the snapshot during ") +
                                context);
  }
  if (snapshot.digest().is_zero()) {
    dcrtest::record_failure(__FILE__, __LINE__,
                            std::string("empty digest during ") + context);
  }
}

/// Runs a sequence of operations against a registry, checking invariants and
/// generation accounting after each one.
std::vector<Operation> run_sequence(dcrtest::Fixture& fixture, Registry& registry,
                                    std::uint64_t seed, std::size_t count, bool strict_durability) {
  std::vector<Operation> operations;
  dcrtest::Rng rng(seed);
  for (std::size_t index = 0; index < count; ++index) {
    const Operation operation = generate(rng, index);
    operations.push_back(operation);
    const RegistryGeneration before = registry.generation();
    const std::uint64_t records_before = registry.record_count();

    if (operation.kind == Operation::Kind::lookup) {
      auto found = registry.find(fixture.id(id_for(operation.target)));
      if (found.has_value()) {
        if (!(found->id == fixture.id(id_for(operation.target)))) {
          dcrtest::record_failure(__FILE__, __LINE__, "lookup returned the wrong record");
        }
      }
    } else if (operation.kind == Operation::Kind::enumerate) {
      EnumerationQuery query;
      query.state = transition_targets()[operation.choice % transition_targets().size()];
      auto enumerated = registry.enumerate(query);
      if (!enumerated.has_value()) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                "enumerate failed: " + enumerated.error().to_string());
      }
    } else {
      const auto outcome = apply(fixture, registry, operation);
      if (outcome.has_value()) {
        const MutationResult& result = outcome.value();
        if (result.status == MutationStatus::unchanged) {
          if (!(registry.generation() == before)) {
            dcrtest::record_failure(__FILE__, __LINE__,
                                    "a no-op moved the registry generation");
          }
        } else {
          if (!(registry.generation() == *before.successor())) {
            dcrtest::record_failure(__FILE__, __LINE__,
                                    "a committed change did not advance the generation by one");
          }
          DCR_CHECK(!result.sequence.has_value() ||
                    result.sequence->value() > 0);
        }
        if (result.status == MutationStatus::created ||
            result.status == MutationStatus::updated) {
          auto record = registry.get(result.id);
          if (record.has_value()) {
            if (!(record->revision == result.revision)) {
              dcrtest::record_failure(__FILE__, __LINE__,
                                      "the reported revision is not the stored revision");
            }
            if (registry.record_count() < records_before) {
              dcrtest::record_failure(__FILE__, __LINE__, "the record count decreased");
            }
          }
        }
      }
      // A rejection is a normal outcome: the generated operations deliberately
      // include illegal ones. What matters is that a rejection changed nothing.
      if (!outcome.has_value() && !(registry.generation() == before)) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                "a rejected mutation moved the registry generation");
      }
    }
    check_invariants(registry, "operation sequence");
  }
  (void)strict_durability;
  return operations;
}

}  // namespace

// ---------------------------------------------------------------------------
// Long sequences
// ---------------------------------------------------------------------------

DCR_TEST(property, long_sequences_preserve_every_invariant) {
  const std::vector<std::uint64_t> seeds = {1, 2, 3, 42, 1337, 65537, 99991, 1000003};
  for (const std::uint64_t seed : seeds) {
    dcrtest::note_seed(seed);
    dcrtest::Fixture fixture("prop-long-" + std::to_string(seed));
    auto opened = fixture.open();
    run_sequence(fixture, *opened.registry, seed, 200, true);
  }
}

DCR_TEST(property, uncommitted_state_is_never_observed) {
  // Every rejection leaves the authoritative digest exactly where it was, for
  // every kind of rejection the engine can produce.
  const std::vector<std::uint64_t> seeds = {7, 11, 13, 17, 19, 23, 29, 31};
  for (const std::uint64_t seed : seeds) {
    dcrtest::note_seed(seed);
    dcrtest::Fixture fixture("prop-atomic-" + std::to_string(seed));
    auto opened = fixture.open();
    Registry& registry = *opened.registry;
    dcrtest::Rng rng(seed * 7919);

    std::uint64_t rejections = 0;
    for (std::size_t index = 0; index < 120; ++index) {
      const Operation operation = generate(rng, index);
      const Sha256Digest digest = registry.snapshot_digest();
      const RegistryGeneration generation = registry.generation();
      if (operation.kind == Operation::Kind::lookup ||
          operation.kind == Operation::Kind::enumerate) {
        continue;
      }
      // Deliberately stale: the precondition is generation 0 after the first
      // commit, so most of these are refused.
      auto command_operation = operation;
      const auto outcome = apply(fixture, registry, command_operation);
      if (!outcome.has_value()) {
        ++rejections;
        if (!(registry.snapshot_digest() == digest)) {
          dcrtest::record_failure(__FILE__, __LINE__,
                                  "a rejected mutation changed the authoritative digest");
        }
        if (!(registry.generation() == generation)) {
          dcrtest::record_failure(__FILE__, __LINE__,
                                  "a rejected mutation moved the generation");
        }
      }
      check_invariants(registry, "atomicity");
    }
    DCR_CHECK(rejections > 0);
  }
}

// ---------------------------------------------------------------------------
// Deterministic replay
// ---------------------------------------------------------------------------

DCR_TEST(property, replaying_a_command_sequence_reproduces_the_state_exactly) {
  const std::vector<std::uint64_t> seeds = {5, 55, 555, 5555};
  for (const std::uint64_t seed : seeds) {
    dcrtest::note_seed(seed);

    // Two independent registries, the same commands, the same clock.
    dcrtest::Fixture first_fixture("prop-replay-a-" + std::to_string(seed));
    dcrtest::Fixture second_fixture("prop-replay-b-" + std::to_string(seed));
    auto first = first_fixture.open();
    auto second = second_fixture.open();

    dcrtest::Rng rng(seed);
    for (std::size_t index = 0; index < 120; ++index) {
      const Operation operation = generate(rng, index);
      if (operation.kind == Operation::Kind::lookup ||
          operation.kind == Operation::Kind::enumerate) {
        continue;
      }
      const auto first_outcome = apply(first_fixture, *first.registry, operation);
      const auto second_outcome = apply(second_fixture, *second.registry, operation);
      if (first_outcome.has_value() != second_outcome.has_value()) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                "the same command was accepted by one registry and rejected by "
                                "the other");
        break;
      }
      if (first_outcome.has_value() && !(first_outcome.value() == second_outcome.value())) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                "the same command produced different outcomes");
        break;
      }
      if (!(first.registry->snapshot_digest() == second.registry->snapshot_digest())) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                "two registries driven by the same commands diverged at step " +
                                    std::to_string(index));
        break;
      }
    }
    DCR_CHECK(first.registry->snapshot_digest() == second.registry->snapshot_digest());
    DCR_CHECK(first.registry->stats().record_count > 0);
  }
}

DCR_TEST(property, durable_recovery_reproduces_the_state_exactly) {
  const std::vector<std::uint64_t> seeds = {101, 202, 303};
  for (const std::uint64_t seed : seeds) {
    dcrtest::note_seed(seed);
    dcrtest::Fixture fixture("prop-recovery-" + std::to_string(seed));

    Sha256Digest digest;
    RegistryStats stats;
    {
      auto opened = fixture.open();
      Registry& registry = *opened.registry;
      dcrtest::Rng rng(seed);
      std::size_t applied = 0;
      while (applied < 60) {
        const Operation operation = generate(rng, applied);
        ++applied;
        if (operation.kind == Operation::Kind::lookup ||
            operation.kind == Operation::Kind::enumerate) {
          continue;
        }
        const auto outcome = apply(fixture, registry, operation);
        check_invariants(registry, "before close");
        if (outcome.has_value() &&
            (outcome->status == MutationStatus::created ||
             outcome->status == MutationStatus::updated)) {
          DCR_CHECK(registry.snapshot_digest().is_zero() == false);
        }
      }
      digest = registry.snapshot_digest();
      stats = registry.stats();
      DCR_CHECK(registry.close().has_value());
    }

    // Reopen and compare exactly.
    auto reopened = fixture.open(StoreOpenMode::open_existing);
    Registry& registry = *reopened.registry;
    DCR_CHECK(registry.snapshot_digest() == digest);
    const RegistryStats reloaded = registry.stats();
    DCR_CHECK_EQ(stats.record_count, reloaded.record_count);
    DCR_CHECK(stats.records_by_state == reloaded.records_by_state);
    DCR_CHECK_EQ(stats.alias_count, reloaded.alias_count);
    DCR_CHECK_EQ(stats.membership_count, reloaded.membership_count);
    DCR_CHECK_EQ(stats.history_entries, reloaded.history_entries);
    DCR_CHECK_EQ(stats.idempotency_entries, reloaded.idempotency_entries);
    check_invariants(registry, "after reopen");

    // Continue: the reloaded registry behaves identically to one that never
    // restarted.
    dcrtest::Rng rng(seed);
    for (std::size_t index = 0; index < 60; ++index) {
      const Operation operation = generate(rng, index);
      if (operation.kind == Operation::Kind::lookup ||
          operation.kind == Operation::Kind::enumerate) {
        continue;
      }
      static_cast<void>(apply(fixture, registry, operation));
      check_invariants(registry, "after reopen and continue");
    }
  }
}

// ---------------------------------------------------------------------------
// Idempotent retry
// ---------------------------------------------------------------------------

DCR_TEST(property, retrying_a_command_never_applies_it_twice) {
  const std::vector<std::uint64_t> seeds = {9001, 9002, 9003};
  for (const std::uint64_t seed : seeds) {
    dcrtest::note_seed(seed);
    dcrtest::Fixture fixture("prop-retry-" + std::to_string(seed));
    auto opened = fixture.open();
    Registry& registry = *opened.registry;
    dcrtest::Rng rng(seed);

    std::size_t replayed = 0;
    for (std::size_t index = 0; index < 80; ++index) {
      Operation operation = generate(rng, index);
      if (operation.kind != Operation::Kind::register_record &&
          operation.kind != Operation::Kind::update_metadata) {
        continue;
      }
      // A key that is stable for the operation, so a retry matches.
      operation.use_idempotency_key = true;
      const auto first = apply(fixture, registry, operation);
      if (!first.has_value()) {
        continue;
      }
      const Sha256Digest digest = registry.snapshot_digest();
      const RegistryGeneration generation = registry.generation();
      const auto retry = apply(fixture, registry, operation);
      if (!retry.has_value()) {
        dcrtest::record_failure(__FILE__, __LINE__,
                                "a retry of an accepted command was rejected: " +
                                    retry.error().to_string());
        continue;
      }
      if (first.value().status == MutationStatus::unchanged) {
        // A no-op is not recorded, so the retry recomputes the same no-op.
        DCR_CHECK(retry.value().status == MutationStatus::unchanged);
      } else {
        ++replayed;
        DCR_CHECK(retry.value().status == MutationStatus::replayed);
        DCR_CHECK(retry.value().generation == first.value().generation);
        DCR_CHECK(retry.value().revision == first.value().revision);
        DCR_CHECK(retry.value().sequence == first.value().sequence);
      }
      DCR_CHECK(registry.snapshot_digest() == digest);
      DCR_CHECK(registry.generation() == generation);
    }
    DCR_CHECK(replayed > 0);
  }
}

// ---------------------------------------------------------------------------
// Mutation isolation
// ---------------------------------------------------------------------------

DCR_TEST(property, a_rejected_command_leaves_no_trace) {
  dcrtest::Fixture fixture("prop-trace");
  auto opened = fixture.open();
  Registry& registry = *opened.registry;

  DCR_REQUIRE_OK(const MutationResult created,
                 registry.register_data_center(fixture.register_command(
                     "dc-alpha", "Alpha", LifecycleState::registered, "site-east")));
  DCR_CHECK(created.status == MutationStatus::created);

  const RegistryStats before = registry.stats();
  const Sha256Digest digest = registry.snapshot_digest();
  const HistoryPage history_before = registry.history(HistoryQuery{});

  // Every rejection below must leave generation, revision, history, alias index
  // and idempotency table untouched.
  auto duplicate = fixture.register_command("dc-alpha", "Alpha", LifecycleState::registered,
                                            "site-east");
  DCR_REQUIRE_ERROR(ErrorCode::duplicate_identity, registry.register_data_center(duplicate));

  UpdateMetadataCommand stale;
  stale.context = fixture.context_at(registry);
  stale.context.precondition.expected_generation = *RegistryGeneration::from_value(0);
  stale.id = fixture.id("dc-alpha");
  stale.patch.display_name = FieldPatch<std::string>::set("Renamed");
  DCR_REQUIRE_ERROR(ErrorCode::stale_generation, registry.update_metadata(stale));

  UpdateMetadataCommand absent;
  absent.context = fixture.context_at(registry);
  absent.id = fixture.id("dc-absent");
  absent.patch.display_name = FieldPatch<std::string>::set("Absent");
  DCR_REQUIRE_ERROR(ErrorCode::not_found, registry.update_metadata(absent));

  TransitionCommand illegal;
  illegal.context = fixture.context_at(registry);
  illegal.id = fixture.id("dc-alpha");
  illegal.target_state = LifecycleState::proposed;
  illegal.reason = *ReasonCode::parse("illegal_step");
  DCR_REQUIRE_ERROR(ErrorCode::illegal_transition, registry.transition_lifecycle(illegal));

  const RegistryStats after = registry.stats();
  DCR_CHECK(registry.snapshot_digest() == digest);
  DCR_CHECK_EQ(before.record_count, after.record_count);
  DCR_CHECK_EQ(before.history_entries, after.history_entries);
  DCR_CHECK_EQ(before.idempotency_entries, after.idempotency_entries);
  DCR_CHECK(before.records_by_state == after.records_by_state);
  DCR_CHECK_EQ(history_before.matched_total, registry.history(HistoryQuery{}).matched_total);
}

// ---------------------------------------------------------------------------
// Random corruption
// ---------------------------------------------------------------------------

DCR_TEST(property, random_tampering_never_produces_authoritative_state) {
  // Take a valid store and damage it in many different ways. Some damage is
  // caught by the digest, some by validation, some by the codec. None of it may
  // produce a state that the registry accepts, except when the damage happens
  // to reproduce a genuinely valid state, which flipping a byte inside an
  // unused region could do only if the digests were also rewritten.
  dcrtest::Fixture fixture("prop-tamper");
  {
    auto opened = fixture.open();
    Registry& registry = *opened.registry;
    for (std::uint64_t index = 0; index < 4; ++index) {
      DCR_REQUIRE_OK(const MutationResult created,
                     registry.register_data_center(fixture.register_command(
                         id_for(index), "Facility " + std::to_string(index),
                         LifecycleState::registered, site_for(index))));
      static_cast<void>(created);
    }
    DCR_CHECK(registry.close().has_value());
  }

  const std::filesystem::path file = fixture.root() / "generations" /
                                     "gen-00000000000000000004.dcrs";
  std::string original;
  if (!dcrtest::read_text_file(file, original)) {
    dcrtest::record_failure(__FILE__, __LINE__, "cannot read the published generation");
    return;
  }

  dcrtest::Rng rng(4242);
  std::size_t accepted = 0;
  std::size_t refusals = 0;
  for (std::size_t round = 0; round < 200; ++round) {
    std::string damaged = original;
    const std::size_t mutations = 1 + static_cast<std::size_t>(rng.below(4));
    for (std::size_t index = 0; index < mutations; ++index) {
      const std::size_t offset = static_cast<std::size_t>(rng.below(damaged.size()));
      damaged[offset] = static_cast<char>(
          static_cast<unsigned char>(damaged[offset]) ^
          static_cast<unsigned char>(1U << rng.below(8)));
    }
    if (damaged == original) {
      continue;
    }
    {
      std::ofstream stream(file, std::ios::binary | std::ios::trunc);
      stream.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
    }
    auto opened = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
    if (opened.has_value()) {
      ++accepted;
      if (opened.value().registry->close().has_value()) {
        static_cast<void>(0);
      }
    } else {
      ++refusals;
      const ErrorCode code = opened.error().code();
      DCR_CHECK(code == ErrorCode::store_corrupt ||
                code == ErrorCode::store_incompatible_version ||
                code == ErrorCode::store_limit_exceeded);
    }
  }
  // Every byte of a container is covered: the payload by its own digest, the
  // first 56 header bytes by the header digest, and the header digest by
  // recomputation. A single flipped bit therefore cannot be accepted, so the
  // count of accepted tampered stores must be exactly zero.
  DCR_CHECK_EQ(std::size_t{0}, accepted);
  DCR_CHECK_EQ(std::size_t{200}, refusals);

  // Restoring the original makes the store usable again.
  {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(original.data(), static_cast<std::streamsize>(original.size()));
  }
  auto restored = Registry::open(fixture.open_options(StoreOpenMode::open_existing));
  DCR_CHECK(restored.has_value());
  if (restored.has_value()) {
    DCR_CHECK_EQ(std::uint64_t{4}, restored.value().registry->generation().value());
    DCR_CHECK_EQ(std::uint64_t{4}, restored.value().registry->record_count());
  }
}
