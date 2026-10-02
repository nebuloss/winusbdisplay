/* SPDX-License-Identifier: GPL-2.0-only */

#include "usb.h"

#include <setupapi.h>
#include <winusb.h>

#include <initguid.h>
#include <devpkey.h>

extern "C" {
#include <hidsdi.h>
}

#include <cstdio>
#include <cstring>
#include <cwctype>

#include "proto.h"

namespace usbdisplay {
namespace {

/* Must match DeviceInterfaceGUIDs in inf/usbdisplay_winusb.inf. Deliberately
 * different from the frozen driver's, so both packages can be installed
 * without each finding the other's device. */
// {C32EB32B-5AAA-41C2-90E3-380CC3B0E031}
const GUID kDataInterfaceGuid = {
    0xc32eb32b,
    0x5aaa,
    0x41c2,
    {0x90, 0xe3, 0x38, 0x0c, 0xc3, 0xb0, 0xe0, 0x31}};

constexpr unsigned long kBulkTimeoutMs = 5000;

bool ParseVidPid(const std::wstring& path, uint16_t* vid, uint16_t* pid) {
  std::wstring lower;
  lower.reserve(path.size());
  for (wchar_t c : path) {
    lower.push_back(static_cast<wchar_t>(towlower(c)));
  }
  const size_t vpos = lower.find(L"vid_");
  const size_t ppos = lower.find(L"pid_");
  if (vpos == std::wstring::npos || ppos == std::wstring::npos) {
    return false;
  }
  *vid = static_cast<uint16_t>(wcstoul(lower.c_str() + vpos + 4, nullptr, 16));
  *pid = static_cast<uint16_t>(wcstoul(lower.c_str() + ppos + 4, nullptr, 16));
  return true;
}

bool IsMacroSilicon(uint16_t vid) {
  return vid == kVendorIdUsb2 || vid == kVendorIdUsb3;
}

/* Every interface of one physical device shares a container id, which is the
 * only reliable way to tell which HID collection belongs to which display
 * interface when two adapters are plugged in. */
bool ReadContainerId(HDEVINFO set, SP_DEVINFO_DATA* devinfo, GUID* out) {
  DEVPROPTYPE type = 0;
  GUID value = {};
  DWORD size = 0;
  if (!SetupDiGetDevicePropertyW(set, devinfo, &DEVPKEY_Device_ContainerId,
                                 &type, reinterpret_cast<PBYTE>(&value),
                                 sizeof(value), &size, 0) ||
      type != DEVPROP_TYPE_GUID) {
    return false;
  }
  *out = value;
  return true;
}

/* Walks the interfaces of one class, handing each path and container id to
 * the caller. Collapses a chunk of SetupAPI ceremony that would otherwise
 * appear twice. */
template <typename Fn>
void ForEachInterface(const GUID& class_guid, Fn&& callback) {
  HDEVINFO set = SetupDiGetClassDevsW(&class_guid, nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) {
    return;
  }

  SP_DEVICE_INTERFACE_DATA iface;
  iface.cbSize = sizeof(iface);
  for (DWORD index = 0;
       SetupDiEnumDeviceInterfaces(set, nullptr, &class_guid, index, &iface);
       ++index) {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
    if (!needed) {
      continue;
    }
    std::vector<uint8_t> buffer(needed);
    auto* detail =
        reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    SP_DEVINFO_DATA devinfo;
    devinfo.cbSize = sizeof(devinfo);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr,
                                          &devinfo)) {
      continue;
    }

    GUID container = {};
    const bool has_container = ReadContainerId(set, &devinfo, &container);
    callback(std::wstring(detail->DevicePath), container, has_container);
  }

  SetupDiDestroyDeviceInfoList(set);
}

}  // namespace

void Link::SetPlatformError(const char* what, unsigned long code) {
  char buffer[512];
  char* text = nullptr;
  DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&text), 0, nullptr);
  if (length && text) {
    while (length && (text[length - 1] == '\r' || text[length - 1] == '\n')) {
      text[--length] = '\0';
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
  error_ = buffer;
}

UsbLink::~UsbLink() {
  if (winusb_) {
    WinUsb_Free(static_cast<WINUSB_INTERFACE_HANDLE>(winusb_));
  }
  if (winusb_file_ && winusb_file_ != INVALID_HANDLE_VALUE) {
    CloseHandle(winusb_file_);
  }
  if (hid_ && hid_ != INVALID_HANDLE_VALUE) {
    CloseHandle(hid_);
  }
}

std::vector<UsbLink::Interface> UsbLink::Enumerate() {
  std::vector<Interface> found;

  GUID hid_guid;
  HidD_GetHidGuid(&hid_guid);
  ForEachInterface(hid_guid, [&](const std::wstring& path,
                                 const GUID& container, bool has_container) {
    Interface entry;
    entry.path = path;
    if (!ParseVidPid(path, &entry.vid, &entry.pid) ||
        !IsMacroSilicon(entry.vid)) {
      return;
    }
    entry.container = container;
    entry.has_container = has_container;
    entry.is_hid = true;
    found.push_back(entry);
  });

  ForEachInterface(kDataInterfaceGuid, [&](const std::wstring& path,
                                           const GUID& container,
                                           bool has_container) {
    Interface entry;
    entry.path = path;
    ParseVidPid(path, &entry.vid, &entry.pid);
    entry.container = container;
    entry.has_container = has_container;
    entry.is_hid = false;
    found.push_back(entry);
  });

  return found;
}

std::unique_ptr<UsbLink> UsbLink::Open(bool require_panel, std::string* error) {
  const std::vector<Interface> interfaces = Enumerate();

  std::vector<const Interface*> control;
  std::vector<const Interface*> data;
  for (const Interface& entry : interfaces) {
    (entry.is_hid ? control : data).push_back(&entry);
  }

  if (control.empty()) {
    if (error) {
      *error =
          "no MacroSilicon HID interface present. Check the dongle is "
          "plugged in with: pnputil /enum-devices /connected";
    }
    return nullptr;
  }

  std::string reason = "no usable interface";
  for (const Interface* hid : control) {
    std::unique_ptr<UsbLink> link(new UsbLink());
    if (!link->OpenHid(hid->path, &reason)) {
      continue;
    }
    link->vid_ = hid->vid;
    link->pid_ = hid->pid;

    /* Prefer the data interface on the same physical device. Fall back to a
     * lone unmatched one only when neither carries a container id, which
     * happens on some hubs and is harmless with a single adapter. */
    const Interface* chosen = nullptr;
    for (const Interface* candidate : data) {
      if (hid->has_container && candidate->has_container &&
          IsEqualGUID(hid->container, candidate->container)) {
        chosen = candidate;
        break;
      }
    }
    if (!chosen && data.size() == 1 && control.size() == 1) {
      chosen = data.front();
    }

    if (chosen) {
      std::string data_error;
      if (!link->OpenWinUsb(chosen->path, &data_error) && require_panel) {
        reason = data_error;
        continue;
      }
    } else if (require_panel) {
      reason =
          "the display interface is not bound to WinUSB. Install "
          "inf/usbdisplay_winusb.inf, or run without the data plane for the "
          "read-only commands";
      continue;
    }

    return link;
  }

  if (error) {
    *error = reason;
  }
  return nullptr;
}

bool UsbLink::OpenHid(const std::wstring& path, std::string* error) {
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    /* Feature reports do not actually need read or write access, and some
     * collections refuse to grant it. A zero access handle still works. */
    handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  }
  if (handle == INVALID_HANDLE_VALUE) {
    *error = "could not open the HID control interface";
    return false;
  }

  HIDP_CAPS caps;
  memset(&caps, 0, sizeof(caps));
  PHIDP_PREPARSED_DATA preparsed = nullptr;
  if (HidD_GetPreparsedData(handle, &preparsed)) {
    HidP_GetCaps(preparsed, &caps);
    HidD_FreePreparsedData(preparsed);
  }

  /* Windows puts the report id in front of the payload, so the buffer is one
   * byte longer than the eight bytes that reach the wire. A collection that
   * cannot carry that is one of the dongle's other HID interfaces. */
  if (caps.FeatureReportByteLength < kControlSize + 1) {
    CloseHandle(handle);
    char buffer[192];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                "HID collection carries no usable feature report "
                "(length %u, need at least %zu)",
                static_cast<unsigned>(caps.FeatureReportByteLength),
                kControlSize + 1);
    *error = buffer;
    return false;
  }

  hid_ = handle;
  feature_report_size_ = caps.FeatureReportByteLength;
  feature_scratch_.resize(feature_report_size_);
  return true;
}

bool UsbLink::OpenWinUsb(const std::wstring& path, std::string* error) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    char buffer[256];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                "could not open the display interface: 0x%08lX%s", code,
                code == ERROR_ACCESS_DENIED
                    ? ". The pixel pipe is exclusive, so the driver and the "
                      "tool cannot both hold it; disable one of them"
                    : "");
    *error = buffer;
    return false;
  }

  WINUSB_INTERFACE_HANDLE winusb = nullptr;
  if (!WinUsb_Initialize(file, &winusb)) {
    const DWORD code = GetLastError();
    CloseHandle(file);
    char buffer[128];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                "WinUsb_Initialize failed: 0x%08lX", code);
    *error = buffer;
    return false;
  }

  USB_INTERFACE_DESCRIPTOR descriptor;
  if (!WinUsb_QueryInterfaceSettings(winusb, 0, &descriptor)) {
    WinUsb_Free(winusb);
    CloseHandle(file);
    *error = "WinUsb_QueryInterfaceSettings failed";
    return false;
  }

  for (UCHAR i = 0; i < descriptor.bNumEndpoints; ++i) {
    WINUSB_PIPE_INFORMATION pipe;
    if (!WinUsb_QueryPipe(winusb, 0, i, &pipe)) {
      continue;
    }
    const bool is_in = (pipe.PipeId & 0x80) != 0;
    if (pipe.PipeType != UsbdPipeTypeBulk || is_in ||
        (pipe.PipeId & 0x0F) != kBulkOutEndpoint) {
      continue;
    }

    ULONG timeout = kBulkTimeoutMs;
    WinUsb_SetPipePolicy(winusb, pipe.PipeId, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
    /* The chip is told a transfer has ended by an explicit zero length
     * packet, which Chip::SendFrame sends. Letting WinUSB append its own on
     * every short transfer would send a second one. */
    UCHAR off = FALSE;
    WinUsb_SetPipePolicy(winusb, pipe.PipeId, SHORT_PACKET_TERMINATE,
                         sizeof(off), &off);
    UCHAR on = TRUE;
    WinUsb_SetPipePolicy(winusb, pipe.PipeId, AUTO_CLEAR_STALL, sizeof(on),
                         &on);

    winusb_file_ = file;
    winusb_ = winusb;
    bulk_pipe_ = pipe.PipeId;
    bulk_packet_size_ = pipe.MaximumPacketSize;

    /* Start from a known state, whatever the last owner of this pipe left
     * behind.
     *
     * The chip reads the bulk stream as a sequence of blocks: a header
     * saying how many pixels follow, then the pixels, then a zero length
     * packet. A driver that is killed partway through a frame, which is
     * what happens on every reinstall and on any crash, leaves the chip
     * waiting for the rest of a block that will never arrive. The next
     * driver's first header is then consumed as the tail of that block
     * and every frame after it is misread.
     *
     * That is invisible from everywhere else: control is HID on another
     * interface entirely, so the registers all answer correctly, the
     * output reports itself enabled, and every write succeeds. The panel
     * just stays dark, and until this reset the only cure was to unplug
     * the adapter.
     *
     * Reset clears the halt condition and the data toggle, and the zero
     * length packet after it closes whatever block the chip still thinks
     * is open. */
    WinUsb_ResetPipe(winusb, bulk_pipe_);
    ULONG written = 0;
    WinUsb_WritePipe(winusb, bulk_pipe_, nullptr, 0, &written, nullptr);
    return true;
  }

  WinUsb_Free(winusb);
  CloseHandle(file);
  char buffer[160];
  _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
              "the display interface has no bulk OUT pipe on endpoint %u "
              "(it reports %u endpoints)",
              kBulkOutEndpoint, descriptor.bNumEndpoints);
  *error = buffer;
  return false;
}

std::string UsbLink::Describe() const {
  char buffer[256];
  if (winusb_) {
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                "%04X:%04X, control over HID, pixels over WinUSB pipe 0x%02X "
                "(%u byte packets)",
                vid_, pid_, bulk_pipe_, bulk_packet_size_);
  } else {
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                "%04X:%04X, control over HID, no data plane", vid_, pid_);
  }
  return buffer;
}

bool UsbLink::ControlWrite(const uint8_t* payload) {
  memset(feature_scratch_.data(), 0, feature_scratch_.size());
  feature_scratch_[0] = 0; /* report id */
  memcpy(feature_scratch_.data() + 1, payload, kControlSize);
  if (!HidD_SetFeature(hid_, feature_scratch_.data(),
                       static_cast<ULONG>(feature_scratch_.size()))) {
    SetPlatformError("HidD_SetFeature", GetLastError());
    return false;
  }
  return true;
}

bool UsbLink::ControlRead(uint8_t* payload) {
  memset(feature_scratch_.data(), 0, feature_scratch_.size());
  feature_scratch_[0] = 0; /* report id */
  if (!HidD_GetFeature(hid_, feature_scratch_.data(),
                       static_cast<ULONG>(feature_scratch_.size()))) {
    SetPlatformError("HidD_GetFeature", GetLastError());
    return false;
  }
  memcpy(payload, feature_scratch_.data() + 1, kControlSize);
  return true;
}

bool UsbLink::BulkWrite(const uint8_t* data, size_t len) {
  if (!winusb_) {
    SetError("no data plane: the display interface is not bound to WinUSB");
    return false;
  }

  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_);
  ULONG written = 0;
  if (!WinUsb_WritePipe(handle, bulk_pipe_, const_cast<uint8_t*>(data),
                        static_cast<ULONG>(len), &written, nullptr)) {
    SetPlatformError("WinUsb_WritePipe", GetLastError());
    /* A stalled or timed out pipe stays that way. Without resetting it here
     * every later write fails the same way and the panel never comes back;
     * before this the only recovery was to unplug the dongle. */
    WinUsb_AbortPipe(handle, bulk_pipe_);
    WinUsb_ResetPipe(handle, bulk_pipe_);
    return false;
  }
  if (written != len) {
    SetError("short bulk write");
    WinUsb_ResetPipe(handle, bulk_pipe_);
    return false;
  }
  return true;
}

bool UsbLink::StillPresent() const {
  /* Answered from what Windows publishes rather than by asking the adapter.
   * A control exchange with hardware that has just been pulled can block
   * until it times out. */
  for (const Interface& entry : Enumerate()) {
    if (!entry.is_hid) {
      return true;
    }
  }
  return false;
}

void UsbLink::Cancel() {
  if (!winusb_) {
    return;
  }
  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_);

  /* Abort, then reset, then close the block.
   *
   * Aborting alone stops the host sending and leaves the chip waiting for
   * the rest of a block it has already started reading. The next driver
   * to open this pipe then has its first frame header eaten as the tail
   * of that block, and nothing it sends afterwards is ever interpreted
   * correctly. Reset clears the endpoint and the zero length packet ends
   * the block, so what is left behind is a chip ready for a new one. */
  WinUsb_AbortPipe(handle, bulk_pipe_);
  WinUsb_ResetPipe(handle, bulk_pipe_);
  ULONG written = 0;
  WinUsb_WritePipe(handle, bulk_pipe_, nullptr, 0, &written, nullptr);
}

std::unique_ptr<FileLink> FileLink::Open(const std::string& directory,
                                         std::string* error) {
  if (!CreateDirectoryA(directory.c_str(), nullptr) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    if (error) {
      *error = "could not create the dump directory";
    }
    return nullptr;
  }
  std::unique_ptr<FileLink> link(new FileLink());
  link->directory_ = directory;
  return link;
}

std::string FileLink::Describe() const {
  return "loopback, writing to " + directory_;
}

bool FileLink::ControlWrite(const uint8_t* payload) {
  char path[MAX_PATH];
  _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\control.log",
              directory_.c_str());
  FILE* file = nullptr;
  if (fopen_s(&file, path, "ab") != 0 || !file) {
    SetError("could not append to the control log");
    return false;
  }
  for (size_t i = 0; i < kControlSize; ++i) {
    fprintf(file, "%02X%s", payload[i], i + 1 == kControlSize ? "\n" : " ");
  }
  fclose(file);
  return true;
}

bool FileLink::ControlRead(uint8_t* payload) {
  memset(payload, 0, kControlSize);
  return true;
}

bool FileLink::BulkWrite(const uint8_t* data, size_t len) {
  char path[MAX_PATH];
  _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\frame%04u.bin",
              directory_.c_str(), sequence_++);
  FILE* file = nullptr;
  if (fopen_s(&file, path, "wb") != 0 || !file) {
    SetError("could not write the frame dump");
    return false;
  }
  const bool ok = fwrite(data, 1, len, file) == len;
  fclose(file);
  if (!ok) {
    SetError("short write to the frame dump");
  }
  return ok;
}

}  // namespace usbdisplay
