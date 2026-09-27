// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two things this example makes concrete:
//
//   1. A reader in one process sees a stable generation while a writer in
//      another process commits, because published generations are immutable
//      and the reader never takes the writer's lock.
//   2. Retry semantics: every rejection a caller can meet has a defined
//      response, and the same command with the same idempotency key never
//      applies twice.
//
// It runs entirely on the local filesystem. Nothing here contacts a network or
// a facility device.

#include <cstdio>
#include <filesystem>
#include <string>

#include "dcr/data_center_registry.hpp"

namespace {

using namespace dcr;

class FixedClock final : public Clock {
 public:
  FixedClock() : now_(*Timestamp::from_unix_millis(1767225600000LL)) {}
  [[nodiscard]] Timestamp now() const override { return now_; }

  void advance(std::int64_t millis) {
    const auto next = Timestamp::from_unix_millis(now_.unix_millis() + millis);
    if (next.has_value()) {
      now_ = next.value();
    }
  }

 private:
  Timestamp now_;
};

CommandContext context(std::string_view principal) {
  CommandContext command_context;
  command_context.provenance.source = ProvenanceSource::api;
  command_context.provenance.principal = *PrincipalId::parse(principal);
  command_context.provenance.detail = "lifecycle example";
  return command_context;
}

/// The context for a command that changes an existing record: it states the
/// generation the caller believes it is mutating.
CommandContext context_at(const Registry& registry, std::string_view principal) {
  CommandContext command_context = context(principal);
  command_context.precondition.expected_generation = registry.generation();
  return command_context;
}

RegisterCommand make_registration(const CommandContext& command_context, std::string_view id_text,
                                  std::string_view name, std::string_view site_text) {
  RegisterCommand command;
  command.context = command_context;
  command.id = *DataCenterId::parse(id_text);
  command.draft.display_name = std::string(name);
  command.draft.memberships = {
      SiteMembership{*SiteId::parse(site_text), MembershipRole::primary}};
  command.draft.ownership = OwnershipScope::unassigned();
  command.draft.compatibility = *CompatibilityKey::make(1, 0, 0);
  command.initial_state = LifecycleState::registered;
  return command;
}

int fail(const char* what, const Error& error) {
  std::fprintf(stderr, "%s: %s\n", what, error.to_string().c_str());
  return 1;
}

}  // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "dcr-lifecycle-example";
  std::error_code code;
  std::filesystem::remove_all(root, code);

  FixedClock clock;
  OpenOptions writer_options;
  writer_options.store.root = root;
  writer_options.store.mode = StoreOpenMode::open_or_create;
  writer_options.clock = &clock;

  auto writer_result = Registry::open(writer_options);
  if (!writer_result.has_value()) {
    return fail("open writer", writer_result.error());
  }
  Registry& writer = *writer_result.value().registry;

  // A registration carrying an idempotency key, followed by an honest retry of
  // the very same command. The retry reports the original outcome and writes
  // nothing: that is what makes a retrying client safe.
  RegisterCommand registration =
      make_registration(context("operator"), "dc-alpha", "Alpha", "site-east");
  registration.context.idempotency_key = *IdempotencyKey::parse("lifecycle-example-0001");
  auto first = writer.register_data_center(registration);
  if (!first.has_value()) {
    return fail("register", first.error());
  }
  const RegistryGeneration after_first = writer.generation();
  auto second = writer.register_data_center(registration);
  if (!second.has_value()) {
    return fail("replay", second.error());
  }
  std::printf("writer: first=%s replay=%s (generation %s -> %s)\n",
              std::string(to_string(first.value().status)).c_str(),
              std::string(to_string(second.value().status)).c_str(),
              after_first.to_string().c_str(), writer.generation().to_string().c_str());
  if (!(writer.generation() == after_first)) {
    std::fprintf(stderr, "a replayed command must not advance the generation\n");
    return 1;
  }

  // A reader process opens the same store without taking the writer lock.
  OpenOptions reader_options;
  reader_options.store.root = root;
  reader_options.store.mode = StoreOpenMode::read_only;
  reader_options.clock = &clock;
  auto reader_result = Registry::open(reader_options);
  if (!reader_result.has_value()) {
    return fail("open reader", reader_result.error());
  }
  Registry& reader = *reader_result.value().registry;
  std::printf("reader: generation %s, %llu record(s)\n",
              reader.generation().to_string().c_str(),
              static_cast<unsigned long long>(reader.record_count()));

  // The writer retires the record and creates its successor in one generation.
  clock.advance(1000);
  ReplaceCommand replacement;
  replacement.context = context_at(writer, "operator");
  replacement.retired_id = *DataCenterId::parse("dc-alpha");
  replacement.new_id = *DataCenterId::parse("dc-alpha-2");
  replacement.draft.display_name = "Alpha Two";
  replacement.draft.memberships = {
      SiteMembership{*SiteId::parse("site-east"), MembershipRole::primary}};
  replacement.draft.ownership = OwnershipScope::unassigned();
  replacement.draft.compatibility = *CompatibilityKey::make(1, 1, 0x1);
  replacement.initial_state = LifecycleState::active;
  replacement.reason = *ReasonCode::parse("facility_rebuilt");
  auto replaced = writer.replace_data_center(replacement);
  if (!replaced.has_value()) {
    return fail("replace", replaced.error());
  }
  std::printf("writer: replaced %s with %s in generation %s\n",
              replaced.value().related_id->value().c_str(), replaced.value().id.value().c_str(),
              replaced.value().generation.to_string().c_str());

  // The reader is still on the generation it opened: a snapshot cannot change
  // underneath a reader.
  std::printf("reader: still generation %s until it refreshes\n",
              reader.generation().to_string().c_str());
  auto refreshed = reader.refresh();
  if (!refreshed.has_value()) {
    return fail("refresh", refreshed.error());
  }
  std::printf("reader: refreshed to generation %s, %llu record(s)\n",
              refreshed.value().generation.to_string().c_str(),
              static_cast<unsigned long long>(reader.record_count()));

  // A retired identity is never revived, and the successor is not usable as a
  // stand-in for it.
  RetireCommand retire;
  retire.context = context_at(writer, "operator");
  retire.id = *DataCenterId::parse("dc-alpha");
  retire.reason = *ReasonCode::parse("decommissioned");
  auto refusal = writer.retire_data_center(retire);
  if (refusal.has_value()) {
    std::fprintf(stderr, "retiring a replaced record should have been refused\n");
    return 1;
  }
  std::printf("writer: retiring the predecessor was refused with %s\n",
              std::string(to_string(refusal.error().code())).c_str());

  // A stale retry is refused too, and the caller can see exactly why.
  UpdateMetadataCommand stale;
  stale.context = context_at(writer, "operator");
  stale.context.precondition.expected_generation = *RegistryGeneration::from_value(1);
  stale.id = *DataCenterId::parse("dc-alpha-2");
  stale.patch.display_name = FieldPatch<std::string>::set("Stale Rename");
  auto stale_refusal = writer.update_metadata(stale);
  if (stale_refusal.has_value()) {
    std::fprintf(stderr, "a stale update should have been refused\n");
    return 1;
  }
  std::printf("writer: stale update refused with %s (retryable: %s)\n",
              std::string(to_string(stale_refusal.error().code())).c_str(),
              is_retryable(stale_refusal.error().code()) ? "yes" : "no");

  const Sha256Digest digest = writer.snapshot_digest();
  std::printf("writer: final digest %s\n", digest.to_hex().c_str());

  if (auto closed = writer.close(); !closed.has_value()) {
    return fail("close writer", closed.error());
  }
  if (auto closed = reader.close(); !closed.has_value()) {
    return fail("close reader", closed.error());
  }

  // Reopen writable and confirm the state survived exactly.
  auto reopened = Registry::open(writer_options);
  if (!reopened.has_value()) {
    return fail("reopen", reopened.error());
  }
  const bool same = reopened.value().registry->snapshot_digest() == digest;
  std::printf("reopened digest matches: %s\n", same ? "yes" : "no");
  if (!same) {
    return 1;
  }
  if (auto closed = reopened.value().registry->close(); !closed.has_value()) {
    return fail("close", closed.error());
  }
  std::filesystem::remove_all(root, code);
  std::printf("lifecycle example completed\n");
  return 0;
}
