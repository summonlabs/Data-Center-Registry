// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace dcrtest {
namespace {

std::filesystem::path test_scratch_root() {
  // Relative to the current working directory, which CTest sets to the build
  // tree. Never an absolute machine-specific path.
  return std::filesystem::path("dcr-test-scratch");
}

std::uint64_t unique_counter() {
  static std::uint64_t counter = 0;
  return counter++;
}

std::string sanitize(std::string_view label) {
  std::string out;
  out.reserve(label.size());
  for (const char value : label) {
    const bool ok = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                    (value >= '0' && value <= '9') || value == '-' || value == '_';
    out.push_back(ok ? value : '-');
  }
  if (out.empty()) {
    out = "test";
  }
  return out;
}

}  // namespace

TempDir::TempDir(std::string_view label) {
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  path_ = test_scratch_root() /
          (sanitize(label) + "-" + std::to_string(ticks) + "-" +
           std::to_string(unique_counter()));
  std::error_code code;
  std::filesystem::remove_all(path_, code);
  std::filesystem::create_directories(path_, code);
}

TempDir::~TempDir() {
  std::error_code code;
  std::filesystem::remove_all(path_, code);
}

std::filesystem::path TempDir::child(std::string_view name) const { return path_ / name; }

Rng::Rng(std::uint64_t seed) {
  // splitmix64 expands the seed into the xoshiro state, so that neighbouring
  // seeds produce unrelated streams.
  std::uint64_t state = seed + 0x9E3779B97F4A7C15ULL;
  for (auto& slot : state_) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    slot = z ^ (z >> 31U);
  }
}

std::uint64_t Rng::next_u64() {
  const std::uint64_t result = ((state_[1] * 5U) << 7U) | ((state_[1] * 5U) >> 57U);
  const std::uint64_t rotated = result * 9U;
  const std::uint64_t temp = state_[1] << 17U;

  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= temp;
  state_[3] = (state_[3] << 45U) | (state_[3] >> 19U);
  return rotated;
}

std::uint64_t Rng::below(std::uint64_t bound) {
  if (bound == 0) {
    return 0;
  }
  return next_u64() % bound;
}

bool Rng::chance(std::uint32_t numerator, std::uint32_t denominator) {
  if (denominator == 0) {
    return false;
  }
  return below(denominator) < numerator;
}

std::string Rng::token(std::string_view prefix, std::size_t length) {
  static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string out(prefix);
  for (std::size_t index = 0; index < length; ++index) {
    out.push_back(kAlphabet[below(sizeof(kAlphabet) - 1)]);
  }
  return out;
}

std::string Rng::text(std::size_t length) {
  static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string out;
  out.reserve(length);
  for (std::size_t index = 0; index < length; ++index) {
    out.push_back(kAlphabet[below(sizeof(kAlphabet) - 1)]);
  }
  return out;
}

std::string data_center_id_text(std::uint64_t index) {
  // Long enough to satisfy the canonical minimum length for an identity.
  std::string text = "dc-fac-";
  text += std::to_string(index);
  return text;
}

std::string site_id_text(std::uint64_t index) {
  std::string text = "site-fac-";
  text += std::to_string(index);
  return text;
}

Fixture::Fixture(std::string_view label)
    : dir_(label), clock_(dcr::Timestamp::unix_epoch()), limits_(dcr::RegistryLimits::defaults()) {}

dcr::StoreOptions Fixture::store_options(dcr::StoreOpenMode mode) const {
  dcr::StoreOptions options;
  options.root = dir_.path();
  options.mode = mode;
  options.limits = limits_;
  options.durability = dcr::DurabilityMode::strict;
  return options;
}

dcr::OpenOptions Fixture::open_options(dcr::StoreOpenMode mode) const {
  dcr::OpenOptions options;
  options.store = store_options(mode);
  options.clock = &clock_;
  return options;
}

dcr::OpenedRegistry Fixture::open(dcr::StoreOpenMode mode) {
  auto opened = dcr::Registry::open(open_options(mode));
  if (!opened.has_value()) {
    std::printf("     fixture could not open the registry: %s\n",
                opened.error().to_string().c_str());
    std::abort();
  }
  return std::move(opened).value();
}

dcr::ProvenanceInput Fixture::provenance() const {
  dcr::ProvenanceInput input;
  input.source = dcr::ProvenanceSource::api;
  auto principal = dcr::PrincipalId::parse("operator");
  if (!principal.has_value()) {
    std::abort();
  }
  input.principal = principal.value();
  input.detail = "test";
  return input;
}

dcr::CommandContext Fixture::context() const {
  dcr::CommandContext command_context;
  command_context.provenance = provenance();
  return command_context;
}

dcr::CommandContext Fixture::context_with_key(std::string_view key,
                                              std::uint64_t generation) const {
  dcr::CommandContext command_context = context();
  auto parsed_key = dcr::IdempotencyKey::parse(key);
  if (!parsed_key.has_value()) {
    std::printf("     fixture idempotency key '%s' is invalid: %s\n", std::string(key).c_str(),
                parsed_key.error().to_string().c_str());
    std::abort();
  }
  command_context.idempotency_key = parsed_key.value();
  auto parsed_generation = dcr::RegistryGeneration::from_value(generation);
  if (!parsed_generation.has_value()) {
    std::abort();
  }
  command_context.precondition.expected_generation = parsed_generation.value();
  return command_context;
}

dcr::CommandContext Fixture::context_at(const dcr::Registry& registry,
                                        std::string_view principal_text) const {
  dcr::CommandContext command_context = context();
  command_context.precondition.expected_generation = registry.generation();
  auto principal = dcr::PrincipalId::parse(principal_text);
  if (!principal.has_value()) {
    std::printf("     fixture principal '%s' is invalid: %s\n", std::string(principal_text).c_str(),
                principal.error().to_string().c_str());
    std::abort();
  }
  command_context.provenance.principal = principal.value();
  return command_context;
}

dcr::DataCenterId Fixture::id(std::string_view text) const {
  auto parsed = dcr::DataCenterId::parse(text);
  if (!parsed.has_value()) {
    std::printf("     fixture identity '%s' is invalid: %s\n", std::string(text).c_str(),
                parsed.error().to_string().c_str());
    std::abort();
  }
  return parsed.value();
}

dcr::SiteId Fixture::site(std::string_view text) const {
  auto parsed = dcr::SiteId::parse(text);
  if (!parsed.has_value()) {
    std::printf("     fixture site '%s' is invalid: %s\n", std::string(text).c_str(),
                parsed.error().to_string().c_str());
    std::abort();
  }
  return parsed.value();
}

dcr::Alias Fixture::alias(std::string_view text) const {
  auto parsed = dcr::Alias::parse(text);
  if (!parsed.has_value()) {
    std::printf("     fixture alias '%s' is invalid: %s\n", std::string(text).c_str(),
                parsed.error().to_string().c_str());
    std::abort();
  }
  return parsed.value();
}

dcr::CompatibilityKey Fixture::compatibility(std::uint16_t major, std::uint16_t minor,
                                             std::uint64_t mask) {
  auto key = dcr::CompatibilityKey::make(major, minor, mask);
  if (!key.has_value()) {
    std::abort();
  }
  return key.value();
}

dcr::RecordDraft Fixture::draft(std::string_view display_name,
                                std::string_view site_text) const {
  dcr::RecordDraft record_draft;
  record_draft.display_name = std::string(display_name);
  if (!site_text.empty()) {
    record_draft.memberships.push_back(dcr::SiteMembership{site(site_text),
                                                           dcr::MembershipRole::primary});
  }
  record_draft.ownership = dcr::OwnershipScope::unassigned();
  record_draft.compatibility = compatibility(1, 0, 0);
  return record_draft;
}

dcr::RegisterCommand Fixture::register_command(std::string_view id_text,
                                               std::string_view display_name,
                                               dcr::LifecycleState initial_state,
                                               std::string_view site_text) const {
  dcr::RegisterCommand command;
  command.context = context();
  command.id = id(id_text);
  command.draft = draft(display_name, site_text);
  command.initial_state = initial_state;
  return command;
}

void Fixture::advance(std::int64_t millis) {
  const auto result = clock_.advance_millis(millis);
  if (!result.has_value()) {
    std::abort();
  }
}

ScopedFlag ScopedFlag::create(const std::filesystem::path& path) {
  ScopedFlag flag;
  flag.path_ = path;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << "ready\n";
  return flag;
}

ScopedFlag::~ScopedFlag() {
  if (!path_.empty()) {
    std::error_code code;
    std::filesystem::remove(path_, code);
  }
}

ScopedFlag::ScopedFlag(ScopedFlag&& other) noexcept : path_(std::move(other.path_)) {
  other.path_.clear();
}

ScopedFlag& ScopedFlag::operator=(ScopedFlag&& other) noexcept {
  if (this != &other) {
    if (!path_.empty()) {
      std::error_code code;
      std::filesystem::remove(path_, code);
    }
    path_ = std::move(other.path_);
    other.path_.clear();
  }
  return *this;
}

bool ScopedFlag::exists() const {
  if (path_.empty()) {
    return false;
  }
  std::error_code code;
  return std::filesystem::exists(path_, code) && !code;
}

bool child_process_supported() {
#ifdef _WIN32
  return true;
#else
  return true;
#endif
}

int run_child_process(const std::filesystem::path& executable,
                      const std::vector<std::string>& arguments) {
  // _spawnv joins the argument vector with spaces and does not quote it, so an
  // argument containing a space would be split in two, and the build tree this
  // test runs from can contain spaces. Every argument is therefore quoted
  // here; the C runtime strips the quotes when the child parses its command
  // line.
  const auto quoted = [](const std::string& value) {
    std::string out = "\"";
    for (const char character : value) {
      if (character == '"') {
        out += "\\\"";
      } else {
        out.push_back(character);
      }
    }
    out += '"';
    return out;
  };
#ifdef _WIN32
  const std::string program = executable.string();
  std::vector<std::string> quoted_arguments;
  quoted_arguments.reserve(arguments.size() + 1);
  quoted_arguments.push_back(quoted(program));
  for (const auto& argument : arguments) {
    quoted_arguments.push_back(quoted(argument));
  }
  std::vector<const char*> argv;
  argv.reserve(quoted_arguments.size() + 1);
  for (const auto& argument : quoted_arguments) {
    argv.push_back(argument.c_str());
  }
  argv.push_back(nullptr);
  const intptr_t status = _spawnv(_P_WAIT, program.c_str(), argv.data());
  return static_cast<int>(status);
#else
  std::vector<char*> argv;
  const std::string program = executable.string();
  argv.push_back(const_cast<char*>(program.c_str()));
  for (const auto& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int spawn_status = posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(),
                                       environ);
  if (spawn_status != 0) {
    return -1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
#endif
}

std::filesystem::path helper_executable(const char* environment_variable) {
  const char* value = std::getenv(environment_variable);
  if (value == nullptr || *value == '\0') {
    return {};
  }
  return std::filesystem::path(value);
}

int run_child_process_captured(const std::filesystem::path& executable,
                               const std::vector<std::string>& arguments,
                               const std::filesystem::path& output_file) {
  // The shell is asked to run the tool and redirect its streams to a file.
  // Nothing is captured through a pipe.
  std::string line = "\"" + executable.string() + "\"";
  for (const auto& argument : arguments) {
    line += " \"";
    for (const char character : argument) {
      if (character == '"') {
        line += "\\\"";
      } else {
        line.push_back(character);
      }
    }
    line += '"';
  }
  line += " > \"";
  line += output_file.string();
  line += "\" 2>&1";
#ifdef _WIN32
  const std::string command = "cmd /c \"" + line + "\"";
#else
  const std::string command = line;
#endif
  const int status = std::system(command.c_str());
  if (status == -1) {
    return -1;
  }
#ifdef _WIN32
  return status;
#else
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
#endif
}

bool read_text_file(const std::filesystem::path& path, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out = buffer.str();
  return true;
}

bool file_contains_line(const std::filesystem::path& path, std::string_view marker,
                        std::string& contents) {
  if (!read_text_file(path, contents)) {
    return false;
  }
  std::size_t start = 0;
  while (start <= contents.size()) {
    const std::size_t end = contents.find('\n', start);
    const std::string_view line(contents.data() + start,
                                (end == std::string::npos ? contents.size() : end) - start);
    if (line == marker) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

}  // namespace dcrtest
