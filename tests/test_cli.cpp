// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The inspection tool as a consumer.
//
// The tool is started as a real process and driven through its command line.
// What is asserted here is that it is a genuine consumer of the public API: it
// reports the same outcomes, faces the same preconditions, and cannot reach
// past them. Exit codes and output are checked, so a tool that started
// accepting a stale writer would fail this suite.
//
// The tool is located through DCR_TEST_CLI, which the test runner sets.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "dcr/data_center_registry.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace dcr;

namespace {

std::filesystem::path cli_program() { return dcrtest::helper_executable("DCR_TEST_CLI"); }

/// Runs the tool with its output captured, and returns the exit code. The
/// captured output is left in `captured`.
int run_cli(const std::vector<std::string>& arguments, std::string& captured,
            const std::filesystem::path& scratch) {
  const std::filesystem::path program = cli_program();
  if (program.empty()) {
    dcrtest::record_failure(__FILE__, __LINE__,
                            "DCR_TEST_CLI is not set, so the tool was not exercised");
    return -1;
  }
  const std::filesystem::path output = scratch / "cli-output.txt";
  std::error_code code;
  std::filesystem::remove(output, code);
  const int status = dcrtest::run_child_process_captured(program, arguments, output);
  captured.clear();
  if (!dcrtest::read_text_file(output, captured)) {
    captured = "<no output captured>";
  }
  return status;
}

bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

DCR_TEST(cli, the_tool_is_available) {
  const std::filesystem::path program = cli_program();
  if (program.empty()) {
    dcrtest::record_failure(__FILE__, __LINE__, "DCR_TEST_CLI is not set");
    return;
  }
  DCR_CHECK(std::filesystem::exists(program));
}

DCR_TEST(cli, initialises_registers_and_inspects) {
  dcrtest::Fixture fixture("cli-basic");
  const std::string root = fixture.root().string();
  std::string output;

  DCR_CHECK_EQ(0, run_cli({"init", "--root", root}, output, fixture.root()));
  DCR_CHECK(contains(output, "created an empty registry"));

  // create_new never adopts an existing store, even through the tool.
  DCR_CHECK_EQ(4, run_cli({"init", "--root", root}, output, fixture.root()));
  DCR_CHECK(contains(output, "already exists"));

  DCR_CHECK_EQ(0, run_cli({"register", "--root", root, "--id", "dc-ashburn-01", "--name",
                           "Ashburn One", "--site", "site-east", "--state", "registered",
                           "--alias", "legacy-ashburn", "--compatibility",
                           "1.0+0x0000000000000001", "--principal", "operator"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "created dc-ashburn-01 at generation 1"));

  // A registration through the tool is subject to the same rules as any other
  // caller: a duplicate identity is refused with the same category.
  DCR_CHECK_EQ(3, run_cli({"register", "--root", root, "--id", "dc-ashburn-01", "--name",
                           "Duplicate", "--site", "site-east", "--state", "registered"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "duplicate_identity"));

  DCR_CHECK_EQ(0, run_cli({"list", "--root", root, "--json"}, output, fixture.root()));
  DCR_CHECK(contains(output, "\"id\": \"dc-ashburn-01\""));
  DCR_CHECK(contains(output, "\"display_name\": \"Ashburn One\""));

  DCR_CHECK_EQ(0, run_cli({"show", "--root", root, "--id", "dc-ashburn-01"}, output,
                          fixture.root()));
  DCR_CHECK(contains(output, "display name        Ashburn One"));
  DCR_CHECK(contains(output, "membership          site-east (primary)"));

  DCR_CHECK_EQ(0, run_cli({"stats", "--root", root}, output, fixture.root()));
  DCR_CHECK(contains(output, "generation           1"));
  DCR_CHECK(contains(output, "records              1 of 65536 allowed"));

  DCR_CHECK_EQ(0, run_cli({"limits", "--root", root}, output, fixture.root()));
  DCR_CHECK(contains(output, "max_records                  65536"));

  DCR_CHECK_EQ(0, run_cli({"inspect", "--root", root, "--json"}, output, fixture.root()));
  DCR_CHECK(contains(output, "\"consistent\": \"true\""));

  DCR_CHECK_EQ(0, run_cli({"verify", "--root", root}, output, fixture.root()));
  DCR_CHECK(contains(output, "consistent  yes"));

  // An unknown identity is a rejection, not a crash and not an empty success.
  DCR_CHECK_EQ(3, run_cli({"show", "--root", root, "--id", "dc-absent"}, output, fixture.root()));
  DCR_CHECK(contains(output, "not_found"));
}

DCR_TEST(cli, mutations_respect_preconditions_and_lifecycle) {
  dcrtest::Fixture fixture("cli-mutations");
  const std::string root = fixture.root().string();
  std::string output;

  DCR_CHECK_EQ(0, run_cli({"init", "--root", root}, output, fixture.root()));
  DCR_CHECK_EQ(0, run_cli({"register", "--root", root, "--id", "dc-alpha", "--name", "Alpha",
                           "--site", "site-east", "--state", "registered"},
                          output, fixture.root()));

  // A mutation without a precondition is a usage error, never a silent write.
  DCR_CHECK_EQ(2, run_cli({"set-name", "--root", root, "--id", "dc-alpha", "--name", "Renamed"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "--expected-generation"));

  DCR_CHECK_EQ(0, run_cli({"set-name", "--root", root, "--id", "dc-alpha", "--name", "Renamed",
                           "--expected-generation", "1"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "updated dc-alpha at generation 2"));

  // The same command with the old generation is stale.
  DCR_CHECK_EQ(3, run_cli({"set-name", "--root", root, "--id", "dc-alpha", "--name",
                           "Renamed Again", "--expected-generation", "1"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "stale_generation"));

  DCR_CHECK_EQ(0, run_cli({"transition", "--root", root, "--id", "dc-alpha", "--to", "active",
                           "--reason", "commissioned", "--expected-generation", "2"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "updated dc-alpha at generation 3"));

  // An illegal transition is refused with the reason the library gives.
  DCR_CHECK_EQ(3, run_cli({"transition", "--root", root, "--id", "dc-alpha", "--to", "proposed",
                           "--expected-generation", "3"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "illegal_transition"));

  DCR_CHECK_EQ(0, run_cli({"attach-site", "--root", root, "--id", "dc-alpha", "--site",
                           "site-west", "--state", "standby", "--expected-generation", "3"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "updated dc-alpha"));

  DCR_CHECK_EQ(3, run_cli({"detach-site", "--root", root, "--id", "dc-alpha", "--site",
                           "site-east", "--expected-generation", "4"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "membership_violation"));

  DCR_CHECK_EQ(0, run_cli({"detach-site", "--root", root, "--id", "dc-alpha", "--site",
                           "site-west", "--expected-generation", "4"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "updated dc-alpha"));

  DCR_CHECK_EQ(0, run_cli({"history", "--root", root, "--id", "dc-alpha"}, output, fixture.root()));
  DCR_CHECK(contains(output, "registered"));
  DCR_CHECK(contains(output, "metadata_updated"));
  DCR_CHECK(contains(output, "lifecycle_transition"));
  DCR_CHECK(contains(output, "site_attached"));
  DCR_CHECK(contains(output, "site_detached"));

  // Replacement through the tool, then the terminal rules.
  DCR_CHECK_EQ(0, run_cli({"replace", "--root", root, "--id", "dc-alpha", "--successor",
                           "dc-alpha-2", "--name", "Alpha Two", "--site", "site-east",
                           "--state", "active", "--reason", "facility_rebuilt",
                           "--compatibility", "1.1+0x0000000000000001",
                           "--expected-generation", "5"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "created dc-alpha-2 at generation 6"));

  DCR_CHECK_EQ(3, run_cli({"retire", "--root", root, "--id", "dc-alpha", "--expected-generation",
                           "6"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "terminal_state"));

  DCR_CHECK_EQ(0, run_cli({"list", "--root", root, "--no-terminal"}, output, fixture.root()));
  DCR_CHECK(contains(output, "dc-alpha-2"));
  DCR_CHECK(!contains(output, "dc-alpha  "));

  DCR_CHECK_EQ(0, run_cli({"list", "--root", root, "--state", "replaced"}, output, fixture.root()));
  DCR_CHECK(contains(output, "dc-alpha"));
}

DCR_TEST(cli, refuses_usage_errors_and_unreadable_stores) {
  dcrtest::Fixture fixture("cli-errors");
  const std::string root = fixture.root().string();
  std::string output;

  // No command at all.
  DCR_CHECK_EQ(2, run_cli({}, output, fixture.root()));
  DCR_CHECK(contains(output, "usage: dcr"));

  // An unknown command.
  DCR_CHECK_EQ(2, run_cli({"frobnicate", "--root", root}, output, fixture.root()));
  DCR_CHECK(contains(output, "unknown command"));

  // No root.
  DCR_CHECK_EQ(2, run_cli({"stats"}, output, fixture.root()));
  DCR_CHECK(contains(output, "--root is required"));

  // An unknown option.
  DCR_CHECK_EQ(2, run_cli({"stats", "--root", root, "--nonsense"}, output, fixture.root()));
  DCR_CHECK(contains(output, "unknown option"));

  // A malformed identity is a usage error, reported before anything is opened.
  DCR_CHECK_EQ(2, run_cli({"show", "--root", root, "--id", "DC-ASHBURN"}, output,
                          fixture.root()));
  DCR_CHECK(contains(output, "DataCenterId"));

  // A store that does not exist is a store error, not a usage error.
  DCR_CHECK_EQ(4, run_cli({"stats", "--root", (fixture.root() / "absent").string()}, output,
                          fixture.root()));
  DCR_CHECK(contains(output, "store_not_found"));

  // The store is never left half-created by a failed command.
  DCR_CHECK(!std::filesystem::exists(fixture.root() / "absent" / "CURRENT"));
}

DCR_TEST(cli, mutations_are_visible_to_the_library_and_the_other_way_round) {
  // The tool and the library share one store and one set of rules. A change
  // made through the tool is exactly a change made through the API.
  dcrtest::Fixture fixture("cli-shared");
  const std::string root = fixture.root().string();
  std::string output;

  DCR_CHECK_EQ(0, run_cli({"init", "--root", root}, output, fixture.root()));
  DCR_CHECK_EQ(0, run_cli({"register", "--root", root, "--id", "dc-alpha", "--name", "Alpha",
                           "--site", "site-east", "--state", "registered"},
                          output, fixture.root()));

  auto opened = fixture.open(StoreOpenMode::open_existing);
  Registry& registry = *opened.registry;
  DCR_CHECK_EQ(std::uint64_t{1}, registry.generation().value());
  DCR_REQUIRE_OK(const DataCenterRecord record, registry.get(fixture.id("dc-alpha")));
  DCR_CHECK_EQ(std::string("Alpha"), record.display_name);
  DCR_CHECK(record.last_provenance.source() == ProvenanceSource::cli);

  // A library mutation is visible to the tool.
  UpdateMetadataCommand update;
  update.context = fixture.context_at(registry);
  update.id = fixture.id("dc-alpha");
  update.patch.display_name = FieldPatch<std::string>::set("Renamed By Library");
  DCR_REQUIRE_OK(const MutationResult updated, registry.update_metadata(update));
  DCR_CHECK(updated.status == MutationStatus::updated);
  DCR_CHECK(registry.close().has_value());

  DCR_CHECK_EQ(0, run_cli({"show", "--root", root, "--id", "dc-alpha"}, output, fixture.root()));
  DCR_CHECK(contains(output, "Renamed By Library"));
  DCR_CHECK(contains(output, "principal=operator"));

  // The tool's own mutation respects the generation the library left behind.
  DCR_CHECK_EQ(3, run_cli({"set-name", "--root", root, "--id", "dc-alpha", "--name", "Too Soon",
                           "--expected-generation", "1"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "stale_generation"));
  DCR_CHECK_EQ(0, run_cli({"set-name", "--root", root, "--id", "dc-alpha", "--name",
                           "Renamed By Tool", "--expected-generation", "2"},
                          output, fixture.root()));
  DCR_CHECK(contains(output, "updated dc-alpha at generation 3"));
}
