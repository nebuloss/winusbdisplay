/* SPDX-License-Identifier: GPL-2.0-only */

#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace usbdisplay {
namespace {

std::mutex g_mutex;
std::string g_path;
bool g_resolved = false;

/* LOCAL SERVICE can write to the Windows temp directory; fall back to the
 * process temp path if that is locked down. */
const std::string& LogPath() {
  if (!g_resolved) {
    g_resolved = true;
    char windir[MAX_PATH] = {0};
    if (GetWindowsDirectoryA(windir, MAX_PATH)) {
      std::string candidate = std::string(windir) + "\\Temp\\usbdisplaydd.log";
      HANDLE probe = CreateFileA(candidate.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        g_path = candidate;
        return g_path;
      }
    }
    char temp[MAX_PATH] = {0};
    if (GetTempPathA(MAX_PATH, temp)) {
      g_path = std::string(temp) + "usbdisplaydd.log";
    }
  }
  return g_path;
}

void Write(const char* text) {
  const std::string& path = LogPath();
  if (path.empty()) {
    return;
  }
  HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
  CloseHandle(file);
}

}  // namespace

/* Keeps the previous session's log, and starts a fresh one.
 *
 * The previous copy matters more than it looks. This runs on every driver
 * load, so without it the record of the last session is destroyed by the
 * next one, and the sessions worth reading are exactly the ones followed
 * by a restart: a driver that failed at boot leaves evidence that the
 * first reinstall afterwards erases. That cost a whole debugging session,
 * during which the boot fault was invisible because the only log on disk
 * described a later, healthy run.
 *
 * One generation is enough. The fault is always in the run immediately
 * before the restart. */
void LogReset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string& path = LogPath();
  if (path.empty()) {
    return;
  }

  const std::string previous = path + ".prev";
  DeleteFileA(previous.c_str());
  MoveFileA(path.c_str(), previous.c_str());

  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    CloseHandle(file);
  }
}

void Log(const char* format, ...) {
  std::lock_guard<std::mutex> lock(g_mutex);

  SYSTEMTIME now;
  GetLocalTime(&now);

  char body[1024];
  va_list args;
  va_start(args, format);
  _vsnprintf_s(body, sizeof(body), _TRUNCATE, format, args);
  va_end(args);

  char line[1200];
  _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u [%lu] %s\r\n",
              now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
              GetCurrentProcessId(), body);
  Write(line);
}

}  // namespace usbdisplay
