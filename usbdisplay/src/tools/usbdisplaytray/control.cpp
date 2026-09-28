/* SPDX-License-Identifier: GPL-2.0-only */

#include "control.h"

#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

#include <algorithm>

namespace usbdisplay {
namespace {

int Clamp(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

/* Brightness over the display cable, adjusting the monitor's own backlight.
 *
 * Two ways in, and both are tried. The high level call is simpler and is
 * what most monitors answer; the raw feature code beneath it works on some
 * that the high level call refuses, apparently because it skips a
 * capabilities negotiation that those monitors get wrong. */
class DdcCiControl : public BrightnessControl {
 public:
  explicit DdcCiControl(HANDLE monitor) : monitor_(monitor) {}

  ~DdcCiControl() override {
    if (monitor_) {
      DestroyPhysicalMonitor(monitor_);
    }
  }

  const wchar_t* Method() const override { return L"display cable"; }
  bool IsHardware() const override { return true; }

  bool Get(int* percent) override {
    DWORD minimum = 0, current = 0, maximum = 0;
    if (GetMonitorBrightness(monitor_, &minimum, &current, &maximum) &&
        maximum > minimum) {
      minimum_ = minimum;
      maximum_ = maximum;
      high_level_ = true;
      *percent = static_cast<int>(((current - minimum) * 100) /
                                  (maximum - minimum));
      return true;
    }

    MC_VCP_CODE_TYPE type;
    DWORD value = 0, vcp_maximum = 0;
    if (GetVCPFeatureAndVCPFeatureReply(monitor_, kBrightnessCode, &type,
                                        &value, &vcp_maximum) &&
        vcp_maximum > 0) {
      minimum_ = 0;
      maximum_ = vcp_maximum;
      high_level_ = false;
      *percent = static_cast<int>((value * 100) / vcp_maximum);
      return true;
    }
    return false;
  }

  bool Set(int percent) override {
    percent = Clamp(percent, 0, 100);
    const DWORD value =
        minimum_ + static_cast<DWORD>(((maximum_ - minimum_) * percent) / 100);
    if (high_level_) {
      return SetMonitorBrightness(monitor_, value) != FALSE;
    }
    return SetVCPFeature(monitor_, kBrightnessCode, value) != FALSE;
  }

  /* True only if the monitor answered at least once, so a monitor that is
   * merely switched off is not mistaken for one that cannot be controlled. */
  bool Probe() {
    int ignored = 0;
    return Get(&ignored);
  }

 private:
  static constexpr BYTE kBrightnessCode = 0x10;

  HANDLE monitor_ = nullptr;
  DWORD minimum_ = 0;
  DWORD maximum_ = 100;
  bool high_level_ = true;
};

/* Brightness by lookup table, applied to the pixels rather than the
 * backlight.
 *
 * Windows rejects a table it considers too far from linear, which is a
 * defence against a program making the screen unreadable and leaving the
 * user unable to find the window to fix it. The floor below keeps the
 * steepest table we produce inside what it will accept. */
class GammaControl : public BrightnessControl {
 public:
  explicit GammaControl(const std::wstring& device) : device_(device) {}

  const wchar_t* Method() const override { return L"image"; }
  bool IsHardware() const override { return false; }

  bool Get(int* percent) override {
    /* The table is write-only in practice: reading it back gives whatever
     * was last applied, which is what we already know, and after a mode
     * change it is the identity again even though the user's setting has
     * not changed. The caller's stored value is the better answer. */
    *percent = applied_;
    return true;
  }

  bool Set(int percent) override {
    percent = Clamp(percent, kMinimumPercent, 100);
    if (percent == applied_) {
      return true;
    }

    /* Rate limited, because on an indirect display this is not free: the
     * driver has to redraw every pixel it has already sent, which on a USB
     * link is around a quarter of a second of traffic. Dragging a slider
     * produces a change per pixel of travel, and sending all of them asks
     * for several times what the link can carry, which freezes the picture
     * instead of dimming it.
     *
     * The driver defends itself against this as well, and should: it cannot
     * assume every caller is well behaved. But there is no reason to make
     * it do that work. */
    const ULONGLONG now = GetTickCount64();
    if (now - last_applied_ms_ < kMinimumIntervalMs) {
      wanted_ = percent;
      return true;
    }

    HDC dc = CreateDCW(L"DISPLAY", device_.c_str(), nullptr, nullptr);
    if (!dc) {
      return false;
    }

    WORD table[3][256];
    const double scale = percent / 100.0;
    for (int i = 0; i < 256; ++i) {
      const WORD value = static_cast<WORD>(i * 257 * scale);
      table[0][i] = table[1][i] = table[2][i] = value;
    }

    const bool ok = SetDeviceGammaRamp(dc, table) != FALSE;
    DeleteDC(dc);
    if (ok) {
      applied_ = percent;
      wanted_ = percent;
      last_applied_ms_ = now;
    }
    return ok;
  }

  /* Applies whatever the last rate-limited call asked for. Called when the
   * slider settles, so the final position always takes effect even if it
   * arrived during the quiet period. */
  void Flush() override {
    if (wanted_ != applied_) {
      last_applied_ms_ = 0;
      Set(wanted_);
    }
  }

  bool Probe() {
    /* Asking for the table is enough to know whether this display has one;
     * it avoids flashing the screen just to find out. */
    HDC dc = CreateDCW(L"DISPLAY", device_.c_str(), nullptr, nullptr);
    if (!dc) {
      return false;
    }
    WORD table[3][256];
    const bool ok = GetDeviceGammaRamp(dc, table) != FALSE;
    DeleteDC(dc);
    return ok;
  }

 private:
  /* Below roughly a third, Windows starts refusing the table outright, so
   * the slider stops there rather than appearing to do nothing. */
  static constexpr int kMinimumPercent = 30;

  /* One change every this often at most. Roughly what a full redraw costs
   * on the USB link, so the panel keeps up rather than falling behind. */
  static constexpr ULONGLONG kMinimumIntervalMs = 250;

  std::wstring device_;
  int applied_ = 100;
  int wanted_ = 100;
  ULONGLONG last_applied_ms_ = 0;
};

/* Reads the monitor's own name out of the capability block Windows stored
 * when it enumerated the device.
 *
 * Worth the trouble because the name Windows offers through the ordinary
 * calls is "Generic PnP Monitor" for almost everything, which is no help at
 * all when there are two of them in a list. The real name is in the
 * capability block, in a descriptor tagged 0xFC. */
std::wstring ReadNameFromCapabilities(const std::wstring& interface_name) {
  /* The interface name looks like \\?\DISPLAY#ACR0490#5&...#{guid}. The
   * registry wants it with the leading marker removed and the separators
   * turned back into path separators. */
  std::wstring path = interface_name;
  const size_t start = path.find(L"DISPLAY#");
  if (start == std::wstring::npos) {
    return std::wstring();
  }
  path = path.substr(start);
  const size_t guid = path.rfind(L"#{");
  if (guid != std::wstring::npos) {
    path.resize(guid);
  }
  std::replace(path.begin(), path.end(), L'#', L'\\');

  const std::wstring key =
      L"SYSTEM\\CurrentControlSet\\Enum\\" + path + L"\\Device Parameters";

  HKEY handle = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_QUERY_VALUE,
                    &handle) != ERROR_SUCCESS) {
    return std::wstring();
  }

  BYTE edid[512] = {};
  DWORD size = sizeof(edid);
  DWORD type = 0;
  const bool ok = RegQueryValueExW(handle, L"EDID", nullptr, &type, edid,
                                   &size) == ERROR_SUCCESS;
  RegCloseKey(handle);
  if (!ok || size < 128) {
    return std::wstring();
  }

  /* Four descriptors of eighteen bytes from offset 54. The one starting
   * 00 00 00 FC holds the name, padded with spaces and ended by a newline. */
  for (int i = 0; i < 4; ++i) {
    const BYTE* descriptor = edid + 54 + i * 18;
    if (descriptor[0] != 0 || descriptor[1] != 0 || descriptor[2] != 0 ||
        descriptor[3] != 0xFC) {
      continue;
    }
    std::wstring name;
    for (int j = 5; j < 18; ++j) {
      const BYTE c = descriptor[j];
      if (c == 0x0A) {
        break;
      }
      name.push_back(static_cast<wchar_t>(c));
    }
    while (!name.empty() && name.back() == L' ') {
      name.pop_back();
    }
    return name;
  }
  return std::wstring();
}

/* Brightness for a display driven by this project.
 *
 * Preferred over the gamma table for our own hardware, for two reasons.
 *
 * Windows validates a gamma table and refuses anything it considers too far
 * from linear, which in practice rules out the lower half of the range: the
 * slider stops responding part way down with no explanation. This path has
 * no such check, so the whole range is usable.
 *
 * It is also simply better. The driver applies it while converting, scaling
 * luma about its black point and leaving colour alone, where a gamma table
 * scales everything and crushes the dark tones. The difference is visible at
 * low settings.
 *
 * The value lives in the machine-wide settings because the driver runs as a
 * service account with no user settings to read. The installer widens
 * permissions on that one key so this needs no special privileges. */
class UsbDisplayControl : public BrightnessControl {
 public:
  const wchar_t* Method() const override { return L"driver"; }
  bool IsHardware() const override { return false; }

  bool Get(int* percent) override {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
      *percent = 100;
      return true;
    }
    DWORD value = 100, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, L"Brightness", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &size) !=
            ERROR_SUCCESS ||
        type != REG_DWORD) {
      value = 100;
    }
    RegCloseKey(key);
    *percent = Clamp(static_cast<int>(value), 0, 100);
    return true;
  }

  bool Set(int percent) override {
    percent = Clamp(percent, 0, 100);
    if (percent == applied_) {
      return true;
    }

    /* Rate limited for the same reason as the gamma path: a change makes
     * the driver redraw everything it has already sent, which on a USB link
     * is a quarter of a second of traffic. */
    const ULONGLONG now = GetTickCount64();
    if (now - last_applied_ms_ < kMinimumIntervalMs) {
      wanted_ = percent;
      return true;
    }

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
      return false;
    }
    const DWORD value = static_cast<DWORD>(percent);
    const bool ok = RegSetValueExW(key, L"Brightness", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value),
                                   sizeof(value)) == ERROR_SUCCESS;
    RegCloseKey(key);

    if (ok) {
      applied_ = percent;
      wanted_ = percent;
      last_applied_ms_ = now;
    }
    return ok;
  }

  void Flush() override {
    if (wanted_ != applied_) {
      last_applied_ms_ = 0;
      Set(wanted_);
    }
  }

  /* Tested by writing, not by opening.
   *
   * Opening for write succeeds in cases where writing then fails, so a
   * check that only opens reports this path as available and the slider
   * silently does nothing. The value written is the one already there, so
   * a successful test changes nothing. */
  bool Probe() {
    int current = 100;
    Get(&current);

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
      return false;
    }
    const DWORD value = static_cast<DWORD>(current);
    const bool writable =
        RegSetValueExW(key, L"Brightness", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value),
                       sizeof(value)) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!writable) {
      return false;
    }

    applied_ = current;
    wanted_ = current;
    return true;
  }

 private:
  static constexpr ULONGLONG kMinimumIntervalMs = 250;
  static constexpr const wchar_t* kKey = L"SOFTWARE\\usbdisplay";

  int applied_ = 100;
  int wanted_ = 100;
  ULONGLONG last_applied_ms_ = 0;
};

struct Candidate {
  HMONITOR handle;
  std::wstring device;
  std::wstring name;
  std::wstring adapter;
  std::wstring identity;
  bool primary;
};

/* What is driving a given display, by name.
 *
 * This matters more than it looks. A passthrough adapter reports the panel's
 * own capabilities, so a monitor plugged into one is indistinguishable by
 * name from the same monitor plugged in directly: the list would show the
 * same thing twice with no way to tell which slider was which. What differs
 * is the thing driving it, which is the graphics card in one case and this
 * driver in the other. */
std::wstring AdapterFor(const std::wstring& device) {
  for (DWORD i = 0; i < 64; ++i) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, i, &adapter, 0)) {
      break;
    }
    if (device == adapter.DeviceName) {
      return adapter.DeviceString;
    }
  }
  return std::wstring();
}

std::vector<Candidate> g_candidates;

BOOL CALLBACK OnMonitor(HMONITOR handle, HDC, LPRECT, LPARAM) {
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(handle, &info)) {
    return TRUE;
  }

  Candidate candidate;
  candidate.handle = handle;
  candidate.device = info.szDevice;
  candidate.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

  /* The monitor beneath the display gives a friendly name and an identity
   * that survives a restart, which is what saved settings key on. The
   * device name alone will not do: Windows renumbers those. */
  DISPLAY_DEVICEW attached = {};
  attached.cb = sizeof(attached);
  if (EnumDisplayDevicesW(info.szDevice, 0, &attached, 0)) {
    candidate.name = attached.DeviceString;
  }
  candidate.adapter = AdapterFor(info.szDevice);

  DISPLAY_DEVICEW monitor = {};
  monitor.cb = sizeof(monitor);
  if (EnumDisplayDevicesW(info.szDevice, 0, &monitor,
                          EDD_GET_DEVICE_INTERFACE_NAME)) {
    candidate.identity = monitor.DeviceID;

    /* Prefer what the monitor calls itself over what Windows calls it,
     * which is "Generic PnP Monitor" for almost everything. */
    const std::wstring real = ReadNameFromCapabilities(monitor.DeviceID);
    if (!real.empty()) {
      candidate.name = real;
    }
  }
  if (candidate.identity.empty()) {
    candidate.identity = candidate.device;
  }
  if (candidate.name.empty()) {
    candidate.name = candidate.device;
  }

  g_candidates.push_back(candidate);
  return TRUE;
}

/* Opens the display-cable control for a monitor, if it answers. */
std::unique_ptr<BrightnessControl> TryDdcCi(HMONITOR handle) {
  DWORD count = 0;
  if (!GetNumberOfPhysicalMonitorsFromHMONITOR(handle, &count) || count == 0) {
    return nullptr;
  }

  std::vector<PHYSICAL_MONITOR> physical(count);
  if (!GetPhysicalMonitorsFromHMONITOR(handle, count, physical.data())) {
    return nullptr;
  }

  /* Only the first is used. More than one behind a single handle means
   * mirrored displays, which share a brightness by definition. */
  std::unique_ptr<DdcCiControl> control(
      new DdcCiControl(physical[0].hPhysicalMonitor));
  for (DWORD i = 1; i < count; ++i) {
    DestroyPhysicalMonitor(physical[i].hPhysicalMonitor);
  }

  if (!control->Probe()) {
    return nullptr;
  }
  return control;
}

}  // namespace

std::vector<Monitor> FindMonitors() {
  g_candidates.clear();
  EnumDisplayMonitors(nullptr, nullptr, OnMonitor, 0);

  std::vector<Monitor> monitors;
  for (const Candidate& candidate : g_candidates) {
    Monitor monitor;
    monitor.name = candidate.name;
    monitor.adapter = candidate.adapter;
    monitor.device = candidate.device;
    monitor.identity = candidate.identity;
    monitor.primary = candidate.primary;

    /* Our own display first, because we have a better route to it than
     * anything Windows offers; then the backlight, which is the best of the
     * general mechanisms; then the image, which always works but gives up
     * some quality at the dark end. */
    if (candidate.adapter.find(L"USB Display") != std::wstring::npos) {
      std::unique_ptr<UsbDisplayControl> own(new UsbDisplayControl());
      if (own->Probe()) {
        monitor.control = std::move(own);
      }
    }
    if (!monitor.control) {
      monitor.control = TryDdcCi(candidate.handle);
    }
    if (!monitor.control) {
      std::unique_ptr<GammaControl> gamma(new GammaControl(candidate.device));
      if (gamma->Probe()) {
        monitor.control = std::move(gamma);
      }
    }

    if (monitor.control) {
      monitor.control->Get(&monitor.brightness);
    }
    monitors.push_back(std::move(monitor));
  }

  /* Primary first, then as Windows lists them, so the order matches what a
   * user sees in the display settings. */
  std::stable_sort(monitors.begin(), monitors.end(),
                   [](const Monitor& a, const Monitor& b) {
                     return a.primary && !b.primary;
                   });
  return monitors;
}

}  // namespace usbdisplay
