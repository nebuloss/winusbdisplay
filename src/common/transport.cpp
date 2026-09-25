/* SPDX-License-Identifier: GPL-2.0-only */

#include "transport.h"

#include <windows.h>

#include <cstdio>

namespace ms912x {

void Transport::SetWin32Error(const char* what, unsigned long code) {
  char buffer[512];
  char* text = nullptr;
  DWORD len = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&text), 0, nullptr);
  if (len && text) {
    while (len && (text[len - 1] == '\r' || text[len - 1] == '\n')) {
      text[--len] = '\0';
    }
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%s failed: %s (0x%08lX)",
                what, text, code);
  } else {
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%s failed: 0x%08lX", what,
                code);
  }
  if (text) {
    LocalFree(text);
  }
  last_error_ = buffer;
}

}  // namespace ms912x
