/* SPDX-License-Identifier: GPL-2.0-only
 *
 * msbright: system tray brightness control for the MacroSilicon USB display.
 *
 * Windows does not route DDC/CI to indirect displays, so tools like Twinkle
 * Tray cannot reach this monitor (see docs/protocol-notes.md). Brightness is
 * published as a registry value that the display driver polls and applies
 * during colour conversion, which dims the panel for real. This app is a
 * front end for that value.
 *
 * The UI is a borderless flyout with custom drawn sliders rather than a
 * classic menu, so it matches the look of the Windows volume and brightness
 * popups. It follows the system light/dark setting and accent colour.
 *
 * Settings live in HKLM because the driver runs as LOCAL SERVICE and cannot
 * read a user hive. The installer widens the ACL on that one key so this app
 * does not need to run elevated.
 */

#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>

#include <objidl.h>

#include <gdiplus.h>

#include <cstdio>

namespace {

constexpr wchar_t kSettingsKey[] = L"SOFTWARE\\winusbdisplay";
constexpr wchar_t kWindowClass[] = L"MsBrightFlyout";
constexpr wchar_t kMutexName[] = L"Local\\msbright-single-instance";
constexpr wchar_t kRunKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"msbright";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT kTrayId = 1;

constexpr UINT kCmdExit = 3000;
constexpr UINT kCmdReset = 3001;
constexpr UINT kCmdAutostart = 3002;

constexpr int kDefaultBrightness = 100;
constexpr int kDefaultContrast = 50;

/* Flyout geometry, in unscaled pixels; everything is scaled by the DPI of the
 * monitor the flyout appears on. */
constexpr int kBaseWidth = 300;
constexpr int kBaseHeight = 168;
constexpr int kBasePadding = 18;
constexpr int kBaseTrack = 5;
constexpr int kBaseThumb = 9;

struct Slider {
  const wchar_t* label;
  int value;
  int minimum;
  RECT track;  /* hit area, generous vertically so it is easy to grab */
};

Slider g_sliders[2] = {
    {L"Brightness", kDefaultBrightness, 10, {}},
    {L"Contrast", kDefaultContrast, 0, {}},
};
constexpr int kBrightnessIndex = 0;
constexpr int kContrastIndex = 1;

NOTIFYICONDATAW g_tray = {};
HWND g_window = nullptr;
int g_dragging = -1;
int g_dpi = 96;
ULONG_PTR g_gdiplus_token = 0;

int Scale(int value) { return MulDiv(value, g_dpi, 96); }

int Clamp(int value, int low, int high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

/* ---------------------------------------------------------------- settings */

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

bool WriteSetting(const wchar_t* name, int value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr,
                      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_WOW64_64KEY,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  DWORD data = static_cast<DWORD>(Clamp(value, 0, 100));
  const LSTATUS status =
      RegSetValueExW(key, name, 0, REG_DWORD,
                     reinterpret_cast<const BYTE*>(&data), sizeof(data));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool IsAutostartEnabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  const bool present =
      RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) ==
      ERROR_SUCCESS;
  RegCloseKey(key);
  return present;
}

void SetAutostart(bool enable) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr,
                      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  if (enable) {
    wchar_t path[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
      wchar_t quoted[MAX_PATH + 2];
      _snwprintf_s(quoted, _TRUNCATE, L"\"%s\"", path);
      RegSetValueExW(
          key, kRunValue, 0, REG_SZ,
          reinterpret_cast<const BYTE*>(quoted),
          static_cast<DWORD>((wcslen(quoted) + 1) * sizeof(wchar_t)));
    }
  } else {
    RegDeleteValueW(key, kRunValue);
  }
  RegCloseKey(key);
}

/* ------------------------------------------------------------------- theme */

bool IsDarkMode() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                    L"Personalize",
                    0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD value = 1;
  DWORD size = sizeof(value);
  DWORD type = 0;
  RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                   reinterpret_cast<LPBYTE>(&value), &size);
  RegCloseKey(key);
  return value == 0;
}

Gdiplus::Color AccentColour(bool dark) {
  DWORD colour = 0;
  BOOL opaque = FALSE;
  if (SUCCEEDED(DwmGetColorizationColor(&colour, &opaque))) {
    /* DWM gives 0xAARRGGBB. Force full alpha, the flyout is already opaque. */
    BYTE r = static_cast<BYTE>((colour >> 16) & 0xFF);
    BYTE g = static_cast<BYTE>((colour >> 8) & 0xFF);
    BYTE b = static_cast<BYTE>(colour & 0xFF);
    /* Very dark accents vanish against a dark flyout, so floor the luminance. */
    if (dark && (r + g + b) < 180) {
      r = static_cast<BYTE>(min(255, r + 70));
      g = static_cast<BYTE>(min(255, g + 70));
      b = static_cast<BYTE>(min(255, b + 70));
    }
    return Gdiplus::Color(255, r, g, b);
  }
  return dark ? Gdiplus::Color(255, 96, 205, 255)
              : Gdiplus::Color(255, 0, 103, 192);
}

/* ------------------------------------------------------------------ layout */

void LayoutSliders(const RECT& client) {
  const int padding = Scale(kBasePadding);
  const int row_height = Scale(48);
  const int first_top = Scale(46);

  for (int i = 0; i < 2; ++i) {
    RECT& track = g_sliders[i].track;
    track.left = padding;
    track.right = client.right - padding;
    const int centre = first_top + i * row_height + Scale(14);
    /* Tall hit area: the visible track is thin but grabbing it should not
     * demand pixel accuracy. */
    track.top = centre - Scale(12);
    track.bottom = centre + Scale(12);
  }
}

int ValueFromPoint(const Slider& slider, int x) {
  const int thumb = Scale(kBaseThumb);
  const int left = slider.track.left + thumb;
  const int right = slider.track.right - thumb;
  if (right <= left) {
    return slider.value;
  }
  const int span = right - left;
  const int offset = Clamp(x - left, 0, span);
  const int range = 100 - slider.minimum;
  return slider.minimum + (offset * range + span / 2) / span;
}

/* --------------------------------------------------------------- rendering */

void DrawFlyout(HDC target, const RECT& client) {
  const bool dark = IsDarkMode();
  const Gdiplus::Color background = dark ? Gdiplus::Color(255, 32, 32, 32)
                                         : Gdiplus::Color(255, 249, 249, 249);
  const Gdiplus::Color text = dark ? Gdiplus::Color(255, 255, 255, 255)
                                   : Gdiplus::Color(255, 26, 26, 26);
  const Gdiplus::Color muted = dark ? Gdiplus::Color(255, 160, 160, 160)
                                    : Gdiplus::Color(255, 100, 100, 100);
  const Gdiplus::Color rail = dark ? Gdiplus::Color(255, 78, 78, 78)
                                   : Gdiplus::Color(255, 206, 206, 206);
  const Gdiplus::Color accent = AccentColour(dark);

  /* Draw into a memory bitmap so the flyout never flickers while dragging. */
  const int width = client.right;
  const int height = client.bottom;
  HDC memory = CreateCompatibleDC(target);
  HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
  HGDIOBJ previous = SelectObject(memory, bitmap);

  {
    Gdiplus::Graphics graphics(memory);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    graphics.Clear(background);

    Gdiplus::FontFamily family(L"Segoe UI Variable Display");
    if (!family.IsAvailable()) {
      family = Gdiplus::FontFamily(L"Segoe UI");
    }
    Gdiplus::Font title_font(&family, 11.5f, Gdiplus::FontStyleSemiBold,
                             Gdiplus::UnitPoint);
    Gdiplus::Font label_font(&family, 9.5f, Gdiplus::FontStyleRegular,
                             Gdiplus::UnitPoint);
    Gdiplus::SolidBrush text_brush(text);
    Gdiplus::SolidBrush muted_brush(muted);

    const Gdiplus::REAL padding = static_cast<Gdiplus::REAL>(Scale(kBasePadding));
    graphics.DrawString(L"USB display", -1, &title_font,
                        Gdiplus::PointF(padding, static_cast<Gdiplus::REAL>(
                                                     Scale(14))),
                        &text_brush);

    Gdiplus::StringFormat right_aligned;
    right_aligned.SetAlignment(Gdiplus::StringAlignmentFar);

    const int thumb = Scale(kBaseThumb);
    const int rail_height = Scale(kBaseTrack);

    for (const Slider& slider : g_sliders) {
      const int centre = (slider.track.top + slider.track.bottom) / 2;
      const Gdiplus::REAL label_y =
          static_cast<Gdiplus::REAL>(centre - Scale(30));

      graphics.DrawString(slider.label, -1, &label_font,
                          Gdiplus::PointF(padding, label_y), &muted_brush);

      wchar_t percent[16];
      _snwprintf_s(percent, _TRUNCATE, L"%d%%", slider.value);
      Gdiplus::RectF value_box(
          padding, label_y,
          static_cast<Gdiplus::REAL>(slider.track.right - slider.track.left),
          static_cast<Gdiplus::REAL>(Scale(18)));
      graphics.DrawString(percent, -1, &label_font, value_box, &right_aligned,
                          &muted_brush);

      const int left = slider.track.left + thumb;
      const int right = slider.track.right - thumb;
      const int span = right - left;
      const int range = 100 - slider.minimum;
      const int filled =
          left + (range > 0 ? (slider.value - slider.minimum) * span / range
                            : 0);

      /* Unfilled rail, then the filled portion, then the thumb on top. */
      Gdiplus::SolidBrush rail_brush(rail);
      graphics.FillRectangle(
          &rail_brush, Gdiplus::Rect(left, centre - rail_height / 2, span,
                                     rail_height));

      Gdiplus::SolidBrush accent_brush(accent);
      graphics.FillRectangle(
          &accent_brush, Gdiplus::Rect(left, centre - rail_height / 2,
                                       filled - left, rail_height));

      /* A ring rather than a disc, matching the Windows 11 slider. */
      graphics.FillEllipse(&accent_brush,
                           Gdiplus::Rect(filled - thumb, centre - thumb,
                                         thumb * 2, thumb * 2));
      Gdiplus::SolidBrush centre_brush(background);
      const int inner = thumb - Scale(4);
      graphics.FillEllipse(&centre_brush,
                           Gdiplus::Rect(filled - inner, centre - inner,
                                         inner * 2, inner * 2));
    }
  }

  BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
  SelectObject(memory, previous);
  DeleteObject(bitmap);
  DeleteDC(memory);
}

/* ------------------------------------------------------------------ flyout */

void UpdateTooltip() {
  _snwprintf_s(g_tray.szTip, _TRUNCATE, L"USB display\nBrightness %d%%",
               g_sliders[kBrightnessIndex].value);
  Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void CommitSlider(int index) {
  const wchar_t* name = index == kBrightnessIndex ? L"Brightness" : L"Contrast";
  WriteSetting(name, g_sliders[index].value);
  UpdateTooltip();
}

void PositionFlyout() {
  NOTIFYICONIDENTIFIER id = {};
  id.cbSize = sizeof(id);
  id.hWnd = g_window;
  id.uID = kTrayId;

  RECT icon = {};
  POINT anchor;
  if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &icon))) {
    anchor.x = (icon.left + icon.right) / 2;
    anchor.y = icon.top;
  } else {
    GetCursorPos(&anchor);
  }

  HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(monitor, &info);

  UINT dpi_x = 96;
  UINT dpi_y = 96;
  if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
    g_dpi = static_cast<int>(dpi_x);
  }

  const int width = Scale(kBaseWidth);
  const int height = Scale(kBaseHeight);
  const int margin = Scale(12);

  int x = anchor.x - width / 2;
  int y = anchor.y - height - margin;

  /* Keep it inside the work area, so it behaves on a taskbar that is not at
   * the bottom of the screen. */
  if (x + width > info.rcWork.right) {
    x = info.rcWork.right - width - margin;
  }
  if (x < info.rcWork.left) {
    x = info.rcWork.left + margin;
  }
  if (y < info.rcWork.top) {
    y = anchor.y + margin;
  }

  SetWindowPos(g_window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);

  RECT client = {0, 0, width, height};
  LayoutSliders(client);
}

void ShowFlyout() {
  for (int i = 0; i < 2; ++i) {
    const wchar_t* name = i == kBrightnessIndex ? L"Brightness" : L"Contrast";
    g_sliders[i].value = ReadSetting(
        name, i == kBrightnessIndex ? kDefaultBrightness : kDefaultContrast);
  }
  PositionFlyout();
  ShowWindow(g_window, SW_SHOW);
  SetForegroundWindow(g_window);
  InvalidateRect(g_window, nullptr, FALSE);
}

void HideFlyout() {
  if (g_dragging >= 0) {
    ReleaseCapture();
    g_dragging = -1;
  }
  ShowWindow(g_window, SW_HIDE);
}

void ShowContextMenu() {
  HMENU menu = CreatePopupMenu();
  if (!menu) {
    return;
  }
  AppendMenuW(menu, MF_STRING, kCmdReset, L"Reset to default");
  AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
              kCmdAutostart, L"Start with Windows");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

  POINT cursor;
  GetCursorPos(&cursor);
  SetForegroundWindow(g_window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_window,
                 nullptr);
  PostMessageW(g_window, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case WM_TRAYICON:
      if (LOWORD(lparam) == WM_LBUTTONUP) {
        if (IsWindowVisible(window)) {
          HideFlyout();
        } else {
          ShowFlyout();
        }
      } else if (LOWORD(lparam) == WM_RBUTTONUP) {
        HideFlyout();
        ShowContextMenu();
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT paint;
      HDC dc = BeginPaint(window, &paint);
      RECT client;
      GetClientRect(window, &client);
      DrawFlyout(dc, client);
      EndPaint(window, &paint);
      return 0;
    }

    case WM_ERASEBKGND:
      /* DrawFlyout paints every pixel from a back buffer. */
      return 1;

    case WM_LBUTTONDOWN: {
      const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      for (int i = 0; i < 2; ++i) {
        if (PtInRect(&g_sliders[i].track, point)) {
          g_dragging = i;
          SetCapture(window);
          g_sliders[i].value = ValueFromPoint(g_sliders[i], point.x);
          CommitSlider(i);
          InvalidateRect(window, nullptr, FALSE);
          break;
        }
      }
      return 0;
    }

    case WM_MOUSEMOVE:
      if (g_dragging >= 0) {
        const int x = GET_X_LPARAM(lparam);
        const int value = ValueFromPoint(g_sliders[g_dragging], x);
        if (value != g_sliders[g_dragging].value) {
          g_sliders[g_dragging].value = value;
          CommitSlider(g_dragging);
          InvalidateRect(window, nullptr, FALSE);
        }
      }
      return 0;

    case WM_LBUTTONUP:
      if (g_dragging >= 0) {
        ReleaseCapture();
        g_dragging = -1;
      }
      return 0;

    case WM_MOUSEWHEEL: {
      /* Wheel adjusts brightness in five point steps. */
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 5 : -5;
      Slider& slider = g_sliders[kBrightnessIndex];
      slider.value = Clamp(slider.value + delta, slider.minimum, 100);
      CommitSlider(kBrightnessIndex);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }

    case WM_ACTIVATE:
      /* Dismiss when focus moves elsewhere, like the system flyouts. */
      if (LOWORD(wparam) == WA_INACTIVE) {
        HideFlyout();
      }
      return 0;

    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        HideFlyout();
      }
      return 0;

    case WM_COMMAND: {
      const UINT id = LOWORD(wparam);
      if (id == kCmdExit) {
        DestroyWindow(window);
      } else if (id == kCmdReset) {
        g_sliders[kBrightnessIndex].value = kDefaultBrightness;
        g_sliders[kContrastIndex].value = kDefaultContrast;
        CommitSlider(kBrightnessIndex);
        CommitSlider(kContrastIndex);
      } else if (id == kCmdAutostart) {
        SetAutostart(!IsAutostartEnabled());
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
  HICON icon = nullptr;
  ExtractIconExW(L"shell32.dll", 15, nullptr, &icon, 1);
  if (!icon) {
    icon = LoadIconW(nullptr, IDI_APPLICATION);
  }
  return icon;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
  /* Scriptable form, so the same binary works from the command line:
   *   msbright.exe 60        set brightness
   *   msbright.exe 60 40     set brightness and contrast */
  if (command_line && *command_line) {
    int brightness = 0;
    int contrast = -1;
    const int parsed =
        swscanf_s(command_line, L"%d %d", &brightness, &contrast);
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

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  Gdiplus::GdiplusStartupInput startup;
  if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &startup, nullptr) !=
      Gdiplus::Ok) {
    return 1;
  }

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = kWindowClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (!RegisterClassExW(&wc)) {
    return 1;
  }

  g_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass,
                             L"USB display", WS_POPUP, 0, 0, kBaseWidth,
                             kBaseHeight, nullptr, nullptr, instance, nullptr);
  if (!g_window) {
    return 1;
  }

  /* Rounded corners and a drop shadow on Windows 11; both are ignored on
   * earlier releases, which simply get square corners. */
  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(g_window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                        sizeof(corner));
  const BOOL dark = IsDarkMode();
  DwmSetWindowAttribute(g_window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));

  g_tray.cbSize = sizeof(g_tray);
  g_tray.hWnd = g_window;
  g_tray.uID = kTrayId;
  g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_tray.uCallbackMessage = WM_TRAYICON;
  g_tray.hIcon = LoadTrayIcon();
  if (!Shell_NotifyIconW(NIM_ADD, &g_tray)) {
    return 1;
  }
  g_sliders[kBrightnessIndex].value =
      ReadSetting(L"Brightness", kDefaultBrightness);
  g_sliders[kContrastIndex].value = ReadSetting(L"Contrast", kDefaultContrast);
  UpdateTooltip();

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  Gdiplus::GdiplusShutdown(g_gdiplus_token);
  if (single) {
    CloseHandle(single);
  }
  return 0;
}
