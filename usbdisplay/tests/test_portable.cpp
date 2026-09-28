/* SPDX-License-Identifier: GPL-2.0-only
 *
 * That the portable half of this project stays portable.
 *
 * Everything the tests cover builds with any compiler on any system, which
 * is what lets the whole suite run in seconds without Windows. That is easy
 * to lose by accident: the Microsoft compiler quietly supplies headers a
 * file never asked for, and offers its own safer spellings of standard
 * functions that exist nowhere else. Code written against it compiles
 * cleanly and then fails somewhere else entirely.
 *
 * It has happened twice. Both times the fault was found by a compiler
 * rather than by review, and once by a compiler in a build machine rather
 * than on the desk where it was written. So it is checked here instead,
 * where it costs a few milliseconds and fails immediately.
 *
 * These read the source rather than the compiled result, which is unusual
 * for a test and is the point: the fault is in what was written, and it can
 * only be seen before a compiler has had a chance to forgive it.
 */

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "testing.h"

namespace {

/* Opens a file from wherever the tests happen to have been started. */
std::string ReadSource(const std::string& relative) {
  for (const std::string& prefix : {std::string(), std::string("../"),
                                    std::string("usbdisplay/")}) {
    std::ifstream file(prefix + relative);
    if (file.is_open()) {
      return std::string((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    }
  }
  return std::string();
}

/* Every file the portable build compiles, except this one.
 *
 * This file is excluded deliberately: it has to name the things it is
 * looking for, so scanning itself finds every one of them and reports the
 *list as a pile of faults. It is still listed in the makefile check
 * below, which is the part that matters. */
const char* kPortableSources[] = {
    "src/core/proto.cpp",      "src/core/macrosilicon.cpp",
    "src/render/rect.cpp",     "src/render/damage.cpp",
    "src/render/convert.cpp",  "tests/testing.cpp",
    "tests/test_rect.cpp",     "tests/test_damage.cpp",
    "tests/test_convert.cpp",  "tests/test_chip.cpp",
};

/* Headers those files include, which must be equally portable. */
const char* kPortableHeaders[] = {
    "src/core/proto.h",   "src/core/mode.h",         "src/core/link.h",
    "src/core/display_device.h", "src/core/macrosilicon.h",
    "src/render/rect.h",  "src/render/damage.h",     "src/render/convert.h",
    "tests/testing.h",
};

bool Mentions(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST(portability, no_compiler_specific_function_spellings) {
  /* The checked variants Microsoft's compiler offers. They are a reasonable
   * thing to use in code that is Windows-only, and fatal in code that is
   * not: no other compiler has them. */
  const char* forbidden[] = {
      "fopen_s(",    "strcpy_s(",  "strcat_s(",   "sprintf_s(",
      "_snprintf_s(", "wcscpy_s(", "_wfopen_s(",  "memcpy_s(",
      "localtime_s(", "_strdup(",  "_stricmp(",   "_wcsicmp(",
  };

  for (const char* source : kPortableSources) {
    const std::string text = ReadSource(source);
    CHECK_BECAUSE(!text.empty(),
                  std::string("could not read ") + source +
                      "; if it moved, update the list in this test");
    for (const char* call : forbidden) {
      CHECK_BECAUSE(!Mentions(text, call),
                    std::string(source) + " uses " + call +
                        " which only Microsoft's compiler provides. The "
                        "standard spelling works everywhere.");
    }
  }
}

TEST(portability, portable_sources_include_no_platform_headers) {
  const char* forbidden[] = {
      "<windows.h>", "<Windows.h>", "<winusb.h>", "<setupapi.h>",
      "<hidsdi.h>",  "<d3d11.h>",   "<wrl/client.h>", "<iddcx.h>",
      "<wdf.h>",
  };

  for (const char* source : kPortableSources) {
    const std::string text = ReadSource(source);
    if (text.empty()) {
      continue;
    }
    for (const char* header : forbidden) {
      CHECK_BECAUSE(!Mentions(text, header),
                    std::string(source) + " includes " + header +
                        ". Anything needing that belongs in src/core/usb.*, "
                        "src/core/open_device.cpp or src/driver, which are "
                        "not part of the portable build.");
    }
  }
  for (const char* header : kPortableHeaders) {
    const std::string text = ReadSource(header);
    if (text.empty()) {
      continue;
    }
    for (const char* forbidden_header : forbidden) {
      CHECK_BECAUSE(!Mentions(text, forbidden_header),
                    std::string(header) + " includes " + forbidden_header +
                        ", which drags a platform into every file that "
                        "includes it.");
    }
  }
}

TEST(portability, every_portable_source_is_in_the_makefile) {
  /* A file added to the Windows build but not the makefile would simply not
   * be covered by the quick checks, and nobody would notice until something
   * broke much later. */
  const std::string makefile = ReadSource("Makefile");
  CHECK_BECAUSE(!makefile.empty(), "could not read the makefile");
  if (makefile.empty()) {
    return;
  }

  for (const char* source : kPortableSources) {
    CHECK_BECAUSE(Mentions(makefile, source),
                  std::string(source) + " is missing from the makefile, so "
                      "it is not covered by the checks that run without "
                      "Windows");
  }
  CHECK_BECAUSE(Mentions(makefile, "tests/test_portable.cpp"),
                "this file must itself be in the makefile, or these checks "
                "never run where they matter most");
}

TEST(portability, files_include_what_they_use) {
  /* Microsoft's compiler supplies a great deal transitively that others do
   * not, so a file can use std::vector without including <vector> and only
   * fail elsewhere. This caught five such files the first time it was run
   * by hand; now it runs every time. */
  const struct {
    const char* symbol;
    const char* header;
  } requirements[] = {
      {"std::vector", "<vector>"},   {"std::string", "<string>"},
      {"std::to_string", "<string>"}, {"memcpy(", "<cstring>"},
      {"memcmp(", "<cstring>"},      {"memset(", "<cstring>"},
      {"printf(", "<cstdio>"},       {"std::map", "<map>"},
      {"std::ifstream", "<fstream>"},
  };

  /* What testing.h already provides to every test that includes it. */
  const char* from_testing[] = {"<cstdint>", "<cstdio>", "<cstring>",
                                "<string>", "<vector>"};

  for (const char* source : kPortableSources) {
    const std::string text = ReadSource(source);
    if (text.empty()) {
      continue;
    }
    const bool has_testing = Mentions(text, "\"testing.h\"");

    for (const auto& requirement : requirements) {
      if (!Mentions(text, requirement.symbol)) {
        continue;
      }
      if (Mentions(text, requirement.header)) {
        continue;
      }
      bool supplied = false;
      if (has_testing) {
        for (const char* provided : from_testing) {
          supplied = supplied || std::string(provided) == requirement.header;
        }
      }
      CHECK_BECAUSE(supplied,
                    std::string(source) + " uses " + requirement.symbol +
                        " but does not include " + requirement.header +
                        ". Microsoft's compiler supplies it anyway; others "
                        "do not.");
    }
  }
}
