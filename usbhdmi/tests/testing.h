/* SPDX-License-Identifier: GPL-2.0-only
 *
 * A very small test framework.
 *
 * There is no test framework dependency in this project on purpose: it has
 * to build with nothing but the Visual Studio build tools that are already
 * required for the driver, on a machine that may have no package manager and
 * no network. This is about eighty lines and does everything needed.
 *
 * Tests register themselves at static initialisation time:
 *
 *     TEST(damage, far_apart_regions_stay_split) {
 *       ...
 *       CHECK_EQ(periods, 2);
 *     }
 *
 * A failing CHECK records the failure and keeps going, so one run reports
 * everything that is broken rather than only the first thing.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  const char* group;
  const char* name;
  void (*body)();
};

/* Registers a test. Called from the constructor of a file scope object, so
 * every test in every translation unit is known before main runs. */
void Register(const TestCase& test);

/* Runs everything, or only the groups whose name contains `filter`.
 * Returns the number of failed tests. */
int RunAll(const char* filter);

/* Called by the CHECK macros. `detail` is already formatted. */
void ReportFailure(const char* file, int line, const std::string& detail);

/* Turns whatever a CHECK compared into something printable. A template so
 * that any integer width works without a pile of overloads, several of which
 * would collide anyway: size_t and unsigned long long are the same type
 * here. */
template <typename T>
std::string Describe(const T& value) {
  return std::to_string(value);
}
inline std::string Describe(bool value) { return value ? "true" : "false"; }
inline std::string Describe(const char* value) {
  return value ? value : "(null)";
}
inline std::string Describe(const std::string& value) { return value; }

struct Registrar {
  explicit Registrar(const TestCase& test) { Register(test); }
};

}  // namespace testing

#define TEST(group_name, test_name)                                        \
  static void group_name##_##test_name##_body();                           \
  static ::testing::Registrar group_name##_##test_name##_registrar(        \
      {#group_name, #test_name, &group_name##_##test_name##_body});        \
  static void group_name##_##test_name##_body()

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      ::testing::ReportFailure(__FILE__, __LINE__,                         \
                               std::string("expected ") + #condition);     \
    }                                                                      \
  } while (0)

#define CHECK_EQ(actual, expected)                                         \
  do {                                                                     \
    const auto check_actual_ = (actual);                                   \
    const auto check_expected_ = (expected);                               \
    if (!(check_actual_ == check_expected_)) {                             \
      ::testing::ReportFailure(                                            \
          __FILE__, __LINE__,                                              \
          std::string(#actual) + " is " +                                  \
              ::testing::Describe(check_actual_) + ", expected " +         \
              ::testing::Describe(check_expected_));                       \
    }                                                                      \
  } while (0)

/* Attaches a sentence explaining why the expectation exists. Failures print
 * it, so a broken test says what behaviour was lost rather than only which
 * numbers stopped matching. */
#define CHECK_EQ_BECAUSE(actual, expected, why)                            \
  do {                                                                     \
    const auto check_actual_ = (actual);                                   \
    const auto check_expected_ = (expected);                               \
    if (!(check_actual_ == check_expected_)) {                             \
      ::testing::ReportFailure(                                            \
          __FILE__, __LINE__,                                              \
          std::string(#actual) + " is " +                                  \
              ::testing::Describe(check_actual_) + ", expected " +         \
              ::testing::Describe(check_expected_) + "\n         " + why); \
    }                                                                      \
  } while (0)

#define CHECK_BECAUSE(condition, why)                                      \
  do {                                                                     \
    if (!(condition)) {                                                    \
      ::testing::ReportFailure(                                            \
          __FILE__, __LINE__,                                              \
          std::string("expected ") + #condition + "\n         " + why);    \
    }                                                                      \
  } while (0)
