/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Which brightness mechanisms, if any, reach a given monitor.
 *
 * Brightness on Windows is not one interface but four, and which of them a
 * monitor answers decides which third-party tools can control it. This
 * reports all four for every attached display, so the question "why can that
 * app not dim this screen" has an evidenced answer rather than a guess.
 *
 *   DDC/CI       dxva2.dll, the standard for external monitors. Travels from
 *                the OS to the graphics card's I2C master and out along the
 *                display cable.
 *   WMI          root\WMI WmiMonitorBrightness, the path used for laptop
 *                panels. Created by monitor.sys only when the kernel display
 *                miniport exposes a brightness interface.
 *   gamma ramp   SetDeviceGammaRamp, a per-display lookup table. Not real
 *                brightness, but it dims, and several tools fall back to it.
 *   ours         the value this driver applies during colour conversion.
 */

#include <windows.h>

#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct MonitorInfo {
  std::wstring device;       /* \\.\DISPLAY3 */
  std::wstring description;  /* what the monitor calls itself */
  HMONITOR handle = nullptr;
  RECT bounds = {};
  bool primary = false;
};

std::vector<MonitorInfo> g_monitors;

BOOL CALLBACK OnMonitor(HMONITOR handle, HDC, LPRECT, LPARAM) {
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(handle, &info)) {
    return TRUE;
  }

  MonitorInfo entry;
  entry.handle = handle;
  entry.device = info.szDevice;
  entry.bounds = info.rcMonitor;
  entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

  /* The friendly name lives on the display device, not the monitor. */
  DISPLAY_DEVICEW display = {};
  display.cb = sizeof(display);
  if (EnumDisplayDevicesW(info.szDevice, 0, &display, 0)) {
    entry.description = display.DeviceString;
  }

  g_monitors.push_back(entry);
  return TRUE;
}

/* DDC/CI, the path an external monitor answers. */
void ProbeDdcCi(const MonitorInfo& monitor) {
  DWORD count = 0;
  if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor.handle, &count) ||
      count == 0) {
    printf("    DDC/CI      no physical monitor behind this handle\n");
    return;
  }

  std::vector<PHYSICAL_MONITOR> physical(count);
  if (!GetPhysicalMonitorsFromHMONITOR(monitor.handle, count,
                                       physical.data())) {
    printf("    DDC/CI      could not open, error %lu\n", GetLastError());
    return;
  }

  for (DWORD i = 0; i < count; ++i) {
    DWORD minimum = 0, current = 0, maximum = 0;
    if (GetMonitorBrightness(physical[i].hPhysicalMonitor, &minimum, &current,
                             &maximum)) {
      printf("    DDC/CI      WORKS, brightness %lu (%lu..%lu)\n", current,
             minimum, maximum);
    } else {
      const DWORD code = GetLastError();
      printf("    DDC/CI      no (error %lu%s)\n", code,
             code == ERROR_NOT_SUPPORTED
                 ? ", the display stack does not carry I2C here"
                 : "");

      /* The low level path sometimes answers where the high level one does
       * not, so it is worth asking separately before concluding. */
      MC_VCP_CODE_TYPE type;
      DWORD value = 0, vcp_maximum = 0;
      if (GetVCPFeatureAndVCPFeatureReply(physical[i].hPhysicalMonitor, 0x10,
                                          &type, &value, &vcp_maximum)) {
        printf("    DDC/CI      but the raw brightness code answers: %lu/%lu\n",
               value, vcp_maximum);
      }
    }
  }

  DestroyPhysicalMonitors(count, physical.data());
}

/* The gamma lookup table. Not brightness, but it dims, and it is what
 * several tools fall back to when nothing else works. */
void ProbeGammaRamp(const MonitorInfo& monitor) {
  HDC dc = CreateDCW(L"DISPLAY", monitor.device.c_str(), nullptr, nullptr);
  if (!dc) {
    printf("    gamma ramp  no device context\n");
    return;
  }

  WORD original[3][256] = {};
  if (!GetDeviceGammaRamp(dc, original)) {
    printf("    gamma ramp  not supported (error %lu)\n", GetLastError());
    DeleteDC(dc);
    return;
  }

  /* Half brightness, briefly, then put it back. */
  WORD dimmed[3][256] = {};
  for (int i = 0; i < 256; ++i) {
    const WORD value = static_cast<WORD>(i * 257 / 2);
    dimmed[0][i] = dimmed[1][i] = dimmed[2][i] = value;
  }

  const bool applied = SetDeviceGammaRamp(dc, dimmed) != FALSE;
  if (applied) {
    printf("    gamma ramp  accepted, dimming for one second\n");
    Sleep(1000);
    SetDeviceGammaRamp(dc, original);
  } else {
    printf("    gamma ramp  refused (error %lu)\n", GetLastError());
  }
  DeleteDC(dc);
}

void ProbeWmi() {
  /* Queried through PowerShell rather than COM: this runs once, and the
   * cost of getting WMI wrong in C++ is out of proportion to the value. */
  printf("\nWMI, the path laptop panels use\n");
  const int result = system(
      "powershell -NoProfile -Command \""
      "$b = Get-CimInstance -Namespace root/WMI -ClassName WmiMonitorBrightness"
      " -ErrorAction SilentlyContinue; "
      "if ($b) { $b | ForEach-Object { "
      "  Write-Host ('    ' + $_.InstanceName + '  brightness ' "
      "+ $_.CurrentBrightness) } } "
      "else { Write-Host '    no monitor exposes WmiMonitorBrightness' }\"");
  (void)result;
  printf(
      "    This appears only when the kernel display driver offers a\n"
      "    brightness interface. An indirect display has no such driver of\n"
      "    its own, so it cannot appear here without a kernel component.\n");
}

}  // namespace

int main() {
  printf("Brightness paths, per monitor\n\n");

  EnumDisplayMonitors(nullptr, nullptr, OnMonitor, 0);
  if (g_monitors.empty()) {
    printf("no monitors found\n");
    return 1;
  }

  for (const MonitorInfo& monitor : g_monitors) {
    printf("%S  %S%s\n", monitor.device.c_str(), monitor.description.c_str(),
           monitor.primary ? "  (primary)" : "");
    printf("    at %ldx%ld, %ldx%ld\n", monitor.bounds.left,
           monitor.bounds.top, monitor.bounds.right - monitor.bounds.left,
           monitor.bounds.bottom - monitor.bounds.top);
    ProbeDdcCi(monitor);
    ProbeGammaRamp(monitor);
    printf("\n");
  }

  ProbeWmi();
  return 0;
}
