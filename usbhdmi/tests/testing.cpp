/* SPDX-License-Identifier: GPL-2.0-only */

#include "testing.h"

namespace testing {
namespace {

/* A function local static, so registration during static initialisation
 * cannot race the construction of the container it registers into. */
std::vector<TestCase>& Tests() {
  static std::vector<TestCase> tests;
  return tests;
}

std::vector<std::string>& Failures() {
  static std::vector<std::string> failures;
  return failures;
}

}  // namespace

void Register(const TestCase& test) { Tests().push_back(test); }

void ReportFailure(const char* file, int line, const std::string& detail) {
  /* Just the file name: the full path is noise and differs per machine. */
  const char* name = strrchr(file, '\\');
  name = name ? name + 1 : file;
  Failures().push_back(std::string(name) + ":" + std::to_string(line) + ": " +
                       detail);
}

int RunAll(const char* filter) {
  int failed = 0;
  int passed = 0;
  const char* group = nullptr;

  for (const TestCase& test : Tests()) {
    if (filter && *filter && !strstr(test.group, filter) &&
        !strstr(test.name, filter)) {
      continue;
    }

    if (!group || strcmp(group, test.group) != 0) {
      group = test.group;
      printf("\n%s\n", group);
    }

    Failures().clear();
    test.body();

    if (Failures().empty()) {
      ++passed;
      printf("  ok    %s\n", test.name);
    } else {
      ++failed;
      printf("  FAIL  %s\n", test.name);
      for (const std::string& failure : Failures()) {
        printf("        %s\n", failure.c_str());
      }
    }
  }

  printf("\n%d passed, %d failed\n", passed, failed);
  return failed;
}

}  // namespace testing

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : "";
  return testing::RunAll(filter) == 0 ? 0 : 1;
}
