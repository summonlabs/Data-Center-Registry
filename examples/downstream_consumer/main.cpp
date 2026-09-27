// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// An independent consumer of the installed package.
//
// It includes only installed headers, links only the exported target, and uses
// the registry the way a later DCCP repository would: declare a facility,
// trust its generation, read a snapshot, and refuse to write when its view is
// stale. Nothing here reaches into the build tree.

#include <cstdio>
#include <filesystem>
#include <string>

#include <dcr/data_center_registry.hpp>

namespace {

using namespace dcr;

int fail(const char* what, const Error& error) {
  std::fprintf(stderr, "%s: %s\n", what, error.to_string().c_str());
  return 1;
}

}  // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "dcr-downstream-consumer";
  std::error_code code;
  std::filesystem::remove_all(root, code);

  std::printf("linked against data_center_registry %s\n",
              std::string(dcr::kVersionString).c_str());

  OpenOptions options;
  options.store.root = root;
  options.store.mode = StoreOpenMode::open_or_create;

  auto opened = Registry::open(options);
  if (!opened.has_value()) {
    return fail("open", opened.error());
  }
  Registry& registry = *opened.value().registry;
  std::printf("opened at generation %s\n", registry.generation().to_string().c_str());

  // Declare a facility. The consumer supplies its own identity and its own
  // compatibility contract; the registry supplies the authority.
  RegisterCommand registration;
  auto principal = PrincipalId::parse("dccp-consumer");
  if (!principal.has_value()) {
    return fail("principal", principal.error());
  }
  registration.context.provenance.source = ProvenanceSource::api;
  registration.context.provenance.principal = principal.value();
  registration.context.provenance.detail = "downstream consumer";
  registration.id = *DataCenterId::parse("dc-consumer-01");
  registration.draft.display_name = "Consumer Facility";
  registration.draft.ownership = OwnershipScope::unassigned();
  registration.draft.compatibility = *CompatibilityKey::make(1, 0, 0);
  registration.draft.memberships = {
      SiteMembership{*SiteId::parse("site-consumer"), MembershipRole::primary}};
  registration.initial_state = LifecycleState::registered;

  auto created = registry.register_data_center(registration);
  if (!created.has_value()) {
    return fail("register", created.error());
  }
  std::printf("%s %s at generation %s\n", std::string(to_string(created.value().status)).c_str(),
              created.value().id.value().c_str(), created.value().generation.to_string().c_str());

  // Take a snapshot and hold it: it stays valid while the registry moves on.
  auto snapshot = registry.snapshot();
  if (!snapshot.has_value()) {
    return fail("snapshot", snapshot.error());
  }
  const RegistryGeneration observed = snapshot.value().generation();
  const Sha256Digest observed_digest = snapshot.value().digest();

  // A later consumer write, preconditioned on the generation it read.
  const DataCenterId id = *DataCenterId::parse("dc-consumer-01");
  TransitionCommand transition;
  transition.context.provenance.source = ProvenanceSource::api;
  transition.context.provenance.principal = principal.value();
  transition.context.precondition.expected_generation = observed;
  transition.id = id;
  transition.target_state = LifecycleState::active;
  transition.reason = *ReasonCode::parse("commissioned");
  auto transitioned = registry.transition_lifecycle(transition);
  if (!transitioned.has_value()) {
    return fail("transition", transitioned.error());
  }
  std::printf("%s %s\n", std::string(to_string(transitioned.value().status)).c_str(),
              id.value().c_str());

  // Repeating the same command is an idempotent no-op: the record is already
  // active, so the work the command describes is already done.
  auto repeated = registry.transition_lifecycle(transition);
  if (!repeated.has_value()) {
    return fail("repeated transition", repeated.error());
  }
  if (repeated.value().status != MutationStatus::unchanged) {
    std::fprintf(stderr, "repeating a completed transition should be a no-op\n");
    return 1;
  }
  std::printf("repeated transition: %s\n",
              std::string(to_string(repeated.value().status)).c_str());

  // A command that would really change something, carrying the generation the
  // consumer read before the first transition, is stale instead.
  TransitionCommand stale = transition;
  stale.target_state = LifecycleState::maintenance;
  auto refused = registry.transition_lifecycle(stale);
  if (refused.has_value()) {
    std::fprintf(stderr, "a stale transition was accepted\n");
    return 1;
  }
  std::printf("stale retry refused: %s (retryable: %s)\n",
              std::string(to_string(refused.error().code())).c_str(),
              is_retryable(refused.error().code()) ? "yes" : "no");

  // The held snapshot still describes what it described.
  const std::uint64_t observed_records = snapshot.value().record_count();
  std::printf("held snapshot: generation %s, %llu record(s), digest %s\n",
              observed.to_string().c_str(),
              static_cast<unsigned long long>(observed_records),
              observed_digest.to_hex().c_str());
  if (!(observed_digest == snapshot.value().digest())) {
    std::fprintf(stderr, "the held snapshot changed\n");
    return 1;
  }

  const RegistryStats stats = registry.stats();
  std::printf("registry: generation %s, %llu record(s), active=%llu\n",
              stats.generation.to_string().c_str(),
              static_cast<unsigned long long>(stats.record_count),
              static_cast<unsigned long long>(stats.count_of(LifecycleState::active)));

  const auto closed = registry.close();
  if (!closed.has_value()) {
    return fail("close", closed.error());
  }
  std::filesystem::remove_all(root, code);
  std::printf("downstream consumer completed\n");
  return 0;
}
