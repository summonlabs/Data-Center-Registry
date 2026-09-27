// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "persistence/store.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "core/codec.hpp"
#include "dcr/version.hpp"
#include "internal/checked.hpp"
#include "internal/text.hpp"

namespace dcr::internal {
namespace {

constexpr std::string_view kMagic = "DCRSNAP1";
constexpr std::size_t kHeaderSize = 88;
constexpr std::uint32_t kEndianMarker = 0x01020304U;
constexpr std::uint64_t kMaxCurrentFileBytes = 1024;
constexpr std::size_t kMaxGenerationFiles = 256;
constexpr std::size_t kMaxQuarantineFiles = 8;
constexpr std::string_view kCurrentHeader = "DCR-CURRENT 1";
constexpr std::string_view kGenerationPrefix = "gen-";
constexpr std::string_view kGenerationSuffix = ".dcrs";
constexpr std::size_t kGenerationDigits = 20;

// --- little-endian helpers -------------------------------------------------

void put_u16(std::string& out, std::size_t offset, std::uint16_t value) {
  out[offset] = static_cast<char>(value & 0xFFU);
  out[offset + 1] = static_cast<char>((value >> 8U) & 0xFFU);
}

void put_u32(std::string& out, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    out[offset + index] = static_cast<char>((value >> (index * 8U)) & 0xFFU);
  }
}

void put_u64(std::string& out, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    out[offset + index] = static_cast<char>((value >> (index * 8U)) & 0xFFU);
  }
}

[[nodiscard]] std::uint16_t get_u16(std::string_view data, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[offset])) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(
                                     static_cast<std::uint8_t>(data[offset + 1]))
                                 << 8U));
}

[[nodiscard]] std::uint32_t get_u32(std::string_view data, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + index]))
             << (index * 8U);
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(std::string_view data, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + index]))
             << (index * 8U);
  }
  return value;
}

// --- CURRENT ---------------------------------------------------------------

[[nodiscard]] std::string format_current(const CurrentPointer& pointer) {
  std::string text(kCurrentHeader);
  text += '\n';
  text += "file=";
  text += pointer.file_name;
  text += '\n';
  text += "generation=";
  text += pointer.generation.to_string();
  text += '\n';
  text += "digest=";
  text += pointer.digest.to_hex();
  text += '\n';
  return text;
}

[[nodiscard]] Result<CurrentPointer> parse_current(std::string_view text) {
  constexpr std::string_view kField = "CURRENT";
  DCR_TRY_ASSIGN(const std::vector<std::string> lines, split(text, '\n', true, kField));
  if (lines.size() != 5 || lines[4] != "") {
    return Error(ErrorCode::store_corrupt,
                 "CURRENT must be exactly four lines followed by a newline", std::string(kField));
  }
  if (lines[0] != kCurrentHeader) {
    return Error(ErrorCode::store_incompatible_version,
                 "CURRENT declares an unknown format header '" + lines[0] + "'",
                 std::string(kField));
  }
  if (!starts_with(lines[1], "file=") || !starts_with(lines[2], "generation=") ||
      !starts_with(lines[3], "digest=")) {
    return Error(ErrorCode::store_corrupt,
                 "CURRENT does not have the expected file, generation and digest lines",
                 std::string(kField));
  }
  const std::string_view file_name(lines[1].data() + 5, lines[1].size() - 5);
  auto generation = parse_generation_file_name(file_name);
  if (!generation.has_value()) {
    return Error(ErrorCode::store_corrupt,
                 "CURRENT names '" + std::string(file_name) +
                     "', which is not a generation file name",
                 std::string(kField));
  }
  DCR_TRY_ASSIGN(const std::uint64_t declared,
                 parse_u64(std::string_view(lines[2]).substr(11), "CURRENT.generation"));
  if (declared != generation->value()) {
    return Error(ErrorCode::store_corrupt,
                 "CURRENT declares generation " + std::to_string(declared) +
                     " but names the file for generation " + generation->to_string(),
                 std::string(kField));
  }
  DCR_TRY_ASSIGN(Sha256Digest digest,
                 Sha256Digest::from_hex(std::string_view(lines[3]).substr(7)));
  return CurrentPointer{generation.value(), std::string(file_name), digest};
}

[[nodiscard]] Result<std::optional<CurrentPointer>> read_current(const StorePaths& paths) {
  if (!path_exists(paths.current_file)) {
    return std::optional<CurrentPointer>();
  }
  DCR_TRY_ASSIGN(std::string text,
                 read_file_bounded(paths.current_file, kMaxCurrentFileBytes, "CURRENT"));
  DCR_TRY_ASSIGN(CurrentPointer pointer, parse_current(text));
  return std::optional<CurrentPointer>(std::move(pointer));
}

[[nodiscard]] Result<void> write_current_atomically(const StorePaths& paths,
                                                    const CurrentPointer& pointer) {
  auto staging = StagingFile::create(paths.staging_dir, "current");
  if (!staging.has_value()) {
    return staging.error();
  }
  StagingFile file = std::move(staging).value();
  if (auto result = file.write(format_current(pointer)); !result.has_value()) {
    return result.error();
  }
  if (auto result = file.flush_and_close(); !result.has_value()) {
    return result.error();
  }
  if (auto result = atomic_replace(file.path(), paths.current_file); !result.has_value()) {
    return result.error();
  }
  file.keep();
  return flush_directory(paths.root);
}

// --- generation files ------------------------------------------------------

struct GenerationScan {
  /// One status per generation file found, newest first. Only the digest was
  /// checked unless `full_decode` was requested.
  std::vector<GenerationFileStatus> files;
};

}  // namespace

// ---------------------------------------------------------------------------
// StorePaths and file naming
// ---------------------------------------------------------------------------

StorePaths StorePaths::for_root(const std::filesystem::path& root) {
  StorePaths paths;
  paths.root = root;
  paths.lock_file = root / "registry.lock";
  paths.current_file = root / "CURRENT";
  paths.generations_dir = root / "generations";
  paths.staging_dir = root / "tmp";
  paths.quarantine_dir = root / "uncommitted";
  return paths;
}

std::string generation_file_name(RegistryGeneration generation) {
  return std::string(kGenerationPrefix) + format_decimal_padded(generation.value(),
                                                                kGenerationDigits) +
         std::string(kGenerationSuffix);
}

std::optional<RegistryGeneration> parse_generation_file_name(std::string_view name) {
  if (!starts_with(name, kGenerationPrefix) || !ends_with(name, kGenerationSuffix)) {
    return std::nullopt;
  }
  const std::size_t body_size = name.size() - kGenerationPrefix.size() - kGenerationSuffix.size();
  if (body_size != kGenerationDigits) {
    return std::nullopt;
  }
  const std::string_view body = name.substr(kGenerationPrefix.size(), kGenerationDigits);
  auto value = parse_decimal_fixed(body, kGenerationDigits, "generation");
  if (!value.has_value()) {
    return std::nullopt;
  }
  auto generation = RegistryGeneration::from_value(value.value());
  if (!generation.has_value()) {
    return std::nullopt;
  }
  return generation.value();
}

// ---------------------------------------------------------------------------
// Container
// ---------------------------------------------------------------------------

Result<std::string> encode_container(const RegistryState& state) {
  DCR_TRY_ASSIGN(std::string payload, encode_state(state));
  if (payload.size() > state.limits.max_snapshot_bytes) {
    return Error(ErrorCode::limit_exceeded,
                 "the payload of " + std::to_string(payload.size()) +
                     " bytes exceeds max_snapshot_bytes",
                 "max_snapshot_bytes");
  }

  const Sha256Digest payload_digest = sha256_text(payload);
  std::string container(kHeaderSize, '\0');
  std::memcpy(container.data(), kMagic.data(), kMagic.size());
  put_u16(container, 8, kSnapshotFormatMajor);
  put_u16(container, 10, kSnapshotFormatMinor);
  put_u32(container, 12, kEndianMarker);
  put_u64(container, 16, static_cast<std::uint64_t>(payload.size()));
  std::memcpy(container.data() + 24, payload_digest.bytes().data(), Sha256Digest::kSize);
  const Sha256Digest header_digest = sha256_bytes(container.data(), 56);
  std::memcpy(container.data() + 56, header_digest.bytes().data(), Sha256Digest::kSize);
  container += payload;
  return container;
}

Result<RegistryState> decode_container(std::string_view bytes, const RegistryLimits& limits) {
  if (bytes.size() < kHeaderSize) {
    return Error(ErrorCode::store_corrupt,
                 "the snapshot is " + std::to_string(bytes.size()) +
                     " bytes, shorter than the 88-byte header",
                 "snapshot");
  }
  if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    return Error(ErrorCode::store_corrupt, "the snapshot magic does not match", "snapshot");
  }
  const std::uint16_t major = get_u16(bytes, 8);
  const std::uint16_t minor = get_u16(bytes, 10);
  if (major != kSnapshotFormatMajor) {
    return Error(ErrorCode::store_incompatible_version,
                 "the snapshot is format major " + std::to_string(major) +
                     ", but this build reads and writes " +
                     std::to_string(kSnapshotFormatMajor),
                 "snapshot");
  }
  if (minor > kSnapshotFormatMinor) {
    return Error(ErrorCode::store_incompatible_version,
                 "the snapshot is format minor " + std::to_string(minor) +
                     ", which is newer than " + std::to_string(kSnapshotFormatMinor),
                 "snapshot");
  }
  if (get_u32(bytes, 12) != kEndianMarker) {
    return Error(ErrorCode::store_corrupt,
                 "the snapshot endian marker is wrong; the file was not written by this "
                 "implementation on this byte order",
                 "snapshot");
  }
  const std::uint64_t payload_length = get_u64(bytes, 16);
  if (payload_length != static_cast<std::uint64_t>(bytes.size() - kHeaderSize)) {
    return Error(ErrorCode::store_corrupt,
                 "the snapshot declares a payload of " + std::to_string(payload_length) +
                     " bytes but carries " +
                     std::to_string(bytes.size() - kHeaderSize) + " bytes",
                 "snapshot");
  }
  Sha256Digest::Bytes header_digest_bytes{};
  std::memcpy(header_digest_bytes.data(), bytes.data() + 56, Sha256Digest::kSize);
  const Sha256Digest header_digest = sha256_bytes(bytes.data(), 56);
  if (!(header_digest == Sha256Digest(header_digest_bytes))) {
    return Error(ErrorCode::store_corrupt, "the snapshot header digest does not match",
                 "snapshot");
  }
  const std::string_view payload = bytes.substr(kHeaderSize);
  Sha256Digest::Bytes payload_digest_bytes{};
  std::memcpy(payload_digest_bytes.data(), bytes.data() + 24, Sha256Digest::kSize);
  const Sha256Digest payload_digest = sha256_text(payload);
  if (!(payload_digest == Sha256Digest(payload_digest_bytes))) {
    return Error(ErrorCode::store_corrupt, "the snapshot payload digest does not match",
                 "snapshot");
  }
  DCR_TRY_ASSIGN(RegistryState state, decode_state(payload, limits));
  if (state.format_major != major || state.format_minor > minor) {
    return Error(ErrorCode::store_corrupt,
                 "the snapshot header and payload disagree about the format version", "snapshot");
  }
  return state;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

namespace {

/// Reads a generation file, verifies its container, and returns the digest of
/// its payload without decoding the payload when `decode` is false.
[[nodiscard]] Result<Sha256Digest> verify_generation_file(const std::filesystem::path& path,
                                                          std::uint64_t max_bytes, bool decode,
                                                          const RegistryLimits& limits,
                                                          std::optional<RegistryState>* state_out) {
  DCR_TRY_ASSIGN(const std::uint64_t size, regular_file_size(path, "snapshot"));
  if (size > max_bytes) {
    return Error(ErrorCode::store_limit_exceeded,
                 "'" + path.filename().string() + "' is " + std::to_string(size) +
                     " bytes, which exceeds the accepted maximum of " + std::to_string(max_bytes) +
                     " bytes",
                 "snapshot");
  }
  DCR_TRY_ASSIGN(std::string bytes, read_file_bounded(path, max_bytes, "snapshot"));
  if (bytes.size() < kHeaderSize) {
    return Error(ErrorCode::store_corrupt,
                 "'" + path.filename().string() + "' is shorter than a snapshot header",
                 "snapshot");
  }
  if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    return Error(ErrorCode::store_corrupt,
                 "'" + path.filename().string() + "' does not start with the snapshot magic",
                 "snapshot");
  }
  if (get_u16(bytes, 8) != kSnapshotFormatMajor) {
    return Error(ErrorCode::store_incompatible_version,
                 "'" + path.filename().string() + "' is snapshot format major " +
                     std::to_string(get_u16(bytes, 8)) + ", but this build reads " +
                     std::to_string(kSnapshotFormatMajor),
                 "snapshot");
  }
  if (get_u16(bytes, 10) > kSnapshotFormatMinor) {
    return Error(ErrorCode::store_incompatible_version,
                 "'" + path.filename().string() + "' is snapshot format minor " +
                     std::to_string(get_u16(bytes, 10)) + ", which is newer than " +
                     std::to_string(kSnapshotFormatMinor),
                 "snapshot");
  }
  if (get_u32(bytes, 12) != kEndianMarker) {
    return Error(ErrorCode::store_corrupt,
                 "'" + path.filename().string() + "' has a wrong endian marker", "snapshot");
  }
  const std::uint64_t payload_length = get_u64(bytes, 16);
  if (payload_length != static_cast<std::uint64_t>(bytes.size() - kHeaderSize)) {
    return Error(ErrorCode::store_corrupt,
                 "'" + path.filename().string() + "' declares " +
                     std::to_string(payload_length) + " payload bytes but carries " +
                     std::to_string(bytes.size() - kHeaderSize),
                 "snapshot");
  }
  Sha256Digest::Bytes header_digest_bytes{};
  std::memcpy(header_digest_bytes.data(), bytes.data() + 56, Sha256Digest::kSize);
  if (!(sha256_bytes(bytes.data(), 56) == Sha256Digest(header_digest_bytes))) {
    return Error(ErrorCode::store_corrupt,
                 "'" + path.filename().string() + "' has a header digest mismatch", "snapshot");
  }
  const std::string_view payload = std::string_view(bytes).substr(kHeaderSize);
  Sha256Digest::Bytes payload_digest_bytes{};
  std::memcpy(payload_digest_bytes.data(), bytes.data() + 24, Sha256Digest::kSize);
  const Sha256Digest payload_digest = Sha256Digest(payload_digest_bytes);
  if (!(sha256_text(payload) == payload_digest)) {
    return Error(ErrorCode::store_corrupt,
                 "'" + path.filename().string() + "' has a payload digest mismatch", "snapshot");
  }
  if (decode) {
    DCR_TRY_ASSIGN(RegistryState state, decode_state(payload, limits));
    if (state_out != nullptr) {
      *state_out = std::move(state);
    }
  }
  return payload_digest;
}

[[nodiscard]] Result<GenerationScan> scan_generations(const StorePaths& paths, bool full_decode,
                                                      const RegistryLimits& limits) {
  GenerationScan scan;
  if (!path_exists(paths.generations_dir)) {
    return scan;
  }
  DCR_TRY_ASSIGN(std::vector<std::string> names, list_directory(paths.generations_dir));
  std::sort(names.begin(), names.end());
  if (names.size() > kMaxGenerationFiles) {
    return Error(ErrorCode::store_corrupt,
                 "the store holds " + std::to_string(names.size()) +
                     " generation files, which is more than the " +
                     std::to_string(kMaxGenerationFiles) +
                     " this build will examine; generation retention is not working as "
                     "configured",
                 "generations");
  }

  const std::uint64_t max_bytes = limits.max_snapshot_bytes + kHeaderSize;
  for (const auto& name : names) {
    auto generation = parse_generation_file_name(name);
    if (!generation.has_value()) {
      continue;
    }
    GenerationFileStatus status;
    status.file_name = name;
    status.file_generation = generation.value();
    std::optional<RegistryState> state;
    auto digest = verify_generation_file(paths.generations_dir / name, max_bytes, full_decode,
                                         limits, &state);
    if (!digest.has_value()) {
      status.detail = digest.error().message();
      status.digest_ok = false;
      status.parsed = false;
      scan.files.push_back(std::move(status));
      continue;
    }
    status.digest_ok = true;
    status.payload_digest = digest.value();
    if (full_decode) {
      if (state.has_value()) {
        status.parsed = true;
        status.generation_matches_name = state->generation == generation.value();
        if (!status.generation_matches_name) {
          status.detail = "the payload reports generation " +
                          state->generation.to_string() + " but the file name says " +
                          generation->to_string();
        }
      }
    } else {
      status.parsed = false;
      status.generation_matches_name = true;
    }
    scan.files.push_back(std::move(status));
  }

  std::sort(scan.files.begin(), scan.files.end(),
            [](const GenerationFileStatus& left, const GenerationFileStatus& right) {
              return right.file_generation < left.file_generation;
            });
  return scan;
}

}  // namespace

Result<RegistryState> load_state(const StoreOptions& options, OpenReport& report,
                             bool store_existed) {
  const StorePaths paths = StorePaths::for_root(options.root);
  report.mode = options.mode;

  DCR_TRY_ASSIGN(const std::optional<CurrentPointer> current, read_current(paths));
  DCR_TRY_ASSIGN(GenerationScan scan, scan_generations(paths, false, options.limits));

  std::vector<GenerationFileStatus> verified;
  for (const auto& status : scan.files) {
    if (status.digest_ok) {
      verified.push_back(status);
    } else {
      report.unusable_generations.push_back(status.file_generation);
    }
  }
  // scan.files is newest first; verified inherits that order.

  if (!current.has_value()) {
    if (verified.empty()) {
      if (report.unusable_generations.empty()) {
        if (!store_existed) {
          if (options.mode == StoreOpenMode::read_only ||
              options.mode == StoreOpenMode::open_existing) {
            return Error(ErrorCode::store_not_found,
                         "no registry store exists at '" + options.root.string() + "'",
                         "store.root");
          }
          report.disposition = RecoveryDisposition::created_empty;
          report.created = true;
        } else {
          // A store that exists but has never committed anything is empty, not
          // missing: this is the state between initialisation and the first
          // commit.
          report.disposition = RecoveryDisposition::opened_empty;
          report.created = false;
        }
        report.generation = RegistryGeneration::minimum();
        report.record_count = 0;
        RegistryState empty = RegistryState::empty(options.limits);
        DCR_TRY_ASSIGN(const std::string empty_payload, encode_state(empty));
        report.snapshot_digest = sha256_text(empty_payload);
        return empty;
      }
      return Error(ErrorCode::store_corrupt,
                   "no CURRENT file is present and every generation file present is unusable; "
                   "there is no state this build is willing to treat as authoritative",
                   "CURRENT");
    }
    if (options.recovery != RecoveryPolicy::last_known_good) {
      return Error(ErrorCode::store_corrupt,
                   "CURRENT is missing but " + std::to_string(verified.size()) +
                       " generation file(s) exist; recovery policy is strict, so this build will "
                       "not guess which generation an operator meant",
                   "CURRENT");
    }
    const auto& newest = verified.front();
    std::optional<RegistryState> state;
    DCR_TRY_ASSIGN(const Sha256Digest adopted_digest,
                   verify_generation_file(paths.generations_dir / newest.file_name,
                                          options.limits.max_snapshot_bytes + kHeaderSize, true,
                                          options.limits, &state));
    if (!state.has_value()) {
      return Error(ErrorCode::store_corrupt, "the adopted generation did not decode", "CURRENT");
    }
    report.disposition = RecoveryDisposition::recovered_last_known_good;
    report.recovered = true;
    report.detail = "CURRENT was missing; adopted generation " +
                    newest.file_generation.to_string() + " under last-known-good recovery";
    report.generation = state->generation;
    report.record_count = static_cast<std::uint64_t>(state->records.size());
    report.snapshot_digest = adopted_digest;
    for (const auto& status : scan.files) {
      if (status.file_generation > newest.file_generation) {
        report.quarantined_generations.push_back(status.file_generation);
      }
    }
    return std::move(*state);
  }

  // CURRENT is present. Read and verify the generation it names.
  const std::filesystem::path current_path = paths.generations_dir / current->file_name;
  std::optional<RegistryState> state;
  const auto current_digest =
      verify_generation_file(current_path, options.limits.max_snapshot_bytes + kHeaderSize, true,
                             options.limits, &state);

  if (current_digest.has_value() && state.has_value() &&
      state->generation == current->generation && (current_digest.value() == current->digest)) {
    report.disposition = RecoveryDisposition::opened_current;
    report.generation = state->generation;
    report.record_count = static_cast<std::uint64_t>(state->records.size());
    report.snapshot_digest = current_digest.value();
    // Anything beyond CURRENT, whether it verifies or not, was never committed:
    // a verified file is a publication that was interrupted before CURRENT was
    // replaced, and an unverified one is a write that never completed. Both are
    // set aside by a writable open so that the next publication can use the
    // generation number.
    for (const auto& status : scan.files) {
      if (status.file_generation > current->generation) {
        report.quarantined_generations.push_back(status.file_generation);
      }
    }
    return std::move(*state);
  }

  const std::string reason =
      current_digest.has_value()
          ? (state.has_value() && !(state->generation == current->generation)
                 ? "the generation file reports generation " + state->generation.to_string() +
                       " but CURRENT names " + current->generation.to_string()
                 : "CURRENT names a digest that does not match the generation file")
          : current_digest.error().message();

  if (options.recovery != RecoveryPolicy::last_known_good) {
    // A generation that is too large or written by a newer format is described
    // by its own category: telling an operator "corrupt" when the real problem
    // is a bound or a version would hide the remedy.
    const ErrorCode category = current_digest.has_value() ? ErrorCode::store_corrupt
                                                         : current_digest.error().code();
    if (category == ErrorCode::store_limit_exceeded ||
        category == ErrorCode::store_incompatible_version) {
      return Error(category,
                   "the generation CURRENT names is not usable: " + reason +
                       "; recovery policy is strict, so no other generation will be adopted",
                   "CURRENT");
    }
    return Error(ErrorCode::store_corrupt,
                 "the generation CURRENT names is not usable: " + reason +
                     "; recovery policy is strict, so no other generation will be adopted",
                 "CURRENT");
  }

  for (const auto& status : verified) {
    if (status.file_generation == current->generation) {
      continue;
    }
    std::optional<RegistryState> candidate;
    const auto candidate_digest =
        verify_generation_file(paths.generations_dir / status.file_name,
                               options.limits.max_snapshot_bytes + kHeaderSize, true,
                               options.limits, &candidate);
    if (!candidate_digest.has_value() || !candidate.has_value()) {
      continue;
    }
    report.disposition = RecoveryDisposition::recovered_last_known_good;
    report.recovered = true;
    report.discarded_generation = current->generation;
    report.detail = "CURRENT named generation " + current->generation.to_string() +
                    ", which is not usable (" + reason + "); adopted generation " +
                    candidate->generation.to_string() + " under last-known-good recovery";
    report.generation = candidate->generation;
    report.record_count = static_cast<std::uint64_t>(candidate->records.size());
    report.snapshot_digest = candidate_digest.value();
    for (const auto& other : scan.files) {
      if (other.file_generation != status.file_generation &&
          other.file_generation != current->generation) {
        report.quarantined_generations.push_back(other.file_generation);
      }
    }
    return std::move(*candidate);
  }

  return Error(ErrorCode::store_corrupt,
               "CURRENT is unusable (" + reason +
                   ") and no other generation file verifies; there is no state this build is "
                   "willing to treat as authoritative",
               "CURRENT");
}

// ---------------------------------------------------------------------------
// SnapshotStore
// ---------------------------------------------------------------------------

SnapshotStore::~SnapshotStore() { close(); }

SnapshotStore::SnapshotStore(SnapshotStore&& other) noexcept
    : options_(std::move(other.options_)),
      paths_(std::move(other.paths_)),
      lock_(std::move(other.lock_)),
      writable_(other.writable_) {
  other.writable_ = false;
}

SnapshotStore& SnapshotStore::operator=(SnapshotStore&& other) noexcept {
  if (this != &other) {
    close();
    options_ = std::move(other.options_);
    paths_ = std::move(other.paths_);
    lock_ = std::move(other.lock_);
    writable_ = other.writable_;
    other.writable_ = false;
  }
  return *this;
}

void SnapshotStore::close() noexcept {
  lock_.release();
  writable_ = false;
}

Result<void> SnapshotStore::quarantine_uncommitted(
    const std::vector<RegistryGeneration>& generations, OpenReport& report) const {
  if (generations.empty()) {
    return {};
  }
  DCR_TRY(ensure_directory(paths_.quarantine_dir));
  std::size_t moved = 0;
  for (const auto& generation : generations) {
    const std::filesystem::path source = paths_.generations_dir / generation_file_name(generation);
    if (!path_exists(source)) {
      continue;
    }
    std::filesystem::path target = paths_.quarantine_dir / generation_file_name(generation);
    if (path_exists(target)) {
      target = paths_.quarantine_dir /
               (generation_file_name(generation) + ".duplicate-" + unique_suffix());
    }
    std::error_code code;
    std::filesystem::rename(source, target, code);
    if (code) {
      return Error(ErrorCode::store_io_error,
                   "cannot move the uncommitted generation '" + source.string() +
                       "' aside: " + code.message(),
                   "generations");
    }
    ++moved;
  }
  if (moved > 0) {
    report.detail += report.detail.empty() ? "" : " ";
    report.detail += std::to_string(moved) +
                     " verified generation(s) were newer than CURRENT and had never been "
                     "committed; they were moved to uncommitted/ rather than adopted or deleted";
  }
  return {};
}

Result<void> SnapshotStore::cleanup_retention(RegistryGeneration newest) const {
  if (!path_exists(paths_.generations_dir)) {
    return {};
  }
  DCR_TRY_ASSIGN(std::vector<std::string> names, list_directory(paths_.generations_dir));
  std::vector<std::pair<RegistryGeneration, std::string>> kept;
  for (const auto& name : names) {
    auto generation = parse_generation_file_name(name);
    if (!generation.has_value()) {
      continue;
    }
    kept.emplace_back(generation.value(), name);
  }
  std::sort(kept.begin(), kept.end(),
            [](const auto& left, const auto& right) { return right.first < left.first; });
  for (std::size_t index = options_.generation_retention; index < kept.size(); ++index) {
    if (kept[index].first == newest) {
      continue;
    }
    DCR_TRY(remove_file_if_exists(paths_.generations_dir / kept[index].second));
  }

  if (path_exists(paths_.quarantine_dir)) {
    DCR_TRY_ASSIGN(std::vector<std::string> quarantined, list_directory(paths_.quarantine_dir));
    std::vector<std::pair<RegistryGeneration, std::string>> ordered;
    for (const auto& name : quarantined) {
      auto generation = parse_generation_file_name(name);
      if (generation.has_value()) {
        ordered.emplace_back(generation.value(), name);
      }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& left, const auto& right) { return right.first < left.first; });
    for (std::size_t index = kMaxQuarantineFiles; index < ordered.size(); ++index) {
      DCR_TRY(remove_file_if_exists(paths_.quarantine_dir / ordered[index].second));
    }
  }
  return {};
}

Result<StoreOpenResult> SnapshotStore::open(const StoreOptions& options) {
  if (auto result = options.validate(); !result.has_value()) {
    return result.error();
  }
  StoreOpenResult result;
  SnapshotStore store;
  store.options_ = options;
  store.paths_ = StorePaths::for_root(options.root);

  const bool wants_write = options.mode != StoreOpenMode::read_only;
  // Whether a store layout was already present, as opposed to being created by
  // this call. An existing layout with nothing committed is an empty store; a
  // missing one is either created here or reported as not found.
  const bool store_existed = path_exists(store.paths_.current_file) ||
                             path_exists(store.paths_.lock_file) ||
                             path_exists(store.paths_.generations_dir);

  if (options.mode == StoreOpenMode::create_new) {
    // A store exists as soon as its layout does, not only once something has
    // been committed: re-initialising an empty registry must not silently
    // adopt it.
    if (store_existed) {
      return Error(ErrorCode::invalid_argument,
                   "a registry already exists at '" + options.root.string() +
                       "'; create_new does not adopt an existing store",
                   "store.root");
    }
  } else if (!path_exists(store.paths_.root)) {
    if (options.mode == StoreOpenMode::open_or_create) {
      DCR_TRY(ensure_directory(store.paths_.root));
    } else {
      return Error(ErrorCode::store_not_found,
                   "no registry store exists at '" + options.root.string() + "'", "store.root");
    }
  } else if (!store_existed && options.mode != StoreOpenMode::open_or_create) {
    // The root exists but holds no store, and this mode does not create one.
    // Nothing has been written at this point, so an unrelated directory is left
    // exactly as it was found.
    return Error(ErrorCode::store_not_found,
                 "no registry store exists at '" + options.root.string() +
                     "'; the directory exists but holds no registry layout",
                 "store.root");
  }

  if (wants_write) {
    DCR_TRY(ensure_directory(store.paths_.root));
    DCR_TRY(ensure_directory(store.paths_.generations_dir));
    DCR_TRY(ensure_directory(store.paths_.staging_dir));

    auto lock = ExclusiveFileLock::acquire(store.paths_.lock_file);
    if (!lock.has_value()) {
      return lock.error();
    }
    store.lock_ = std::move(lock).value();
    store.writable_ = true;
    // Holder information is diagnostic only: the lock is the operating-system
    // lock, not the file contents, so a failure to write it is not fatal.
    const auto holder = store.lock_.write_holder_info(
        std::string("holder=registry\n"));
    (void)holder;
    DCR_TRY(clear_directory(store.paths_.staging_dir));
  }

  OpenReport report;
  report.mode = options.mode;
  DCR_TRY_ASSIGN(RegistryState state, load_state(options, report, store_existed));

  if (options.mode == StoreOpenMode::read_only) {
    // A read-only open never creates anything, so an empty store is either an
    // error or an existing empty store, and the disposition says the store was
    // opened for reading.
    if (report.disposition != RecoveryDisposition::opened_empty) {
      report.disposition = RecoveryDisposition::opened_read_only;
    }
    report.created = false;
  }

  if (store.writable_) {
    DCR_TRY(store.quarantine_uncommitted(report.quarantined_generations, report));
    if (report.disposition == RecoveryDisposition::recovered_last_known_good &&
        report.generation.value() != 0) {
      // The store now adopts this generation, so CURRENT must say so before
      // anything else publishes on top of it.
      const CurrentPointer pointer{report.generation, generation_file_name(report.generation),
                                   report.snapshot_digest};
      DCR_TRY(write_current_atomically(store.paths_, pointer));
    }
    DCR_TRY(store.cleanup_retention(report.generation));
  }

  result.state = std::move(state);
  result.report = std::move(report);
  result.store = std::move(store);
  return result;
}

Result<Sha256Digest> SnapshotStore::publish(const RegistryState& state,
                                            RegistryGeneration expected_base) {
  if (!writable_) {
    return Error(ErrorCode::store_read_only,
                 "this registry was opened read-only and cannot publish", "store.mode");
  }
  auto expected_next = expected_base.successor();
  if (!expected_next.has_value()) {
    return expected_next.error();
  }
  if (!(state.generation == expected_next.value())) {
    return Error(ErrorCode::internal_error,
                 "a publication must advance the registry generation by exactly one",
                 "generation");
  }

  // Fence: the generation on disk must be the one this publication is based on.
  DCR_TRY_ASSIGN(const std::optional<CurrentPointer> current, read_current(paths_));
  if (current.has_value()) {
    if (!(current->generation == expected_base)) {
      return Error(ErrorCode::stale_generation,
                   "this registry is based on generation " + expected_base.to_string() +
                       " but the store's CURRENT names generation " +
                       current->generation.to_string() + "; refusing to overwrite a newer commit",
                   "CURRENT");
    }
  } else if (expected_base.value() != 0) {
    return Error(ErrorCode::store_corrupt,
                 "CURRENT is missing but this registry is at generation " +
                     expected_base.to_string() + "; refusing to publish",
                 "CURRENT");
  }

  DCR_TRY_ASSIGN(std::string container, encode_container(state));
  if (container.size() > options_.limits.max_snapshot_bytes + kHeaderSize) {
    return Error(ErrorCode::limit_exceeded, "the encoded snapshot exceeds max_snapshot_bytes",
                 "max_snapshot_bytes");
  }
  const Sha256Digest payload_digest = sha256_text(
      std::string_view(container).substr(kHeaderSize));

  const std::string file_name = generation_file_name(state.generation);
  const std::filesystem::path target = paths_.generations_dir / file_name;
  const std::uint64_t max_bytes = options_.limits.max_snapshot_bytes + kHeaderSize;

  if (path_exists(target)) {
    // An unreferenced file for this generation already exists. It is either the
    // exact state being published (a crash between the file rename and the
    // CURRENT replacement, replayed) or it is a different state claiming the
    // same generation, which is ambiguous and is refused.
    std::optional<RegistryState> existing;
    DCR_TRY_ASSIGN(const Sha256Digest existing_digest,
                   verify_generation_file(target, max_bytes, true, options_.limits, &existing));
    if (!existing.has_value() || !(existing_digest == payload_digest)) {
      return Error(ErrorCode::store_corrupt,
                   "'" + file_name +
                       "' already exists with different content; two states are claiming the "
                       "same generation. Resolve the store before publishing again.",
                   "generations");
    }
  } else {
    auto staging = StagingFile::create(paths_.staging_dir, "snapshot");
    if (!staging.has_value()) {
      return staging.error();
    }
    StagingFile file = std::move(staging).value();
    if (auto result = file.write(container); !result.has_value()) {
      return result.error();
    }
    if (options_.durability == DurabilityMode::strict) {
      if (auto result = file.flush_and_close(); !result.has_value()) {
        return result.error();
      }
    } else {
      file.keep();
    }

    // Verify what was written before it becomes visible: re-read the staged
    // file and confirm it is byte for byte the snapshot being published. The
    // digest covers every payload byte, so this proves the file on disk holds
    // exactly the state that was validated in memory. The payload is not
    // decoded again here: it was produced by the encoder from a state that was
    // validated immediately before encoding, and a decode on every commit would
    // double the cost of the commit path for no additional guarantee.
    DCR_TRY_ASSIGN(const Sha256Digest written_digest,
                   verify_generation_file(file.path(), max_bytes, false, options_.limits,
                                          nullptr));
    if (!(written_digest == payload_digest)) {
      return Error(ErrorCode::store_io_error,
                   "the staged snapshot did not read back as the state being published",
                   "tmp");
    }

    if (auto result = atomic_replace(file.path(), target); !result.has_value()) {
      return result.error();
    }
    file.keep();
    if (options_.durability == DurabilityMode::strict) {
      if (auto result = flush_directory(paths_.generations_dir); !result.has_value()) {
        return result.error();
      }
    }
  }

  const CurrentPointer pointer{state.generation, file_name, payload_digest};
  if (auto result = write_current_atomically(paths_, pointer); !result.has_value()) {
    return result.error();
  }

  DCR_TRY(cleanup_retention(state.generation));
  DCR_TRY(clear_directory(paths_.staging_dir));
  return payload_digest;
}

}  // namespace dcr::internal

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

namespace dcr {

using internal::GenerationScan;
using internal::path_exists;
using internal::read_current;
using internal::scan_generations;
using internal::StorePaths;

Result<void> StoreOptions::validate() const {
  if (root.empty()) {
    return Error(ErrorCode::invalid_argument, "a store root must be given", "store.root");
  }
  if (generation_retention < 2 || generation_retention > 1024) {
    return Error(ErrorCode::invalid_argument,
                 "generation_retention must be between 2 and 1024; a retention of 1 would let a "
                 "writer delete the file a reader is opening",
                 "generation_retention");
  }
  auto limits_result = limits.validate();
  if (!limits_result.has_value()) {
    return limits_result.error();
  }
  return {};
}

Result<StoreInspection> inspect_store(const StoreOptions& options) {
  if (auto result = options.validate(); !result.has_value()) {
    return result.error();
  }
  const StorePaths paths = StorePaths::for_root(options.root);
  StoreInspection inspection;
  inspection.root_exists = path_exists(paths.root);
  if (!inspection.root_exists) {
    inspection.detail = "no store root exists at '" + options.root.string() + "'";
    return inspection;
  }

  DCR_TRY_ASSIGN(GenerationScan scan, scan_generations(paths, true, options.limits));
  inspection.generation_files = scan.files;

  const auto current = read_current(paths);
  if (!current.has_value()) {
    inspection.detail = "CURRENT is unusable: " + current.error().message();
  } else if (!current.value().has_value()) {
    inspection.detail = "CURRENT is not present";
  } else {
    inspection.current_present = true;
    inspection.current_parseable = true;
    inspection.current_generation = current.value()->generation;
  }

  for (const auto& status : inspection.generation_files) {
    if (!status.digest_ok || !status.generation_matches_name) {
      continue;
    }
    if (!inspection.newest_valid_generation.has_value() ||
        inspection.newest_valid_generation->value() < status.file_generation.value()) {
      inspection.newest_valid_generation = status.file_generation;
    }
  }

  // An open would adopt the generation CURRENT names when it verifies and
  // nothing newer exists; otherwise the policy decides.
  if (inspection.current_present) {
    const auto& named = *current.value();
    const GenerationFileStatus* matching = nullptr;
    for (const auto& status : inspection.generation_files) {
      if (status.file_generation == named.generation) {
        matching = &status;
        break;
      }
    }
    // The file must verify *and* be the file CURRENT says it is: CURRENT records
    // the payload digest, so an edit that leaves the pointer stale is reported
    // here exactly as it is refused on open.
    const bool pointed_at = matching != nullptr && matching->digest_ok &&
                            matching->generation_matches_name &&
                            matching->payload_digest == named.digest;
    const bool usable = pointed_at;
    bool newer_valid = false;
    if (inspection.newest_valid_generation.has_value() &&
        inspection.newest_valid_generation->value() > named.generation.value()) {
      newer_valid = true;
    }
    // A generation file beyond CURRENT that does not verify is residue from a
    // failed write that never went away. The authoritative state is still
    // CURRENT, but the store is not consistent: something needs an operator's
    // attention.
    bool newer_unusable = false;
    for (const auto& status : inspection.generation_files) {
      if (!status.digest_ok && status.file_generation.value() > named.generation.value()) {
        newer_unusable = true;
        break;
      }
    }
    if (usable && !newer_valid && !newer_unusable) {
      inspection.current_valid = true;
      inspection.selected_generation = named.generation;
      inspection.disposition = RecoveryDisposition::opened_current;
      inspection.consistent = true;
    } else if (usable) {
      inspection.current_valid = true;
      inspection.selected_generation = named.generation;
      inspection.disposition = RecoveryDisposition::opened_current;
      if (newer_valid) {
        inspection.detail = "CURRENT is valid, but generation " +
                            inspection.newest_valid_generation->to_string() +
                            " is present and was never committed; a writable open sets it aside "
                            "in uncommitted/";
      } else {
        inspection.detail = "CURRENT is valid, but an unusable generation file newer than CURRENT "
                            "is present; a writable open will not publish over it until it is "
                            "resolved";
      }
    } else if (matching != nullptr && matching->digest_ok && matching->generation_matches_name) {
      inspection.disposition = RecoveryDisposition::recovered_last_known_good;
      inspection.selected_generation = inspection.newest_valid_generation;
      inspection.detail = "CURRENT names a generation whose payload digest is not the digest "
                          "CURRENT records; the pointer and the file disagree";
    } else {
      inspection.disposition = RecoveryDisposition::recovered_last_known_good;
      inspection.selected_generation = inspection.newest_valid_generation;
      inspection.detail = "CURRENT names a generation that is not usable; only a last-known-good "
                          "open can adopt this store";
    }
  } else if (inspection.newest_valid_generation.has_value()) {
    inspection.disposition = RecoveryDisposition::recovered_last_known_good;
    inspection.selected_generation = inspection.newest_valid_generation;
    inspection.detail = "CURRENT is missing; only a last-known-good open can adopt this store";
  } else {
    inspection.disposition = RecoveryDisposition::created_empty;
    inspection.selected_generation = RegistryGeneration::minimum();
    inspection.consistent = !inspection.current_present;
    inspection.detail = "the store holds no committed generation";
  }
  return inspection;
}

}  // namespace dcr
