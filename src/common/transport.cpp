/* SPDX-License-Identifier: GPL-2.0-only */

#include "transport.h"

#include <windows.h>

#include <setupapi.h>

#include <initguid.h>
#include <devpkey.h>

#include <cstdio>
#include <vector>

namespace ms912x {

bool GetContainerIdForInterface(const std::wstring& interface_path,
                                const GUID& interface_class, GUID* container) {
  HDEVINFO info = SetupDiGetClassDevsW(&interface_class, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (info == INVALID_HANDLE_VALUE) {
    return false;
  }

  bool found = false;
  SP_DEVICE_INTERFACE_DATA iface;
  iface.cbSize = sizeof(iface);
  for (DWORD index = 0; !found && SetupDiEnumDeviceInterfaces(
           info, nullptr, &interface_class, index, &iface);
       ++index) {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &needed,
                                     nullptr);
    if (!needed) {
      continue;
    }
    std::vector<uint8_t> buffer(needed);
    auto* detail =
        reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    SP_DEVINFO_DATA devinfo;
    devinfo.cbSize = sizeof(devinfo);
    if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, needed,
                                          nullptr, &devinfo)) {
      continue;
    }
    if (_wcsicmp(detail->DevicePath, interface_path.c_str()) != 0) {
      continue;
    }

    DEVPROPTYPE type = 0;
    GUID value = {};
    DWORD size = 0;
    if (SetupDiGetDevicePropertyW(info, &devinfo, &DEVPKEY_Device_ContainerId,
                                  &type, reinterpret_cast<PBYTE>(&value),
                                  sizeof(value), &size, 0) &&
        type == DEVPROP_TYPE_GUID) {
      *container = value;
      found = true;
    }
  }

  SetupDiDestroyDeviceInfoList(info);
  return found;
}

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
