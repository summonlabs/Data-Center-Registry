// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Shared fixtures for the test suites.
//
// Everything here is deterministic: temporary directories are created under
// the build tree with a unique name, and the pseudo-random generator is seeded
// explicitly so that a failure report names a seed that reproduces it.

#ifndef DCR_TESTS_TEST_SUPPORT_HPP
#define DCR_TESTS_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/commands.hpp"
#include "dcr/registry.hpp"
#include "dcr/time.hpp"

namespace dcrtest {

/// A directory that is removed when the fixture goes out of scope.
class TempDir {
 public:
  explicit TempDir(std::string_view label);
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path child(std::string_view name) const;

 private:
  std::filesystem::path path_;
};

/// xoshiro256** with an explicit seed. Reproducible across platforms and
/// standard libraries, which std::mt19937 distributions are not.
class Rng {
 public:
  explicit Rng(std::uint64_t seed);

  [[nodiscard]] std::uint64_t next_u64();
  /// Uniform in [0, bound); bound must be non-zero.
  [[nodiscard]] std::uint64_t below(std::uint64_t bound);
  [[nodiscard]] bool chance(std::uint32_t numerator, std::uint32_t denominator);
  [[nodiscard]] std::string token(std::string_view prefix, std::size_t length);
  [[nodiscard]] std::string text(std::size_t length);

 private:
  std::uint64_t state_[4];
};

/// Builds a valid data-center identity: `dc-` and a lower-case slug.
[[nodiscard]] std::string data_center_id_text(std::uint64_t index);
[[nodiscard]] std::string site_id_text(std::uint64_t index);

/// The standard fixture: a fixed clock, default limits, and helpers that build
/// valid commands so that each test states only what it is actually testing.
class Fixture {
 public:
  explicit Fixture(std::string_view label);

  [[nodiscard]] const std::filesystem::path& root() const { return dir_.path(); }
  [[nodiscard]] dcr::ManualClock& clock() { return clock_; }
  [[nodiscard]] const dcr::RegistryLimits& limits() const { return limits_; }
  void set_limits(const dcr::RegistryLimits& limits) { limits_ = limits; }

  [[nodiscard]] dcr::OpenOptions open_options(dcr::StoreOpenMode mode) const;
  [[nodiscard]] dcr::StoreOptions store_options(dcr::StoreOpenMode mode) const;

  /// Opens the registry, requiring success.
  [[nodiscard]] dcr::OpenedRegistry open(dcr::StoreOpenMode mode =
                                             dcr::StoreOpenMode::open_or_create);

  /// The provenance every mutation in a test uses unless it says otherwise.
  [[nodiscard]] dcr::ProvenanceInput provenance() const;
  [[nodiscard]] dcr::CommandContext context() const;
  [[nodiscard]] dcr::CommandContext context_with_key(std::string_view key,
                                                     std::uint64_t generation) const;

  /// A command context preconditioned on the registry's current generation.
  [[nodiscard]] dcr::CommandContext context_at(const dcr::Registry& registry,
                                               std::string_view principal = "operator") const;

  [[nodiscard]] dcr::DataCenterId id(std::string_view text) const;
  [[nodiscard]] dcr::SiteId site(std::string_view text) const;
  [[nodiscard]] dcr::Alias alias(std::string_view text) const;

  /// A registration command for `id` with a default draft.
  [[nodiscard]] dcr::RegisterCommand register_command(std::string_view id_text,
                                                      std::string_view display_name,
                                                      dcr::LifecycleState initial_state,
                                                      std::string_view site_text) const;

  void advance(std::int64_t millis);

  /// A default draft that satisfies the live-state membership rule.
  [[nodiscard]] dcr::RecordDraft draft(std::string_view display_name,
                                       std::string_view site_text) const;

  [[nodiscard]] static dcr::CompatibilityKey compatibility(std::uint16_t major,
                                                           std::uint16_t minor,
                                                           std::uint64_t mask);

 private:
  TempDir dir_;
  dcr::ManualClock clock_;
  dcr::RegistryLimits limits_;
};

/// Runs a child process and returns its exit code, or -1 when it could not be
/// started. The child is given the arguments verbatim and writes its own output
/// to a file, so nothing is captured through a pipe.
[[nodiscard]] int run_child_process(const std::filesystem::path& executable,
                                    const std::vector<std::string>& arguments);

/// Runs a child process with its standard output and standard error redirected
/// to a file, and returns its exit code. Used to check what a tool prints.
/// The redirection goes through the platform shell, which is the only portable
/// way to capture a child's streams without a pipe.
[[nodiscard]] int run_child_process_captured(const std::filesystem::path& executable,
                                            const std::vector<std::string>& arguments,
                                            const std::filesystem::path& output_file);

/// A file that exists while the object does, used to hold one process in a
/// known state until another is ready. There is no timeout involved: the
/// process that waits is the one being tested, and the process that releases it
/// always does so.
class ScopedFlag {
 public:
  /// Creates the flag file.
  static ScopedFlag create(const std::filesystem::path& path);

  ScopedFlag() = default;
  ~ScopedFlag();

  ScopedFlag(const ScopedFlag&) = delete;
  ScopedFlag& operator=(const ScopedFlag&) = delete;
  ScopedFlag(ScopedFlag&& other) noexcept;
  ScopedFlag& operator=(ScopedFlag&& other) noexcept;

  /// True once another process has created the file this object waits for.
  [[nodiscard]] bool exists() const;

 private:
  std::filesystem::path path_;
};

/// True when the child-process harness is built for this platform.
[[nodiscard]] bool child_process_supported();

/// The path of a helper executable that the test run was told about through its
/// environment. Returns an empty path when the variable is not set.
[[nodiscard]] std::filesystem::path helper_executable(const char* environment_variable);

/// Reads a whole file as text. Returns false when it cannot be read.
[[nodiscard]] bool read_text_file(const std::filesystem::path& path, std::string& out);

/// True when the file exists and holds `marker` as a line.
[[nodiscard]] bool file_contains_line(const std::filesystem::path& path,
                                      std::string_view marker, std::string& contents);

}  // namespace dcrtest

#endif  // DCR_TESTS_TEST_SUPPORT_HPP
