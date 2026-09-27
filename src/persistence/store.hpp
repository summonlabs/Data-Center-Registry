// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The snapshot store.
//
// On-disk layout
//   <root>/registry.lock            writer lock, held from open to close
//   <root>/CURRENT                  names the authoritative generation
//   <root>/generations/gen-<n>.dcrs immutable published snapshots
//   <root>/tmp/                     staging area; never authoritative
//   <root>/uncommitted/             verified generations that were never
//                                   committed, kept rather than deleted
//
// The commit point is the atomic replacement of CURRENT. A generation file
// that exists without CURRENT naming it was written but never committed, so it
// is not authoritative. A writable open moves such files aside; it never
// adopts them, because doing so would turn an unacknowledged write into
// authoritative state.

#ifndef DCR_PERSISTENCE_STORE_HPP
#define DCR_PERSISTENCE_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/state.hpp"
#include "dcr/persistence.hpp"
#include "dcr/result.hpp"
#include "internal/files.hpp"

namespace dcr::internal {

/// Fixed locations inside a store root.
struct StorePaths {
  std::filesystem::path root;
  std::filesystem::path lock_file;
  std::filesystem::path current_file;
  std::filesystem::path generations_dir;
  std::filesystem::path staging_dir;
  std::filesystem::path quarantine_dir;

  [[nodiscard]] static StorePaths for_root(const std::filesystem::path& root);
};

/// The parsed contents of CURRENT.
struct CurrentPointer {
  RegistryGeneration generation;
  std::string file_name;
  Sha256Digest digest;
};

/// The file name for a generation: `gen-` and exactly 20 decimal digits, so
/// that lexicographic order equals numeric order.
[[nodiscard]] std::string generation_file_name(RegistryGeneration generation);

/// Parses `gen-<20 digits>.dcrs`. Returns an empty optional for any other name,
/// which is how foreign files are ignored rather than interpreted.
[[nodiscard]] std::optional<RegistryGeneration> parse_generation_file_name(
    std::string_view name);

/// Encodes a container: an 88-byte header plus the canonical payload.
[[nodiscard]] Result<std::string> encode_container(const RegistryState& state);

/// Decodes and verifies a container. Rejects a bad magic, a wrong format
/// version, a wrong endian marker, a length that does not match the file, a
/// digest mismatch, and a payload that does not decode.
[[nodiscard]] Result<RegistryState> decode_container(std::string_view bytes,
                                                     const RegistryLimits& limits);

/// The outcome of opening a store: the store itself, the state it holds, and
/// the report describing how it was opened. Defined after SnapshotStore
/// because it holds one by value.
struct StoreOpenResult;

/// A durable snapshot store. Move-only. A writable store holds the writer lock
/// for its whole lifetime.
class SnapshotStore {
 public:
  SnapshotStore() = default;
  ~SnapshotStore();

  SnapshotStore(const SnapshotStore&) = delete;
  SnapshotStore& operator=(const SnapshotStore&) = delete;
  SnapshotStore(SnapshotStore&& other) noexcept;
  SnapshotStore& operator=(SnapshotStore&& other) noexcept;

  /// Opens the store, or creates it, and loads the authoritative state
  /// according to the recovery policy.
  [[nodiscard]] static Result<StoreOpenResult> open(const StoreOptions& options);

  [[nodiscard]] bool writable() const noexcept { return writable_; }
  [[nodiscard]] const StoreOptions& options() const noexcept { return options_; }
  [[nodiscard]] const StorePaths& paths() const noexcept { return paths_; }

  /// Publishes a new generation. `expected_base` is the generation the caller
  /// believes is authoritative; the store compares it against CURRENT and
  /// refuses to publish over anything else. This is the second line of defence
  /// behind the writer lock, and it is what makes a stale writer unable to
  /// overwrite a newer commit even if the lock were bypassed.
  [[nodiscard]] Result<Sha256Digest> publish(const RegistryState& state,
                                             RegistryGeneration expected_base);

  void close() noexcept;

 private:
  [[nodiscard]] Result<void> cleanup_retention(RegistryGeneration newest) const;
  [[nodiscard]] Result<void> quarantine_uncommitted(
      const std::vector<RegistryGeneration>& generations, OpenReport& report) const;

  StoreOptions options_;
  StorePaths paths_;
  ExclusiveFileLock lock_;
  bool writable_ = false;
};

/// Reads and verifies the newest state without taking the writer lock.
[[nodiscard]] Result<RegistryState> load_state(const StoreOptions& options, OpenReport& report,
                             bool store_existed);

/// The outcome of opening a store: the store itself, the state it holds, and
/// the report describing how it was opened.
struct StoreOpenResult {
  SnapshotStore store;
  RegistryState state;
  OpenReport report;
};

}  // namespace dcr::internal

#endif  // DCR_PERSISTENCE_STORE_HPP
