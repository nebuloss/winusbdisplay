/* SPDX-License-Identifier: GPL-2.0-only
 *
 * A brightness control in the notification area, for every attached monitor.
 *
 * It exists because the obvious alternatives do not cover this hardware. The
 * usual third-party tools reach a monitor over the display cable, which the
 * USB adapter this project drives has no way to answer, and the ones that
 * offer a fallback either do not ship it yet or leave it switched off. So
 * the driver gained the ability to be dimmed and this gives it a slider,
 * alongside sliders for the ordinary monitors so one tool covers the lot.
 *
 * Left click for the sliders, right click for the menu. Plain Win32 and
 * GDI+, no dependencies, so it is a single executable that can simply be
 * run.
 */

#include <windows.h>

#include <objidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <gdiplus.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "control.h"

using namespace usbdisplay;

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr int kMenuExit = 100;
constexpr int kMenuAutostart = 101;
constexpr int kMenuRefresh = 102;
constexpr int kMenuResetAll = 103;

/* Re-check which monitors are attached on this cadence. Cheap, and it is how
 * the USB display appears in the list moments after being plugged in without
 * the user having to ask. */
constexpr UINT kRescanTimer = 1;
constexpr UINT kRescanMs = 4000;

/* Settings are written this long after the slider stops moving. Dragging a
 * slider produces a change per pixel and none of those are worth a registry
 * write; the monitor still follows immediately. */
constexpr UINT kSaveTimer = 2;
constexpr UINT kSaveDelayMs = 600;

/* Watches for the USB display arriving without becoming a screen, early in
 * the session only. See EnsureDisplayIsOnTheDesktop. */
constexpr UINT kExtendTimer = 3;
constexpr UINT kExtendMs = 3000;
constexpr unsigned kExtendAttempts = 40;

const wchar_t kWindowClass[] = L"UsbDisplayBrightnessTray";
const wchar_t kSettingsKey[] = L"SOFTWARE\\usbdisplay\\Brightness";
const wchar_t kAutostartKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kAutostartValue[] = L"usbdisplay brightness";

/* Layout, in pixels before scaling for the display. */
constexpr int kRowHeight = 58;
constexpr int kPadding = 14;
constexpr int kTrackHeight = 4;
constexpr int kKnobRadius = 7;
constexpr int kFlyoutWidth = 300;

HWND g_window = nullptr;
HINSTANCE g_instance = nullptr;
NOTIFYICONDATAW g_tray = {};
ULONG_PTR g_gdiplus = 0;
std::vector<Monitor> g_monitors;
int g_dragging = -1;
int g_dpi = 96;
unsigned long long g_hidden_at_ms = 0;
unsigned g_extend_attempts = 0;

int Scale(int value) { return MulDiv(value, g_dpi, 96); }

int Clamp(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

/* ---- saved settings ----------------------------------------------------- */

/* Keyed on the monitor's own identity rather than its position or device
 * name, both of which Windows reassigns freely. */
int LoadBrightness(const std::wstring& identity, int fallback) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE,
                    &key) != ERROR_SUCCESS) {
    return fallback;
  }
  DWORD value = 0, size = sizeof(value), type = 0;
  const bool ok = RegQueryValueExW(key, identity.c_str(), nullptr, &type,
                                   reinterpret_cast<LPBYTE>(&value), &size) ==
                      ERROR_SUCCESS &&
                  type == REG_DWORD;
  RegCloseKey(key);
  return ok ? Clamp(static_cast<int>(value), 0, 100) : fallback;
}

void SaveBrightness(const std::wstring& identity, int percent) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return;
  }
  const DWORD value = static_cast<DWORD>(percent);
  RegSetValueExW(key, identity.c_str(), 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
}

/* Whether the note below has already been shown. */
bool WasAnnounced() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE,
                    &key) != ERROR_SUCCESS) {
    return false;
  }
  const bool seen = RegQueryValueExW(key, L"Announced", nullptr, nullptr,
                                     nullptr, nullptr) == ERROR_SUCCESS;
  RegCloseKey(key);
  return seen;
}

void MarkAnnounced() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return;
  }
  const DWORD one = 1;
  RegSetValueExW(key, L"Announced", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&one), sizeof(one));
  RegCloseKey(key);
}

bool IsAutostartEnabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kAutostartKey, 0, KEY_QUERY_VALUE,
                    &key) != ERROR_SUCCESS) {
    return false;
  }
  const bool present =
      RegQueryValueExW(key, kAutostartValue, nullptr, nullptr, nullptr,
                       nullptr) == ERROR_SUCCESS;
  RegCloseKey(key);
  return present;
}

void SetAutostart(bool enable) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kAutostartKey, 0, KEY_SET_VALUE,
                    &key) != ERROR_SUCCESS) {
    return;
  }
  if (enable) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
    RegSetValueExW(key, kAutostartValue, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(quoted.c_str()),
                   static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, kAutostartValue);
  }
  RegCloseKey(key);
}

/* ---- appearance --------------------------------------------------------- */

bool IsDarkMode() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                    L"Personalize",
                    0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD value = 1, size = sizeof(value), type = 0;
  RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                   reinterpret_cast<LPBYTE>(&value), &size);
  RegCloseKey(key);
  return value == 0;
}

Gdiplus::Color AccentColour(bool dark) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\DWM", 0,
                    KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
    DWORD value = 0, size = sizeof(value), type = 0;
    const bool ok = RegQueryValueExW(key, L"AccentColor", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&value),
                                     &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (ok) {
      /* Stored as alpha, blue, green, red. */
      return Gdiplus::Color(255, static_cast<BYTE>(value & 0xFF),
                            static_cast<BYTE>((value >> 8) & 0xFF),
                            static_cast<BYTE>((value >> 16) & 0xFF));
    }
  }
  return dark ? Gdiplus::Color(255, 96, 165, 250)
              : Gdiplus::Color(255, 0, 120, 212);
}

/* ---- layout ------------------------------------------------------------- */

int FlyoutHeight() {
  const int rows = g_monitors.empty() ? 1 : static_cast<int>(g_monitors.size());
  return Scale(kPadding * 2 + rows * kRowHeight);
}

RECT TrackRect(int index) {
  RECT rect;
  rect.left = Scale(kPadding);
  rect.right = Scale(kFlyoutWidth - kPadding);
  const int top = Scale(kPadding + index * kRowHeight + 38);
  rect.top = top;
  rect.bottom = top + Scale(kTrackHeight);
  return rect;
}

int RowFromPoint(int y) {
  const int row = (y - Scale(kPadding)) / Scale(kRowHeight);
  return (row >= 0 && row < static_cast<int>(g_monitors.size())) ? row : -1;
}

/* Where the pointer is along the track, 0 to 100.
 *
 * Rounded to nearest rather than truncated, and inset by the knob radius at
 * both ends, so the knob sits under the pointer instead of trailing it and
 * both extremes are actually reachable. */
int ValueFromPoint(int index, int x) {
  const RECT track = TrackRect(index);
  const int knob = Scale(kKnobRadius);
  const int left = track.left + knob;
  const int span = (track.right - knob) - left;
  if (span <= 0) {
    return 100;
  }
  return Clamp(((x - left) * 100 + span / 2) / span, 0, 100);
}

/* ---- drawing ------------------------------------------------------------ */

void DrawFlyout(HDC target, int width, int height) {
  const bool dark = IsDarkMode();

  /* Composed into a memory bitmap and copied over in one go, so dragging a
   * slider never flickers. */
  HDC memory = CreateCompatibleDC(target);
  HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
  HGDIOBJ previous = SelectObject(memory, bitmap);

  Gdiplus::Graphics canvas(memory);
  canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  canvas.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

  const Gdiplus::Color background =
      dark ? Gdiplus::Color(255, 32, 32, 32) : Gdiplus::Color(255, 249, 249, 249);
  const Gdiplus::Color text =
      dark ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 16, 16, 16);
  const Gdiplus::Color faint =
      dark ? Gdiplus::Color(255, 160, 160, 160) : Gdiplus::Color(255, 104, 104, 104);
  const Gdiplus::Color trough =
      dark ? Gdiplus::Color(255, 72, 72, 72) : Gdiplus::Color(255, 210, 210, 210);
  const Gdiplus::Color accent = AccentColour(dark);

  canvas.Clear(background);

  Gdiplus::FontFamily family(L"Segoe UI");
  Gdiplus::Font title(&family, static_cast<Gdiplus::REAL>(Scale(12)),
                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
  Gdiplus::Font subtitle(&family, static_cast<Gdiplus::REAL>(Scale(10)),
                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
  Gdiplus::SolidBrush text_brush(text);
  Gdiplus::SolidBrush faint_brush(faint);
  Gdiplus::SolidBrush trough_brush(trough);
  Gdiplus::SolidBrush accent_brush(accent);

  if (g_monitors.empty()) {
    Gdiplus::PointF where(static_cast<Gdiplus::REAL>(Scale(kPadding)),
                          static_cast<Gdiplus::REAL>(Scale(kPadding + 10)));
    canvas.DrawString(L"No monitors found", -1, &title, where, &faint_brush);
  }

  for (size_t i = 0; i < g_monitors.size(); ++i) {
    const Monitor& monitor = g_monitors[i];
    const int top = Scale(kPadding + static_cast<int>(i) * kRowHeight);

    Gdiplus::PointF name_at(static_cast<Gdiplus::REAL>(Scale(kPadding)),
                            static_cast<Gdiplus::REAL>(top));
    canvas.DrawString(monitor.name.c_str(), -1, &title, name_at,
                      monitor.control ? &text_brush : &faint_brush);

    /* The value on the right, or why there is no value. */
    std::wstring detail;
    if (monitor.control) {
      detail = std::to_wstring(monitor.brightness) + L"%";
    } else {
      detail = L"not adjustable";
    }
    Gdiplus::RectF right(
        static_cast<Gdiplus::REAL>(Scale(kFlyoutWidth - kPadding - 90)),
        static_cast<Gdiplus::REAL>(top),
        static_cast<Gdiplus::REAL>(Scale(90)),
        static_cast<Gdiplus::REAL>(Scale(18)));
    Gdiplus::StringFormat align;
    align.SetAlignment(Gdiplus::StringAlignmentFar);
    canvas.DrawString(detail.c_str(), -1, &title, right, &align,
                      monitor.control ? &text_brush : &faint_brush);

    /* Beneath the name: what drives this monitor, and how brightness
     * reaches it. The first tells two identical monitors apart, which
     * matters because a passthrough adapter reports the panel's own name
     * and the list would otherwise show the same thing twice. The second
     * is worth saying because the two methods behave differently at the
     * dark end. */
    std::wstring beneath = monitor.adapter;
    if (monitor.control) {
      if (!beneath.empty()) {
        beneath += L"  \u00b7  ";
      }
      beneath += monitor.control->Method();
    }
    if (!beneath.empty()) {
      Gdiplus::RectF where(
          static_cast<Gdiplus::REAL>(Scale(kPadding)),
          static_cast<Gdiplus::REAL>(top + Scale(19)),
          static_cast<Gdiplus::REAL>(Scale(kFlyoutWidth - kPadding * 2)),
          static_cast<Gdiplus::REAL>(Scale(16)));
      Gdiplus::StringFormat clip;
      clip.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
      clip.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
      canvas.DrawString(beneath.c_str(), -1, &subtitle, where, &clip,
                        &faint_brush);
    }

    const RECT track = TrackRect(static_cast<int>(i));
    const int track_width = track.right - track.left;
    const Gdiplus::REAL radius = static_cast<Gdiplus::REAL>(Scale(kTrackHeight)) / 2;

    Gdiplus::RectF trough_rect(
        static_cast<Gdiplus::REAL>(track.left),
        static_cast<Gdiplus::REAL>(track.top),
        static_cast<Gdiplus::REAL>(track_width),
        static_cast<Gdiplus::REAL>(track.bottom - track.top));
    canvas.FillRectangle(&trough_brush, trough_rect);

    if (!monitor.control) {
      continue;
    }

    const int knob_inset = Scale(kKnobRadius);
    const int span = track_width - knob_inset * 2;
    const int filled = knob_inset + (span * monitor.brightness) / 100;
    if (filled > 0) {
      Gdiplus::RectF filled_rect(
          static_cast<Gdiplus::REAL>(track.left),
          static_cast<Gdiplus::REAL>(track.top),
          static_cast<Gdiplus::REAL>(filled),
          static_cast<Gdiplus::REAL>(track.bottom - track.top));
      canvas.FillRectangle(&accent_brush, filled_rect);
    }
    (void)radius;

    const int knob = knob_inset;
    const Gdiplus::REAL cx = static_cast<Gdiplus::REAL>(track.left + filled);
    const Gdiplus::REAL cy =
        static_cast<Gdiplus::REAL>((track.top + track.bottom) / 2);
    Gdiplus::SolidBrush knob_brush(dark ? Gdiplus::Color(255, 255, 255, 255)
                                        : Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::Pen knob_pen(accent, static_cast<Gdiplus::REAL>(Scale(2)));
    canvas.FillEllipse(&knob_brush, cx - knob, cy - knob, knob * 2.0f,
                       knob * 2.0f);
    canvas.DrawEllipse(&knob_pen, cx - knob, cy - knob, knob * 2.0f,
                       knob * 2.0f);
  }

  BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
  SelectObject(memory, previous);
  DeleteObject(bitmap);
  DeleteDC(memory);
}

/* ---- behaviour ---------------------------------------------------------- */

void ApplyBrightness(int index, int percent, bool save) {
  if (index < 0 || index >= static_cast<int>(g_monitors.size())) {
    return;
  }
  Monitor& monitor = g_monitors[index];
  if (!monitor.control) {
    return;
  }
  monitor.brightness = Clamp(percent, 0, 100);
  monitor.control->Set(monitor.brightness);
  if (save) {
    SaveBrightness(monitor.identity, monitor.brightness);
  }
}

/* Makes the USB display an actual screen, if it has arrived without being
 * one.
 *
 * A monitor arriving is not the same as a screen appearing, and the gap
 * between those two is what made the display look broken while every signal
 * inside the driver said it was working. Measured in exactly that state:
 * the driver loaded, the monitor announced, a swapchain assigned, frames
 * going out, nothing dropped, 489 MB sent, and one single screen on the
 * desktop. Windows had simply not extended onto it, so there was nothing
 * for the user to see and nothing in the driver to find.
 *
 * This lives in the tray program because of where it has to run. The
 * desktop layout belongs to an interactive session, so SYSTEM cannot change
 * it, which rules out the startup repair task. The tray already starts at
 * sign-in as the signed-in user, unelevated, which is exactly the identity
 * needed, so the alternative was a second program that did nothing else.
 *
 * Two deliberate limits:
 *
 * Conditional. If the display is on the desktop in any arrangement this
 * does nothing, so somebody who has put the panel above the built-in screen
 * or made it their only one does not find it rearranged behind their back.
 *
 * Early in the session only, for about two minutes. The fault is a startup
 * one: the driver often is not up yet when the tray starts, because it may
 * take the repair task two attempts to get it there. Past that this stops
 * looking, so a user who later detaches this screen on purpose is not
 * fought over it every few seconds. */
void EnsureDisplayIsOnTheDesktop() {
  bool present = false;
  for (DWORD i = 0;; ++i) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, i, &adapter, 0)) {
      break;
    }
    if (wcsstr(adapter.DeviceString, L"USB Display") == nullptr) {
      continue;
    }
    present = true;
    if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0) {
      /* Already a screen, so the job is done and there is no reason to keep
       * watching. */
      g_extend_attempts = kExtendAttempts;
      return;
    }
  }

  if (!present) {
    /* The driver is not up yet, or there is no adapter plugged in. Either
     * way there is nothing to extend onto; keep waiting. */
    return;
  }

  SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
}

/* Re-reads the attached monitors, keeping the values already on screen.
 *
 * Called on a timer and when Windows reports a display change, so plugging
 * the adapter in makes a slider appear without the user doing anything. */
void Rescan(bool restore_saved) {
  std::vector<Monitor> found = FindMonitors();

  for (Monitor& monitor : found) {
    if (!monitor.control) {
      continue;
    }
    const int saved = LoadBrightness(monitor.identity, -1);
    if (saved >= 0) {
      monitor.brightness = saved;
      /* A gamma table does not survive the display being reconfigured, and
       * a monitor that was switched off has forgotten its setting too, so
       * the saved value is reapplied rather than assumed to still hold. */
      if (restore_saved) {
        monitor.control->Set(saved);
      }
    }
  }

  g_monitors = std::move(found);
  if (g_window) {
    InvalidateRect(g_window, nullptr, FALSE);
  }
}

void UpdateTooltip() {
  std::wstring tip = L"Brightness";
  for (const Monitor& monitor : g_monitors) {
    if (!monitor.control) {
      continue;
    }
    tip += L"\n" + monitor.name + L"  " +
           std::to_wstring(monitor.brightness) + L"%";
  }
  if (tip.size() >= ARRAYSIZE(g_tray.szTip)) {
    tip.resize(ARRAYSIZE(g_tray.szTip) - 1);
  }
  wcscpy_s(g_tray.szTip, tip.c_str());
  g_tray.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void PositionFlyout() {
  /* Anchored to the icon rather than the pointer, so it appears in the same
   * place however the icon was clicked, and lands correctly whichever edge
   * the taskbar is on. */
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

  HMONITOR screen = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(screen, &info);

  UINT dpi_x = 96, dpi_y = 96;
  if (SUCCEEDED(GetDpiForMonitor(screen, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
    g_dpi = static_cast<int>(dpi_x);
  }

  const int width = Scale(kFlyoutWidth);
  const int height = FlyoutHeight();
  const int margin = Scale(12);

  int x = anchor.x - width / 2;
  int y = anchor.y - height - margin;
  if (x + width > info.rcWork.right) {
    x = info.rcWork.right - width - margin;
  }
  if (x < info.rcWork.left) {
    x = info.rcWork.left + margin;
  }
  if (y < info.rcWork.top) {
    /* Taskbar at the top: drop below the icon instead. */
    y = anchor.y + margin;
  }

  SetWindowPos(g_window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}

void ShowFlyout() {
  Rescan(false);
  PositionFlyout();
  /* SW_SHOW, not SW_SHOWNOACTIVATE. Without activation the window is told it
   * has lost focus the moment it appears, and hides itself again: from the
   * user's point of view clicking the icon does nothing at all. */
  ShowWindow(g_window, SW_SHOW);
  SetForegroundWindow(g_window);
  InvalidateRect(g_window, nullptr, FALSE);
}

void HideFlyout() {
  if (g_dragging >= 0) {
    ReleaseCapture();
    g_dragging = -1;
  }
  if (IsWindowVisible(g_window)) {
    /* Remembered so that the click which dismissed the window is not then
     * taken as a click asking to open it again. */
    g_hidden_at_ms = GetTickCount64();
  }
  ShowWindow(g_window, SW_HIDE);
}

void ShowContextMenu() {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kMenuRefresh, L"Look for monitors again");
  AppendMenuW(menu, MF_STRING, kMenuResetAll, L"Set every monitor to 100%");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
              kMenuAutostart, L"Start with Windows");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

  POINT cursor;
  GetCursorPos(&cursor);
  /* Required, or the menu refuses to close when clicked away from. */
  SetForegroundWindow(g_window);
  const int chosen = TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y,
      0, g_window, nullptr);
  /* Without this the menu can stay on screen after a click elsewhere. */
  PostMessageW(g_window, WM_NULL, 0, 0);
  DestroyMenu(menu);

  switch (chosen) {
    case kMenuExit:
      DestroyWindow(g_window);
      break;
    case kMenuAutostart:
      SetAutostart(!IsAutostartEnabled());
      break;
    case kMenuRefresh:
      Rescan(true);
      UpdateTooltip();
      break;
    case kMenuResetAll:
      for (size_t i = 0; i < g_monitors.size(); ++i) {
        ApplyBrightness(static_cast<int>(i), 100, true);
      }
      UpdateTooltip();
      InvalidateRect(g_window, nullptr, FALSE);
      break;
    default:
      break;
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case kTrayMessage:
      if (LOWORD(lparam) == WM_LBUTTONUP) {
        if (IsWindowVisible(window)) {
          HideFlyout();
        } else if (GetTickCount64() - g_hidden_at_ms > 250) {
          /* The guard covers clicking the icon while the window is open:
           * that click dismisses it first, and without this the same click
           * would immediately reopen it. */
          ShowFlyout();
        }
      } else if (LOWORD(lparam) == WM_RBUTTONUP) {
        ShowContextMenu();
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT paint;
      HDC dc = BeginPaint(window, &paint);
      RECT client;
      GetClientRect(window, &client);
      DrawFlyout(dc, client.right, client.bottom);
      EndPaint(window, &paint);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      const int row = RowFromPoint(GET_Y_LPARAM(lparam));
      if (row >= 0 && g_monitors[row].control) {
        g_dragging = row;
        SetCapture(window);
        ApplyBrightness(row, ValueFromPoint(row, GET_X_LPARAM(lparam)), false);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }

    case WM_MOUSEMOVE:
      if (g_dragging >= 0) {
        /* Signed, because a drag routinely leaves the window and the
         * unsigned form turns a position of -3 into 65533, which sends the
         * slider to the far end. */
        ApplyBrightness(g_dragging,
                        ValueFromPoint(g_dragging, GET_X_LPARAM(lparam)),
                        false);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;

    case WM_LBUTTONUP:
      if (g_dragging >= 0) {
        ReleaseCapture();
        /* Written once the slider settles rather than on every pixel. */
        SetTimer(window, kSaveTimer, kSaveDelayMs, nullptr);
        g_dragging = -1;
        UpdateTooltip();
      }
      return 0;

    case WM_MOUSEWHEEL: {
      POINT at = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &at);
      const int row = RowFromPoint(at.y);
      if (row >= 0 && g_monitors[row].control) {
        const int step = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 5 : -5;
        ApplyBrightness(row, g_monitors[row].brightness + step, false);
        SetTimer(window, kSaveTimer, kSaveDelayMs, nullptr);
        UpdateTooltip();
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }

    case WM_TIMER:
      if (wparam == kRescanTimer) {
        /* Only while hidden: rebuilding the list under a moving slider
         * would make it jump. */
        if (!IsWindowVisible(window)) {
          const size_t before = g_monitors.size();
          Rescan(true);
          if (g_monitors.size() != before) {
            UpdateTooltip();
          }
        }
      } else if (wparam == kExtendTimer) {
        if (++g_extend_attempts >= kExtendAttempts) {
          KillTimer(window, kExtendTimer);
        }
        EnsureDisplayIsOnTheDesktop();
      } else if (wparam == kSaveTimer) {
        KillTimer(window, kSaveTimer);
        for (Monitor& monitor : g_monitors) {
          if (monitor.control) {
            /* The final slider position may have arrived during a rate
             * limited control's quiet period, so make sure it lands. */
            monitor.control->Flush();
            SaveBrightness(monitor.identity, monitor.brightness);
          }
        }
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;

    case WM_DISPLAYCHANGE:
      /* A monitor arrived, left, or changed mode. A gamma table does not
       * survive that, so the saved values are put back. */
      Rescan(true);
      UpdateTooltip();
      return 0;

    case WM_DPICHANGED:
      g_dpi = HIWORD(wparam);
      if (IsWindowVisible(window)) {
        PositionFlyout();
      }
      return 0;

    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE) {
        HideFlyout();
      }
      return 0;

    case WM_DESTROY:
      Shell_NotifyIconW(NIM_DELETE, &g_tray);
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

HICON MakeTrayIcon() {
  /* Taken from the shell rather than drawn.
   *
   * Drawing one seemed tidier and was not: a hand-drawn icon has to be
   * coloured to contrast with the taskbar, which follows a different theme
   * setting from applications and changes underneath you, and getting it
   * wrong means drawing the icon in the same colour as the thing behind it.
   * The shell's own display icon is always the right colour, always the
   * right size, and already means "screen" to anyone looking at it. */
  HICON icon = nullptr;
  ExtractIconExW(L"shell32.dll", 15, nullptr, &icon, 1);
  if (!icon) {
    icon = LoadIconW(nullptr, IDI_APPLICATION);
  }
  return icon;
}

}  // namespace

/* Prints what was detected and exits, for when the interface is not
 * behaving and the question is whether the fault is in finding the monitors
 * or in drawing them. */
int RunDiagnostic() {
  /* Written to a file rather than a console. This is a windowed program, so
   * it has no console to write to, and attaching to the parent's works only
   * when there is one and only for some shells. */
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring report = path;
  const size_t slash = report.find_last_of(L'\\');
  report = (slash == std::wstring::npos ? std::wstring() : report.substr(0, slash + 1)) +
           L"monitors.txt";

  FILE* out = nullptr;
  if (_wfopen_s(&out, report.c_str(), L"w, ccs=UTF-8") != 0 || !out) {
    return 1;
  }

  std::vector<Monitor> monitors = FindMonitors();
  fwprintf(out, L"%d monitor(s)\n\n", static_cast<int>(monitors.size()));
  for (const Monitor& monitor : monitors) {
    fwprintf(out, L"  %s\n", monitor.name.c_str());
    fwprintf(out, L"    driven by   %s\n", monitor.adapter.c_str());
    fwprintf(out, L"    device      %s%s\n", monitor.device.c_str(),
            monitor.primary ? L"  (primary)" : L"");
    fwprintf(out, L"    identity    %s\n", monitor.identity.c_str());
    if (monitor.control) {
      fwprintf(out, L"    brightness  %d%% via %s (%s)\n", monitor.brightness,
              monitor.control->Method(),
              monitor.control->IsHardware() ? L"backlight" : L"image");
    } else {
      fwprintf(out, L"    brightness  not adjustable\n");
    }
    fwprintf(out, L"\n");
  }
  fclose(out);
  return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
  if (command_line && wcsstr(command_line, L"--list")) {
    return RunDiagnostic();
  }
  g_instance = instance;

  /* One instance only: two would fight over every gamma table. */
  HANDLE once = CreateMutexW(nullptr, TRUE, L"usbdisplay-brightness-tray");
  if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;
  }

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  Gdiplus::GdiplusStartupInput startup;
  Gdiplus::GdiplusStartup(&g_gdiplus, &startup, nullptr);

  WNDCLASSEXW cls = {};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = WindowProc;
  cls.hInstance = instance;
  cls.lpszClassName = kWindowClass;
  cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassExW(&cls);

  g_window = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass, L"Brightness", WS_POPUP,
      0, 0, Scale(kFlyoutWidth), Scale(200), nullptr, nullptr, instance,
      nullptr);
  if (!g_window) {
    return 1;
  }
  g_dpi = GetDpiForWindow(g_window);

  /* Rounded corners and a shadow on Windows 11; ignored on earlier
   * releases, which simply get square corners. */
  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(g_window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                        sizeof(corner));
  const BOOL dark_titlebar = IsDarkMode() ? TRUE : FALSE;
  DwmSetWindowAttribute(g_window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                        &dark_titlebar, sizeof(dark_titlebar));

  /* Saved values are applied at startup, which is what makes the setting
   * survive a reboot: nothing else remembers it for a gamma-controlled
   * display. */
  Rescan(true);

  g_tray.cbSize = sizeof(g_tray);
  g_tray.hWnd = g_window;
  g_tray.uID = kTrayId;
  g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_tray.uVersion = NOTIFYICON_VERSION_4;
  g_tray.uCallbackMessage = kTrayMessage;
  g_tray.hIcon = MakeTrayIcon();
  wcscpy_s(g_tray.szTip, L"Brightness");
  if (!Shell_NotifyIconW(NIM_ADD, &g_tray)) {
    MessageBoxW(nullptr,
                L"Could not add an icon to the notification area.",
                L"Brightness", MB_OK | MB_ICONERROR);
    return 1;
  }
  UpdateTooltip();

  /* Windows files a new notification icon away in the overflow by default,
   * so an application that simply appears there has, from the user's point
   * of view, not started at all. Say so once, the first time it runs. */
  if (!WasAnnounced()) {
    MarkAnnounced();
    g_tray.uFlags = NIF_INFO;
    wcscpy_s(g_tray.szInfoTitle, L"Brightness is running");
    wcscpy_s(g_tray.szInfo,
             L"Its icon may be hidden: click the arrow beside the clock to "
             L"find it, and drag it onto the taskbar to keep it there.");
    g_tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
  }

  SetTimer(g_window, kRescanTimer, kRescanMs, nullptr);

  /* Check once straight away, then on a timer, because the common case is a
   * display that is already waiting by the time this starts. */
  EnsureDisplayIsOnTheDesktop();
  SetTimer(g_window, kExtendTimer, kExtendMs, nullptr);

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (g_tray.hIcon) {
    DestroyIcon(g_tray.hIcon);
  }
  Gdiplus::GdiplusShutdown(g_gdiplus);
  if (once) {
    CloseHandle(once);
  }
  return 0;
}
