/* SPDX-License-Identifier: GPL-2.0-only
 *
 * msbright: system tray brightness control for the MacroSilicon USB display.
 *
 * Windows does not route DDC/CI to indirect displays, so tools like Twinkle
 * Tray cannot reach this monitor (see docs/protocol-notes.md). Brightness is
 * instead published as a registry value that the display driver polls and
 * applies during colour conversion, which dims the panel for real. This app
 * is just a convenient front end for that value.
 *
 * It writes to HKLM rather than HKCU because the driver runs as LOCAL SERVICE
 * and cannot see a user's hive. The installer widens the ACL on that one key
 * so this app does not need to run elevated.
 */

#include <windows.h>

#include <shellapi.h>

#include <cstdio>

namespace {

constexpr wchar_t kSettingsKey[] = L"SOFTWARE\\winusbdisplay";
constexpr wchar_t kWindowClass[] = L"MsBrightTrayWindow";
constexpr wchar_t kMutexName[] = L"Local\\msbright-single-instance";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT kTrayId = 1;

/* Menu command ids. Brightness presets are offset by their value so one
 * handler covers them all. */
constexpr UINT kCmdBrightnessBase = 1000;  /* + percent */
constexpr UINT kCmdContrastBase = 2000;    /* + percent */
constexpr UINT kCmdExit = 3000;
constexpr UINT kCmdReset = 3001;

const int kBrightnessSteps[] = {100, 90, 75, 60, 50, 40, 30, 20, 10};
const int kContrastSteps[] = {70, 60, 50, 40, 30};

constexpr int kDefaultBrightness = 100;
constexpr int kDefaultContrast = 50;

NOTIFYICONDATAW g_tray = {};
HWND g_window = nullptr;
int g_brightness = kDefaultBrightness;
int g_contrast = kDefaultContrast;

int Clamp(int value, int low, int high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

int ReadSetting(const wchar_t* name, int fallback) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return fallback;
  }
  DWORD value = 0;
  DWORD size = sizeof(value);
  DWORD type = 0;
  int result = fallback;
  if (RegQueryValueExW(key, name, nullptr, &type,
                       reinterpret_cast<LPBYTE>(&value), &size) ==
          ERROR_SUCCESS &&
      type == REG_DWORD) {
    result = Clamp(static_cast<int>(value), 0, 100);
  }
  RegCloseKey(key);
  return result;
}

/* Returns false when the key is not writable, which normally means the
 * installer has not run and widened its ACL. */
bool WriteSetting(const wchar_t* name, int value) {
  HKEY key = nullptr;
  DWORD disposition = 0;
  LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr,
                                   REG_OPTION_NON_VOLATILE,
                                   KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
                                   &key, &disposition);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  DWORD data = static_cast<DWORD>(Clamp(value, 0, 100));
  status = RegSetValueExW(key, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&data), sizeof(data));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

void UpdateTooltip() {
  _snwprintf_s(g_tray.szTip, _TRUNCATE,
               L"USB display\nBrightness %d%%  Contrast %d%%", g_brightness,
               g_contrast);
  Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void ShowBalloon(const wchar_t* title, const wchar_t* text) {
  NOTIFYICONDATAW balloon = g_tray;
  balloon.uFlags = NIF_INFO;
  balloon.dwInfoFlags = NIIF_WARNING;
  wcscpy_s(balloon.szInfoTitle, title);
  wcscpy_s(balloon.szInfo, text);
  Shell_NotifyIconW(NIM_MODIFY, &balloon);
}

void ApplyBrightness(int value) {
  g_brightness = Clamp(value, 0, 100);
  if (!WriteSetting(L"Brightness", g_brightness)) {
    ShowBalloon(L"Cannot save brightness",
                L"The settings key is not writable. Run the installer, or "
                L"start this app as administrator.");
    return;
  }
  UpdateTooltip();
}

void ApplyContrast(int value) {
  g_contrast = Clamp(value, 0, 100);
  if (!WriteSetting(L"Contrast", g_contrast)) {
    ShowBalloon(L"Cannot save contrast",
                L"The settings key is not writable. Run the installer, or "
                L"start this app as administrator.");
    return;
  }
  UpdateTooltip();
}

void ShowMenu() {
  HMENU menu = CreatePopupMenu();
  if (!menu) {
    return;
  }

  HMENU brightness = CreatePopupMenu();
  for (int step : kBrightnessSteps) {
    wchar_t label[32];
    _snwprintf_s(label, _TRUNCATE, L"%d%%", step);
    /* Radio check the nearest step so the menu reflects reality even when the
     * value was set from the command line. */
    const bool current = g_brightness == step;
    AppendMenuW(brightness, MF_STRING | (current ? MF_CHECKED : 0),
                kCmdBrightnessBase + step, label);
  }

  HMENU contrast = CreatePopupMenu();
  for (int step : kContrastSteps) {
    wchar_t label[32];
    _snwprintf_s(label, _TRUNCATE, L"%d%%", step);
    AppendMenuW(contrast, MF_STRING | (g_contrast == step ? MF_CHECKED : 0),
                kCmdContrastBase + step, label);
  }

  wchar_t header[64];
  _snwprintf_s(header, _TRUNCATE, L"USB display  -  %d%%", g_brightness);
  AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, header);
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(brightness),
              L"Brightness");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(contrast),
              L"Contrast");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCmdReset, L"Reset to default");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

  POINT cursor;
  GetCursorPos(&cursor);
  /* Required so the menu dismisses when the user clicks elsewhere. */
  SetForegroundWindow(g_window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0,
                 g_window, nullptr);
  PostMessageW(g_window, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case WM_TRAYICON:
      if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_LBUTTONUP) {
        ShowMenu();
      }
      return 0;

    case WM_COMMAND: {
      const UINT id = LOWORD(wparam);
      if (id == kCmdExit) {
        DestroyWindow(window);
      } else if (id == kCmdReset) {
        ApplyBrightness(kDefaultBrightness);
        ApplyContrast(kDefaultContrast);
      } else if (id >= kCmdContrastBase) {
        ApplyContrast(static_cast<int>(id - kCmdContrastBase));
      } else if (id >= kCmdBrightnessBase) {
        ApplyBrightness(static_cast<int>(id - kCmdBrightnessBase));
      }
      return 0;
    }

    case WM_DESTROY:
      Shell_NotifyIconW(NIM_DELETE, &g_tray);
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

HICON LoadTrayIcon() {
  /* A display glyph from the shell icon set, so no icon resource is needed.
   * Falls back to the generic application icon. */
  HICON icon = nullptr;
  ExtractIconExW(L"shell32.dll", 15, nullptr, &icon, 1);
  if (!icon) {
    icon = LoadIconW(nullptr, IDI_APPLICATION);
  }
  return icon;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
  /* Command line use, so the same binary can be scripted:
   *   msbright.exe 60        set brightness
   *   msbright.exe 60 40     set brightness and contrast */
  if (command_line && *command_line) {
    int brightness = 0;
    int contrast = -1;
    const int parsed = swscanf_s(command_line, L"%d %d", &brightness,
                                 &contrast);
    if (parsed >= 1) {
      bool ok = WriteSetting(L"Brightness", brightness);
      if (parsed >= 2 && contrast >= 0) {
        ok = WriteSetting(L"Contrast", contrast) && ok;
      }
      return ok ? 0 : 1;
    }
    return 2;
  }

  HANDLE single = CreateMutexW(nullptr, TRUE, kMutexName);
  if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;
  }

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&wc)) {
    return 1;
  }

  /* Message-only window: the app has no UI beyond the tray icon. */
  g_window = CreateWindowExW(0, kWindowClass, L"msbright", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, instance, nullptr);
  if (!g_window) {
    return 1;
  }

  g_brightness = ReadSetting(L"Brightness", kDefaultBrightness);
  g_contrast = ReadSetting(L"Contrast", kDefaultContrast);

  g_tray.cbSize = sizeof(g_tray);
  g_tray.hWnd = g_window;
  g_tray.uID = kTrayId;
  g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_tray.uCallbackMessage = WM_TRAYICON;
  g_tray.hIcon = LoadTrayIcon();
  UpdateTooltip();

  if (!Shell_NotifyIconW(NIM_ADD, &g_tray)) {
    return 1;
  }
  UpdateTooltip();

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (single) {
    CloseHandle(single);
  }
  return 0;
}
