// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// dcr: inspection and narrowly scoped administration for a registry store.
//
// The tool is a consumer of the public API, exactly like any other. It cannot
// bypass a precondition, a lifecycle rule or a generation check, because it has
// no access the library does not give it. Every mutating command that changes
// an existing record requires an explicit expected generation, and arguments
// are validated before the store is touched, so a usage error is never
// reported as a store error.
//
// Exit codes
//   0 success
//   2 usage error, or arguments that do not form a valid command
//   3 the registry rejected the operation; the error category is printed
//   4 the store could not be opened or read
//
// Output is deterministic: the same store produces the same bytes. --json
// switches to a machine-readable form with a stable shape.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/data_center_registry.hpp"

namespace {

using namespace dcr;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitRejected = 3;
constexpr int kExitStore = 4;

struct Arguments {
  std::string command;
  std::filesystem::path root;
  bool json = false;
  bool include_terminal = true;
  std::optional<std::string> id;
  std::optional<std::string> name;
  std::optional<std::string> site;
  std::optional<std::string> state;
  std::optional<std::string> to_state;
  std::optional<std::string> principal;
  std::optional<std::string> reason;
  std::optional<std::string> detail;
  std::optional<std::string> key;
  std::optional<std::string> successor;
  std::optional<std::string> region;
  std::optional<std::string> facility_operator;
  std::optional<std::string> scope;
  std::optional<std::string> order;
  std::optional<std::string> compatibility;
  std::optional<std::uint64_t> expected_generation;
  std::optional<std::uint64_t> limit;
  std::vector<std::string> aliases;
};

void usage() {
  std::fputs(
      "dcr - Data Center Registry inspection and administration\n"
      "\n"
      "usage: dcr <command> --root <dir> [options]\n"
      "\n"
      "read-only commands\n"
      "  inspect                     summarise the registry and its store\n"
      "  verify                      audit the store's integrity in detail\n"
      "  list                        list records\n"
      "  show                        show one record\n"
      "  history                     show the recorded history\n"
      "  stats                       show counters and the state digest\n"
      "  limits                      show the limits in force\n"
      "\n"
      "mutating commands\n"
      "  init                        create an empty store\n"
      "  register                    register a new data center\n"
      "  set-name                    change a record's display name\n"
      "  transition                  move a record to another lifecycle state\n"
      "  attach-site                 add or change a site membership\n"
      "  detach-site                 remove a site membership\n"
      "  retire                      retire a record with no successor\n"
      "  replace                     retire a record and create its successor\n"
      "\n"
      "every mutating command except init and register requires\n"
      "  --expected-generation <n>   the generation the caller is mutating\n"
      "\n"
      "common options\n"
      "  --root <dir>                store root (required)\n"
      "  --json                      machine-readable output\n"
      "  --principal <id>            who the change is attributed to\n"
      "  --reason <code>             machine-readable reason code\n"
      "  --detail <text>             provenance detail\n"
      "  --idempotency-key <key>     retry key for a mutating command\n"
      "\n"
      "record options\n"
      "  --id <dc-id>                canonical data-center identity\n"
      "  --name <text>               display name\n"
      "  --alias <alias>             alias (repeatable)\n"
      "  --site <site-id>            site identity\n"
      "  --state <state>             initial state, filter, or membership role\n"
      "  --to <state>                transition target\n"
      "  --successor <dc-id>         replacement identity\n"
      "  --region <code>             administrative region\n"
      "  --operator <id>             facility operator reference\n"
      "  --scope <scope-id>          ownership scope identifier\n"
      "  --compatibility <key>       compatibility key, e.g. 1.0+0x0000000000000001\n"
      "  --order <order>             enumeration order\n"
      "  --limit <n>                 maximum rows\n"
      "  --no-terminal               exclude retired and replaced records\n",
      stderr);
}

int fail_usage(const std::string& message) {
  std::fprintf(stderr, "dcr: %s\n", message.c_str());
  return kExitUsage;
}

int fail_store(const Error& error) {
  std::printf("error: %s\n", error.to_string().c_str());
  return kExitStore;
}

int fail_rejected(const Error& error) {
  std::printf("rejected: %s\n", error.to_string().c_str());
  return kExitRejected;
}

/// Propagates a rejected argument conversion as a usage error. `destination` is
/// either a declaration with an initialiser or an existing lvalue.
#define DCR_CLI_TRY_IMPL(destination, expression, identifier)                   \
  auto DCR_TRY_CAT(dcr_cli_try_, identifier) = (expression);                    \
  if (!DCR_TRY_CAT(dcr_cli_try_, identifier).has_value()) {                     \
    return fail_usage(DCR_TRY_CAT(dcr_cli_try_, identifier).error().message()); \
  }                                                                             \
  destination = std::move(DCR_TRY_CAT(dcr_cli_try_, identifier)).value()

#define DCR_CLI_TRY(destination, expression) \
  DCR_CLI_TRY_IMPL(destination, expression, __COUNTER__)

std::optional<Arguments> parse_arguments(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value_of = [&](const char* name) -> std::optional<std::string> {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "dcr: %s needs a value\n", name);
        return std::nullopt;
      }
      return std::string(argv[++index]);
    };
    if (argument == "--help" || argument == "-h") {
      usage();
      std::exit(kExitOk);
    }
    if (argument == "--json") {
      arguments.json = true;
      continue;
    }
    if (argument == "--no-terminal") {
      arguments.include_terminal = false;
      continue;
    }
    if (argument == "--root") {
      auto value = value_of("--root");
      if (!value.has_value()) {
        return std::nullopt;
      }
      arguments.root = *value;
      continue;
    }
    if (argument == "--alias") {
      auto value = value_of("--alias");
      if (!value.has_value()) {
        return std::nullopt;
      }
      arguments.aliases.push_back(*value);
      continue;
    }
    if (argument == "--expected-generation" || argument == "--limit") {
      auto value = value_of(argument.c_str());
      if (!value.has_value()) {
        return std::nullopt;
      }
      const bool is_generation = argument == "--expected-generation";
      const std::uint64_t parsed = std::strtoull(value->c_str(), nullptr, 10);
      if (is_generation) {
        arguments.expected_generation = parsed;
      } else {
        arguments.limit = parsed;
      }
      continue;
    }
    const bool known_option =
        argument == "--id" || argument == "--name" || argument == "--site" ||
        argument == "--state" || argument == "--to" || argument == "--principal" ||
        argument == "--reason" || argument == "--detail" ||
        argument == "--idempotency-key" || argument == "--successor" ||
        argument == "--region" || argument == "--operator" || argument == "--scope" ||
        argument == "--order" || argument == "--compatibility";
    if (known_option) {
      auto value = value_of(argument.c_str());
      if (!value.has_value()) {
        return std::nullopt;
      }
      if (argument == "--id") {
        arguments.id = *value;
      } else if (argument == "--name") {
        arguments.name = *value;
      } else if (argument == "--site") {
        arguments.site = *value;
      } else if (argument == "--state") {
        arguments.state = *value;
      } else if (argument == "--to") {
        arguments.to_state = *value;
      } else if (argument == "--principal") {
        arguments.principal = *value;
      } else if (argument == "--reason") {
        arguments.reason = *value;
      } else if (argument == "--detail") {
        arguments.detail = *value;
      } else if (argument == "--idempotency-key") {
        arguments.key = *value;
      } else if (argument == "--successor") {
        arguments.successor = *value;
      } else if (argument == "--region") {
        arguments.region = *value;
      } else if (argument == "--operator") {
        arguments.facility_operator = *value;
      } else if (argument == "--scope") {
        arguments.scope = *value;
      } else if (argument == "--order") {
        arguments.order = *value;
      } else {
        arguments.compatibility = *value;
      }
      continue;
    }
    if (!argument.empty() && argument[0] == '-') {
      std::fprintf(stderr, "dcr: unknown option '%s'\n", argument.c_str());
      return std::nullopt;
    }
    if (arguments.command.empty()) {
      arguments.command = argument;
    } else {
      std::fprintf(stderr, "dcr: unexpected argument '%s'\n", argument.c_str());
      return std::nullopt;
    }
  }
  if (arguments.command.empty()) {
    usage();
    std::exit(kExitUsage);
  }
  if (arguments.root.empty()) {
    std::fprintf(stderr, "dcr: --root is required\n");
    return std::nullopt;
  }
  return arguments;
}

StoreOptions store_options(const Arguments& arguments, StoreOpenMode mode) {
  StoreOptions options;
  options.root = arguments.root;
  options.mode = mode;
  return options;
}

OpenOptions open_options(const Arguments& arguments, StoreOpenMode mode) {
  OpenOptions options;
  options.store = store_options(arguments, mode);
  return options;
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char value : text) {
    switch (value) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(value) < 0x20U) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", value);
          out += buffer;
        } else {
          out.push_back(value);
        }
        break;
    }
  }
  return out;
}

void json_string(std::string_view key, std::string_view value, bool last = false) {
  std::printf("  \"%s\": \"%s\"%s\n", std::string(key).c_str(), json_escape(value).c_str(),
              last ? "" : ",");
}

void json_number(std::string_view key, std::uint64_t value, bool last = false) {
  std::printf("  \"%s\": %llu%s\n", std::string(key).c_str(),
              static_cast<unsigned long long>(value), last ? "" : ",");
}

void print_record_json(const DataCenterRecord& record, std::string_view indent) {
  const std::string pad(indent);
  std::printf("%s{\n", pad.c_str());
  std::printf("%s  \"id\": \"%s\",\n", pad.c_str(), json_escape(record.id.value()).c_str());
  std::printf("%s  \"display_name\": \"%s\",\n", pad.c_str(),
              json_escape(record.display_name).c_str());
  std::printf("%s  \"state\": \"%s\",\n", pad.c_str(),
              std::string(to_string(record.state)).c_str());
  std::printf("%s  \"revision\": \"%s\",\n", pad.c_str(), record.revision.to_string().c_str());
  std::printf("%s  \"created_generation\": \"%s\",\n", pad.c_str(),
              record.created_generation.to_string().c_str());
  std::printf("%s  \"compatibility\": \"%s\",\n", pad.c_str(),
              record.compatibility.to_string().c_str());
  std::printf("%s  \"ownership\": \"%s\",\n", pad.c_str(),
              std::string(to_string(record.ownership.kind())).c_str());
  if (record.ownership.scope().has_value()) {
    std::printf("%s  \"ownership_scope\": \"%s\",\n", pad.c_str(),
                json_escape(record.ownership.scope()->value()).c_str());
  }
  if (record.retired_generation.has_value()) {
    std::printf("%s  \"retired_generation\": \"%s\",\n", pad.c_str(),
                record.retired_generation->to_string().c_str());
  }
  if (record.retirement.has_value()) {
    if (record.retirement->reason.has_value()) {
      std::printf("%s  \"retirement_reason\": \"%s\",\n", pad.c_str(),
                  json_escape(record.retirement->reason->value()).c_str());
    }
    if (record.retirement->replaced_by.has_value()) {
      std::printf("%s  \"replaced_by\": \"%s\",\n", pad.c_str(),
                  json_escape(record.retirement->replaced_by->value()).c_str());
    }
  }
  if (record.replaces.has_value()) {
    std::printf("%s  \"replaces\": \"%s\",\n", pad.c_str(),
                json_escape(record.replaces->value()).c_str());
  }
  std::printf("%s  \"aliases\": [", pad.c_str());
  for (std::size_t index = 0; index < record.aliases.size(); ++index) {
    std::printf("%s\"%s\"", index == 0 ? "" : ", ",
                json_escape(record.aliases[index].value()).c_str());
  }
  std::printf("],\n");
  std::printf("%s  \"memberships\": [", pad.c_str());
  for (std::size_t index = 0; index < record.memberships.size(); ++index) {
    std::printf("%s{\"site\": \"%s\", \"role\": \"%s\"}", index == 0 ? "" : ", ",
                json_escape(record.memberships[index].site.value()).c_str(),
                std::string(to_string(record.memberships[index].role)).c_str());
  }
  std::printf("],\n");
  std::printf("%s  \"facility\": {", pad.c_str());
  bool first = true;
  const auto emit = [&first](const char* key, const std::string& value) {
    std::printf("%s\"%s\": \"%s\"", first ? "" : ", ", key, json_escape(value).c_str());
    first = false;
  };
  if (record.facility.country.has_value()) {
    emit("country", record.facility.country->value());
  }
  if (record.facility.region.has_value()) {
    emit("region", record.facility.region->value());
  }
  if (record.facility.metro.has_value()) {
    emit("metro", record.facility.metro->value());
  }
  if (record.facility.tier.has_value()) {
    emit("tier", std::string(to_string(*record.facility.tier)));
  }
  if (record.facility.facility_operator.has_value()) {
    emit("operator", record.facility.facility_operator->value());
  }
  std::printf("},\n");
  std::printf("%s  \"last_sequence\": \"%s\",\n", pad.c_str(),
              record.last_sequence.to_string().c_str());
  std::printf("%s  \"provenance\": {\"source\": \"%s\", \"principal\": \"%s\", "
              "\"recorded_at\": \"%s\"}\n",
              pad.c_str(), std::string(to_string(record.last_provenance.source())).c_str(),
              json_escape(record.last_provenance.principal().value()).c_str(),
              record.last_provenance.recorded_at().to_string().c_str());
  std::printf("%s}", pad.c_str());
}

void print_record_text(const DataCenterRecord& record) {
  std::printf("%-24s  %-12s  rev %-4s  gen %-6s  %s\n", record.id.value().c_str(),
              std::string(to_string(record.state)).c_str(), record.revision.to_string().c_str(),
              record.created_generation.to_string().c_str(), record.display_name.c_str());
}

// ---------------------------------------------------------------------------
// Read-only commands
// ---------------------------------------------------------------------------

int command_inspect(const Arguments& arguments) {
  auto inspection = Registry::inspect(store_options(arguments, StoreOpenMode::open_existing));
  if (!inspection.has_value()) {
    return fail_store(inspection.error());
  }
  const StoreInspection& value = inspection.value();
  if (arguments.json) {
    std::printf("{\n");
    json_string("root_exists", value.root_exists ? "true" : "false");
    json_string("current_present", value.current_present ? "true" : "false");
    json_string("current_valid", value.current_valid ? "true" : "false");
    json_string("consistent", value.consistent ? "true" : "false");
    json_string("disposition", to_string(value.disposition));
    json_string("current_generation",
                value.current_generation.has_value() ? value.current_generation->to_string() : "");
    json_string("newest_valid_generation",
                value.newest_valid_generation.has_value()
                    ? value.newest_valid_generation->to_string()
                    : "");
    json_string("selected_generation",
                value.selected_generation.has_value() ? value.selected_generation->to_string()
                                                      : "");
    json_string("detail", value.detail);
    std::printf("  \"generation_files\": [\n");
    for (std::size_t index = 0; index < value.generation_files.size(); ++index) {
      const auto& file = value.generation_files[index];
      std::printf("    {\"file\": \"%s\", \"generation\": \"%s\", \"digest_ok\": %s, "
                  "\"parsed\": %s, \"generation_matches_name\": %s, \"detail\": \"%s\"}%s\n",
                  json_escape(file.file_name).c_str(), file.file_generation.to_string().c_str(),
                  file.digest_ok ? "true" : "false", file.parsed ? "true" : "false",
                  file.generation_matches_name ? "true" : "false",
                  json_escape(file.detail).c_str(),
                  index + 1 == value.generation_files.size() ? "" : ",");
    }
    std::printf("  ]\n}\n");
    return kExitOk;
  }
  std::printf("root                 %s\n", arguments.root.string().c_str());
  std::printf("store root exists    %s\n", value.root_exists ? "yes" : "no");
  std::printf("CURRENT present      %s\n", value.current_present ? "yes" : "no");
  std::printf("CURRENT valid        %s\n", value.current_valid ? "yes" : "no");
  std::printf("consistent           %s\n", value.consistent ? "yes" : "no");
  std::printf("disposition          %s\n", std::string(to_string(value.disposition)).c_str());
  std::printf("current generation   %s\n", value.current_generation.has_value()
                                               ? value.current_generation->to_string().c_str()
                                               : "-");
  std::printf("newest valid         %s\n", value.newest_valid_generation.has_value()
                                               ? value.newest_valid_generation->to_string().c_str()
                                               : "-");
  std::printf("generation files     %zu\n", value.generation_files.size());
  for (const auto& file : value.generation_files) {
    std::printf("  %s  generation=%s  digest_ok=%s  parsed=%s  matches_name=%s%s\n",
                file.file_name.c_str(), file.file_generation.to_string().c_str(),
                file.digest_ok ? "yes" : "no", file.parsed ? "yes" : "no",
                file.generation_matches_name ? "yes" : "no",
                file.detail.empty() ? "" : ("  " + file.detail).c_str());
  }
  if (!value.detail.empty()) {
    std::printf("detail               %s\n", value.detail.c_str());
  }
  return kExitOk;
}

int command_verify(const Arguments& arguments) {
  auto inspection = Registry::inspect(store_options(arguments, StoreOpenMode::open_existing));
  if (!inspection.has_value()) {
    return fail_store(inspection.error());
  }
  const StoreInspection& value = inspection.value();
  std::size_t unusable = 0;
  for (const auto& file : value.generation_files) {
    if (!file.digest_ok || !file.generation_matches_name) {
      ++unusable;
    }
  }
  if (arguments.json) {
    std::printf("{\"consistent\": %s, \"current_valid\": %s, \"unusable_files\": %zu, "
                "\"generation_files\": %zu, \"detail\": \"%s\"}\n",
                value.consistent ? "true" : "false", value.current_valid ? "true" : "false",
                unusable, value.generation_files.size(), json_escape(value.detail).c_str());
  } else {
    std::printf("store       %s\n", arguments.root.string().c_str());
    std::printf("files       %zu checked, %zu unusable\n", value.generation_files.size(), unusable);
    std::printf("CURRENT     %s\n", value.current_valid ? "valid" : "invalid or absent");
    std::printf("consistent  %s\n", value.consistent ? "yes" : "no");
    if (!value.detail.empty()) {
      std::printf("detail      %s\n", value.detail.c_str());
    }
  }
  return value.consistent ? kExitOk : kExitRejected;
}

int command_list(const Arguments& arguments) {
  EnumerationQuery query;
  query.include_terminal = arguments.include_terminal;
  if (arguments.state.has_value()) {
    DCR_CLI_TRY(query.state, parse_lifecycle_state(*arguments.state));
  }
  if (arguments.site.has_value()) {
    DCR_CLI_TRY(query.site, SiteId::parse(*arguments.site));
  }
  if (arguments.order.has_value()) {
    DCR_CLI_TRY(query.order, parse_enumeration_order(*arguments.order));
  }
  if (arguments.limit.has_value()) {
    query.limit = static_cast<std::size_t>(*arguments.limit);
  }
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::read_only));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  auto records = opened.value().registry->enumerate(query);
  if (!records.has_value()) {
    return fail_rejected(records.error());
  }
  if (arguments.json) {
    std::printf("[\n");
    for (std::size_t index = 0; index < records.value().size(); ++index) {
      print_record_json(records.value()[index], "  ");
      std::printf("%s\n", index + 1 == records.value().size() ? "" : ",");
    }
    std::printf("]\n");
    return kExitOk;
  }
  for (const auto& record : records.value()) {
    print_record_text(record);
  }
  std::printf("%zu record(s), generation %s\n", records.value().size(),
              opened.value().registry->generation().to_string().c_str());
  return kExitOk;
}

int command_show(const Arguments& arguments) {
  if (!arguments.id.has_value()) {
    return fail_usage("show needs --id");
  }
  DCR_CLI_TRY(const DataCenterId id, DataCenterId::parse(*arguments.id));
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::read_only));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  auto record = opened.value().registry->get(id);
  if (!record.has_value()) {
    return fail_rejected(record.error());
  }
  if (arguments.json) {
    print_record_json(record.value(), "");
    std::printf("\n");
    return kExitOk;
  }
  const DataCenterRecord& value = record.value();
  std::printf("id                  %s\n", value.id.value().c_str());
  std::printf("display name        %s\n", value.display_name.c_str());
  std::printf("state               %s\n", std::string(to_string(value.state)).c_str());
  std::printf("revision            %s\n", value.revision.to_string().c_str());
  std::printf("created generation  %s\n", value.created_generation.to_string().c_str());
  std::printf("compatibility       %s\n", value.compatibility.to_string().c_str());
  std::printf("ownership           %s", std::string(to_string(value.ownership.kind())).c_str());
  if (value.ownership.scope().has_value()) {
    std::printf(" (%s)", value.ownership.scope()->value().c_str());
  }
  std::printf("\n");
  for (const auto& alias : value.aliases) {
    std::printf("alias               %s\n", alias.value().c_str());
  }
  for (const auto& membership : value.memberships) {
    std::printf("membership          %s (%s)\n", membership.site.value().c_str(),
                std::string(to_string(membership.role)).c_str());
  }
  if (value.facility.country.has_value()) {
    std::printf("country             %s\n", value.facility.country->value().c_str());
  }
  if (value.facility.region.has_value()) {
    std::printf("region              %s\n", value.facility.region->value().c_str());
  }
  if (value.facility.metro.has_value()) {
    std::printf("metro               %s\n", value.facility.metro->value().c_str());
  }
  if (value.facility.tier.has_value()) {
    std::printf("tier                %s\n", std::string(to_string(*value.facility.tier)).c_str());
  }
  if (value.facility.coordinates.has_value()) {
    std::printf("coordinates         %d, %d (ten-millionths of a degree)\n",
                value.facility.coordinates->latitude_e7(),
                value.facility.coordinates->longitude_e7());
  }
  if (value.facility.address.has_value()) {
    for (const auto& line : value.facility.address->lines()) {
      std::printf("address             %s\n", line.c_str());
    }
    if (!value.facility.address->locality().empty()) {
      std::printf("locality            %s\n", value.facility.address->locality().c_str());
    }
    if (!value.facility.address->administrative_area().empty()) {
      std::printf("administrative area %s\n",
                  value.facility.address->administrative_area().c_str());
    }
    if (!value.facility.address->postal_code().empty()) {
      std::printf("postal code         %s\n", value.facility.address->postal_code().c_str());
    }
  }
  for (const auto& entry : value.facility.external_refs.entries()) {
    std::printf("external ref        %s = %s\n", entry.key.value().c_str(), entry.value.c_str());
  }
  for (const auto& entry : value.extensions.entries()) {
    std::printf("extension           %s = %s\n", entry.key.value().c_str(), entry.value.c_str());
  }
  if (value.retired_generation.has_value()) {
    std::printf("retired generation  %s\n", value.retired_generation->to_string().c_str());
  }
  if (value.retirement.has_value()) {
    std::printf("retirement reason   %s\n", value.retirement->reason.has_value()
                                               ? value.retirement->reason->value().c_str()
                                               : "(not recorded)");
    if (value.retirement->replaced_by.has_value()) {
      std::printf("replaced by         %s\n", value.retirement->replaced_by->value().c_str());
    }
  }
  if (value.replaces.has_value()) {
    std::printf("replaces            %s\n", value.replaces->value().c_str());
  }
  std::printf("last sequence       %s\n", value.last_sequence.to_string().c_str());
  std::printf("provenance          source=%s principal=%s at=%s\n",
              std::string(to_string(value.last_provenance.source())).c_str(),
              value.last_provenance.principal().value().c_str(),
              value.last_provenance.recorded_at().to_string().c_str());
  if (value.last_provenance.epoch().has_value()) {
    std::printf("provenance epoch    %s\n", value.last_provenance.epoch()->to_string().c_str());
  }
  if (value.last_provenance.reason().has_value()) {
    std::printf("provenance reason   %s\n", value.last_provenance.reason()->value().c_str());
  }
  if (!value.last_provenance.detail().empty()) {
    std::printf("provenance detail   %s\n", value.last_provenance.detail().c_str());
  }
  return kExitOk;
}

int command_history(const Arguments& arguments) {
  HistoryQuery query;
  if (arguments.id.has_value()) {
    DCR_CLI_TRY(query.id, DataCenterId::parse(*arguments.id));
  }
  if (arguments.limit.has_value()) {
    query.limit = static_cast<std::size_t>(*arguments.limit);
  }
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::read_only));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  const HistoryPage page = opened.value().registry->history(query);
  if (arguments.json) {
    std::printf("{\"matched\": %llu, \"dropped\": %llu, \"truncated\": %s, \"entries\": [\n",
                static_cast<unsigned long long>(page.matched_total),
                static_cast<unsigned long long>(page.dropped_entries),
                page.truncated ? "true" : "false");
    for (std::size_t index = 0; index < page.entries.size(); ++index) {
      const auto& entry = page.entries[index];
      const std::string reason_text =
          entry.provenance.reason().has_value()
              ? json_escape(entry.provenance.reason()->value())
              : std::string();
      std::printf("  {\"sequence\": \"%s\", \"generation\": \"%s\", \"id\": \"%s\", "
                  "\"action\": \"%s\", \"revision\": \"%s\", \"new_state\": \"%s\", "
                  "\"principal\": \"%s\", \"source\": \"%s\", \"reason\": \"%s\", "
                  "\"at\": \"%s\"}%s\n",
                  entry.sequence.to_string().c_str(), entry.generation.to_string().c_str(),
                  json_escape(entry.id.value()).c_str(),
                  std::string(to_string(entry.action)).c_str(), entry.revision.to_string().c_str(),
                  std::string(to_string(entry.new_state)).c_str(),
                  json_escape(entry.provenance.principal().value()).c_str(),
                  std::string(to_string(entry.provenance.source())).c_str(), reason_text.c_str(),
                  entry.provenance.recorded_at().to_string().c_str(),
                  index + 1 == page.entries.size() ? "" : ",");
    }
    std::printf("]}\n");
    return kExitOk;
  }
  for (const auto& entry : page.entries) {
    std::printf("%6s  gen %-6s  %-24s  %-22s  %-12s  rev %-4s  %s\n",
                entry.sequence.to_string().c_str(), entry.generation.to_string().c_str(),
                entry.id.value().c_str(), std::string(to_string(entry.action)).c_str(),
                std::string(to_string(entry.new_state)).c_str(),
                entry.revision.to_string().c_str(),
                entry.provenance.principal().value().c_str());
  }
  std::printf("%llu matching entr(ies), %llu dropped from the retained window%s\n",
              static_cast<unsigned long long>(page.matched_total),
              static_cast<unsigned long long>(page.dropped_entries),
              page.truncated ? ", page truncated by --limit" : "");
  return kExitOk;
}

int command_stats(const Arguments& arguments) {
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::read_only));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  const RegistryStats stats = opened.value().registry->stats();
  const RegistryLimits limits = opened.value().registry->limits();
  if (arguments.json) {
    std::printf("{\n");
    json_string("generation", stats.generation.to_string());
    json_string("external_epoch",
                stats.external_epoch.has_value() ? stats.external_epoch->to_string() : "");
    json_number("records", stats.record_count);
    json_number("aliases", stats.alias_count);
    json_number("memberships", stats.membership_count);
    json_number("distinct_sites", stats.distinct_site_count);
    json_number("distinct_ownership_scopes", stats.distinct_ownership_scope_count);
    json_number("history_entries", stats.history_entries);
    json_number("history_dropped", stats.history_dropped);
    json_number("idempotency_entries", stats.idempotency_entries);
    json_number("payload_bytes", stats.snapshot_payload_bytes);
    json_string("digest", stats.snapshot_digest.to_hex());
    json_number("max_records", limits.max_records);
    std::printf("  \"by_state\": {");
    bool first = true;
    for (const LifecycleState state : kAllLifecycleStates) {
      std::printf("%s\"%s\": %llu", first ? "" : ", ", std::string(to_string(state)).c_str(),
                  static_cast<unsigned long long>(stats.count_of(state)));
      first = false;
    }
    std::printf("}\n}\n");
    return kExitOk;
  }
  std::printf("generation           %s\n", stats.generation.to_string().c_str());
  std::printf("external epoch       %s\n", stats.external_epoch.has_value()
                                               ? stats.external_epoch->to_string().c_str()
                                               : "(none recorded)");
  std::printf("records              %llu of %llu allowed\n",
              static_cast<unsigned long long>(stats.record_count),
              static_cast<unsigned long long>(limits.max_records));
  for (const LifecycleState state : kAllLifecycleStates) {
    std::printf("  %-12s       %llu\n", std::string(to_string(state)).c_str(),
                static_cast<unsigned long long>(stats.count_of(state)));
  }
  std::printf("aliases              %llu\n", static_cast<unsigned long long>(stats.alias_count));
  std::printf("memberships          %llu over %llu site(s)\n",
              static_cast<unsigned long long>(stats.membership_count),
              static_cast<unsigned long long>(stats.distinct_site_count));
  std::printf("ownership scopes     %llu\n",
              static_cast<unsigned long long>(stats.distinct_ownership_scope_count));
  std::printf("history entries      %llu (%llu dropped)\n",
              static_cast<unsigned long long>(stats.history_entries),
              static_cast<unsigned long long>(stats.history_dropped));
  std::printf("idempotency entries  %llu\n",
              static_cast<unsigned long long>(stats.idempotency_entries));
  std::printf("payload bytes        %llu\n",
              static_cast<unsigned long long>(stats.snapshot_payload_bytes));
  std::printf("digest               %s\n", stats.snapshot_digest.to_hex().c_str());
  return kExitOk;
}

int command_limits(const Arguments& arguments) {
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::read_only));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  const RegistryLimits limits = opened.value().registry->limits();
  std::printf("max_records                  %llu\n",
              static_cast<unsigned long long>(limits.max_records));
  std::printf("max_query_results            %llu\n",
              static_cast<unsigned long long>(limits.max_query_results));
  std::printf("max_aliases_per_record       %u\n", limits.max_aliases_per_record);
  std::printf("max_memberships_per_record   %u\n", limits.max_memberships_per_record);
  std::printf("max_metadata_entries         %u\n", limits.max_metadata_entries);
  std::printf("max_metadata_value_bytes     %u\n", limits.max_metadata_value_bytes);
  std::printf("max_display_name_bytes       %u\n", limits.max_display_name_bytes);
  std::printf("max_address_lines            %u\n", limits.max_address_lines);
  std::printf("max_address_line_bytes       %u\n", limits.max_address_line_bytes);
  std::printf("max_provenance_detail_bytes  %u\n", limits.max_provenance_detail_bytes);
  std::printf("max_history_entries          %u\n", limits.max_history_entries);
  std::printf("max_idempotency_entries      %u\n", limits.max_idempotency_entries);
  std::printf("max_snapshot_bytes           %llu\n",
              static_cast<unsigned long long>(limits.max_snapshot_bytes));
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Mutating commands
// ---------------------------------------------------------------------------

Result<CommandContext> build_context(const Arguments& arguments) {
  CommandContext context;
  context.provenance.source = ProvenanceSource::cli;
  DCR_TRY_ASSIGN(PrincipalId principal,
                 PrincipalId::parse(arguments.principal.value_or("operator")));
  context.provenance.principal = std::move(principal);
  context.provenance.detail = arguments.detail.value_or("dcr command");
  if (arguments.key.has_value()) {
    DCR_TRY_ASSIGN(IdempotencyKey key, IdempotencyKey::parse(*arguments.key));
    context.idempotency_key = std::move(key);
  }
  if (arguments.expected_generation.has_value()) {
    DCR_TRY_ASSIGN(RegistryGeneration generation,
                   RegistryGeneration::from_value(*arguments.expected_generation));
    context.precondition.expected_generation = generation;
  }
  return context;
}

Result<CompatibilityKey> compatibility_of(const Arguments& arguments) {
  if (arguments.compatibility.has_value()) {
    return CompatibilityKey::parse(*arguments.compatibility);
  }
  return CompatibilityKey::make(1, 0, 0);
}

Result<RecordDraft> draft_of(const Arguments& arguments) {
  RecordDraft draft;
  if (arguments.name.has_value()) {
    draft.display_name = *arguments.name;
  }
  for (const auto& alias_text : arguments.aliases) {
    DCR_TRY_ASSIGN(Alias alias, Alias::parse(alias_text));
    draft.aliases.push_back(std::move(alias));
  }
  if (arguments.site.has_value()) {
    DCR_TRY_ASSIGN(SiteId site, SiteId::parse(*arguments.site));
    draft.memberships.push_back(SiteMembership{std::move(site), MembershipRole::primary});
  }
  if (arguments.scope.has_value()) {
    DCR_TRY_ASSIGN(OwnershipScopeId scope, OwnershipScopeId::parse(*arguments.scope));
    DCR_TRY_ASSIGN(draft.ownership,
                   OwnershipScope::make(OwnershipKind::platform, std::move(scope)));
  }
  DCR_TRY_ASSIGN(draft.compatibility, compatibility_of(arguments));
  if (arguments.region.has_value()) {
    DCR_TRY_ASSIGN(RegionCode region, RegionCode::parse(*arguments.region));
    draft.facility.region = std::move(region);
  }
  if (arguments.facility_operator.has_value()) {
    DCR_TRY_ASSIGN(PrincipalId facility_operator,
                   PrincipalId::parse(*arguments.facility_operator));
    draft.facility.facility_operator = std::move(facility_operator);
  }
  return draft;
}

int report_mutation(const Arguments& arguments, const MutationResult& result) {
  if (arguments.json) {
    std::printf("{\n");
    json_string("status", to_string(result.status));
    json_string("id", result.id.value());
    json_string("generation", result.generation.to_string());
    json_string("revision", result.revision.to_string());
    json_string("related_id", result.related_id.has_value() ? result.related_id->value() : "");
    json_string("sequence", result.sequence.has_value() ? result.sequence->to_string() : "", true);
    std::printf("}\n");
  } else {
    std::printf("%s %s at generation %s (revision %s)\n",
                std::string(to_string(result.status)).c_str(), result.id.value().c_str(),
                result.generation.to_string().c_str(), result.revision.to_string().c_str());
  }
  return kExitOk;
}

int command_init(const Arguments& arguments) {
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::create_new));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  if (arguments.json) {
    std::printf("{\"created\": true, \"generation\": \"%s\"}\n",
                opened.value().registry->generation().to_string().c_str());
  } else {
    std::printf("created an empty registry at %s (generation %s)\n",
                arguments.root.string().c_str(),
                opened.value().registry->generation().to_string().c_str());
  }
  return kExitOk;
}

int command_register(const Arguments& arguments) {
  if (!arguments.id.has_value() || !arguments.name.has_value()) {
    return fail_usage("register needs --id and --name");
  }
  RegisterCommand command;
  DCR_CLI_TRY(command.context, build_context(arguments));
  DCR_CLI_TRY(command.id, DataCenterId::parse(*arguments.id));
  DCR_CLI_TRY(command.draft, draft_of(arguments));
  if (arguments.state.has_value()) {
    DCR_CLI_TRY(command.initial_state, parse_lifecycle_state(*arguments.state));
  }
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::open_existing));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  auto outcome = opened.value().registry->register_data_center(command);
  if (!outcome.has_value()) {
    return fail_rejected(outcome.error());
  }
  return report_mutation(arguments, outcome.value());
}

int command_set_name(const Arguments& arguments) {
  if (!arguments.id.has_value() || !arguments.name.has_value()) {
    return fail_usage("set-name needs --id and --name");
  }
  if (!arguments.expected_generation.has_value()) {
    return fail_usage("set-name needs --expected-generation");
  }
  UpdateMetadataCommand command;
  DCR_CLI_TRY(command.context, build_context(arguments));
  DCR_CLI_TRY(command.id, DataCenterId::parse(*arguments.id));
  command.patch.display_name = FieldPatch<std::string>::set(*arguments.name);
  if (!arguments.aliases.empty()) {
    std::vector<Alias> aliases;
    for (const auto& alias_text : arguments.aliases) {
      DCR_CLI_TRY(const Alias alias, Alias::parse(alias_text));
      aliases.push_back(alias);
    }
    command.patch.aliases = FieldPatch<std::vector<Alias>>::set(std::move(aliases));
  }
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::open_existing));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  auto outcome = opened.value().registry->update_metadata(command);
  if (!outcome.has_value()) {
    return fail_rejected(outcome.error());
  }
  return report_mutation(arguments, outcome.value());
}

int command_transition(const Arguments& arguments) {
  if (!arguments.id.has_value() || !arguments.to_state.has_value()) {
    return fail_usage("transition needs --id and --to");
  }
  if (!arguments.expected_generation.has_value()) {
    return fail_usage("transition needs --expected-generation");
  }
  TransitionCommand command;
  DCR_CLI_TRY(command.context, build_context(arguments));
  DCR_CLI_TRY(command.id, DataCenterId::parse(*arguments.id));
  DCR_CLI_TRY(command.target_state, parse_lifecycle_state(*arguments.to_state));
  if (arguments.reason.has_value()) {
    DCR_CLI_TRY(command.reason, ReasonCode::parse(*arguments.reason));
  }
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::open_existing));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  auto outcome = opened.value().registry->transition_lifecycle(command);
  if (!outcome.has_value()) {
    return fail_rejected(outcome.error());
  }
  return report_mutation(arguments, outcome.value());
}

int command_attach_site(const Arguments& arguments) {
  if (!arguments.id.has_value() || !arguments.site.has_value()) {
    return fail_usage("attach-site needs --id and --site");
  }
  if (!arguments.expected_generation.has_value()) {
    return fail_usage("attach-site needs --expected-generation");
  }
  AttachSiteCommand command;
  DCR_CLI_TRY(command.context, build_context(arguments));
  DCR_CLI_TRY(command.id, DataCenterId::parse(*arguments.id));
  DCR_CLI_TRY(const SiteId membership_site, SiteId::parse(*arguments.site));
  MembershipRole membership_role = MembershipRole::primary;
  if (arguments.state.has_value()) {
    DCR_CLI_TRY(membership_role, parse_membership_role(*arguments.state));
  }
  command.membership = SiteMembership{membership_site, membership_role};
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::open_existing));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  auto outcome = opened.value().registry->attach_site(command);
  if (!outcome.has_value()) {
    return fail_rejected(outcome.error());
  }
  return report_mutation(arguments, outcome.value());
}

int command_detach_site(const Arguments& arguments) {
  if (!arguments.id.has_value() || !arguments.site.has_value()) {
    return fail_usage("detach-site needs --id and --site");
  }
  if (!arguments.expected_generation.has_value()) {
    return fail_usage("detach-site needs --expected-generation");
  }
  DetachSiteCommand command;
  DCR_CLI_TRY(command.context, build_context(arguments));
  DCR_CLI_TRY(command.id, DataCenterId::parse(*arguments.id));
  DCR_CLI_TRY(command.site, SiteId::parse(*arguments.site));
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::open_existing));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  auto outcome = opened.value().registry->detach_site(command);
  if (!outcome.has_value()) {
    return fail_rejected(outcome.error());
  }
  return report_mutation(arguments, outcome.value());
}

int command_retire(const Arguments& arguments) {
  if (!arguments.id.has_value()) {
    return fail_usage("retire needs --id");
  }
  if (!arguments.expected_generation.has_value()) {
    return fail_usage("retire needs --expected-generation");
  }
  RetireCommand command;
  DCR_CLI_TRY(command.context, build_context(arguments));
  DCR_CLI_TRY(command.id, DataCenterId::parse(*arguments.id));
  if (arguments.reason.has_value()) {
    DCR_CLI_TRY(command.reason, ReasonCode::parse(*arguments.reason));
  }
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::open_existing));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  auto outcome = opened.value().registry->retire_data_center(command);
  if (!outcome.has_value()) {
    return fail_rejected(outcome.error());
  }
  return report_mutation(arguments, outcome.value());
}

int command_replace(const Arguments& arguments) {
  if (!arguments.id.has_value() || !arguments.successor.has_value() ||
      !arguments.name.has_value()) {
    return fail_usage("replace needs --id, --successor and --name");
  }
  if (!arguments.expected_generation.has_value()) {
    return fail_usage("replace needs --expected-generation");
  }
  ReplaceCommand command;
  DCR_CLI_TRY(command.context, build_context(arguments));
  DCR_CLI_TRY(command.retired_id, DataCenterId::parse(*arguments.id));
  DCR_CLI_TRY(command.new_id, DataCenterId::parse(*arguments.successor));
  DCR_CLI_TRY(command.draft, draft_of(arguments));
  DCR_CLI_TRY(command.reason, ReasonCode::parse(arguments.reason.value_or("facility_replaced")));
  if (arguments.state.has_value()) {
    DCR_CLI_TRY(command.initial_state, parse_lifecycle_state(*arguments.state));
  }
  auto opened = Registry::open(open_options(arguments, StoreOpenMode::open_existing));
  if (!opened.has_value()) {
    return fail_store(opened.error());
  }
  auto outcome = opened.value().registry->replace_data_center(command);
  if (!outcome.has_value()) {
    return fail_rejected(outcome.error());
  }
  return report_mutation(arguments, outcome.value());
}

int dispatch(const Arguments& arguments) {
  const std::string& command = arguments.command;
  if (command == "inspect") {
    return command_inspect(arguments);
  }
  if (command == "verify") {
    return command_verify(arguments);
  }
  if (command == "list") {
    return command_list(arguments);
  }
  if (command == "show") {
    return command_show(arguments);
  }
  if (command == "history") {
    return command_history(arguments);
  }
  if (command == "stats") {
    return command_stats(arguments);
  }
  if (command == "limits") {
    return command_limits(arguments);
  }
  if (command == "init") {
    return command_init(arguments);
  }
  if (command == "register") {
    return command_register(arguments);
  }
  if (command == "set-name") {
    return command_set_name(arguments);
  }
  if (command == "transition") {
    return command_transition(arguments);
  }
  if (command == "attach-site") {
    return command_attach_site(arguments);
  }
  if (command == "detach-site") {
    return command_detach_site(arguments);
  }
  if (command == "retire") {
    return command_retire(arguments);
  }
  if (command == "replace") {
    return command_replace(arguments);
  }
  std::fprintf(stderr, "dcr: unknown command '%s'\n", command.c_str());
  usage();
  return kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
  auto arguments = parse_arguments(argc, argv);
  if (!arguments.has_value()) {
    return kExitUsage;
  }
  return dispatch(arguments.value());
}
