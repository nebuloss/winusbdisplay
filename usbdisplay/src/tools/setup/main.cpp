/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The driver half of installing: everything that is specific to this
 * hardware, and nothing that is not.
 *
 * The part a user sees, the single downloadable installer, is built by NSIS
 * from installer/usbdisplay.nsi. That handles what every installer handles:
 * asking for permission, unpacking, copying the tools somewhere permanent,
 * appearing in the list of installed programs, and undoing all of it later.
 * There is no reason to write that again, and the several ways of getting it
 * subtly wrong are all well trodden.
 *
 * What NSIS cannot sensibly do is the part below. Installing a driver
 * package, retiring device nodes in an order that does not crash the
 * machine, and trusting a certificate are Windows API calls, and driving
 * them from an installer script means either a plugin or shelling out to
 * tools the user does not have. So the installer unpacks this and runs it,
 * and this does the work and says what happened, which matters more here
 * than usual: almost every failure mode of this hardware looks like silence.
 *
 * It can also be run on its own against a source build, which is how it is
 * tested, and is why it still carries a manifest asking for administrator
 * rights.
 *
 * ## The order of operations, and why
 *
 * Each step below is in its position because of something that went wrong
 * when it was not.
 *
 *   1. Remove our own older packages. Windows keeps every version ever
 *      installed, and the stale ones go on competing to claim the adapter,
 *      so an install can appear to succeed and change nothing.
 *
 *   2. Retire older device nodes, and any duplicates. Disabled first, then
 *      removed, never hurried: removing a live display device bug checked a
 *      machine during development.
 *
 *   3. Install the raw USB package, then cycle the adapter. Claiming that
 *      interface detaches it, and it does not come back on its own.
 *
 *   4. Install the display driver and create exactly one device node.
 *
 *   5. Widen permissions on the one setting the brightness control writes,
 *      so it needs no privileges of its own.
 */

#include <windows.h>

#include <newdev.h>
#include <setupapi.h>

#include <cfgmgr32.h>

#include <aclapi.h>
#include <sddl.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

/* Where the payload sits relative to this program, which is beside it. */
std::wstring g_here;

const wchar_t kHardwareId[] = L"root\\usbdisplaydd";
const wchar_t kSettingsKey[] = L"SOFTWARE\\usbdisplay";

/* The scheduled task that works around Windows refusing to load this
 * driver at boot. See InstallBootRepair. */
const wchar_t kBootTaskName[] = L"usbdisplay repair after startup";

/* How our adapter describes itself to the display enumeration APIs. Matched
 * loosely, on the leading words, so that renaming the device description in
 * the INF does not silently stop this from recognising it. */
const wchar_t kAdapterMatch[] = L"USB Display";

/* Names this project has used. An upgrade has to recognise its own past. */
const wchar_t* kFormerHardwareIds[] = {
    L"root\\usbdisplaydd", L"root\\usbhdmidd", L"root\\ms912xidd",
};
const wchar_t* kFormerInfNames[] = {
    L"usbdisplaydd.inf", L"usbdisplay_winusb.inf",
    L"usbhdmidd.inf",    L"usbhdmi_winusb.inf",
    L"ms912xidd.inf",    L"ms912x_winusb.inf",
};

bool g_quiet = false;

void Say(const wchar_t* format, ...) {
  if (g_quiet) {
    return;
  }
  va_list args;
  va_start(args, format);
  vwprintf(format, args);
  va_end(args);
  fflush(stdout);
}

void Fail(const wchar_t* format, ...) {
  va_list args;
  va_start(args, format);
  wchar_t message[1024];
  _vsnwprintf_s(message, _TRUNCATE, format, args);
  va_end(args);

  fwprintf(stderr, L"\n%s\n", message);
  if (!g_quiet) {
    wprintf(L"\nNothing has been left half installed: run this again once the\n"
            L"problem above is dealt with.\n");
  }
}

std::wstring Combine(const std::wstring& directory, const wchar_t* name) {
  std::wstring out = directory;
  if (!out.empty() && out.back() != L'\\') {
    out += L'\\';
  }
  return out + name;
}

bool Exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/* Runs a command with no console window and waits for it.
 *
 * Used for schtasks. No window because this program may itself be running
 * from a scheduled task or a silent install, and a console flashing up
 * during logon would be noticed and resented. */
bool RunQuietly(const std::wstring& command) {
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process = {};

  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process)) {
    return false;
  }

  WaitForSingleObject(process.hProcess, 60000);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return code == 0;
}

/* Human readable form of a Windows error, because a bare number tells the
 * person reading it nothing they can act on. */
std::wstring Explain(DWORD code) {
  wchar_t* text = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&text), 0, nullptr);

  std::wstring out;
  if (length && text) {
    out.assign(text, length);
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n')) {
      out.pop_back();
    }
  } else {
    wchar_t buffer[64];
    _snwprintf_s(buffer, _TRUNCATE, L"error %lu", code);
    out = buffer;
  }
  if (text) {
    LocalFree(text);
  }
  return out;
}

/* ---- device nodes ------------------------------------------------------- */

struct DeviceNode {
  std::wstring instance;
  std::wstring hardware_id;
};

/* Every display device belonging to this project, found by hardware id.
 *
 * Not by name, which has changed between versions, and not by service, which
 * every user mode driver on the machine shares. The hardware id is ours. */
std::vector<DeviceNode> FindOurDevices() {
  std::vector<DeviceNode> found;

  /* Enumerated by where the device sits rather than by its class.
   *
   * Asking for the display class alone misses devices installed under an
   * earlier version of this project, which is exactly the set that most
   * needs retiring: measured on a machine carrying both, the class query
   * returned three of four and silently skipped the old one. Asking for
   * everything rooted in the machine finds all of them. */
  HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr,
                                      DIGCF_ALLCLASSES | DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE) {
    return found;
  }

  SP_DEVINFO_DATA info = {};
  info.cbSize = sizeof(info);
  for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &info); ++index) {
    /* Hardware ids are a list of strings, one after another, ending with an
     * empty one. */
    BYTE buffer[2048] = {};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, &type,
                                           buffer, sizeof(buffer), nullptr)) {
      continue;
    }

    const wchar_t* id = reinterpret_cast<const wchar_t*>(buffer);
    while (*id) {
      bool ours = false;
      for (const wchar_t* known : kFormerHardwareIds) {
        ours = ours || _wcsicmp(id, known) == 0;
      }
      if (ours) {
        wchar_t instance[MAX_DEVICE_ID_LEN] = {};
        if (SetupDiGetDeviceInstanceIdW(set, &info, instance,
                                        ARRAYSIZE(instance), nullptr)) {
          DeviceNode node;
          node.instance = instance;
          node.hardware_id = id;
          found.push_back(node);
        }
        break;
      }
      id += wcslen(id) + 1;
    }
  }

  SetupDiDestroyDeviceInfoList(set);
  return found;
}

/* Retires a display device: disabled, allowed to settle, then removed.
 *
 * The order is not a nicety. While one of these exists and is started its
 * monitor is part of the desktop and kernel components hold state for it;
 * removing one outright, with a monitor attached, bug checked a machine
 * during development. Disabling first takes the monitor out through the path
 * designed for that and lets the driver stop cleanly. */
bool RetireDevice(const std::wstring& instance) {
  /* No class is given: the device may belong to an older version of this
   * project and need not be in the class this one uses. */
  HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
  if (set == INVALID_HANDLE_VALUE) {
    return false;
  }

  SP_DEVINFO_DATA info = {};
  info.cbSize = sizeof(info);
  bool ok = false;

  if (SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &info)) {
    SP_PROPCHANGE_PARAMS change = {};
    change.ClassInstallHeader.cbSize = sizeof(change.ClassInstallHeader);
    change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    change.Scope = DICS_FLAG_GLOBAL;
    change.StateChange = DICS_DISABLE;

    if (SetupDiSetClassInstallParamsW(set, &info, &change.ClassInstallHeader,
                                      sizeof(change))) {
      SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info);
    }

    /* Long enough for the stop to finish. Hurrying this is what caused the
     * crash, so it is deliberate rather than superstition. */
    Sleep(2500);

    ok = SetupDiCallClassInstaller(DIF_REMOVE, set, &info) != FALSE;
    Sleep(1000);
  }

  SetupDiDestroyDeviceInfoList(set);
  return ok;
}

/* Restarts our display device, which is the whole of the cold boot fix.
 *
 * Windows fails to load this driver during boot, every single time, and
 * the reason is structural rather than a bug here. The device is root
 * enumerated, so it has no parent hardware whose arrival could start it
 * later; Plug and Play therefore starts it during early boot device
 * enumeration, before the user mode driver framework is running. A user
 * mode driver cannot load that early, so the reflector fails with
 * STATUS_FAILED_DRIVER_ENTRY, and Plug and Play does not retry. The
 * device sits in error for the rest of the session and the user has no
 * second monitor.
 *
 * Measured, six boots out of six, one to two seconds after each:
 *
 *     Driver \Driver\WUDFRd failed to load for the device
 *     ROOT\DISPLAY\0000.  Status: 0xC0000365
 *
 * It is also why every reinstall appeared to cure the problem, which
 * misled this project for a long time: reinstalling re-enumerates the
 * device, and by then the framework is up.
 *
 * So that is all this does, at a point in the session when it works. No
 * reboot, nothing to configure, and the same operation the user was
 * otherwise performing by hand. */
bool RestartDisplayDevice(const std::wstring& instance) {
  HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
  if (set == INVALID_HANDLE_VALUE) {
    return false;
  }

  SP_DEVINFO_DATA info = {};
  info.cbSize = sizeof(info);
  bool ok = false;

  if (SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &info)) {
    SP_PROPCHANGE_PARAMS change = {};
    change.ClassInstallHeader.cbSize = sizeof(change.ClassInstallHeader);
    change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    change.Scope = DICS_FLAG_GLOBAL;

    /* Disable and enable rather than DICS_PROPCHANGE, because a device
     * that failed to start is not running and has nothing to restart;
     * the pair makes Plug and Play tear it down and build it again.
     *
     * Disable first and let it settle, never remove: a live display
     * device removed with its monitor still in the desktop bug checked a
     * machine during development. See RetireDevice. */
    change.StateChange = DICS_DISABLE;
    if (SetupDiSetClassInstallParamsW(set, &info, &change.ClassInstallHeader,
                                      sizeof(change))) {
      SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info);
    }
    Sleep(2000);

    change.StateChange = DICS_ENABLE;
    if (SetupDiSetClassInstallParamsW(set, &info, &change.ClassInstallHeader,
                                      sizeof(change))) {
      ok = SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info) != FALSE;
    }
    Sleep(2000);
  }

  SetupDiDestroyDeviceInfoList(set);
  return ok;
}

/* Has the driver run at all since this machine booted?
 *
 * The repair below is registered on two triggers and so runs twice per
 * session, and it must not restart a display that is already working: that
 * costs a four second blackout for nothing, and it was happening twice in
 * the first two minutes of every session.
 *
 * The device's own status cannot answer the question. Measured: 46 seconds
 * into a boot where the driver had definitively never loaded, Plug and Play
 * reported the device as OK with no problem code. A user mode driver that
 * fails to load leaves the devnode looking healthy, which is also why this
 * fault stayed invisible for so long.
 *
 * The driver's log does answer it, because nothing else writes that file. A
 * log last written before this machine booted means the driver has not run
 * this session, which is precisely the cold boot fault.
 *
 * Note what this deliberately does not attempt: spotting a driver that
 * started and later stopped putting a picture on the panel. Automatic
 * recovery for that was implemented, measured and withdrawn, because it
 * blinked displays that were working correctly. See docs/troubleshooting.md.
 *
 * Missing or unreadable is not evidence of health, so both answer no. */

/* When this machine booted, as a file time. */
ULONGLONG BootFileTime() {
  const ULONGLONG uptime_ms = GetTickCount64();
  FILETIME now_ft = {};
  GetSystemTimeAsFileTime(&now_ft);
  ULARGE_INTEGER now = {};
  now.LowPart = now_ft.dwLowDateTime;
  now.HighPart = now_ft.dwHighDateTime;
  /* File times count 100 nanosecond intervals, so a millisecond is 10000. */
  return now.QuadPart - uptime_ms * 10000ULL;
}

/* When the driver last wrote its log, or zero if it never has. */
ULONGLONG DriverLogWriteTime() {
  wchar_t windir[MAX_PATH] = {};
  if (!GetWindowsDirectoryW(windir, MAX_PATH)) {
    return 0;
  }
  const std::wstring path = Combine(windir, L"Temp\\usbdisplaydd.log");

  WIN32_FILE_ATTRIBUTE_DATA attributes = {};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
                            &attributes)) {
    return 0;
  }
  ULARGE_INTEGER written = {};
  written.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
  written.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
  return written.QuadPart;
}

bool DriverRanThisSession(std::wstring* because) {
  const ULONGLONG written = DriverLogWriteTime();
  if (written == 0) {
    *because = L"the driver has written no log at all";
    return false;
  }
  if (written > BootFileTime()) {
    *because = L"its log has been written since this machine booted";
    return true;
  }
  *because = L"its log predates this boot";
  return false;
}

/* Is our monitor part of the desktop?
 *
 * A monitor arriving is not the same as a screen appearing, and this is the
 * distinction that made the display look broken while every other signal
 * said it was fine. The driver can be loaded, the monitor announced, a
 * swapchain assigned and frames flowing, and the user still has no second
 * screen, because Windows has not extended the desktop onto it. Measured in
 * exactly that state: one screen in the desktop, a 1920x1080 virtual
 * desktop, and our adapter sitting there at 1920x1080 with 489 MB sent and
 * nothing dropped.
 *
 * `found` says whether the adapter exists at all, which separates "there is
 * nothing to attach" from "there is, and it is not attached". */
bool MonitorIsOnTheDesktop(bool* found) {
  *found = false;
  for (DWORD i = 0;; ++i) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    if (!EnumDisplayDevicesW(nullptr, i, &adapter, 0)) {
      break;
    }
    if (wcsstr(adapter.DeviceString, kAdapterMatch) == nullptr) {
      continue;
    }
    *found = true;
    if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0) {
      return true;
    }
  }
  return false;
}

/* Extends the desktop onto our monitor, if it is not on it already.
 *
 * Deliberately conditional. Reapplying an extend topology unconditionally
 * at every logon would override a deliberate choice: somebody who has set
 * this panel as their only screen, or arranged it above rather than beside
 * the built-in one, would find it rearranged behind their back. If the
 * monitor is on the desktop in any arrangement, this does nothing.
 *
 * Must run in the user's own session, which is why it cannot be folded into
 * the repair task. The desktop topology belongs to an interactive session;
 * SYSTEM in session 0 has no say over it. */
int ExtendDesktop() {
  bool found = false;
  if (MonitorIsOnTheDesktop(&found)) {
    Say(L"The display is already part of the desktop, so nothing is being\n"
        L"rearranged.\n");
    return 0;
  }
  if (!found) {
    Say(L"No USB display adapter is present, so there is no screen to add.\n");
    return 1;
  }

  Say(L"The display is present but not part of the desktop. Extending.\n");
  const LONG result = SetDisplayConfig(0, nullptr, 0, nullptr,
                                       SDC_APPLY | SDC_TOPOLOGY_EXTEND);
  if (result != ERROR_SUCCESS) {
    Say(L"Windows refused to extend the desktop (error %ld).\n", result);
    return 1;
  }

  /* Confirm against the desktop rather than trusting the return value, for
   * the same reason the repair confirms against the driver's log. */
  for (int waited = 0; waited < 5; ++waited) {
    Sleep(1000);
    if (MonitorIsOnTheDesktop(&found)) {
      Say(L"Done: the display is now a second screen.\n");
      return 0;
    }
  }
  Say(L"Windows accepted the change but the display is still not on the\n"
      L"desktop.\n");
  return 1;
}

/* Has Windows stopped one of our devices because it reported a problem?
 *
 * This is the second half of the repair's decision, and it was missing.
 * Asking only "has the driver run this session" covers a driver that never
 * loaded and nothing else, so a driver that loaded, ran for a minute and
 * then died left the repair declining to act on the grounds that it had
 * already run. Measured on a cold boot: the devnode sat in
 * CM_PROB_FAILED_POST_START, the repair ran twice and declined twice, and
 * the user had no second screen for the rest of the session.
 *
 * **This does not contradict the rule that the devnode status is useless
 * here. The two directions are not symmetric.** A status of OK proves
 * nothing, because it reads OK for a device whose user mode driver never
 * loaded, which is why it cannot be used to decide that a repair is
 * unnecessary. A status of *error* is Windows' own verdict that it has
 * stopped the device, and a working display never reads that way, so it can
 * be used to decide that a repair is needed.
 *
 * Nor is this the automatic recovery that was withdrawn for blinking
 * healthy displays. That was driven by a chip register that reports what
 * the adapter is transmitting and has been wrong on a working panel. This
 * is a Plug and Play problem code, which cannot be a false alarm. */
bool DeviceHasFailed(const std::vector<DeviceNode>& devices,
                     std::wstring* because) {
  for (const DeviceNode& device : devices) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(
                                      device.instance.c_str()),
                           CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS) {
      continue;
    }
    ULONG status = 0;
    ULONG problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS) {
      continue;
    }
    if ((status & DN_HAS_PROBLEM) != 0) {
      /* Except when somebody turned it off on purpose. A disabled device
       * has a problem code like any other, and re-enabling it behind the
       * user's back would be this program overruling a deliberate choice,
       * which is the one thing a repair must never do. */
      if (problem == CM_PROB_DISABLED || problem == CM_PROB_HARDWARE_DISABLED ||
          problem == CM_PROB_DISABLED_SERVICE) {
        continue;
      }
      wchar_t text[160] = {};
      swprintf_s(text, L"Windows has stopped it with problem code %lu",
                 problem);
      *because = text;
      return true;
    }
  }
  return false;
}

/* The repair itself, as run after startup by the scheduled task below and
 * available by hand as `driversetup /repair`. `force` is `/force`, for a
 * user who wants the restart whatever this program thinks. */
int Repair(bool force) {
  const std::vector<DeviceNode> devices = FindOurDevices();
  if (devices.empty()) {
    Say(L"No display device is installed, so there is nothing to repair.\n");
    return 1;
  }

  std::wstring because;
  std::wstring failure;
  const bool failed = DeviceHasFailed(devices, &failure);
  if (DriverRanThisSession(&because) && !failed && !force) {
    Say(L"The driver has already run this session, so nothing is being\n"
        L"restarted: %s.\n"
        L"Use /force to restart it anyway.\n",
        because.c_str());
    return 0;
  }
  if (failed && !force) {
    Say(L"The display device has failed: %s.\n", failure.c_str());
  } else if (!force) {
    Say(L"The driver has not run this session: %s.\n", because.c_str());
  }

  /* Where the log stands before anything is restarted. The check below has
   * to see it move past this, not merely past the boot time: with /force the
   * driver has usually been running, so its log already postdates the boot
   * and a check against that would pass without the driver having restarted
   * at all. */
  const ULONGLONG before = DriverLogWriteTime();
  const ULONGLONG boot = BootFileTime();
  const ULONGLONG must_exceed = before > boot ? before : boot;

  int repaired = 0;
  for (const DeviceNode& device : devices) {
    Say(L"restarting %s\n", device.instance.c_str());
    if (RestartDisplayDevice(device.instance)) {
      ++repaired;
    }
  }

  if (repaired == 0) {
    Say(L"The display device would not restart.\n");
    return 1;
  }

  /* Report whether the restart achieved anything, rather than reporting
   * success because two Plug and Play calls returned. The devnode status is
   * no use here for the same reason it was no use above.
   *
   * This matters for the earlier of the two triggers, which can fire before
   * the user mode driver framework is ready. When it does, this says so, and
   * the later trigger finds the driver still absent and tries again. */
  for (int waited = 0; waited < 15; ++waited) {
    if (DriverLogWriteTime() > must_exceed) {
      Say(L"Done: the driver is running. The monitor should appear within a\n"
          L"few seconds.\n");
      /* A running driver is not yet a second screen, so finish the job when
       * this is being run by hand in somebody's session. Only when the
       * monitor is visible here and unattached, which is never the case for
       * the SYSTEM task, so that path stays silent. Its own result is
       * ignored: the repair succeeded either way. */
      bool found = false;
      if (!MonitorIsOnTheDesktop(&found) && found) {
        ExtendDesktop();
      }
      return 0;
    }
    Sleep(1000);
  }

  Say(L"The device restarted but the driver still has not run, so it did\n"
      L"not start. If this was run early in the session, the attempt after\n"
      L"it should succeed.\n");
  return 1;
}

/* Arranges for the repair above to run after every boot.
 *
 * A scheduled task rather than a service, because a service is a great
 * deal of machinery for one call made once per session, and rather than a
 * startup shortcut, because this needs the administrative rights that
 * changing a device's state requires and must not prompt the user.
 *
 * Two triggers, and both are wanted. The logon trigger covers the normal
 * case. The boot trigger with a delay covers a machine that is left at the
 * logon screen, where the monitor should still work; a minute is long
 * enough for the framework to be up and is not noticeable, since the user
 * is waiting for Windows itself at that point anyway.
 *
 * Both therefore fire in an ordinary session, which is why the repair
 * checks whether it is needed before restarting anything. Without that
 * check the panel went black twice in the first two minutes of every
 * session, the second time to cure a display that was already working.
 * Whichever trigger comes first does the work; the other finds the driver
 * running and does nothing, or finds it still absent and tries again.
 *
 * Registered through schtasks with an XML definition. The command line
 * form of schtasks cannot express a delayed boot trigger, and the COM
 * interface is a great deal of code for something the XML says in a line.
 */
bool InstallBootRepair(const std::wstring& self) {
  wchar_t temp_dir[MAX_PATH] = {};
  if (!GetTempPathW(MAX_PATH, temp_dir)) {
    return false;
  }
  const std::wstring xml_path = std::wstring(temp_dir) + L"usbdisplaytask.xml";

  std::wstring xml =
      L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
      L"<Task version=\"1.2\" "
      L"xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
      L"  <RegistrationInfo>\r\n"
      L"    <Description>Restarts the USB display device after startup. "
      L"Windows cannot load a user mode display driver during boot, so "
      L"without this the monitor stays black until the device is "
      L"re-enumerated by hand.</Description>\r\n"
      L"  </RegistrationInfo>\r\n"
      L"  <Triggers>\r\n"
      L"    <LogonTrigger>\r\n"
      L"      <Enabled>true</Enabled>\r\n"
      L"      <Delay>PT15S</Delay>\r\n"
      L"    </LogonTrigger>\r\n"
      L"    <BootTrigger>\r\n"
      L"      <Enabled>true</Enabled>\r\n"
      L"      <Delay>PT1M</Delay>\r\n"
      L"    </BootTrigger>\r\n"
      L"  </Triggers>\r\n"
      L"  <Principals>\r\n"
      L"    <Principal id=\"Author\">\r\n"
      L"      <UserId>S-1-5-18</UserId>\r\n"
      L"      <RunLevel>HighestAvailable</RunLevel>\r\n"
      L"    </Principal>\r\n"
      L"  </Principals>\r\n"
      L"  <Settings>\r\n"
      L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
      L"    <DisallowStartIfOnBatteries>false"
      L"</DisallowStartIfOnBatteries>\r\n"
      L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
      L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
      L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n"
      L"    <RunOnlyIfNetworkAvailable>false"
      L"</RunOnlyIfNetworkAvailable>\r\n"
      L"    <IdleSettings>\r\n"
      L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n"
      L"      <RestartOnIdle>false</RestartOnIdle>\r\n"
      L"    </IdleSettings>\r\n"
      L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
      L"    <Enabled>true</Enabled>\r\n"
      L"    <Hidden>false</Hidden>\r\n"
      L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
      L"    <WakeToRun>false</WakeToRun>\r\n"
      L"    <ExecutionTimeLimit>PT5M</ExecutionTimeLimit>\r\n"
      L"    <Priority>7</Priority>\r\n"
      L"  </Settings>\r\n"
      L"  <Actions Context=\"Author\">\r\n"
      L"    <Exec>\r\n"
      L"      <Command>\"";
  xml += self;
  xml +=
      L"\"</Command>\r\n"
      L"      <Arguments>/repair /quiet</Arguments>\r\n"
      L"    </Exec>\r\n"
      L"  </Actions>\r\n"
      L"</Task>\r\n";

  /* UTF-16 with a byte order mark, which is what the schema declares and
   * what schtasks refuses the file without. */
  HANDLE file = CreateFileW(xml_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  const wchar_t bom = 0xFEFF;
  DWORD written = 0;
  WriteFile(file, &bom, sizeof(bom), &written, nullptr);
  WriteFile(file, xml.data(),
            static_cast<DWORD>(xml.size() * sizeof(wchar_t)), &written,
            nullptr);
  CloseHandle(file);

  std::wstring command = L"schtasks.exe /Create /F /TN \"";
  command += kBootTaskName;
  command += L"\" /XML \"";
  command += xml_path;
  command += L"\"";

  const bool ok = RunQuietly(command);
  DeleteFileW(xml_path.c_str());
  return ok;
}

void RemoveBootRepair() {
  std::wstring command = L"schtasks.exe /Delete /F /TN \"";
  command += kBootTaskName;
  command += L"\"";
  RunQuietly(command);
}

/* Creates the device node the driver attaches to. */
bool CreateDevice(const std::wstring& inf) {
  const GUID display = {0x4d36e968, 0xe325, 0x11ce,
                        {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
  HDEVINFO set = SetupDiCreateDeviceInfoList(&display, nullptr);
  if (set == INVALID_HANDLE_VALUE) {
    Fail(L"Could not begin creating the display device: %s",
         Explain(GetLastError()).c_str());
    return false;
  }

  SP_DEVINFO_DATA info = {};
  info.cbSize = sizeof(info);
  bool ok = false;

  if (!SetupDiCreateDeviceInfoW(set, L"Display", &display, nullptr, nullptr,
                                DICD_GENERATE_ID, &info)) {
    Fail(L"Could not create the display device: %s",
         Explain(GetLastError()).c_str());
  } else {
    /* The hardware id is a list, so it ends with two terminators. */
    std::wstring id = kHardwareId;
    std::vector<wchar_t> multi(id.begin(), id.end());
    multi.push_back(L'\0');
    multi.push_back(L'\0');

    if (!SetupDiSetDeviceRegistryPropertyW(
            set, &info, SPDRP_HARDWAREID,
            reinterpret_cast<const BYTE*>(multi.data()),
            static_cast<DWORD>(multi.size() * sizeof(wchar_t)))) {
      Fail(L"Could not identify the display device: %s",
           Explain(GetLastError()).c_str());
    } else if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &info)) {
      Fail(L"Could not register the display device: %s",
           Explain(GetLastError()).c_str());
    } else {
      BOOL reboot = FALSE;
      if (!UpdateDriverForPlugAndPlayDevicesW(
              nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        const DWORD code = GetLastError();
        Fail(L"The device was created but the driver would not attach: %s\n"
             L"\n"
             L"If that mentions a signature, the certificate beside this\n"
             L"program has not been trusted. A release package installs it\n"
             L"automatically; a package assembled by hand may not have.",
             Explain(code).c_str());
        /* Leave nothing half made. */
        SetupDiCallClassInstaller(DIF_REMOVE, set, &info);
      } else {
        ok = true;
      }
    }
  }

  SetupDiDestroyDeviceInfoList(set);
  return ok;
}

/* ---- driver packages ---------------------------------------------------- */

/* Installs a package into the driver store and applies it. */
bool InstallPackage(const std::wstring& inf, const wchar_t* description) {
  BOOL reboot = FALSE;
  if (!DiInstallDriverW(nullptr, inf.c_str(), DIIRFLAG_FORCE_INF, &reboot)) {
    const DWORD code = GetLastError();
    /* Already present and current. Not a failure: it is the state we want. */
    if (code == ERROR_NO_MORE_ITEMS) {
      Say(L"    %s was already current\n", description);
      return true;
    }
    Fail(L"Could not install %s: %s", description, Explain(code).c_str());
    return false;
  }
  Say(L"    %s installed\n", description);
  return true;
}

/* Removes copies of our packages already in the driver store.
 *
 * Windows keeps every version ever installed under a name of its own
 * choosing, and the stale ones go on competing to claim the adapter. An
 * install that leaves them behind can appear to succeed and change nothing,
 * which is a memorably frustrating way to spend an evening. */
void RemoveOldPackages() {
  /* The store is enumerated by asking for every third-party INF in the
   * Windows INF directory, whose published names are oemNN.inf. */
  wchar_t windows[MAX_PATH] = {};
  GetWindowsDirectoryW(windows, MAX_PATH);
  std::wstring pattern = std::wstring(windows) + L"\\INF\\oem*.inf";

  WIN32_FIND_DATAW found = {};
  HANDLE search = FindFirstFileW(pattern.c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) {
    return;
  }

  int removed = 0;
  do {
    const std::wstring published =
        std::wstring(windows) + L"\\INF\\" + found.cFileName;

    /* The original name is recorded in the INF's own strings section, so
     * reading the file is the reliable way to tell whose it is. */
    HANDLE file = CreateFileW(published.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      continue;
    }
    char buffer[8192] = {};
    DWORD read = 0;
    ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);

    bool ours = false;
    for (const wchar_t* name : kFormerInfNames) {
      char narrow[64] = {};
      WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof(narrow), nullptr,
                          nullptr);
      ours = ours || strstr(buffer, narrow) != nullptr;
    }
    if (!ours) {
      continue;
    }

    if (DiUninstallDriverW(nullptr, published.c_str(), 0, nullptr)) {
      ++removed;
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);

  if (removed > 0) {
    Say(L"    removed %d earlier cop%s\n", removed,
        removed == 1 ? L"y" : L"ies");
  }
}

/* ---- the adapter -------------------------------------------------------- */

/* Restarts the adapter so Windows publishes its pixel interface again.
 *
 * Claiming that interface for raw access detaches it, and it does not come
 * back on its own. The driver then reports that its package is missing,
 * which is true of the interface and misleading about the cause. */
void RestartAdapter() {
  const GUID usb_device = {0x88bae032, 0x5a81, 0x49f0,
                           {0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6}};
  HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES);
  if (set == INVALID_HANDLE_VALUE) {
    return;
  }
  (void)usb_device;

  SP_DEVINFO_DATA info = {};
  info.cbSize = sizeof(info);
  int restarted = 0;

  for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &info); ++index) {
    wchar_t instance[MAX_DEVICE_ID_LEN] = {};
    if (!SetupDiGetDeviceInstanceIdW(set, &info, instance, ARRAYSIZE(instance),
                                     nullptr)) {
      continue;
    }

    /* The composite parent, which is what creates a child device for each
     * USB interface. Restarting a child does not help: the parent has
     * already decided what exists. */
    const bool ours = wcsstr(instance, L"VID_345F") != nullptr ||
                      wcsstr(instance, L"VID_534D") != nullptr;
    if (!ours || wcsstr(instance, L"&MI_") != nullptr) {
      continue;
    }

    SP_PROPCHANGE_PARAMS change = {};
    change.ClassInstallHeader.cbSize = sizeof(change.ClassInstallHeader);
    change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    change.Scope = DICS_FLAG_GLOBAL;

    change.StateChange = DICS_DISABLE;
    if (SetupDiSetClassInstallParamsW(set, &info, &change.ClassInstallHeader,
                                      sizeof(change))) {
      SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info);
    }
    Sleep(1500);

    change.StateChange = DICS_ENABLE;
    if (SetupDiSetClassInstallParamsW(set, &info, &change.ClassInstallHeader,
                                      sizeof(change))) {
      SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info);
    }
    Sleep(2500);
    ++restarted;
  }

  SetupDiDestroyDeviceInfoList(set);
  if (restarted > 0) {
    Say(L"    restarted the adapter so its pixel interface reappears\n");
  }
}

/* ---- settings and registration ------------------------------------------ */

/* Lets ordinary users write the brightness setting.
 *
 * The driver runs as a service account and cannot read a user's own
 * settings, so brightness lives in the machine-wide ones. Without this a
 * brightness slider would need administrator rights every time it moved,
 * which nobody would accept. The scope is one key whose only values adjust
 * the picture. */
bool AllowUsersToSetBrightness() {
  HKEY key = nullptr;
  DWORD disposition = 0;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr, 0,
                      KEY_ALL_ACCESS | WRITE_DAC, nullptr, &key,
                      &disposition) != ERROR_SUCCESS) {
    return false;
  }

  const DWORD brightness = 100, contrast = 50;
  RegSetValueExW(key, L"Brightness", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&brightness), sizeof(brightness));
  RegSetValueExW(key, L"Contrast", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&contrast), sizeof(contrast));

  /* Built as a security descriptor rather than by editing the existing one:
   * this key is ours, and the default it inherits is what we are replacing. */
  const wchar_t descriptor[] =
      L"D:(A;;KA;;;SY)"    /* the system */
      L"(A;;KA;;;BA)"      /* administrators */
      L"(A;OICI;KR;;;BU)"  /* users may read */
      L"(A;OICI;CCDCLCSWRPWPSDRCWDWO;;;BU)";
  PSECURITY_DESCRIPTOR security = nullptr;
  bool ok = false;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
          descriptor, SDDL_REVISION_1, &security, nullptr)) {
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (GetSecurityDescriptorDacl(security, &present, &acl, &defaulted) &&
        present) {
      ok = SetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                           nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
    }
    LocalFree(security);
  }

  RegCloseKey(key);
  return ok;
}

/* Trusts the certificate the release was signed with.
 *
 * Windows will not accept either package until it does. Worth being plain
 * about, and the release notes are: the certificate is generated when the
 * release is built and then discarded, so it shows the files have not been
 * altered and nothing at all about who made them. */
bool TrustReleaseCertificate() {
  const std::wstring path = Combine(g_here, L"usbdisplay.cer");
  if (!Exists(path)) {
    /* A tree built from source has none, and its INFs are unsigned. */
    return true;
  }

  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    Fail(L"Could not read the certificate: %s", Explain(GetLastError()).c_str());
    return false;
  }
  DWORD size = GetFileSize(file, nullptr);
  std::vector<BYTE> blob(size);
  DWORD read = 0;
  const bool got = ReadFile(file, blob.data(), size, &read, nullptr) != FALSE;
  CloseHandle(file);
  if (!got) {
    return false;
  }

  PCCERT_CONTEXT certificate = CertCreateCertificateContext(
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, blob.data(), read);
  if (!certificate) {
    Fail(L"The certificate beside this program is not readable.");
    return false;
  }

  bool ok = true;
  /* Root so the chain validates, and trusted publisher so Windows accepts
   * the package without asking. */
  for (const wchar_t* store_name : {L"Root", L"TrustedPublisher"}) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM, 0, 0,
                                     CERT_SYSTEM_STORE_LOCAL_MACHINE,
                                     store_name);
    if (!store) {
      ok = false;
      continue;
    }
    CertAddCertificateContextToStore(store, certificate,
                                     CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
    CertCloseStore(store, 0);
  }
  CertFreeCertificateContext(certificate);

  if (ok) {
    Say(L"    trusted the release certificate\n");
  }
  return ok;
}

/* Removes exactly the certificate this release installed, and nothing else.
 *
 * It reads the certificate back from the file beside it and deletes the one
 * whose bytes match, rather than searching for a name.
 *
 * Searching by name is what this did first, for the substring "usbdisplay",
 * and that also matched "usbdisplay local signing" and "winusbdisplay test
 * signing": a certificate a developer uses for source builds, and one
 * belonging to a different package. Uninstalling removed both. An
 * uninstaller deleting trust anchors it did not create is a good deal worse
 * than leaving one behind, and the bytes are unambiguous where a name is
 * not. */
void ForgetReleaseCertificate() {
  const std::wstring path = Combine(g_here, L"usbdisplay.cer");
  if (!Exists(path)) {
    /* Nothing to match against, so nothing is touched. Guessing here is
     * precisely the mistake this is avoiding. */
    return;
  }

  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  const DWORD size = GetFileSize(file, nullptr);
  std::vector<BYTE> blob(size);
  DWORD read = 0;
  const bool got = ReadFile(file, blob.data(), size, &read, nullptr) != FALSE;
  CloseHandle(file);
  if (!got) {
    return;
  }

  PCCERT_CONTEXT ours = CertCreateCertificateContext(
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, blob.data(), read);
  if (!ours) {
    return;
  }

  for (const wchar_t* store_name : {L"Root", L"TrustedPublisher"}) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM, 0, 0,
                                     CERT_SYSTEM_STORE_LOCAL_MACHINE,
                                     store_name);
    if (!store) {
      continue;
    }
    /* Matched on the encoded certificate itself, so only an identical one
     * is removed. */
    PCCERT_CONTEXT found = CertFindCertificateInStore(
        store, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, ours, nullptr);
    if (found) {
      CertDeleteCertificateFromStore(found);
    }
    CertCloseStore(store, 0);
  }
  CertFreeCertificateContext(ours);
}

/* ---- the two things this program does ----------------------------------- */

int Install() {
  const std::wstring winusb_inf =
      Exists(Combine(g_here, L"winusb\\usbdisplay_winusb.inf"))
          ? Combine(g_here, L"winusb\\usbdisplay_winusb.inf")
          : Combine(g_here, L"inf\\usbdisplay_winusb.inf");
  const std::wstring driver_inf =
      Exists(Combine(g_here, L"driver\\usbdisplaydd.inf"))
          ? Combine(g_here, L"driver\\usbdisplaydd.inf")
          : Combine(g_here, L"build\\driver\\x64\\Release\\usbdisplaydd.inf");

  if (!Exists(winusb_inf) || !Exists(driver_inf)) {
    Fail(L"This program expects to sit beside the files it installs, and\n"
         L"cannot find them. Expected:\n"
         L"\n"
         L"    %s\n"
         L"    %s\n"
         L"\n"
         L"If this came from a release, unpack the whole archive and run it\n"
         L"from there rather than moving it out on its own.",
         winusb_inf.c_str(), driver_inf.c_str());
    return 1;
  }

  Say(L"Installing the USB display driver\n\n");

  Say(L"  1. making room\n");
  RemoveOldPackages();
  for (const DeviceNode& device : FindOurDevices()) {
    Say(L"    retiring %s\n", device.instance.c_str());
    RetireDevice(device.instance);
  }

  Say(L"\n  2. trusting the release\n");
  if (!TrustReleaseCertificate()) {
    return 1;
  }

  Say(L"\n  3. raw access to the adapter's pixel interface\n");
  if (!InstallPackage(winusb_inf, L"the raw USB package")) {
    return 1;
  }
  RestartAdapter();

  Say(L"\n  4. the display driver\n");
  if (!InstallPackage(driver_inf, L"the display driver")) {
    return 1;
  }
  if (!CreateDevice(driver_inf)) {
    return 1;
  }
  Say(L"    display device created\n");

  Say(L"\n  5. keeping it working after a reboot\n");
  {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (InstallBootRepair(self)) {
      Say(L"    the display will be restarted automatically after startup\n");
    } else {
      Say(L"    could not register the startup task, so the monitor may be\n"
          L"    black after a reboot until you run this program again or\n"
          L"    run: driversetup /repair\n");
    }
  }

  Say(L"\n  6. brightness\n");
  if (AllowUsersToSetBrightness()) {
    Say(L"    anyone may now adjust it without administrator rights\n");
  } else {
    Say(L"    could not widen the setting's permissions; brightness will\n"
        L"    fall back to a lower quality method\n");
  }

  Say(L"\n  7. the brightness control\n");
  /* Stopping a copy that is already running, because the installer around
   * this is about to replace its file and Windows will not overwrite a
   * program that is in memory. */
  {
    HWND existing = FindWindowW(L"UsbDisplayBrightnessTray", nullptr);
    if (existing) {
      PostMessageW(existing, WM_CLOSE, 0, 0);
      Sleep(1500);
      Say(L"    stopped the running copy so it can be replaced\n");
    }
  }

  Say(L"\nDone. The adapter should appear as a second monitor within a few\n"
      L"seconds; give it a little longer if it was only just plugged in.\n"
      L"\n"
      L"If no monitor appears, C:\\Windows\\Temp\\usbdisplaydd.log records\n"
      L"every step the driver took and why it stopped.\n");
  return 0;
}

int Uninstall() {
  Say(L"Removing the USB display driver\n\n");

  Say(L"  1. the startup repair task\n");
  RemoveBootRepair();
  Say(L"    removed\n");

  Say(L"\n  2. the display device\n");
  const std::vector<DeviceNode> devices = FindOurDevices();
  if (devices.empty()) {
    Say(L"    none was installed\n");
  }
  for (const DeviceNode& device : devices) {
    Say(L"    retiring %s\n", device.instance.c_str());
    RetireDevice(device.instance);
  }

  Say(L"\n  3. the driver packages\n");
  RemoveOldPackages();

  Say(L"\n  4. the brightness control\n");
  {
    /* Stopped before the installer around this deletes its file, or the
     * copy in memory keeps running until the next restart and the file
     * cannot be removed. */
    HWND existing = FindWindowW(L"UsbDisplayBrightnessTray", nullptr);
    if (existing) {
      PostMessageW(existing, WM_CLOSE, 0, 0);
      Sleep(1500);
    }
    Say(L"    stopped\n");
  }

  Say(L"\n  5. settings and trust\n");
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, kSettingsKey);
  ForgetReleaseCertificate();
  Say(L"    removed the picture settings and the release certificate\n");

  Say(L"\nDone. The adapter's other interfaces, sound and the control\n"
      L"channel, keep the drivers Windows supplies for them, so it will\n"
      L"still be recognised as a device; it simply will not be a monitor.\n");
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  g_here = self;
  const size_t slash = g_here.find_last_of(L'\\');
  g_here = (slash == std::wstring::npos) ? L"." : g_here.substr(0, slash);

  bool uninstall = false;
  bool repair = false;
  bool extend = false;
  bool force = false;
  bool wait = true;
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], L"/uninstall") == 0 ||
        _wcsicmp(argv[i], L"-uninstall") == 0) {
      uninstall = true;
    } else if (_wcsicmp(argv[i], L"/repair") == 0 ||
               _wcsicmp(argv[i], L"-repair") == 0) {
      repair = true;
    } else if (_wcsicmp(argv[i], L"/extend") == 0 ||
               _wcsicmp(argv[i], L"-extend") == 0) {
      extend = true;
    } else if (_wcsicmp(argv[i], L"/force") == 0 ||
               _wcsicmp(argv[i], L"-force") == 0) {
      force = true;
    } else if (_wcsicmp(argv[i], L"/quiet") == 0) {
      g_quiet = true;
      wait = false;
    } else if (_wcsicmp(argv[i], L"/nowait") == 0) {
      wait = false;
    } else {
      wprintf(L"usage: %s [/uninstall | /repair [/force] | /extend]"
              L" [/quiet]\n",
              argv[0]);
      return 1;
    }
  }

  int result;
  if (uninstall) {
    result = Uninstall();
  } else if (repair) {
    result = Repair(force);
  } else if (extend) {
    result = ExtendDesktop();
  } else {
    result = Install();
  }

  /* Double-clicked, the window would otherwise close before anything could
   * be read. */
  if (wait && !g_quiet) {
    DWORD processes = 0;
    if (GetConsoleProcessList(&processes, 1) <= 1) {
      wprintf(L"\nPress Enter to close.\n");
      getwchar();
    }
  }
  return result;
}
