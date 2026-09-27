// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Benchmarks for the operations whose cost a reader would want to know.
//
// What is measured
//   bulk load            registering N records, timed end to end
//   durable register     a committed registration, including the flush
//   durable update       a committed metadata update, including the flush
//   durable transition   a committed lifecycle transition, including the flush
//   enumeration          canonical-order enumeration of every record
//   filtered enumeration enumeration by site
//   point lookup         get by canonical identity
//   alias lookup         get by alias
//   history page         one page of the history window
//   statistics           counters plus the canonical payload digest
//   reopen               reading and verifying the newest generation
//
// What is not claimed
//   The numbers describe this library on this machine, with this filesystem,
//   under this build. They are not a hardware claim and not a production
//   claim. Every durable figure includes the flush, because a commit that is
//   not flushed is not a commit.
//
// The workload is SYNTHETIC: the records are generated, not imported from a
// real facility.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

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

struct Samples {
  std::vector<double> micros;

  void add(double micros_value) { micros.push_back(micros_value); }

  [[nodiscard]] double median() const {
    if (micros.empty()) {
      return 0.0;
    }
    std::vector<double> sorted = micros;
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
  }
  [[nodiscard]] double minimum() const {
    return micros.empty() ? 0.0 : *std::min_element(micros.begin(), micros.end());
  }
  [[nodiscard]] double maximum() const {
    return micros.empty() ? 0.0 : *std::max_element(micros.begin(), micros.end());
  }
  [[nodiscard]] double mean() const {
    if (micros.empty()) {
      return 0.0;
    }
    double total = 0.0;
    for (const double value : micros) {
      total += value;
    }
    return total / static_cast<double>(micros.size());
  }
  [[nodiscard]] double ops_per_second() const {
    const double average = mean();
    return average <= 0.0 ? 0.0 : 1000000.0 / average;
  }
};

double micros_since(std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::micro>(elapsed).count();
}

class Stopwatch {
 public:
  Stopwatch() : start_(std::chrono::steady_clock::now()) {}
  [[nodiscard]] double stop_micros() const { return micros_since(start_); }

 private:
  std::chrono::steady_clock::time_point start_;
};

std::string id_text(std::uint64_t index) { return "dc-bench-" + std::to_string(index); }

std::string site_text(std::uint64_t index) { return "site-bench-" + std::to_string(index % 8); }

RegisterCommand make_registration(FixedClock& clock, std::uint64_t index) {
  clock.advance(1);
  RegisterCommand command;
  command.context.provenance.source = ProvenanceSource::import;
  command.context.provenance.principal = *PrincipalId::parse("benchmark");
  command.context.provenance.detail = "synthetic benchmark record";
  command.id = *DataCenterId::parse(id_text(index));
  command.draft.display_name = "Benchmark Facility " + std::to_string(index);
  command.draft.memberships = {
      SiteMembership{*SiteId::parse(site_text(index)), MembershipRole::primary}};
  command.draft.ownership = OwnershipScope::unassigned();
  command.draft.compatibility = *CompatibilityKey::make(1, 0, 0x1);
  command.draft.facility.country = *CountryCode::parse("US");
  command.draft.facility.region = *RegionCode::parse("us-east-1");
  command.draft.aliases = {*Alias::parse("bench-alias-" + std::to_string(index))};
  command.initial_state = LifecycleState::registered;
  return command;
}

struct SizeReport {
  std::uint64_t records = 0;
  double bulk_load_seconds = 0.0;
  double payload_bytes = 0.0;
  Samples durable_register;
  Samples durable_update;
  Samples durable_transition;
  Samples enumerate_all;
  Samples enumerate_site;
  Samples point_lookup;
  Samples alias_lookup;
  Samples history_page;
  Samples statistics;
  double reopen_millis = 0.0;
  bool integrity_ok = false;
  std::string digest;
};

#include <sstream>

SizeReport run_size(const std::filesystem::path& root, std::uint64_t target,
                    std::size_t measured_operations, bool verbose) {
  SizeReport report;
  report.records = target;
  std::error_code code;
  std::filesystem::remove_all(root, code);

  FixedClock clock;

  // --- bulk load, relaxed durability ------------------------------------
  {
    OpenOptions options;
    options.store.root = root;
    options.store.mode = StoreOpenMode::open_or_create;
    options.store.durability = DurabilityMode::relaxed;
    options.clock = &clock;
    auto opened = Registry::open(options);
    if (!opened.has_value()) {
      std::fprintf(stderr, "benchmark could not open a store: %s\n",
                   opened.error().to_string().c_str());
      std::exit(1);
    }
    Registry& registry = *opened.value().registry;
    const Stopwatch watch;
    for (std::uint64_t index = 0; index < target; ++index) {
      auto created = registry.register_data_center(make_registration(clock, index));
      if (!created.has_value()) {
        std::fprintf(stderr, "benchmark bulk load failed: %s\n",
                     created.error().to_string().c_str());
        std::exit(1);
      }
    }
    report.bulk_load_seconds = watch.stop_micros() / 1000000.0;
    report.payload_bytes = static_cast<double>(registry.stats().snapshot_payload_bytes);
    if (auto closed = registry.close(); !closed.has_value()) {
      std::fprintf(stderr, "benchmark could not close: %s\n",
                   closed.error().to_string().c_str());
      std::exit(1);
    }
  }

  // --- durable operations on that registry ------------------------------
  {
    OpenOptions options;
    options.store.root = root;
    options.store.mode = StoreOpenMode::open_existing;
    options.store.durability = DurabilityMode::strict;
    options.clock = &clock;
    const Stopwatch reopen_watch;
    auto opened = Registry::open(options);
    if (!opened.has_value()) {
      std::fprintf(stderr, "benchmark could not reopen: %s\n",
                   opened.error().to_string().c_str());
      std::exit(1);
    }
    report.reopen_millis = reopen_watch.stop_micros() / 1000.0;
    Registry& registry = *opened.value().registry;

    // A durable registration: a new record, a new generation, a flush.
    for (std::size_t index = 0; index < measured_operations; ++index) {
      const Stopwatch watch;
      auto created =
          registry.register_data_center(make_registration(clock, target + index));
      if (!created.has_value()) {
        std::fprintf(stderr, "benchmark registration failed: %s\n",
                     created.error().to_string().c_str());
        std::exit(1);
      }
      report.durable_register.add(watch.stop_micros());
    }

    // A durable metadata update on one record.
    const DataCenterId hot_id = *DataCenterId::parse(id_text(0));
    for (std::size_t index = 0; index < measured_operations; ++index) {
      UpdateMetadataCommand command;
      command.context.provenance.source = ProvenanceSource::cli;
      command.context.provenance.principal = *PrincipalId::parse("benchmark");
      command.context.precondition.expected_generation = registry.generation();
      command.id = hot_id;
      command.patch.display_name =
          FieldPatch<std::string>::set("Benchmark Rename " + std::to_string(index));
      const Stopwatch watch;
      auto updated = registry.update_metadata(command);
      if (!updated.has_value()) {
        std::fprintf(stderr, "benchmark update failed: %s\n",
                     updated.error().to_string().c_str());
        std::exit(1);
      }
      report.durable_update.add(watch.stop_micros());
    }

    // A durable lifecycle transition, bouncing between two legal states.
    for (std::size_t index = 0; index < measured_operations; ++index) {
      TransitionCommand command;
      command.context.provenance.source = ProvenanceSource::cli;
      command.context.provenance.principal = *PrincipalId::parse("benchmark");
      command.context.precondition.expected_generation = registry.generation();
      command.id = hot_id;
      command.target_state =
          index % 2 == 0 ? LifecycleState::degraded : LifecycleState::active;
      command.reason = *ReasonCode::parse("benchmark_cycle");
      const Stopwatch watch;
      auto transitioned = registry.transition_lifecycle(command);
      if (!transitioned.has_value()) {
        std::fprintf(stderr, "benchmark transition failed: %s\n",
                     transitioned.error().to_string().c_str());
        std::exit(1);
      }
      report.durable_transition.add(watch.stop_micros());
    }

    // --- read paths ------------------------------------------------------
    constexpr std::size_t kReadIterations = 50;
    for (std::size_t index = 0; index < kReadIterations; ++index) {
      const Stopwatch watch;
      auto records = registry.enumerate(EnumerationQuery{});
      if (!records.has_value()) {
        std::exit(1);
      }
      report.enumerate_all.add(watch.stop_micros());
      if (records.value().size() != registry.record_count()) {
        std::fprintf(stderr, "benchmark enumeration returned the wrong count\n");
        std::exit(1);
      }
    }

    EnumerationQuery site_query;
    site_query.site = *SiteId::parse(site_text(3));
    for (std::size_t index = 0; index < kReadIterations; ++index) {
      const Stopwatch watch;
      auto records = registry.enumerate(site_query);
      if (!records.has_value()) {
        std::exit(1);
      }
      report.enumerate_site.add(watch.stop_micros());
    }

    for (std::size_t index = 0; index < kReadIterations * 10; ++index) {
      const std::uint64_t which = (index * 7919U) % registry.record_count();
      const DataCenterId wanted = *DataCenterId::parse(id_text(which));
      const Stopwatch watch;
      auto record = registry.get(wanted);
      if (!record.has_value()) {
        std::fprintf(stderr, "benchmark point lookup missed\n");
        std::exit(1);
      }
      report.point_lookup.add(watch.stop_micros());
    }

    for (std::size_t index = 0; index < kReadIterations * 10; ++index) {
      const std::uint64_t which = (index * 7919U) % registry.record_count();
      const Alias wanted = *Alias::parse("bench-alias-" + std::to_string(which));
      const Stopwatch watch;
      auto record = registry.find_by_alias(wanted);
      if (!record.has_value()) {
        std::fprintf(stderr, "benchmark alias lookup missed\n");
        std::exit(1);
      }
      report.alias_lookup.add(watch.stop_micros());
    }

    for (std::size_t index = 0; index < kReadIterations; ++index) {
      HistoryQuery query;
      query.id = hot_id;
      query.limit = 64;
      const Stopwatch watch;
      const HistoryPage page = registry.history(query);
      report.history_page.add(watch.stop_micros());
      (void)page;
    }

    for (std::size_t index = 0; index < kReadIterations; ++index) {
      const Stopwatch watch;
      const RegistryStats stats = registry.stats();
      report.statistics.add(watch.stop_micros());
      report.digest = stats.snapshot_digest.to_hex();
    }

    if (auto closed = registry.close(); !closed.has_value()) {
      std::fprintf(stderr, "benchmark could not close: %s\n",
                   closed.error().to_string().c_str());
      std::exit(1);
    }
  }

  // --- integrity of everything the benchmark wrote ----------------------
  StoreOptions inspection_options;
  inspection_options.root = root;
  inspection_options.mode = StoreOpenMode::open_existing;
  auto inspection = Registry::inspect(inspection_options);
  if (!inspection.has_value()) {
    std::fprintf(stderr, "benchmark integrity inspection failed: %s\n",
                 inspection.error().to_string().c_str());
    std::exit(1);
  }
  report.integrity_ok = inspection.value().consistent;
  if (verbose) {
    std::printf("    store: %zu generation file(s), consistent=%s\n",
                inspection.value().generation_files.size(),
                report.integrity_ok ? "yes" : "no");
  }
  std::filesystem::remove_all(root, code);
  return report;
}

void print_samples(const char* label, const Samples& samples) {
  std::printf("  %-22s %10.1f %10.1f %10.1f %10.1f %12.0f\n", label, samples.minimum(),
              samples.median(), samples.mean(), samples.maximum(), samples.ops_per_second());
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::uint64_t> sizes = {100, 1000, 3000};
  std::size_t operations = 20;
  bool verbose = true;
  std::filesystem::path root =
      std::filesystem::temp_directory_path() / "dcr-benchmark-store";

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--quiet") {
      verbose = false;
    } else if (argument == "--sizes" && index + 1 < argc) {
      sizes.clear();
      std::string list = argv[++index];
      std::size_t start = 0;
      while (start <= list.size()) {
        const std::size_t comma = list.find(',', start);
        const std::string piece = list.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!piece.empty()) {
          sizes.push_back(std::strtoull(piece.c_str(), nullptr, 10));
        }
        if (comma == std::string::npos) {
          break;
        }
        start = comma + 1;
      }
    } else if (argument == "--operations" && index + 1 < argc) {
      operations = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
      if (operations == 0) {
        operations = 1;
      }
    } else if (argument == "--root" && index + 1 < argc) {
      root = argv[++index];
    } else {
      std::fprintf(stderr,
                   "usage: dcr_benchmarks [--sizes 100,1000,3000] [--operations N] "
                   "[--root DIR] [--quiet]\n");
      return 2;
    }
  }
  if (sizes.empty()) {
    sizes = {100};
  }

  const char* build_name =
#ifdef NDEBUG
      "Release";
#else
      "Debug";
#endif
  const std::string compiler_name =
#if defined(_MSC_VER)
      "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
      std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
      std::string("GCC ") + __VERSION__;
#else
      std::string("unknown");
#endif

  std::printf("Data Center Registry benchmarks\n");
  std::printf("  build:      %s\n", build_name);
  std::printf("  compiler:   %s\n", compiler_name.c_str());
  std::printf("  workload:   SYNTHETIC records on a real durable store in %s\n",
              root.string().c_str());
  std::printf("  durable ops include the flush; the reported figures are machine-specific\n");
  std::printf("  and are not a hardware or production claim\n\n");
  std::printf("  %-22s %10s %10s %10s %10s %12s\n", "operation", "min us", "median us",
              "mean us", "max us", "ops/sec");
  std::printf("  %s\n", std::string(80, '-').c_str());

  bool all_ok = true;
  for (const std::uint64_t size : sizes) {
    if (verbose) {
      std::printf("\nregistry size %llu\n", static_cast<unsigned long long>(size));
    }
    const SizeReport report = run_size(root, size, operations, verbose);
    if (verbose) {
      std::printf("    bulk load (relaxed durability): %.2f s, %.2f ms per record\n",
                  report.bulk_load_seconds,
                  report.bulk_load_seconds * 1000.0 / static_cast<double>(report.records));
      std::printf("    canonical payload: %.0f bytes; reopen and verify: %.1f ms\n",
                  report.payload_bytes, report.reopen_millis);
      std::printf("    digest: %s\n", report.digest.c_str());
      std::printf("    integrity after the run: %s\n", report.integrity_ok ? "verified" : "FAILED");
    }
    print_samples("durable register", report.durable_register);
    print_samples("durable update", report.durable_update);
    print_samples("durable transition", report.durable_transition);
    print_samples("enumerate all", report.enumerate_all);
    print_samples("enumerate by site", report.enumerate_site);
    print_samples("point lookup", report.point_lookup);
    print_samples("alias lookup", report.alias_lookup);
    print_samples("history page", report.history_page);
    print_samples("statistics", report.statistics);
    all_ok = all_ok && report.integrity_ok;
  }

  std::printf("\n");
  if (!all_ok) {
    std::printf("one or more benchmark stores failed integrity verification\n");
    return 1;
  }
  std::printf("every benchmark store verified its integrity after the run\n");
  return 0;
}
