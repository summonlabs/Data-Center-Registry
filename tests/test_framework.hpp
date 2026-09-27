// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small test framework.
//
// It has no third-party dependencies, it never terminates the process on a
// failed assertion, and it imposes no timeout of any kind. A test that hangs is
// a defect to be diagnosed, not a test to be killed.

#ifndef DCR_TESTS_TEST_FRAMEWORK_HPP
#define DCR_TESTS_TEST_FRAMEWORK_HPP

#include <cstddef>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace dcrtest {

struct TestCase {
  std::string suite;
  std::string name;
  void (*function)();
};

[[nodiscard]] std::vector<TestCase>& registry();

int register_test(const char* suite, const char* name, void (*function)());

/// Records a failure for the currently running test.
void record_failure(const char* file, int line, const std::string& message);

/// Detects a to_string overload found by argument-dependent lookup, so that
/// every strongly typed enum renders by name.
template <class T, class = void>
struct has_to_string : std::false_type {};

template <class T>
struct has_to_string<T, std::void_t<decltype(to_string(std::declval<const T&>()))>>
    : std::true_type {};

template <class T, class = void>
struct has_member_to_string : std::false_type {};

template <class T>
struct has_member_to_string<T, std::void_t<decltype(std::declval<const T&>().to_string())>>
    : std::true_type {};

[[nodiscard]] std::string describe(const std::string& value);
[[nodiscard]] std::string describe(const char* value);
[[nodiscard]] std::string describe(bool value);

template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (has_to_string<T>::value) {
    return std::string(to_string(value));
  } else if constexpr (has_member_to_string<T>::value) {
    return value.to_string();
  } else {
    std::ostringstream out;
    out << value;
    return out.str();
  }
}

/// Compares two values and records a failure with both rendered. A function
/// rather than a macro conditional, so that comparing two constants does not
/// trip a "conditional expression is constant" warning in a warning-clean
/// build.
template <class T, class U>
void check_equal(const T& expected, const U& actual, const char* file, int line,
                 const char* text) {
  if (!(expected == actual)) {
    record_failure(file, line, std::string("DCR_CHECK_EQ failed: ") + text +
                                  " expected=" + describe(expected) +
                                  " actual=" + describe(actual));
  }
}

int run_all(const std::vector<std::string>& filters, std::size_t repeat, bool list_only);

/// The seed the current property test is running under.
void set_current_seed(unsigned long long seed);
[[nodiscard]] unsigned long long current_seed();

/// Notes that a property test ran under a seed, so a failure report can say how
/// to reproduce it.
void note_seed(unsigned long long seed);

}  // namespace dcrtest

#define DCR_TEST(suite, name)                                                      \
  static void suite##_##name##_body();                                             \
  static const int suite##_##name##_registration =                                 \
      dcrtest::register_test(#suite, #name, suite##_##name##_body);                \
  static void suite##_##name##_body()

#define DCR_CHECK(expression)                                                     \
  do {                                                                            \
    if (!(expression)) {                                                          \
      dcrtest::record_failure(__FILE__, __LINE__, "DCR_CHECK failed: " #expression); \
    }                                                                             \
  } while (false)

#define DCR_CHECK_EQ(expected, actual) \
  dcrtest::check_equal((expected), (actual), __FILE__, __LINE__, #expected " == " #actual)

#define DCR_CHECK_NE(left, right)                                                 \
  do {                                                                            \
    if ((left) == (right)) {                                                      \
      dcrtest::record_failure(__FILE__, __LINE__, "DCR_CHECK_NE failed: " #left " != " #right); \
    }                                                                             \
  } while (false)

#define DCR_REQUIRE(expression)                                                   \
  do {                                                                            \
    if (!(expression)) {                                                          \
      dcrtest::record_failure(__FILE__, __LINE__, "DCR_REQUIRE failed: " #expression); \
      return;                                                                     \
    }                                                                             \
  } while (false)

/// Requires that a Result holds a value, and binds it. Usage:
///   DCR_REQUIRE_OK(const auto value, expression);
#define DCR_REQUIRE_OK_IMPL(destination, expression, identifier)                     \
  auto DCR_TRY_CAT(dcr_required_, identifier) = (expression);                        \
  if (!DCR_TRY_CAT(dcr_required_, identifier).has_value()) {                         \
    dcrtest::record_failure(__FILE__, __LINE__,                                      \
                            std::string("DCR_REQUIRE_OK failed: " #expression) +     \
                                " error=" +                                          \
                                DCR_TRY_CAT(dcr_required_, identifier).error().to_string()); \
    return;                                                                          \
  }                                                                                  \
  destination = std::move(DCR_TRY_CAT(dcr_required_, identifier)).value()

#define DCR_REQUIRE_OK(destination, expression) \
  DCR_REQUIRE_OK_IMPL(destination, expression, __COUNTER__)

/// Requires that a Result is rejected with a specific error category. Braced,
/// so that whatever the expression produced is destroyed at the end of the
/// statement: a failed expectation must not keep a store lock alive for the
/// rest of the test.
#define DCR_REQUIRE_ERROR(expected_code, expression)                              \
  do {                                                                            \
    auto dcr_rejected = (expression);                                             \
    if (dcr_rejected.has_value()) {                                               \
      dcrtest::record_failure(__FILE__, __LINE__,                                 \
                              "DCR_REQUIRE_ERROR failed: " #expression " succeeded"); \
    } else if (dcr_rejected.error().code() != (expected_code)) {                   \
      dcrtest::record_failure(__FILE__, __LINE__,                                 \
                              std::string("DCR_REQUIRE_ERROR failed: " #expression) + \
                                  " expected=" + std::string(to_string(expected_code)) + \
                                  " actual=" + dcr_rejected.error().to_string()); \
    }                                                                             \
  } while (false)

/// Checks that a valueless Result succeeded, without returning from the test.
#define DCR_CHECK_OK(expression)                                                  \
  do {                                                                            \
    auto dcr_checked = (expression);                                              \
    if (!dcr_checked.has_value()) {                                               \
      dcrtest::record_failure(__FILE__, __LINE__,                                 \
                              std::string("DCR_CHECK_OK failed: " #expression) +  \
                                  " error=" + dcr_checked.error().to_string());   \
    }                                                                             \
  } while (false)

#endif  // DCR_TESTS_TEST_FRAMEWORK_HPP
