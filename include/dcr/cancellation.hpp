// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Cancellation.
//
// Mutations are synchronous, but a mutation is not instantaneous: it validates,
// then writes a new generation to disk, then verifies it, then publishes it.
// A caller that no longer wants the result can cancel, and the registry checks
// the token at defined points. The check that matters is the last one, which
// happens after the new generation has been written and verified and before it
// is published: a cancelled mutation may leave a temporary file behind, and the
// registry deletes it, but it can never leave published authoritative state.
//
// A default-constructed token is never cancelled. That is not a sentinel value:
// it means "no cancellation was requested by anyone", which is exactly what a
// caller who did not ask for cancellation wants.

#ifndef DCR_CANCELLATION_HPP
#define DCR_CANCELLATION_HPP

#include <atomic>
#include <memory>

namespace dcr {

/// A handle that observes cancellation. Copyable and cheap.
class CancellationToken {
 public:
  /// A token that is never cancelled.
  CancellationToken() noexcept = default;

  [[nodiscard]] bool is_cancelled() const noexcept {
    return flag_ != nullptr && flag_->load(std::memory_order_acquire);
  }

  /// True when this token is attached to a source that could cancel it.
  [[nodiscard]] bool can_be_cancelled() const noexcept { return flag_ != nullptr; }

 private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<const std::atomic<bool>> flag) noexcept
      : flag_(std::move(flag)) {}

  std::shared_ptr<const std::atomic<bool>> flag_;
};

/// The write end of a cancellation. One source may hand out many tokens.
class CancellationSource {
 public:
  CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

  CancellationSource(const CancellationSource&) = delete;
  CancellationSource& operator=(const CancellationSource&) = delete;

  /// Requests cancellation. Idempotent and safe to call from any thread.
  void cancel() noexcept { flag_->store(true, std::memory_order_release); }

  [[nodiscard]] bool is_cancelled() const noexcept {
    return flag_->load(std::memory_order_acquire);
  }

  [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(flag_); }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace dcr

#endif  // DCR_CANCELLATION_HPP
