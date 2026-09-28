/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The installer.
 *
 * This exists because a driver that arrives as a folder of files and a script
 * is not installed, it is merely present. What people expect, reasonably, is
 * something they can double-click, that asks for permission once, says what
 * it is doing, and afterwards appears in the list of installed programs where
 * it can be removed again. That is what this does.
 *
 * Written as an ordinary program rather than a script for three reasons.
 * It can carry a manifest, so Windows raises the permission prompt itself
 * instead of the user having to know to ask. It calls the installation
 * interfaces directly, so it needs none of the developer tools the scripts
 * reach for. And its error messages can say what actually went wrong, which
 * matters more here than usual: almost every failure mode of this hardware
 * looks like silence.
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
 *
 *   6. Register with the installed programs list, so removing it later is
 *      the ordinary Windows gesture rather than hunting for a script.
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
const wchar_t kUninstallKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\usbdisplay";
const wchar_t kSettingsKey[] = L"SOFTWARE\\usbdisplay";

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

/* Where the tools are copied to, so they survive the downloaded archive
 * being deleted. */
std::wstring ToolsDirectory() {
  wchar_t programs[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"ProgramFiles", programs, MAX_PATH)) {
    return std::wstring();
  }
  return std::wstring(programs) + L"\\usbdisplay";
}

/* Copies the console tool and the brightness control somewhere permanent,
 * and arranges for the latter to start with Windows.
 *
 * Leaving them in the unpacked archive is what most installers of this shape
 * do and it is a poor bargain: the archive goes in the wastebasket, the
 * brightness control vanishes with it, and the next reboot has no way to
 * start something that is no longer there. */
bool InstallTools() {
  const std::wstring directory = ToolsDirectory();
  if (directory.empty()) {
    return false;
  }
  CreateDirectoryW(directory.c_str(), nullptr);

  const wchar_t* names[] = {L"usbdisplaytray.exe", L"usbdisplayctl.exe",
                            L"brightnessprobe.exe"};
  int copied = 0;
  for (const wchar_t* name : names) {
    std::wstring source = Combine(Combine(g_here, L"tools"), name);
    if (!Exists(source)) {
      source = Combine(g_here, name);
    }
    if (!Exists(source)) {
      continue;
    }
    if (CopyFileW(source.c_str(), Combine(directory, name).c_str(), FALSE)) {
      ++copied;
    }
  }
  if (copied == 0) {
    Say(L"    none were found beside this program\n");
    return false;
  }

  /* Started with Windows, per user, because that is whose brightness it is
   * and it needs no privileges to do the job. */
  const std::wstring tray = Combine(directory, L"usbdisplaytray.exe");
  if (Exists(tray)) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) == ERROR_SUCCESS) {
      const std::wstring quoted = L"\"" + tray + L"\"";
      RegSetValueExW(key, L"usbdisplay brightness", 0, REG_SZ,
                     reinterpret_cast<const BYTE*>(quoted.c_str()),
                     static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
      RegCloseKey(key);
    }

    /* Started now as well, so brightness works without a reboot. */
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::wstring command = L"\"" + tray + L"\"";
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup,
                       &process)) {
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
  }

  Say(L"    installed to %s and set to start with Windows\n",
      directory.c_str());
  return true;
}

/* Puts this in the list of installed programs, so removing it later is the
 * ordinary gesture rather than finding a script. */
void RegisterForUninstall() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    return;
  }

  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const std::wstring remove = std::wstring(L"\"") + self + L"\" /uninstall";

  const struct {
    const wchar_t* name;
    const wchar_t* value;
  } strings[] = {
      {L"DisplayName", L"USB Display driver"},
      {L"DisplayVersion", L"0.1.0"},
      {L"Publisher", L"usbdisplay"},
      {L"UninstallString", remove.c_str()},
      {L"QuietUninstallString", remove.c_str()},
      {L"InstallLocation", g_here.c_str()},
      {L"URLInfoAbout", L"https://github.com/nebuloss/winusbdisplay"},
  };
  for (const auto& entry : strings) {
    RegSetValueExW(key, entry.name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(entry.value),
                   static_cast<DWORD>((wcslen(entry.value) + 1) *
                                      sizeof(wchar_t)));
  }

  const DWORD no = 0;
  RegSetValueExW(key, L"NoModify", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&no), sizeof(no));
  RegSetValueExW(key, L"NoRepair", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&no), sizeof(no));
  RegCloseKey(key);
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

void ForgetReleaseCertificate() {
  for (const wchar_t* store_name : {L"Root", L"TrustedPublisher"}) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM, 0, 0,
                                     CERT_SYSTEM_STORE_LOCAL_MACHINE,
                                     store_name);
    if (!store) {
      continue;
    }
    PCCERT_CONTEXT found = nullptr;
    while ((found = CertFindCertificateInStore(
                store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR,
                L"usbdisplay", found)) != nullptr) {
      PCCERT_CONTEXT duplicate = CertDuplicateCertificateContext(found);
      CertDeleteCertificateFromStore(duplicate);
      found = nullptr; /* the enumeration is invalidated by the delete */
    }
    CertCloseStore(store, 0);
  }
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

  Say(L"\n  5. brightness\n");
  if (AllowUsersToSetBrightness()) {
    Say(L"    anyone may now adjust it without administrator rights\n");
  } else {
    Say(L"    could not widen the setting's permissions; brightness will\n"
        L"    fall back to a lower quality method\n");
  }

  Say(L"\n  6. the brightness control\n");
  InstallTools();

  RegisterForUninstall();

  Say(L"\nDone. The adapter should appear as a second monitor within a few\n"
      L"seconds; give it a little longer if it was only just plugged in.\n"
      L"\n"
      L"Run tools\\usbdisplaytray.exe for a brightness control, and\n"
      L"right-click its icon to have it start with Windows.\n"
      L"\n"
      L"If no monitor appears, C:\\Windows\\Temp\\usbdisplaydd.log records\n"
      L"every step the driver took and why it stopped.\n");
  return 0;
}

int Uninstall() {
  Say(L"Removing the USB display driver\n\n");

  Say(L"  1. the display device\n");
  const std::vector<DeviceNode> devices = FindOurDevices();
  if (devices.empty()) {
    Say(L"    none was installed\n");
  }
  for (const DeviceNode& device : devices) {
    Say(L"    retiring %s\n", device.instance.c_str());
    RetireDevice(device.instance);
  }

  Say(L"\n  2. the driver packages\n");
  RemoveOldPackages();

  Say(L"\n  3. the brightness control\n");
  {
    /* Stopped before its files are removed, or the copy in memory keeps
     * running until the next reboot and the files cannot be deleted. */
    HWND existing = FindWindowW(L"UsbDisplayBrightnessTray", nullptr);
    if (existing) {
      PostMessageW(existing, WM_CLOSE, 0, 0);
      Sleep(1500);
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
      RegDeleteValueW(key, L"usbdisplay brightness");
      RegCloseKey(key);
    }

    const std::wstring directory = ToolsDirectory();
    if (!directory.empty()) {
      for (const wchar_t* name : {L"usbdisplaytray.exe", L"usbdisplayctl.exe",
                                  L"brightnessprobe.exe"}) {
        DeleteFileW(Combine(directory, name).c_str());
      }
      RemoveDirectoryW(directory.c_str());
    }
    Say(L"    stopped and removed\n");
  }

  Say(L"\n  4. settings and trust\n");
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, kSettingsKey);
  ForgetReleaseCertificate();
  Say(L"    removed the picture settings and the release certificate\n");

  RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);

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
  bool wait = true;
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], L"/uninstall") == 0 ||
        _wcsicmp(argv[i], L"-uninstall") == 0) {
      uninstall = true;
    } else if (_wcsicmp(argv[i], L"/quiet") == 0) {
      g_quiet = true;
      wait = false;
    } else if (_wcsicmp(argv[i], L"/nowait") == 0) {
      wait = false;
    } else {
      wprintf(L"usage: %s [/uninstall] [/quiet]\n", argv[0]);
      return 1;
    }
  }

  const int result = uninstall ? Uninstall() : Install();

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
