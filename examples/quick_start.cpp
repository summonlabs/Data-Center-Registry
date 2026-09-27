// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A short, complete tour of the stable public API.
//
// It builds a registry in a temporary directory, submits one command of each
// kind, reads the results back through the query surface, closes, reopens and
// checks that the state came back byte for byte. Every call here is the public
// API a downstream consumer uses; nothing reaches into the store layout.

#include <cstdio>
#include <filesystem>
#include <string>

#include "dcr/data_center_registry.hpp"

namespace {

using namespace dcr;

int fail(const char* what, const Error& error) {
  std::fprintf(stderr, "%s: %s\n", what, error.to_string().c_str());
  return 1;
}

/// A clock the example controls, so its output is identical on every run.
class FixedClock final : public Clock {
 public:
  FixedClock() : now_(*Timestamp::from_unix_millis(1767225600000LL)) {}
  [[nodiscard]] Timestamp now() const override { return now_; }

 private:
  Timestamp now_;
};

/// A context with no precondition: registration creates a new record, so it
/// must not claim to be mutating an existing generation.
CommandContext context(std::string_view principal) {
  CommandContext command_context;
  command_context.provenance.source = ProvenanceSource::api;
  command_context.provenance.principal = *PrincipalId::parse(principal);
  command_context.provenance.detail = "quick start";
  return command_context;
}

/// A context preconditioned on the registry's current generation, for every
/// command that changes an existing record.
CommandContext context_at(const Registry& registry, std::string_view principal) {
  CommandContext command_context = context(principal);
  command_context.precondition.expected_generation = registry.generation();
  return command_context;
}

}  // namespace

int main() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     "dcr-quick-start-example";
  std::error_code code;
  std::filesystem::remove_all(root, code);

  FixedClock clock;
  OpenOptions options;
  options.store.root = root;
  options.store.mode = StoreOpenMode::open_or_create;
  options.clock = &clock;

  auto opened = Registry::open(options);
  if (!opened.has_value()) {
    return fail("open", opened.error());
  }
  Registry& registry = *opened.value().registry;
  std::printf("opened %s at generation %s\n", root.string().c_str(),
              registry.generation().to_string().c_str());

  // --- register ---------------------------------------------------------
  RegisterCommand registration;
  registration.context = context("operator");
  registration.id = *DataCenterId::parse("dc-ashburn-01");
  registration.draft.display_name = "Ashburn One";
  registration.draft.aliases = {*Alias::parse("ashburn-one")};
  registration.draft.memberships = {
      SiteMembership{*SiteId::parse("site-us-east"), MembershipRole::primary}};
  registration.draft.ownership =
      *OwnershipScope::make(OwnershipKind::platform, *OwnershipScopeId::parse("scope-platform"));
  registration.draft.compatibility = *CompatibilityKey::make(1, 0, 0x1);
  registration.draft.facility.country = *CountryCode::parse("US");
  registration.draft.facility.region = *RegionCode::parse("us-east-1");
  registration.draft.facility.metro = *MetroCode::parse("ashburn");
  registration.draft.facility.coordinates = *GeoCoordinates::make(390439000, -774875000);
  registration.draft.extensions =
      *MetadataMap::make({{*MetadataKey::parse("hall.count"), "4"}}, "extensions");
  registration.initial_state = LifecycleState::registered;

  auto created = registry.register_data_center(registration);
  if (!created.has_value()) {
    return fail("register", created.error());
  }
  std::printf("%s %s at generation %s\n", std::string(to_string(created.value().status)).c_str(),
              created.value().id.value().c_str(), created.value().generation.to_string().c_str());

  // --- update metadata --------------------------------------------------
  UpdateMetadataCommand update;
  update.context = context_at(registry, "operator");
  update.id = registration.id;
  update.patch.display_name = FieldPatch<std::string>::set("Ashburn One (Building A)");
  update.patch.facility = FieldPatch<FacilityMetadata>::set(registration.draft.facility);
  update.patch.extensions = FieldPatch<MetadataMap>::set(registration.draft.extensions);
  auto updated = registry.update_metadata(update);
  if (!updated.has_value()) {
    return fail("update", updated.error());
  }
  std::printf("%s %s to revision %s\n", std::string(to_string(updated.value().status)).c_str(),
              updated.value().id.value().c_str(), updated.value().revision.to_string().c_str());

  // --- transition -------------------------------------------------------
  TransitionCommand transition;
  transition.context = context_at(registry, "operator");
  transition.id = registration.id;
  transition.target_state = LifecycleState::active;
  transition.reason = *ReasonCode::parse("commissioned");
  auto transitioned = registry.transition_lifecycle(transition);
  if (!transitioned.has_value()) {
    return fail("transition", transitioned.error());
  }
  std::printf("%s %s to %s\n", std::string(to_string(transitioned.value().status)).c_str(),
              transitioned.value().id.value().c_str(), "active");

  // --- attach a secondary site -----------------------------------------
  AttachSiteCommand attach;
  attach.context = context_at(registry, "operator");
  attach.id = registration.id;
  attach.membership = SiteMembership{*SiteId::parse("site-us-east-2"), MembershipRole::standby};
  auto attached = registry.attach_site(attach);
  if (!attached.has_value()) {
    return fail("attach", attached.error());
  }
  std::printf("%s %s to site-us-east-2\n",
              std::string(to_string(attached.value().status)).c_str(),
              attached.value().id.value().c_str());

  // --- replace ----------------------------------------------------------
  ReplaceCommand replacement;
  replacement.context = context_at(registry, "operator");
  replacement.retired_id = registration.id;
  replacement.new_id = *DataCenterId::parse("dc-ashburn-02");
  replacement.draft = registration.draft;
  replacement.draft.display_name = "Ashburn Two";
  replacement.draft.aliases = {*Alias::parse("ashburn-one")};
  replacement.draft.compatibility = *CompatibilityKey::make(1, 1, 0x3);
  replacement.initial_state = LifecycleState::active;
  replacement.reason = *ReasonCode::parse("facility_rebuilt");
  auto replaced = registry.replace_data_center(replacement);
  if (!replaced.has_value()) {
    return fail("replace", replaced.error());
  }
  std::printf("%s %s replacing %s\n", std::string(to_string(replaced.value().status)).c_str(),
              replaced.value().id.value().c_str(),
              replaced.value().related_id->value().c_str());

  // --- read it back -----------------------------------------------------
  auto by_alias = registry.find_by_alias(*Alias::parse("ashburn-one"));
  if (!by_alias.has_value()) {
    return fail("find_by_alias", by_alias.error());
  }
  std::printf("alias ashburn-one resolves to %s\n", by_alias.value().id.value().c_str());

  EnumerationQuery query;
  query.include_terminal = false;
  auto live_records = registry.enumerate(query);
  if (!live_records.has_value()) {
    return fail("enumerate", live_records.error());
  }
  std::printf("%zu live record(s):\n", live_records.value().size());
  for (const auto& record : live_records.value()) {
    std::printf("  %s  %-12s  %s\n", record.id.value().c_str(),
                std::string(to_string(record.state)).c_str(), record.display_name.c_str());
  }

  HistoryQuery history_query;
  const HistoryPage history = registry.history(history_query);
  std::printf("%llu history entr(ies):\n",
              static_cast<unsigned long long>(history.matched_total));
  for (const auto& entry : history.entries) {
    std::printf("  %-6s  %-22s  %s\n", entry.sequence.to_string().c_str(),
                std::string(to_string(entry.action)).c_str(), entry.id.value().c_str());
  }

  const Sha256Digest digest = registry.snapshot_digest();
  const RegistryStats stats = registry.stats();
  std::printf("generation %s, %llu record(s), digest %s\n", stats.generation.to_string().c_str(),
              static_cast<unsigned long long>(stats.record_count), digest.to_hex().c_str());

  // --- close, reopen, verify the state came back ------------------------
  if (auto closed = registry.close(); !closed.has_value()) {
    return fail("close", closed.error());
  }

  OpenOptions reopen_options;
  reopen_options.store.root = root;
  reopen_options.store.mode = StoreOpenMode::open_existing;
  reopen_options.clock = &clock;
  auto reopened = Registry::open(reopen_options);
  if (!reopened.has_value()) {
    return fail("reopen", reopened.error());
  }
  const Sha256Digest reopened_digest = reopened.value().registry->snapshot_digest();
  std::printf("reopened at generation %s, digest %s\n",
              reopened.value().registry->generation().to_string().c_str(),
              reopened_digest.to_hex().c_str());
  if (!(reopened_digest == digest)) {
    std::fprintf(stderr, "the reopened digest does not match the digest before the close\n");
    return 1;
  }

  auto inspection = Registry::inspect(reopen_options.store);
  if (!inspection.has_value()) {
    return fail("inspect", inspection.error());
  }
  std::printf("store consistent: %s\n", inspection.value().consistent ? "yes" : "no");

  if (auto closed = reopened.value().registry->close(); !closed.has_value()) {
    return fail("close", closed.error());
  }
  std::filesystem::remove_all(root, code);
  std::printf("quick start completed\n");
  return 0;
}
