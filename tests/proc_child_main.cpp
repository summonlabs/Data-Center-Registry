// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A helper process for the multiprocess suite.
//
// It is an ordinary executable that opens a real store, performs one command,
// and writes its machine-readable result to a file named on its command line.
// It is deliberately separate from the test binary so that the suite can prove
// things about separate operating-system processes: writer locking, restart
// behaviour, stale writers, and abrupt termination.
//
// Exit codes: 0 completed, 1 usage error, 2 the store rejected the command,
// 3 deliberately aborted after leaving partial state behind (a simulated
// crash), 4 deliberately terminated while holding the writer lock.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dcr/data_center_registry.hpp"

namespace {

using namespace dcr;

void write_result(const std::string& path, const std::string& line) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << line << "\n";
}

DataCenterId parse_id(const std::string& text) {
  auto parsed = DataCenterId::parse(text);
  if (!parsed.has_value()) {
    std::fprintf(stderr, "invalid identity: %s\n", parsed.error().to_string().c_str());
    std::exit(1);
  }
  return parsed.value();
}

PrincipalId parse_principal(const std::string& text) {
  auto parsed = PrincipalId::parse(text);
  if (!parsed.has_value()) {
    std::fprintf(stderr, "invalid principal: %s\n", parsed.error().to_string().c_str());
    std::exit(1);
  }
  return parsed.value();
}

struct ChildContext {
  std::filesystem::path root;
  std::string result_path;
  std::string principal;
  std::uint64_t count = 0;
};

ChildContext parse(const std::vector<std::string>& arguments, std::size_t minimum) {
  if (arguments.size() < minimum) {
    std::fprintf(stderr, "not enough arguments\n");
    std::exit(1);
  }
  ChildContext context;
  context.root = arguments[1];
  context.result_path = arguments[2];
  if (arguments.size() > 3) {
    context.principal = arguments[3];
  }
  if (arguments.size() > 4) {
    context.count = std::strtoull(arguments[4].c_str(), nullptr, 10);
  }
  return context;
}

OpenOptions make_options(const ChildContext& context, StoreOpenMode mode) {
  OpenOptions options;
  options.store.root = context.root;
  options.store.mode = mode;
  return options;
}

[[noreturn]] void fail_open(const std::string& result_path, const Error& error) {
  write_result(result_path, std::string("open_failed ") + std::string(to_string(error.code())) +
                                " " + error.message());
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  if (arguments.empty()) {
    std::fprintf(stderr, "usage: dcr_test_child <command> <root> <result-file> [args]\n");
    return 1;
  }
  const std::string command = arguments[0];

  // Try to open for writing and report the outcome without failing the run.
  if (command == "try-open-writer") {
    const ChildContext context = parse(arguments, 3);
    auto opened = Registry::open(make_options(context, StoreOpenMode::open_existing));
    if (!opened.has_value()) {
      write_result(context.result_path,
                   std::string("refused ") + std::string(to_string(opened.error().code())));
      return 0;
    }
    write_result(context.result_path,
                 "opened generation=" +
                     std::to_string(opened.value().registry->generation().value()));
    return 0;
  }

  // Commit a number of registrations, then close cleanly and exit.
  if (command == "register") {
    const ChildContext context = parse(arguments, 4);
    auto opened = Registry::open(make_options(context, StoreOpenMode::open_existing));
    if (!opened.has_value()) {
      fail_open(context.result_path, opened.error());
    }
    Registry& registry = *opened.value().registry;
    for (std::uint64_t index = 0; index < context.count; ++index) {
      RegisterCommand registration;
      registration.context.provenance.source = ProvenanceSource::cli;
      registration.context.provenance.principal = parse_principal(context.principal);
      registration.context.provenance.detail = "child process";
      registration.id = parse_id("dc-child-" + std::to_string(index));
      registration.draft.display_name = "Child Facility " + std::to_string(index);
      registration.draft.ownership = OwnershipScope::unassigned();
      registration.draft.compatibility = *CompatibilityKey::make(1, 0, 0);
      registration.draft.memberships.push_back(
          SiteMembership{*SiteId::parse("site-child"), MembershipRole::primary});
      registration.initial_state = LifecycleState::registered;
      auto outcome = registry.register_data_center(registration);
      if (!outcome.has_value()) {
        write_result(context.result_path,
                     std::string("rejected ") + std::string(to_string(outcome.error().code())) +
                         " " + outcome.error().message());
        return 2;
      }
    }
    write_result(context.result_path,
                 "registered generation=" + std::to_string(registry.generation().value()) +
                     " records=" + std::to_string(registry.record_count()));
    const auto closed = opened.value().registry->close();
    return closed.has_value() ? 0 : 2;
  }

  // Attempt a mutation preconditioned on a generation the caller supplies.
  if (command == "mutate-at") {
    const ChildContext context = parse(arguments, 5);
    const std::uint64_t expected = std::strtoull(arguments[4].c_str(), nullptr, 10);
    auto opened = Registry::open(make_options(context, StoreOpenMode::open_existing));
    if (!opened.has_value()) {
      fail_open(context.result_path, opened.error());
    }
    Registry& registry = *opened.value().registry;
    UpdateMetadataCommand update;
    update.context.provenance.source = ProvenanceSource::cli;
    update.context.provenance.principal = parse_principal(context.principal);
    update.context.precondition.expected_generation = *RegistryGeneration::from_value(expected);
    update.id = parse_id("dc-child-0");
    update.patch.display_name = FieldPatch<std::string>::set("Overwritten By Stale Writer");
    auto outcome = registry.update_metadata(update);
    if (!outcome.has_value()) {
      write_result(context.result_path,
                   std::string("rejected ") + std::string(to_string(outcome.error().code())));
      return 0;
    }
    write_result(context.result_path,
                 std::string("applied ") + std::string(to_string(outcome.value().status)) +
                     " generation=" + std::to_string(registry.generation().value()));
    return 0;
  }

  // Read the store without taking the writer lock.
  if (command == "read") {
    const ChildContext context = parse(arguments, 3);
    auto opened = Registry::open(make_options(context, StoreOpenMode::read_only));
    if (!opened.has_value()) {
      fail_open(context.result_path, opened.error());
    }
    Registry& registry = *opened.value().registry;
    write_result(context.result_path,
                 "read generation=" + std::to_string(registry.generation().value()) +
                     " records=" + std::to_string(registry.record_count()) +
                     " digest=" + registry.snapshot_digest().to_hex());
    return 0;
  }

  // Simulate a crash: leave staging residue and a truncated generation file,
  // then terminate without unwinding or releasing anything cleanly.
  if (command == "crash-mid-write") {
    const ChildContext context = parse(arguments, 3);
    const std::filesystem::path root = context.root;
    std::filesystem::create_directories(root / "tmp");
    {
      std::ofstream stream(root / "tmp" / "snapshot.staging-crash",
                           std::ios::binary | std::ios::trunc);
      stream << "half a snapshot";
    }
    std::uint64_t generation = 0;
    {
      std::ifstream stream(root / "CURRENT", std::ios::binary);
      std::string line;
      while (std::getline(stream, line)) {
        if (line.rfind("generation=", 0) == 0) {
          generation = std::strtoull(line.c_str() + 11, nullptr, 10);
        }
      }
    }
    generation += 1;
    char name[64];
    std::snprintf(name, sizeof(name), "gen-%020llu.dcrs",
                  static_cast<unsigned long long>(generation));
    {
      std::ofstream truncated(root / "generations" / name, std::ios::binary | std::ios::trunc);
      truncated << "truncated";
    }
    write_result(context.result_path, std::string("crashed with ") + name);
    std::fflush(nullptr);
    std::_Exit(3);
  }

  // Open for writing and terminate without releasing the lock, to prove that
  // the operating system releases it.
  if (command == "abort-while-locked") {
    const ChildContext context = parse(arguments, 3);
    auto opened = Registry::open(make_options(context, StoreOpenMode::open_existing));
    if (!opened.has_value()) {
      fail_open(context.result_path, opened.error());
    }
    write_result(context.result_path,
                 "locked generation=" +
                     std::to_string(opened.value().registry->generation().value()));
    std::fflush(nullptr);
    std::_Exit(4);
  }

  std::fprintf(stderr, "unknown command: %s\n", command.c_str());
  return 1;
}
