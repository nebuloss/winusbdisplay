/* SPDX-License-Identifier: GPL-2.0-only */

#include "hid_transport.h"

#include <windows.h>

#include <setupapi.h>

extern "C" {
#include <hidsdi.h>
}

#include <cstdio>
#include <cstring>

#include "ms912x_proto.h"

namespace ms912x {
namespace {

bool ParseVidPid(const std::wstring& path, uint16_t* vid, uint16_t* pid) {
  std::wstring lower;
  lower.reserve(path.size());
  for (wchar_t c : path) {
    lower.push_back(static_cast<wchar_t>(towlower(c)));
  }
  size_t vpos = lower.find(L"vid_");
  size_t ppos = lower.find(L"pid_");
  if (vpos == std::wstring::npos || ppos == std::wstring::npos) {
    return false;
  }
  *vid = static_cast<uint16_t>(wcstoul(lower.c_str() + vpos + 4, nullptr, 16));
  *pid = static_cast<uint16_t>(wcstoul(lower.c_str() + ppos + 4, nullptr, 16));
  return true;
}

bool IsMacroSilicon(uint16_t vid) {
  return vid == kVidMacroSiliconUsb2 || vid == kVidMacroSiliconUsb3;
}

}  // namespace

HidTransport::~HidTransport() {
  if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(handle_));
  }
}

std::vector<DeviceLocation> HidTransport::Enumerate() {
  std::vector<DeviceLocation> found;
  GUID hid_guid;
  HidD_GetHidGuid(&hid_guid);

  HDEVINFO info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (info == INVALID_HANDLE_VALUE) {
    return found;
  }

  SP_DEVICE_INTERFACE_DATA iface;
  iface.cbSize = sizeof(iface);
  for (DWORD index = 0;
       SetupDiEnumDeviceInterfaces(info, nullptr, &hid_guid, index, &iface);
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
    if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, needed, nullptr,
                                          nullptr)) {
      continue;
    }

    DeviceLocation loc;
    loc.path = detail->DevicePath;
    if (!ParseVidPid(loc.path, &loc.vid, &loc.pid) || !IsMacroSilicon(loc.vid)) {
      continue;
    }
    loc.instance = loc.path;
    loc.has_container_id =
        GetContainerIdForInterface(loc.path, hid_guid, &loc.container_id);
    found.push_back(loc);
  }

  SetupDiDestroyDeviceInfoList(info);
  return found;
}

std::unique_ptr<HidTransport> HidTransport::OpenForContainer(
    const GUID& container, std::string* error) {
  std::vector<DeviceLocation> devices = Enumerate();
  std::string last = "no MacroSilicon HID interface matched the device";
  for (const DeviceLocation& loc : devices) {
    if (!loc.has_container_id ||
        !IsEqualGUID(loc.container_id, container)) {
      continue;
    }
    std::unique_ptr<HidTransport> transport = OpenPath(loc.path, &last);
    if (transport) {
      return transport;
    }
  }
  if (error) {
    *error = last;
  }
  return nullptr;
}

std::unique_ptr<HidTransport> HidTransport::Open(std::string* error) {
  std::vector<DeviceLocation> devices = Enumerate();
  if (devices.empty()) {
    if (error) {
      *error =
          "no MacroSilicon HID interface found; is the dongle plugged in?";
    }
    return nullptr;
  }
  std::string last;
  for (const DeviceLocation& loc : devices) {
    std::unique_ptr<HidTransport> transport = OpenPath(loc.path, &last);
    if (transport) {
      return transport;
    }
  }
  if (error) {
    *error = last;
  }
  return nullptr;
}

std::unique_ptr<HidTransport> HidTransport::OpenPath(const std::wstring& path,
                                                     std::string* error) {
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    /* Some collections refuse read/write; a zero access handle is still
     * enough for HidD_SetFeature on most stacks. */
    handle = CreateFileW(path.c_str(), 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  }
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "CreateFileW on the HID interface failed";
    }
    return nullptr;
  }

  PHIDP_PREPARSED_DATA preparsed = nullptr;
  HIDP_CAPS caps;
  memset(&caps, 0, sizeof(caps));
  if (HidD_GetPreparsedData(handle, &preparsed)) {
    HidP_GetCaps(preparsed, &caps);
    HidD_FreePreparsedData(preparsed);
  }

  /* The dongle's control collection carries 8 byte feature reports. With
   * report id 0 the Windows buffer is one byte longer than the wire payload. */
  size_t report_len = caps.FeatureReportByteLength;
  if (report_len < kControlPayloadSize + 1) {
    CloseHandle(handle);
    if (error) {
      char buf[160];
      _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                  "HID collection has no usable feature report "
                  "(FeatureReportByteLength=%u, need >= %zu)",
                  static_cast<unsigned>(caps.FeatureReportByteLength),
                  kControlPayloadSize + 1);
      *error = buf;
    }
    return nullptr;
  }

  std::unique_ptr<HidTransport> transport(new HidTransport());
  transport->handle_ = handle;
  transport->feature_report_len_ = report_len;
  transport->scratch_.resize(report_len);
  transport->location_.path = path;
  ParseVidPid(path, &transport->location_.vid, &transport->location_.pid);
  return transport;
}

std::string HidTransport::Describe() const {
  char buf[128];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE,
              "HID control plane, %04X:%04X, feature report %zu bytes",
              location_.vid, location_.pid, feature_report_len_);
  return buf;
}

bool HidTransport::ControlSetReport(const uint8_t* data, size_t len) {
  if (len != kControlPayloadSize) {
    SetError("control payload must be 8 bytes");
    return false;
  }
  memset(scratch_.data(), 0, scratch_.size());
  scratch_[0] = 0; /* report id */
  memcpy(scratch_.data() + 1, data, len);
  if (!HidD_SetFeature(static_cast<HANDLE>(handle_), scratch_.data(),
                       static_cast<ULONG>(scratch_.size()))) {
    SetWin32Error("HidD_SetFeature", GetLastError());
    return false;
  }
  return true;
}

bool HidTransport::ControlGetReport(uint8_t* data, size_t len) {
  if (len != kControlPayloadSize) {
    SetError("control payload must be 8 bytes");
    return false;
  }
  memset(scratch_.data(), 0, scratch_.size());
  scratch_[0] = 0; /* report id */
  if (!HidD_GetFeature(static_cast<HANDLE>(handle_), scratch_.data(),
                       static_cast<ULONG>(scratch_.size()))) {
    SetWin32Error("HidD_GetFeature", GetLastError());
    return false;
  }
  memcpy(data, scratch_.data() + 1, len);
  return true;
}

bool HidTransport::BulkWrite(const uint8_t*, size_t) {
  SetError("the HID transport has no data plane; use the WinUSB transport");
  return false;
}

}  // namespace ms912x
