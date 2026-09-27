// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Filesystem primitives for durable publication.
//
// The rules this file exists to enforce:
//
//   * Nothing is ever modified in place. New content is written to a uniquely
//     named file in a staging directory on the same filesystem, flushed,
//     verified and then moved into place with an atomic replace.
//   * A file is read only after its size has been checked against a declared
//     bound, so a hostile or corrupt file cannot cause an unbounded allocation.
//   * The writer lock is an operating-system lock on a fixed path, held for the
//     lifetime of a writable registry and released by the kernel if the process
//     dies, so a crash cannot leave a store permanently locked.
//   * Symlinked generation files are never followed.

#ifndef DCR_INTERNAL_FILES_HPP
#define DCR_INTERNAL_FILES_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "dcr/result.hpp"

namespace dcr::internal {

/// A uniqueness suffix for staging files: process id, a process-local counter
/// and a steadily increasing tick, so two processes and two threads in one
/// process cannot collide.
[[nodiscard]] std::string unique_suffix() noexcept;

/// Creates a directory and any missing parents.
[[nodiscard]] Result<void> ensure_directory(const std::filesystem::path& directory);

/// True when the path exists as anything.
[[nodiscard]] bool path_exists(const std::filesystem::path& path);

/// Size of a regular file, rejecting a symlink, a directory, or a missing
/// path.
[[nodiscard]] Result<std::uint64_t> regular_file_size(const std::filesystem::path& path,
                                                      std::string_view field);

/// Reads a whole regular file. Rejects a path that is not a regular file, a
/// symlink, a file larger than max_bytes, and any short read.
[[nodiscard]] Result<std::string> read_file_bounded(const std::filesystem::path& path,
                                                    std::uint64_t max_bytes,
                                                    std::string_view field);

/// Names of the entries directly inside a directory, in unspecified order.
[[nodiscard]] Result<std::vector<std::string>> list_directory(
    const std::filesystem::path& directory);

/// Deletes a file if it exists. Succeeds when the file is already gone.
[[nodiscard]] Result<void> remove_file_if_exists(const std::filesystem::path& path);

/// Deletes everything inside a directory without deleting the directory.
[[nodiscard]] Result<void> clear_directory(const std::filesystem::path& directory);

/// Flushes a file's contents to stable storage.
[[nodiscard]] Result<void> flush_path(const std::filesystem::path& path);

/// Flushes a directory entry, so that a rename survives power loss. On Windows
/// this is a documented no-op: NTFS metadata ordering is provided by the
/// atomic replace itself and a directory handle cannot be flushed through the
/// Win32 API without a volume handle.
[[nodiscard]] Result<void> flush_directory(const std::filesystem::path& directory);

/// Atomically replaces `target` with `source`, both on the same filesystem.
[[nodiscard]] Result<void> atomic_replace(const std::filesystem::path& source,
                                          const std::filesystem::path& target);

/// An exclusive, non-blocking writer lock on a lock file.
class ExclusiveFileLock {
 public:
  ExclusiveFileLock() noexcept = default;
  ~ExclusiveFileLock();

  ExclusiveFileLock(const ExclusiveFileLock&) = delete;
  ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;
  ExclusiveFileLock(ExclusiveFileLock&& other) noexcept;
  ExclusiveFileLock& operator=(ExclusiveFileLock&& other) noexcept;

  /// Acquires the lock. Fails with store_locked when another process holds it,
  /// and with store_io_error when the lock file cannot be created or opened.
  [[nodiscard]] static Result<ExclusiveFileLock> acquire(const std::filesystem::path& lock_path);

  [[nodiscard]] bool held() const noexcept;

  /// Records diagnostic holder information inside the lock file. The lock
  /// itself is the operating-system lock, not the file contents.
  [[nodiscard]] Result<void> write_holder_info(std::string_view text);

  void release() noexcept;

 private:
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int descriptor_ = -1;
#endif
};

/// A staging file that is deleted unless it is explicitly kept.
class StagingFile {
 public:
  StagingFile() noexcept = default;
  ~StagingFile();

  StagingFile(const StagingFile&) = delete;
  StagingFile& operator=(const StagingFile&) = delete;
  StagingFile(StagingFile&& other) noexcept;
  StagingFile& operator=(StagingFile&& other) noexcept;

  /// Creates a uniquely named file inside `directory`.
  [[nodiscard]] static Result<StagingFile> create(const std::filesystem::path& directory,
                                                  std::string_view base_name);

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] bool open() const noexcept;

  /// Appends bytes. Fails if the file is not open.
  [[nodiscard]] Result<void> write(std::string_view data);

  /// Flushes the file to stable storage and closes it.
  [[nodiscard]] Result<void> flush_and_close();

  /// Closes the handle but keeps the file on disk.
  void keep() noexcept;

  /// Closes and deletes. Idempotent. Also runs from the destructor.
  void abandon() noexcept;

 private:
  void close_handle() noexcept;

  std::filesystem::path path_;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int descriptor_ = -1;
#endif
};

}  // namespace dcr::internal

#endif  // DCR_INTERNAL_FILES_HPP
