// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "internal/files.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "internal/checked.hpp"

namespace dcr::internal {
namespace {

[[nodiscard]] Error io_error(std::string_view what, const std::filesystem::path& path,
                             const std::error_code& code) {
  return Error(ErrorCode::store_io_error,
               std::string(what) + " failed for '" + path.string() + "': " + code.message(),
               path.filename().string());
}

[[nodiscard]] std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

}  // namespace

std::string unique_suffix() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed);
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::string suffix = std::to_string(current_process_id());
  suffix += '-';
  suffix += std::to_string(ticks);
  suffix += '-';
  suffix += std::to_string(sequence);
  return suffix;
}

Result<void> ensure_directory(const std::filesystem::path& directory) {
  std::error_code code;
  const auto status = std::filesystem::symlink_status(directory, code);
  if (!code && std::filesystem::exists(status)) {
    if (std::filesystem::is_directory(status)) {
      return {};
    }
    return Error(ErrorCode::store_io_error,
                 "'" + directory.string() + "' exists and is not a directory",
                 directory.filename().string());
  }
  std::error_code create_code;
  std::filesystem::create_directories(directory, create_code);
  if (create_code && !std::filesystem::is_directory(directory)) {
    return io_error("create_directories", directory, create_code);
  }
  return {};
}

bool path_exists(const std::filesystem::path& path) {
  std::error_code code;
  return std::filesystem::exists(path, code) && !code;
}

Result<std::uint64_t> regular_file_size(const std::filesystem::path& path,
                                        std::string_view field) {
  std::error_code code;
  const auto status = std::filesystem::symlink_status(path, code);
  if (code) {
    return Error(ErrorCode::store_not_found,
                 "cannot stat '" + path.string() + "': " + code.message(), std::string(field));
  }
  if (std::filesystem::is_symlink(status)) {
    return Error(ErrorCode::store_corrupt,
                 "'" + path.string() + "' is a symlink, which is never followed",
                 std::string(field));
  }
  if (!std::filesystem::is_regular_file(status)) {
    return Error(ErrorCode::store_corrupt, "'" + path.string() + "' is not a regular file",
                 std::string(field));
  }
  const auto size = std::filesystem::file_size(path, code);
  if (code) {
    return io_error("file_size", path, code);
  }
  return static_cast<std::uint64_t>(size);
}

Result<std::string> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes,
                                      std::string_view field) {
  DCR_TRY_ASSIGN(const std::uint64_t size, regular_file_size(path, field));
  if (size > max_bytes) {
    return Error(ErrorCode::store_limit_exceeded,
                 "'" + path.string() + "' is " + std::to_string(size) +
                     " bytes, which exceeds the accepted maximum of " +
                     std::to_string(max_bytes) + " bytes",
                 std::string(field));
  }
  DCR_TRY_ASSIGN(const std::size_t size_bytes, narrow_to_size(size, field));

  std::string content;
  content.resize(size_bytes);
  if (size_bytes == 0) {
    return content;
  }

#ifdef _WIN32
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::store_io_error,
                 "cannot open '" + path.string() + "' for reading", std::string(field));
  }
  std::size_t total = 0;
  bool ok = true;
  while (total < size_bytes) {
    const DWORD chunk = static_cast<DWORD>(
        (size_bytes - total) > 0x7FFFFFFFU ? 0x7FFFFFFFU : (size_bytes - total));
    DWORD read = 0;
    if (ReadFile(handle, content.data() + total, chunk, &read, nullptr) == 0 || read == 0) {
      ok = false;
      break;
    }
    total += static_cast<std::size_t>(read);
  }
  ::CloseHandle(handle);
  if (!ok || total != size_bytes) {
    return Error(ErrorCode::store_corrupt,
                 "'" + path.string() + "' changed size while being read", std::string(field));
  }
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    return Error(ErrorCode::store_io_error,
                 "cannot open '" + path.string() + "' for reading", std::string(field));
  }
  std::size_t total = 0;
  bool ok = true;
  while (total < size_bytes) {
    const ssize_t read = ::read(descriptor, content.data() + total, size_bytes - total);
    if (read < 0) {
      ok = false;
      break;
    }
    if (read == 0) {
      break;
    }
    total += static_cast<std::size_t>(read);
  }
  ::close(descriptor);
  if (!ok || total != size_bytes) {
    return Error(ErrorCode::store_corrupt,
                 "'" + path.string() + "' changed size while being read", std::string(field));
  }
#endif
  return content;
}

Result<std::vector<std::string>> list_directory(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  std::error_code code;
  std::filesystem::directory_iterator iterator(directory, code);
  if (code) {
    return io_error("directory_iterator", directory, code);
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    names.push_back(iterator->path().filename().string());
    iterator.increment(code);
    if (code) {
      return io_error("directory_iterator::increment", directory, code);
    }
  }
  return names;
}

Result<void> remove_file_if_exists(const std::filesystem::path& path) {
  std::error_code code;
  if (!std::filesystem::exists(path, code)) {
    return {};
  }
  if (code) {
    return io_error("exists", path, code);
  }
  std::error_code remove_code;
  if (!std::filesystem::remove(path, remove_code) || remove_code) {
    return io_error("remove", path, remove_code);
  }
  return {};
}

Result<void> clear_directory(const std::filesystem::path& directory) {
  std::error_code code;
  if (!std::filesystem::exists(directory, code)) {
    return {};
  }
  std::filesystem::directory_iterator iterator(directory, code);
  if (code) {
    return io_error("directory_iterator", directory, code);
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    std::error_code remove_code;
    std::filesystem::remove_all(iterator->path(), remove_code);
    if (remove_code) {
      return io_error("remove_all", iterator->path(), remove_code);
    }
    iterator.increment(code);
    if (code) {
      return io_error("directory_iterator::increment", directory, code);
    }
  }
  return {};
}

Result<void> flush_path(const std::filesystem::path& path) {
#ifdef _WIN32
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::store_io_error, "cannot open '" + path.string() + "' to flush it",
                 path.filename().string());
  }
  const BOOL flushed = ::FlushFileBuffers(handle);
  ::CloseHandle(handle);
  if (flushed == 0) {
    return Error(ErrorCode::store_io_error, "flushing '" + path.string() + "' failed",
                 path.filename().string());
  }
  return {};
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    return Error(ErrorCode::store_io_error, "cannot open '" + path.string() + "' to flush it",
                 path.filename().string());
  }
  const int result = ::fsync(descriptor);
  ::close(descriptor);
  if (result != 0) {
    return Error(ErrorCode::store_io_error, "flushing '" + path.string() + "' failed",
                 path.filename().string());
  }
  return {};
#endif
}

Result<void> flush_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
  // Windows has no portable way to flush a directory entry: a directory handle
  // obtained with CreateFileW cannot be passed to FlushFileBuffers, and
  // flushing the whole volume requires a volume handle and administrative
  // rights. Durability here therefore rests on FlushFileBuffers on the
  // generation file followed by MoveFileExW with MOVEFILE_WRITE_THROUGH, which
  // waits for the move to reach disk. This is a real difference from the POSIX
  // path and is documented rather than papered over.
  (void)directory;
  return {};
#else
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    return Error(ErrorCode::store_io_error,
                 "cannot open directory '" + directory.string() + "' to flush it",
                 directory.filename().string());
  }
  const int result = ::fsync(descriptor);
  ::close(descriptor);
  if (result != 0) {
    return Error(ErrorCode::store_io_error,
                 "flushing directory '" + directory.string() + "' failed",
                 directory.filename().string());
  }
  return {};
#endif
}

Result<void> atomic_replace(const std::filesystem::path& source,
                            const std::filesystem::path& target) {
  std::error_code code;
  if (!std::filesystem::is_regular_file(source, code) || code) {
    return Error(ErrorCode::store_io_error,
                 "'" + source.string() + "' is not a regular file and cannot be published",
                 source.filename().string());
  }
#ifdef _WIN32
  // MOVEFILE_REPLACE_EXISTING makes the replacement atomic with respect to
  // readers; MOVEFILE_WRITE_THROUGH waits until the move is on disk before
  // returning.
  if (::MoveFileExW(source.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const DWORD last_error = ::GetLastError();
    return Error(ErrorCode::store_io_error,
                 "atomic replace of '" + target.string() + "' failed with error " +
                     std::to_string(static_cast<unsigned long>(last_error)),
                 target.filename().string());
  }
  return {};
#else
  if (::rename(source.c_str(), target.c_str()) != 0) {
    return Error(ErrorCode::store_io_error,
                 "atomic replace of '" + target.string() + "' failed",
                 target.filename().string());
  }
  return {};
#endif
}

// ---------------------------------------------------------------------------
// ExclusiveFileLock
// ---------------------------------------------------------------------------

ExclusiveFileLock::~ExclusiveFileLock() { release(); }

ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept {
#ifdef _WIN32
  handle_ = other.handle_;
  other.handle_ = nullptr;
#else
  descriptor_ = other.descriptor_;
  other.descriptor_ = -1;
#endif
}

ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept {
  if (this != &other) {
    release();
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
#endif
  }
  return *this;
}

bool ExclusiveFileLock::held() const noexcept {
#ifdef _WIN32
  return handle_ != nullptr;
#else
  return descriptor_ >= 0;
#endif
}

Result<ExclusiveFileLock> ExclusiveFileLock::acquire(const std::filesystem::path& lock_path) {
  ExclusiveFileLock lock;
#ifdef _WIN32
  HANDLE handle = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD last_error = ::GetLastError();
    constexpr DWORD kSharingViolation = 32;
    constexpr DWORD kLockViolation = 33;
    if (last_error == kSharingViolation || last_error == kLockViolation) {
      return Error(ErrorCode::store_locked,
                   "another process holds the writer lock on '" + lock_path.string() + "'",
                   "registry.lock");
    }
    return Error(ErrorCode::store_io_error,
                 "cannot open the writer lock file '" + lock_path.string() +
                     "' (error " + std::to_string(static_cast<unsigned long>(last_error)) + ")",
                 "registry.lock");
  }
  lock.handle_ = handle;
  return lock;
#else
  const int descriptor =
      ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (descriptor < 0) {
    return Error(ErrorCode::store_io_error,
                 "cannot open the writer lock file '" + lock_path.string() + "'", "registry.lock");
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    ::close(descriptor);
    return Error(ErrorCode::store_locked,
                 "another process holds the writer lock on '" + lock_path.string() + "'",
                 "registry.lock");
  }
  lock.descriptor_ = descriptor;
  return lock;
#endif
}

Result<void> ExclusiveFileLock::write_holder_info(std::string_view text) {
#ifdef _WIN32
  if (handle_ == nullptr) {
    return Error(ErrorCode::internal_error, "writer lock is not held", "registry.lock");
  }
  LARGE_INTEGER zero;
  zero.QuadPart = 0;
  if (::SetFilePointerEx(static_cast<HANDLE>(handle_), zero, nullptr, FILE_BEGIN) == 0) {
    return Error(ErrorCode::store_io_error, "cannot seek the writer lock file", "registry.lock");
  }
  if (::SetEndOfFile(static_cast<HANDLE>(handle_)) == 0) {
    return Error(ErrorCode::store_io_error, "cannot truncate the writer lock file",
                 "registry.lock");
  }
  DWORD written = 0;
  if (::WriteFile(static_cast<HANDLE>(handle_), text.data(), static_cast<DWORD>(text.size()),
                  &written, nullptr) == 0 ||
      static_cast<std::size_t>(written) != text.size()) {
    return Error(ErrorCode::store_io_error, "cannot write the writer lock file", "registry.lock");
  }
  return {};
#else
  if (descriptor_ < 0) {
    return Error(ErrorCode::internal_error, "writer lock is not held", "registry.lock");
  }
  if (::ftruncate(descriptor_, 0) != 0 || ::lseek(descriptor_, 0, SEEK_SET) != 0) {
    return Error(ErrorCode::store_io_error, "cannot reset the writer lock file", "registry.lock");
  }
  const ssize_t written = ::write(descriptor_, text.data(), text.size());
  if (written < 0 || static_cast<std::size_t>(written) != text.size()) {
    return Error(ErrorCode::store_io_error, "cannot write the writer lock file", "registry.lock");
  }
  return {};
#endif
}

void ExclusiveFileLock::release() noexcept {
#ifdef _WIN32
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (descriptor_ >= 0) {
    ::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
    descriptor_ = -1;
  }
#endif
}

// ---------------------------------------------------------------------------
// StagingFile
// ---------------------------------------------------------------------------

StagingFile::~StagingFile() { abandon(); }

StagingFile::StagingFile(StagingFile&& other) noexcept {
  path_ = std::move(other.path_);
  other.path_.clear();
#ifdef _WIN32
  handle_ = other.handle_;
  other.handle_ = nullptr;
#else
  descriptor_ = other.descriptor_;
  other.descriptor_ = -1;
#endif
}

StagingFile& StagingFile::operator=(StagingFile&& other) noexcept {
  if (this != &other) {
    abandon();
    path_ = std::move(other.path_);
    other.path_.clear();
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
#endif
  }
  return *this;
}

Result<StagingFile> StagingFile::create(const std::filesystem::path& directory,
                                        std::string_view base_name) {
  auto directory_result = ensure_directory(directory);
  if (!directory_result.has_value()) {
    return directory_result.error();
  }

  StagingFile file;
  file.path_ = directory / (std::string(base_name) + ".staging-" + unique_suffix());
#ifdef _WIN32
  HANDLE handle = CreateFileW(file.path_.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::store_io_error,
                 "cannot create the staging file '" + file.path_.string() + "'",
                 file.path_.filename().string());
  }
  file.handle_ = handle;
#else
  const int descriptor = ::open(file.path_.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (descriptor < 0) {
    return Error(ErrorCode::store_io_error,
                 "cannot create the staging file '" + file.path_.string() + "'",
                 file.path_.filename().string());
  }
  file.descriptor_ = descriptor;
#endif
  return file;
}

bool StagingFile::open() const noexcept {
#ifdef _WIN32
  return handle_ != nullptr;
#else
  return descriptor_ >= 0;
#endif
}

Result<void> StagingFile::write(std::string_view data) {
  if (!open()) {
    return Error(ErrorCode::internal_error, "staging file is not open",
                 path_.filename().string());
  }
  if (data.empty()) {
    return {};
  }
#ifdef _WIN32
  std::size_t total = 0;
  while (total < data.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (data.size() - total) > 0x7FFFFFFFU ? 0x7FFFFFFFU : (data.size() - total));
    DWORD written = 0;
    if (::WriteFile(static_cast<HANDLE>(handle_), data.data() + total, chunk, &written, nullptr) ==
            0 ||
        written == 0) {
      return Error(ErrorCode::store_io_error, "writing the staging file failed",
                   path_.filename().string());
    }
    total += static_cast<std::size_t>(written);
  }
  return {};
#else
  std::size_t total = 0;
  while (total < data.size()) {
    const ssize_t written = ::write(descriptor_, data.data() + total, data.size() - total);
    if (written <= 0) {
      return Error(ErrorCode::store_io_error, "writing the staging file failed",
                   path_.filename().string());
    }
    total += static_cast<std::size_t>(written);
  }
  return {};
#endif
}

Result<void> StagingFile::flush_and_close() {
  if (!open()) {
    return Error(ErrorCode::internal_error, "staging file is not open",
                 path_.filename().string());
  }
#ifdef _WIN32
  HANDLE handle = static_cast<HANDLE>(handle_);
  if (::FlushFileBuffers(handle) == 0) {
    close_handle();
    return Error(ErrorCode::store_io_error, "flushing the staging file failed",
                 path_.filename().string());
  }
  close_handle();
  return {};
#else
  if (::fsync(descriptor_) != 0) {
    close_handle();
    return Error(ErrorCode::store_io_error, "flushing the staging file failed",
                 path_.filename().string());
  }
  close_handle();
  return {};
#endif
}

void StagingFile::keep() noexcept { close_handle(); }

void StagingFile::close_handle() noexcept {
#ifdef _WIN32
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (descriptor_ >= 0) {
    ::close(descriptor_);
    descriptor_ = -1;
  }
#endif
}

void StagingFile::abandon() noexcept {
  close_handle();
  if (!path_.empty()) {
    std::error_code code;
    std::filesystem::remove(path_, code);
    path_.clear();
  }
}

}  // namespace dcr::internal
