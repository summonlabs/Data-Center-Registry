// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Result<T>: an explicit value-or-rejection return type.
//
// The library does not use exceptions for expected outcomes. Every fallible
// operation returns a Result. Exceptions are reserved for defects and for
// standard-library failures such as allocation.

#ifndef DCR_RESULT_HPP
#define DCR_RESULT_HPP

#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "dcr/error.hpp"

namespace dcr {

/// Thrown only by Result::value() when the result holds an error, and by
/// Result::error() when it holds a value. Callers that prefer not to branch may
/// catch this; ordinary code should test has_value().
class ResultError : public std::runtime_error {
 public:
  explicit ResultError(const Error& error)
      : std::runtime_error(error.to_string()), error_(error) {}

  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  Error error_;
};

/// Holds either a value or an Error. Never holds both, never holds neither.
template <class T>
class Result {
 public:
  static_assert(!std::is_reference_v<T>, "Result<T> does not hold references");
  static_assert(!std::is_void_v<T>, "use Result<void> for valueless results");

  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  Result(const Result&) = default;
  Result(Result&&) noexcept = default;
  Result& operator=(const Result&) = default;
  Result& operator=(Result&&) noexcept = default;
  ~Result() = default;

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  /// The value. Throws ResultError when this Result holds an error.
  [[nodiscard]] const T& value() const& {
    ensure_value();
    return *value_;
  }
  [[nodiscard]] T& value() & {
    ensure_value();
    return *value_;
  }
  [[nodiscard]] T&& value() && {
    ensure_value();
    return std::move(*value_);
  }

  /// The error. Throws ResultError when this Result holds a value.
  [[nodiscard]] const Error& error() const& {
    ensure_error();
    return error_;
  }

  /// The rejection category when this Result holds an error, otherwise empty.
  /// Useful for tests that assert an exact rejection reason.
  [[nodiscard]] std::optional<ErrorCode> error_code() const noexcept {
    if (value_.has_value()) {
      return std::nullopt;
    }
    return error_.code();
  }

  [[nodiscard]] const T& operator*() const& { return value(); }
  [[nodiscard]] T& operator*() & { return value(); }
  [[nodiscard]] const T* operator->() const { return &value(); }
  [[nodiscard]] T* operator->() { return &value(); }

  /// The value when present, otherwise the supplied fallback.
  template <class U>
  [[nodiscard]] T value_or(U&& fallback) const& {
    return value_.has_value() ? *value_ : static_cast<T>(std::forward<U>(fallback));
  }
  template <class U>
  [[nodiscard]] T value_or(U&& fallback) && {
    return value_.has_value() ? std::move(*value_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  void ensure_value() const {
    if (!value_.has_value()) {
      throw ResultError(error_);
    }
  }
  void ensure_error() const {
    if (value_.has_value()) {
      throw ResultError(
          Error(ErrorCode::internal_error, "Result holds a value, not an error"));
    }
  }

  std::optional<T> value_;
  Error error_;
};

/// Result of an operation that produces no value.
template <>
class Result<void> {
 public:
  Result() noexcept = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  void value() const {
    if (error_.has_value()) {
      throw ResultError(*error_);
    }
  }

  [[nodiscard]] const Error& error() const& {
    if (!error_.has_value()) {
      throw ResultError(Error(ErrorCode::internal_error, "Result holds no error"));
    }
    return *error_;
  }

 private:
  std::optional<Error> error_;
};

/// Shortens the common "propagate the rejection" pattern for a result that
/// carries a value.
///
/// `destination` is either an existing lvalue or a fresh declaration with a
/// type and a name (`const std::uint32_t width, read_width()`); the declared
/// name stays visible in the enclosing scope, which is what lets a sequence of
/// reads build up a value. The macro is deliberately not a braced block, so it
/// must be used as a statement and never as the unbraced body of an `if` or a
/// loop.
#define DCR_TRY_CAT_INNER(left, right) left##right
#define DCR_TRY_CAT(left, right) DCR_TRY_CAT_INNER(left, right)

#define DCR_TRY_ASSIGN_IMPL(destination, expression, identifier)          \
  auto DCR_TRY_CAT(dcr_try_result_, identifier) = (expression);           \
  if (!DCR_TRY_CAT(dcr_try_result_, identifier).has_value()) {            \
    return DCR_TRY_CAT(dcr_try_result_, identifier).error();              \
  }                                                                       \
  destination = std::move(DCR_TRY_CAT(dcr_try_result_, identifier)).value()

#define DCR_TRY_ASSIGN(destination, expression) \
  DCR_TRY_ASSIGN_IMPL(destination, expression, __COUNTER__)

/// Shortens "propagate the rejection" for a valueless result. Same rules as
/// DCR_TRY_ASSIGN.
#define DCR_TRY_IMPL(expression, identifier)               \
  auto DCR_TRY_CAT(dcr_try_result_, identifier) = (expression); \
  if (!DCR_TRY_CAT(dcr_try_result_, identifier).has_value()) {  \
    return DCR_TRY_CAT(dcr_try_result_, identifier).error();    \
  }

#define DCR_TRY(expression) DCR_TRY_IMPL(expression, __COUNTER__)

}  // namespace dcr

#endif  // DCR_RESULT_HPP
