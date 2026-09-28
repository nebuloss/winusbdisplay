/* SPDX-License-Identifier: GPL-2.0-only */

#include "settings.h"

#include <windows.h>

namespace usbdisplay {
namespace {

const wchar_t kSettingsKey[] = L"SOFTWARE\\usbdisplay";

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  DWORD type = 0;
  if (RegQueryValueExW(key, name, nullptr, &type,
                       reinterpret_cast<LPBYTE>(&value), &size) ==
          ERROR_SUCCESS &&
      type == REG_DWORD) {
    return value;
  }
  return fallback;
}

int Clamp(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

}  // namespace

Settings ReadSettings() {
  Settings settings;

  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, KEY_QUERY_VALUE,
                    &key) != ERROR_SUCCESS) {
    return settings;
  }

  settings.brightness =
      Clamp(static_cast<int>(ReadDword(key, L"Brightness", 100)), 0, 100);
  settings.contrast =
      Clamp(static_cast<int>(ReadDword(key, L"Contrast", 50)), 0, 100);
  settings.idle_refresh = ReadDword(key, L"IdleRefresh", 1) != 0;

  /* Stored in pixels rather than as a fraction, so it can be compared
   * directly against a rectangle's area without any unit confusion. */
  settings.gpu_threshold_pixels = static_cast<int64_t>(
      ReadDword(key, L"GpuThresholdPixels",
                static_cast<DWORD>(kGpuThresholdPixels)));

  RegCloseKey(key);
  return settings;
}

}  // namespace usbdisplay
