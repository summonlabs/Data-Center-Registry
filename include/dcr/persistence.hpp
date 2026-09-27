// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Durable store layout, recovery policy and integrity inspection.
//
// On-disk layout
// --------------
//   <root>/registry.lock            writer lock, held for the life of a
//                                   writable registry
//   <root>/CURRENT                  small text file naming the authoritative
//                                   generation
//   <root>/generations/gen-<20d>.dcrs
//                                   immutable published snapshots
//   <root>/tmp/                     staging area for the next generation
//
// Publication protocol
// --------------------
//   plan -> validate -> reserve the next generation -> write it into tmp/
//   -> flush it -> re-open and verify it -> atomically rename it into
//   generations/ -> atomically replace CURRENT -> flush the directory
//   -> retire superseded generations beyond the retention window.
//
// A file that appears in generations/ has therefore been written, flushed and
// verified in full. A crash before the CURRENT replacement leaves the previous
// generation authoritative and the new file unreferenced; a crash after it
// leaves the new generation authoritative. Nothing in tmp/ is ever
// authoritative, and a reader never reads from tmp/.

#ifndef DCR_PERSISTENCE_HPP
#define DCR_PERSISTENCE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dcr/digest.hpp"
#include "dcr/generation.hpp"
#include "dcr/limits.hpp"
#include "dcr/result.hpp"

namespace dcr {

enum class StoreOpenMode : std::uint8_t {
  /// The store must already exist. A missing store is rejected.
  open_existing = 1,
  /// Open the store if it exists, create an empty one if it does not.
  open_or_create = 2,
  /// Create a new empty store. An existing store is rejected rather than
  /// reused, so an initialisation step cannot silently adopt unrelated state.
  create_new = 3,
  /// Open the newest committed generation for reading without taking the
  /// writer lock. Mutations are rejected with store_read_only. Safe while
  /// another process is writing, because published generations are immutable.
  read_only = 4,
};

enum class RecoveryPolicy : std::uint8_t {
  /// Default. The store is authoritative exactly when CURRENT is present,
  /// readable, points at a generation file that verifies, and no verified
  /// generation file exists that is newer than the one CURRENT names. Any
  /// other situation is a hard failure: the registry refuses to guess which
  /// state an operator meant.
  strict = 1,
  /// Explicit opt-in. When CURRENT is unusable, fall back to the newest
  /// generation file that verifies, and report exactly which generation was
  /// discarded. This can discard a committed generation, so it is never the
  /// default and the choice is recorded in the OpenReport.
  last_known_good = 2,
};

enum class DurabilityMode : std::uint8_t {
  /// Default. Flush the generation file and the directory before reporting a
  /// commit. Durable across power loss.
  strict = 1,
  /// Flush without fsync. A committed generation survives process death but
  /// not necessarily power loss. Intended for benchmarks and bulk import, and
  /// never a silent default.
  relaxed = 2,
};

/// What the store looked like when it was opened.
enum class RecoveryDisposition : std::uint8_t {
  /// A new empty store was created by this call.
  created_empty = 1,
  /// An existing store with no committed generation was opened. This is the
  /// state a store is in between its initialisation and its first commit.
  opened_empty = 2,
  /// CURRENT named a verified generation and it was accepted unchanged.
  opened_current = 3,
  /// CURRENT was unusable and the newest verified generation was adopted under
  /// RecoveryPolicy::last_known_good.
  recovered_last_known_good = 4,
  /// A store was opened read-only at its newest verified generation.
  opened_read_only = 5,
};

[[nodiscard]] std::string_view to_string(RecoveryDisposition disposition) noexcept;

/// The result of opening a store.
struct OpenReport {
  StoreOpenMode mode = StoreOpenMode::open_or_create;
  RecoveryDisposition disposition = RecoveryDisposition::created_empty;
  /// True when this call created the store.
  bool created = false;
  /// True when the adopted state came from something other than a clean
  /// CURRENT read.
  bool recovered = false;
  /// The generation now in memory.
  RegistryGeneration generation;
  std::uint64_t record_count = 0;
  Sha256Digest snapshot_digest;
  /// The generation that was committed, verified and then not adopted. Present
  /// only after a last-known-good recovery.
  std::optional<RegistryGeneration> discarded_generation;
  /// Verified generation files that were newer than CURRENT, and therefore
  /// never committed. A writable open moves them into `<root>/uncommitted/`
  /// rather than adopting them or deleting them.
  std::vector<RegistryGeneration> quarantined_generations;
  /// Generation files present but unusable, newest first.
  std::vector<RegistryGeneration> unusable_generations;
  /// Human-readable explanation of a recovery decision. Empty for a clean open.
  std::string detail;
};

/// Everything needed to open or inspect a store.
struct StoreOptions {
  std::filesystem::path root;
  StoreOpenMode mode = StoreOpenMode::open_or_create;
  RecoveryPolicy recovery = RecoveryPolicy::strict;
  DurabilityMode durability = DurabilityMode::strict;
  RegistryLimits limits;
  /// How many published generations to keep, including the current one.
  /// Must be at least 2: readers open a published generation by name, and a
  /// retention of 1 would let a writer delete the file a reader is opening.
  std::uint32_t generation_retention = 3;

  [[nodiscard]] Result<void> validate() const;
};

/// The state of one generation file on disk.
struct GenerationFileStatus {
  std::string file_name;
  /// The generation encoded in the file name.
  RegistryGeneration file_generation;
  /// True when the file's own header and payload digests verify.
  bool digest_ok = false;
  /// The payload digest the file carries. Zero when the file did not verify.
  Sha256Digest payload_digest;
  /// True when the payload decoded into a valid registry state.
  bool parsed = false;
  /// True when the state inside the file reports the same generation as the
  /// file name. A mismatch means the file is not the generation it claims.
  bool generation_matches_name = false;
  /// Explanation for anything that is not fully valid.
  std::string detail;
};

/// A read-only audit of a store. Never repairs, never locks, never mutates.
struct StoreInspection {
  bool root_exists = false;
  bool current_present = false;
  bool current_parseable = false;
  bool current_valid = false;
  std::optional<RegistryGeneration> current_generation;
  /// Every generation file found, newest first.
  std::vector<GenerationFileStatus> generation_files;
  /// The newest generation whose file verified and decoded.
  std::optional<RegistryGeneration> newest_valid_generation;
  /// The generation an open with the same options would adopt.
  std::optional<RegistryGeneration> selected_generation;
  /// What an open would do with this store. `recovered_last_known_good` here
  /// means the store is only openable under RecoveryPolicy::last_known_good.
  RecoveryDisposition disposition = RecoveryDisposition::created_empty;
  /// True when the store is consistent: CURRENT is valid and names the newest
  /// verified generation.
  bool consistent = false;
  std::string detail;
};

/// Audits a store without taking the writer lock and without changing anything.
/// Returns a successful inspection even for an inconsistent store: the
/// inconsistency is reported in the inspection, not as an error. Only an I/O
/// failure or an invalid StoreOptions produces an Error.
[[nodiscard]] Result<StoreInspection> inspect_store(const StoreOptions& options);

}  // namespace dcr

#endif  // DCR_PERSISTENCE_HPP
