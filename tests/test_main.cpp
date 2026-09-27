// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dcrtest {
namespace {

std::vector<std::string>& current_failures() {
  static std::vector<std::string> failures;
  return failures;
}

unsigned long long g_current_seed = 0;
std::vector<unsigned long long> g_seeds_run;

bool matches(const TestCase& test, const std::vector<std::string>& filters) {
  if (filters.empty()) {
    return true;
  }
  const std::string full = test.suite + "." + test.name;
  for (const auto& filter : filters) {
    if (full.find(filter) != std::string::npos || test.suite == filter) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

int register_test(const char* suite, const char* name, void (*function)()) {
  registry().push_back(TestCase{suite, name, function});
  return 0;
}

void record_failure(const char* file, int line, const std::string& message) {
  std::string text = file;
  text += ':';
  text += std::to_string(line);
  text += ": ";
  text += message;
  current_failures().push_back(std::move(text));
}

std::string describe(const std::string& value) { return "\"" + value + "\""; }
std::string describe(const char* value) { return std::string("\"") + value + "\""; }
std::string describe(bool value) { return value ? "true" : "false"; }

void set_current_seed(unsigned long long seed) { g_current_seed = seed; }

unsigned long long current_seed() { return g_current_seed; }

void note_seed(unsigned long long seed) { g_seeds_run.push_back(seed); }

int run_all(const std::vector<std::string>& filters, std::size_t repeat, bool list_only) {
  auto& tests = registry();
  if (list_only) {
    for (const auto& test : tests) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  std::size_t run = 0;
  std::size_t failed = 0;
  const auto started = std::chrono::steady_clock::now();

  for (std::size_t repetition = 0; repetition < repeat; ++repetition) {
    for (const auto& test : tests) {
      if (!matches(test, filters)) {
        continue;
      }
      current_failures().clear();
      g_seeds_run.clear();
      test.function();
      ++run;
      if (!current_failures().empty()) {
        ++failed;
        std::printf("FAIL %s.%s (repeat %zu)\n", test.suite.c_str(), test.name.c_str(),
                    repetition + 1);
        for (const auto& failure : current_failures()) {
          std::printf("     %s\n", failure.c_str());
        }
        for (const auto seed : g_seeds_run) {
          std::printf("     property seed: %llu\n", seed);
        }
      } else {
        std::printf("ok   %s.%s\n", test.suite.c_str(), test.name.c_str());
      }
      std::fflush(stdout);
    }
  }

  const auto finished = std::chrono::steady_clock::now();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
  std::printf("\n%zu of %zu test case(s) failed in %lld ms\n", failed, run,
              static_cast<long long>(elapsed));
  return failed == 0 ? 0 : 1;
}

}  // namespace dcrtest

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  std::size_t repeat = 1;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--repeat" && index + 1 < argc) {
      repeat = static_cast<std::size_t>(std::stoul(argv[++index]));
      if (repeat == 0) {
        repeat = 1;
      }
    } else {
      filters.push_back(argument);
    }
  }
  return dcrtest::run_all(filters, repeat, list_only);
}
