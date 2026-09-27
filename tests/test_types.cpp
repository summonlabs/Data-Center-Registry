// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Foundational value types: strong identities, counters, epochs, timestamps,
// compatibility keys, the lifecycle table, error categories, digests and
// deterministic metadata.

#include <set>
#include <string>
#include <vector>

#include "dcr/data_center_registry.hpp"
#include "test_framework.hpp"

using namespace dcr;

namespace {

std::string digest_hex(std::string_view text) { return sha256_text(text).to_hex(); }

}  // namespace

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

DCR_TEST(sha256, known_vectors) {
  // FIPS 180-4 and the classic test vectors.
  DCR_CHECK_EQ(std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
               digest_hex(""));
  DCR_CHECK_EQ(std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
               digest_hex("abc"));
  DCR_CHECK_EQ(
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
      digest_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));

  std::string million_a(1000000, 'a');
  DCR_CHECK_EQ(
      std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
      digest_hex(million_a));
}

DCR_TEST(sha256, streaming_matches_one_shot) {
  // Exercises the padding boundaries: 55, 56, 63, 64, 65 and the block edges
  // around them.
  for (std::size_t length = 0; length <= 200; ++length) {
    std::string text(length, 'x');
    for (std::size_t index = 0; index < length; ++index) {
      text[index] = static_cast<char>('a' + (index % 26));
    }
    const Sha256Digest one_shot = sha256_text(text);
    for (std::size_t split = 0; split <= length; ++split) {
      Sha256 hasher;
      hasher.update(std::string_view(text).substr(0, split));
      hasher.update(std::string_view(text).substr(split));
      DCR_CHECK_EQ(one_shot.to_hex(), hasher.finish().to_hex());
    }
  }
}

DCR_TEST(sha256, hex_round_trip) {
  DCR_REQUIRE_OK(const Sha256Digest digest, Sha256Digest::from_hex(
                                                "00112233445566778899aabbccddeeff"
                                                "00112233445566778899aabbccddeeff"));
  DCR_CHECK_EQ(std::string("00112233445566778899aabbccddeeff"
                           "00112233445566778899aabbccddeeff"),
               digest.to_hex());
  DCR_CHECK(digest < *Sha256Digest::from_hex(std::string(64, 'f')));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Sha256Digest::from_hex("00"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Sha256Digest::from_hex(std::string(64, 'z')));
}

// ---------------------------------------------------------------------------
// Identity syntax
// ---------------------------------------------------------------------------

DCR_TEST(identity, canonical_data_center_ids) {
  DCR_CHECK(DataCenterId::parse("dc-ashburn-01").has_value());
  DCR_CHECK(DataCenterId::parse("dc-a-b").has_value());
  DCR_CHECK(DataCenterId::parse(std::string("dc-") + std::string(61, 'a')).has_value());

  // Rejected: wrong prefix, wrong case, bad characters, doubled hyphen, length.
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse("site-ashburn-01"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse("dc-Ashburn"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse("dc-ashburn_01"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse("dc-ash--burn"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse("dc--x"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse("dc-x"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse("dc-"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity,
                    DataCenterId::parse(std::string("dc-") + std::string(62, 'a')));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse(""));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse("dc-ash burn"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, DataCenterId::parse("dc-ash\nburn"));
}

DCR_TEST(identity, ordering_is_canonical_text_order) {
  DCR_REQUIRE_OK(const DataCenterId first, DataCenterId::parse("dc-alpha"));
  DCR_REQUIRE_OK(const DataCenterId second, DataCenterId::parse("dc-beta"));
  DCR_CHECK(first < second);
  DCR_CHECK(second > first);
  DCR_CHECK(first == *DataCenterId::parse("dc-alpha"));
  DCR_CHECK(first != second);
  DCR_CHECK_EQ(std::string("dc-alpha"), first.value());

  std::set<DataCenterId> ordered;
  ordered.insert(second);
  ordered.insert(first);
  DCR_CHECK(ordered.begin()->value() == "dc-alpha");
}

DCR_TEST(identity, families_are_distinct) {
  // The families are distinct types, so these are compile-time properties
  // rather than runtime ones. Instantiating the comparisons here would not
  // compile if the types were interchangeable; what is checked at runtime is
  // that each family enforces its own prefix.
  DCR_CHECK(SiteId::parse("site-ashburn-01").has_value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, SiteId::parse("dc-ashburn-01"));
  DCR_CHECK(OwnershipScopeId::parse("scope-platform").has_value());
  DCR_CHECK(EpochIssuerId::parse("cpe-primary").has_value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, EpochIssuerId::parse("cpe-primary-"));
  DCR_CHECK(IdempotencyKey::parse("retry-0001").has_value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, IdempotencyKey::parse("short"));
  DCR_CHECK(ReasonCode::parse("operator_request").has_value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, ReasonCode::parse("OperatorRequest"));
  DCR_CHECK(MetadataKey::parse("power.feed.a").has_value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, MetadataKey::parse("power..a"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, MetadataKey::parse(".power"));
  DCR_CHECK(PrincipalId::parse("operator@example.com").has_value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, PrincipalId::parse("-operator"));
  DCR_CHECK(Alias::parse("legacy-ashburn").has_value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, Alias::parse("ab"));
  DCR_CHECK(RegionCode::parse("us-east-1").has_value());
}

DCR_TEST(identity, country_codes_are_validated_against_iso_3166_1) {
  DCR_CHECK(CountryCode::parse("US").has_value());
  DCR_CHECK(CountryCode::parse("DE").has_value());
  DCR_CHECK(CountryCode::parse("ZZ").has_value() == false);
  DCR_CHECK(CountryCode::parse("XX").has_value() == false);
  DCR_CHECK(CountryCode::parse("us").has_value() == false);
  DCR_CHECK(CountryCode::parse("USA").has_value() == false);
  DCR_CHECK(CountryCode::is_assigned("GB"));
  DCR_CHECK(!CountryCode::is_assigned("Q1"));
}

// ---------------------------------------------------------------------------
// Counters and epochs
// ---------------------------------------------------------------------------

DCR_TEST(counters, monotonic_and_checked) {
  DCR_CHECK_EQ(std::uint64_t{0}, RegistryGeneration::minimum().value());
  DCR_CHECK_EQ(std::uint64_t{1}, MetadataRevision::minimum().value());
  DCR_CHECK_EQ(std::uint64_t{0}, SequenceNumber::minimum().value());

  DCR_REQUIRE_OK(const RegistryGeneration first, RegistryGeneration::minimum().successor());
  DCR_CHECK_EQ(std::uint64_t{1}, first.value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, MetadataRevision::from_value(0));

  DCR_REQUIRE_OK(const RegistryGeneration exhausted,
                 RegistryGeneration::from_value(UINT64_MAX));
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, exhausted.successor());

  DCR_REQUIRE_OK(const RegistryGeneration parsed, RegistryGeneration::parse("42"));
  DCR_CHECK_EQ(std::uint64_t{42}, parsed.value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, RegistryGeneration::parse("042"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, RegistryGeneration::parse("-1"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, RegistryGeneration::parse(""));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, RegistryGeneration::parse("1x"));
}

DCR_TEST(epochs, compare_only_within_one_issuer) {
  DCR_REQUIRE_OK(const EpochIssuerId primary, EpochIssuerId::parse("cpe-primary"));
  DCR_REQUIRE_OK(const EpochIssuerId secondary, EpochIssuerId::parse("cpe-secondary"));
  DCR_REQUIRE_OK(const EpochToken older, EpochToken::make(primary, 7));
  DCR_REQUIRE_OK(const EpochToken newer, EpochToken::make(primary, 9));
  DCR_REQUIRE_OK(const EpochToken other, EpochToken::make(secondary, 1));

  DCR_REQUIRE(older.compare(newer).has_value());
  DCR_CHECK(*older.compare(newer) == std::strong_ordering::less);
  DCR_CHECK(*newer.compare(older) == std::strong_ordering::greater);
  DCR_CHECK(*older.compare(older) == std::strong_ordering::equal);
  // Tokens from different authorities have no defined order.
  DCR_CHECK(!older.compare(other).has_value());

  DCR_CHECK_EQ(std::string("cpe-primary@7"), older.to_string());
  DCR_REQUIRE_OK(const EpochToken parsed, EpochToken::parse("cpe-primary@7"));
  DCR_CHECK(parsed == older);
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, EpochToken::parse("cpe-primary"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, EpochToken::parse("cpe-primary@"));
  // An unknown issuer is an identity error, which is more precise than a generic parse error.
  DCR_REQUIRE_ERROR(ErrorCode::invalid_identity, EpochToken::parse("nope@1"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, EpochToken::parse("cpe-primary@01"));
}

// ---------------------------------------------------------------------------
// Timestamps
// ---------------------------------------------------------------------------

DCR_TEST(timestamps, rfc3339_round_trip) {
  DCR_REQUIRE_OK(const Timestamp epoch, Timestamp::from_unix_millis(0));
  DCR_CHECK_EQ(std::string("1970-01-01T00:00:00.000Z"), epoch.to_string());

  // 2024-02-29T12:34:56.789Z, a leap day.
  const std::int64_t millis = 1709210096789LL;
  DCR_REQUIRE_OK(const Timestamp leap, Timestamp::from_unix_millis(millis));
  DCR_CHECK_EQ(std::string("2024-02-29T12:34:56.789Z"), leap.to_string());
  DCR_REQUIRE_OK(const Timestamp parsed, Timestamp::parse("2024-02-29T12:34:56.789Z"));
  DCR_CHECK_EQ(millis, parsed.unix_millis());

  DCR_REQUIRE_OK(const Timestamp maximum, Timestamp::from_unix_millis(Timestamp::kMaxMillis));
  DCR_CHECK_EQ(std::string("9999-12-31T23:59:59.999Z"), maximum.to_string());

  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Timestamp::from_unix_millis(-1));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Timestamp::from_unix_millis(Timestamp::kMaxMillis + 1));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Timestamp::parse("2023-02-29T00:00:00.000Z"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Timestamp::parse("2024-13-01T00:00:00.000Z"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Timestamp::parse("2024-01-01T24:00:00.000Z"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Timestamp::parse("2024-01-01T00:00:00Z"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, Timestamp::parse(""));
}

DCR_TEST(timestamps, manual_clock_advances_by_whole_milliseconds) {
  DCR_REQUIRE_OK(const Timestamp start, Timestamp::from_unix_millis(1000));
  ManualClock clock(start);
  DCR_CHECK_EQ(std::int64_t{1000}, clock.now().unix_millis());
  DCR_CHECK_OK(clock.advance_millis(500));
  DCR_CHECK_EQ(std::int64_t{1500}, clock.now().unix_millis());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, clock.advance_millis(-1));
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, clock.advance_millis(INT64_MAX));
}

// ---------------------------------------------------------------------------
// Compatibility
// ---------------------------------------------------------------------------

DCR_TEST(compatibility, relation_and_replacement_rules) {
  DCR_REQUIRE_OK(const CompatibilityKey base, CompatibilityKey::make(1, 0, 0x3));
  DCR_REQUIRE_OK(const CompatibilityKey same, CompatibilityKey::make(1, 0, 0x3));
  DCR_REQUIRE_OK(const CompatibilityKey superset, CompatibilityKey::make(1, 2, 0x7));
  DCR_REQUIRE_OK(const CompatibilityKey subset, CompatibilityKey::make(1, 2, 0x1));
  DCR_REQUIRE_OK(const CompatibilityKey older_minor, CompatibilityKey::make(1, 0, 0x7));
  DCR_REQUIRE_OK(const CompatibilityKey other_major, CompatibilityKey::make(2, 0, 0x3));

  DCR_CHECK(classify_compatibility(base, same) == CompatibilityRelation::identical);
  DCR_CHECK(classify_compatibility(base, superset) == CompatibilityRelation::compatible_upgrade);
  DCR_CHECK(classify_compatibility(base, subset) == CompatibilityRelation::incompatible);
  DCR_CHECK(classify_compatibility(superset, older_minor) == CompatibilityRelation::incompatible);
  DCR_CHECK(classify_compatibility(base, other_major) == CompatibilityRelation::incompatible);

  DCR_CHECK(is_acceptable_replacement(base, same));
  DCR_CHECK(is_acceptable_replacement(base, superset));
  DCR_CHECK(!is_acceptable_replacement(base, subset));
  DCR_CHECK(!is_acceptable_replacement(base, other_major));

  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata, CompatibilityKey::make(0, 0, 0));
}

DCR_TEST(compatibility, canonical_text_round_trip) {
  DCR_REQUIRE_OK(const CompatibilityKey key, CompatibilityKey::make(1, 2, 0xABCDEF));
  DCR_CHECK_EQ(std::string("1.2+0x0000000000abcdef"), key.to_string());
  DCR_REQUIRE_OK(const CompatibilityKey parsed, CompatibilityKey::parse(key.to_string()));
  DCR_CHECK(parsed == key);
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, CompatibilityKey::parse("1.2"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, CompatibilityKey::parse("1.2+0xabc"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, CompatibilityKey::parse("0.2+0x0000000000000000"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, CompatibilityKey::parse("1.2+0X0000000000000000"));
}

// ---------------------------------------------------------------------------
// Lifecycle table
// ---------------------------------------------------------------------------

DCR_TEST(lifecycle, transition_table_is_exhaustive_and_data_driven) {
  const std::set<std::pair<LifecycleState, LifecycleState>> expected = {
      {LifecycleState::proposed, LifecycleState::registered},
      {LifecycleState::proposed, LifecycleState::retired},
      {LifecycleState::registered, LifecycleState::active},
      {LifecycleState::registered, LifecycleState::degraded},
      {LifecycleState::registered, LifecycleState::maintenance},
      {LifecycleState::registered, LifecycleState::retired},
      {LifecycleState::active, LifecycleState::degraded},
      {LifecycleState::active, LifecycleState::maintenance},
      {LifecycleState::active, LifecycleState::retired},
      {LifecycleState::degraded, LifecycleState::active},
      {LifecycleState::degraded, LifecycleState::maintenance},
      {LifecycleState::degraded, LifecycleState::retired},
      {LifecycleState::maintenance, LifecycleState::active},
      {LifecycleState::maintenance, LifecycleState::degraded},
      {LifecycleState::maintenance, LifecycleState::retired},
  };

  // Every combination of (from, to) is classified, and the set of legal
  // transitions is exactly the table.
  for (const LifecycleState from : kAllLifecycleStates) {
    for (const LifecycleState to : kAllLifecycleStates) {
      const TransitionVerdict verdict = classify_transition(from, to);
      if (from == to) {
        DCR_CHECK(verdict == TransitionVerdict::unchanged);
        continue;
      }
      const bool legal = expected.count({from, to}) != 0;
      DCR_CHECK_EQ(legal, verdict == TransitionVerdict::legal);
      DCR_CHECK_EQ(legal, is_legal_transition(from, to));
    }
  }

  // Nothing reaches `replaced` through the table: only the replacement
  // operation sets it.
  for (const LifecycleState from : kAllLifecycleStates) {
    DCR_CHECK(classify_transition(from, LifecycleState::replaced) !=
              TransitionVerdict::legal);
  }
  DCR_CHECK(is_terminal(LifecycleState::retired));
  DCR_CHECK(is_terminal(LifecycleState::replaced));
  DCR_CHECK(!is_terminal(LifecycleState::proposed));
  DCR_CHECK(!is_live(LifecycleState::proposed));
  DCR_CHECK(is_live(LifecycleState::maintenance));

  DCR_CHECK_EQ(std::size_t{0}, legal_successors(LifecycleState::retired).size());
  DCR_CHECK_EQ(std::size_t{0}, legal_successors(LifecycleState::replaced).size());
  DCR_CHECK_EQ(std::size_t{3}, legal_successors(LifecycleState::active).size());

  for (const LifecycleState state : kAllLifecycleStates) {
    DCR_REQUIRE_OK(const LifecycleState parsed, parse_lifecycle_state(to_string(state)));
    DCR_CHECK(parsed == state);
  }
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, parse_lifecycle_state("unknown"));
  const std::size_t state_count = kLifecycleStateCount;
  DCR_CHECK_EQ(std::size_t{7}, state_count);
  const std::size_t retired_index = lifecycle_state_index(LifecycleState::retired);
  DCR_CHECK_EQ(std::size_t{5}, retired_index);
}

// ---------------------------------------------------------------------------
// Error categories
// ---------------------------------------------------------------------------

DCR_TEST(errors, every_code_has_one_stable_token) {
  // The tokens are the machine-readable contract. Enumerating them here means a
  // code added without a token, or a duplicated token, fails the build's tests
  // rather than confusing a consumer.
  const std::vector<std::pair<ErrorCode, std::string_view>> codes = {
      {ErrorCode::invalid_identity, "invalid_identity"},
      {ErrorCode::invalid_metadata, "invalid_metadata"},
      {ErrorCode::invalid_argument, "invalid_argument"},
      {ErrorCode::invalid_query, "invalid_query"},
      {ErrorCode::invalid_provenance, "invalid_provenance"},
      {ErrorCode::limit_exceeded, "limit_exceeded"},
      {ErrorCode::not_found, "not_found"},
      {ErrorCode::duplicate_identity, "duplicate_identity"},
      {ErrorCode::alias_conflict, "alias_conflict"},
      {ErrorCode::duplicate_membership, "duplicate_membership"},
      {ErrorCode::membership_violation, "membership_violation"},
      {ErrorCode::stale_generation, "stale_generation"},
      {ErrorCode::stale_revision, "stale_revision"},
      {ErrorCode::stale_authority, "stale_authority"},
      {ErrorCode::illegal_transition, "illegal_transition"},
      {ErrorCode::terminal_state, "terminal_state"},
      {ErrorCode::incompatible_replacement, "incompatible_replacement"},
      {ErrorCode::idempotency_conflict, "idempotency_conflict"},
      {ErrorCode::store_not_found, "store_not_found"},
      {ErrorCode::store_corrupt, "store_corrupt"},
      {ErrorCode::store_incompatible_version, "store_incompatible_version"},
      {ErrorCode::store_locked, "store_locked"},
      {ErrorCode::store_io_error, "store_io_error"},
      {ErrorCode::store_read_only, "store_read_only"},
      {ErrorCode::store_limit_exceeded, "store_limit_exceeded"},
      {ErrorCode::cancelled, "cancelled"},
      {ErrorCode::closed, "closed"},
      {ErrorCode::internal_error, "internal_error"},
  };
  std::set<std::string_view> seen;
  for (const auto& entry : codes) {
    DCR_CHECK_EQ(entry.second, to_string(entry.first));
    DCR_CHECK(seen.insert(entry.second).second);
    ErrorCode parsed = ErrorCode::internal_error;
    DCR_CHECK(parse_error_code(entry.second, parsed));
    DCR_CHECK(parsed == entry.first);
  }
  ErrorCode unused_code = ErrorCode::internal_error;
  DCR_CHECK(!parse_error_code("not_a_code", unused_code));

  DCR_CHECK(is_retryable(ErrorCode::stale_generation));
  DCR_CHECK(is_retryable(ErrorCode::stale_revision));
  DCR_CHECK(is_retryable(ErrorCode::store_locked));
  DCR_CHECK(!is_retryable(ErrorCode::duplicate_identity));
  DCR_CHECK(!is_retryable(ErrorCode::not_found));

  const Error error(ErrorCode::stale_generation, "generation moved on",
                    "precondition.expected_generation");
  DCR_CHECK_EQ(std::string("stale_generation (precondition.expected_generation): generation "
                           "moved on"),
               error.to_string());
}

// ---------------------------------------------------------------------------
// Deterministic extension metadata
// ---------------------------------------------------------------------------

DCR_TEST(metadata, entries_are_a_sorted_set) {
  DCR_REQUIRE_OK(const MetadataKey power, MetadataKey::parse("power.feed"));
  DCR_REQUIRE_OK(const MetadataKey cooling, MetadataKey::parse("cooling.zone"));

  std::vector<MetadataEntry> entries = {{power, "a"}, {cooling, "b"}};
  DCR_REQUIRE_OK(const MetadataMap map, MetadataMap::make(entries, "extensions"));
  DCR_CHECK_EQ(std::size_t{2}, map.size());
  DCR_CHECK_EQ(std::string("cooling.zone"), map.entries()[0].key.value());
  DCR_CHECK_EQ(std::string("power.feed"), map.entries()[1].key.value());
  DCR_CHECK(map.find(power) != nullptr);
  DCR_CHECK_EQ(std::string("a"), *map.find(power));
  DCR_CHECK(map.find(*MetadataKey::parse("absent.key")) == nullptr);

  // The same entries in a different order produce an equal map.
  std::vector<MetadataEntry> reversed = {{cooling, "b"}, {power, "a"}};
  DCR_REQUIRE_OK(const MetadataMap other, MetadataMap::make(reversed, "extensions"));
  DCR_CHECK(map == other);

  // Duplicates are rejected rather than merged.
  std::vector<MetadataEntry> duplicated = {{power, "a"}, {power, "b"}};
  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata, MetadataMap::make(duplicated, "extensions"));

  // The registry's own namespace is reserved.
  std::vector<MetadataEntry> reserved = {{*MetadataKey::parse("dcr.internal"), "x"}};
  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata, MetadataMap::make(reserved, "extensions"));
}

DCR_TEST(metadata, address_coordinates_and_limits) {
  DCR_REQUIRE_OK(const PostalAddress address,
                 PostalAddress::make({"1 Example Way", "Suite 100"}, "Ashburn", "VA", "20147"));
  DCR_CHECK_EQ(std::size_t{2}, address.lines().size());
  DCR_CHECK(!address.is_empty());
  DCR_CHECK(address ==
            *PostalAddress::make({"1 Example Way", "Suite 100"}, "Ashburn", "VA", "20147"));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata, PostalAddress::make({}, "", "", ""));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_argument, PostalAddress::make({"bad\x01line"}, "", "", ""));
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded,
                    PostalAddress::make(std::vector<std::string>(9, "line"), "", "", ""));

  DCR_REQUIRE_OK(const GeoCoordinates coordinates, GeoCoordinates::make(390439000, -774875000));
  DCR_CHECK_EQ(std::int32_t{390439000}, coordinates.latitude_e7());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata, GeoCoordinates::make(900000001, 0));
  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata, GeoCoordinates::make(0, -1800000001));

  FacilityMetadata facility;
  facility.country = *CountryCode::parse("US");
  facility.coordinates = coordinates;
  facility.tier = FacilityTier::tier_iii;
  DCR_CHECK_OK(facility.validate(RegistryLimits::defaults(), "facility"));

  RegistryLimits tight = RegistryLimits::defaults();
  tight.max_address_lines = 1;
  FacilityMetadata with_two_lines;
  with_two_lines.address = address;
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, with_two_lines.validate(tight, "facility"));

  for (const FacilityTier tier :
       {FacilityTier::tier_i, FacilityTier::tier_ii, FacilityTier::tier_iii, FacilityTier::tier_iv}) {
    DCR_REQUIRE_OK(const FacilityTier parsed, parse_facility_tier(to_string(tier)));
    DCR_CHECK(parsed == tier);
  }
  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata, parse_facility_tier("tier_v"));
}

DCR_TEST(metadata, ownership_scope_invariant) {
  DCR_CHECK(OwnershipScope::unassigned().kind() == OwnershipKind::unassigned);
  DCR_CHECK(!OwnershipScope::unassigned().scope().has_value());
  DCR_REQUIRE_OK(const OwnershipScopeId scope, OwnershipScopeId::parse("scope-platform"));
  DCR_REQUIRE_OK(const OwnershipScope owned, OwnershipScope::make(OwnershipKind::platform, scope));
  DCR_CHECK(owned.kind() == OwnershipKind::platform);
  DCR_CHECK(owned.scope().has_value());
  DCR_REQUIRE_ERROR(ErrorCode::invalid_metadata,
                    OwnershipScope::make(OwnershipKind::unassigned, scope));
  for (const OwnershipKind kind :
       {OwnershipKind::unassigned, OwnershipKind::platform, OwnershipKind::tenant,
        OwnershipKind::partner, OwnershipKind::system}) {
    DCR_REQUIRE_OK(const OwnershipKind parsed, parse_ownership_kind(to_string(kind)));
    DCR_CHECK(parsed == kind);
  }
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

DCR_TEST(limits, defaults_are_valid_and_bounds_are_enforced) {
  DCR_CHECK_OK(RegistryLimits::defaults().validate());

  RegistryLimits zero_records = RegistryLimits::defaults();
  zero_records.max_records = 0;
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, zero_records.validate());

  RegistryLimits too_many = RegistryLimits::defaults();
  too_many.max_records = StructuralLimits::kMaxRecords + 1;
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, too_many.validate());

  RegistryLimits query_over_records = RegistryLimits::defaults();
  query_over_records.max_query_results = query_over_records.max_records + 1;
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, query_over_records.validate());

  RegistryLimits tiny_snapshot = RegistryLimits::defaults();
  tiny_snapshot.max_snapshot_bytes = 10;
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, tiny_snapshot.validate());

  RegistryLimits wide_metadata = RegistryLimits::defaults();
  wide_metadata.max_metadata_entries = StructuralLimits::kMaxMetadataEntries + 1;
  DCR_REQUIRE_ERROR(ErrorCode::limit_exceeded, wide_metadata.validate());
}

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

DCR_TEST(provenance, caller_sources_only) {
  DCR_CHECK(is_caller_suppliable(ProvenanceSource::api));
  DCR_CHECK(is_caller_suppliable(ProvenanceSource::cli));
  DCR_CHECK(is_caller_suppliable(ProvenanceSource::import));
  DCR_CHECK(is_caller_suppliable(ProvenanceSource::migration));
  DCR_CHECK(!is_caller_suppliable(ProvenanceSource::recovery));
  DCR_CHECK(!is_caller_suppliable(ProvenanceSource::internal));
  for (const ProvenanceSource source :
       {ProvenanceSource::api, ProvenanceSource::cli, ProvenanceSource::import,
        ProvenanceSource::migration, ProvenanceSource::recovery, ProvenanceSource::internal}) {
    DCR_REQUIRE_OK(const ProvenanceSource parsed, parse_provenance_source(to_string(source)));
    DCR_CHECK(parsed == source);
  }
  DCR_REQUIRE_ERROR(ErrorCode::invalid_provenance, parse_provenance_source("forged"));

  DCR_REQUIRE_OK(const PrincipalId principal, PrincipalId::parse("operator"));
  DCR_REQUIRE_OK(const ProvenanceRecord record,
                 ProvenanceRecord::make(ProvenanceSource::api, principal,
                                        SequenceNumber::minimum(), RegistryGeneration::minimum(),
                                        Timestamp::unix_epoch(), std::nullopt, std::nullopt,
                                        "detail"));
  DCR_CHECK_EQ(std::string("detail"), record.detail());
  DCR_CHECK(!record.epoch().has_value());
  DCR_CHECK(!record.reason().has_value());
}
