/* SPDX-License-Identifier: GPL-2.0-only */

#include "winusb_transport.h"

#include <windows.h>

#include <setupapi.h>
#include <winusb.h>

#include <cstdio>
#include <cstring>

#include "ms912x_proto.h"

namespace ms912x {
namespace {

/* Must match the DeviceInterfaceGUID in inf/ms912x_winusb.inf. */
// {6C2C4F1E-3B7A-4D4F-9F2E-5A1B8C7D3E90}
const GUID kDeviceInterfaceGuid = {
    0x6c2c4f1e,
    0x3b7a,
    0x4d4f,
    {0x9f, 0x2e, 0x5a, 0x1b, 0x8c, 0x7d, 0x3e, 0x90}};

constexpr uint8_t kRequestTypeClassInterfaceOut = 0x21;
constexpr uint8_t kRequestTypeClassInterfaceIn = 0xA1;
constexpr unsigned long kBulkTimeoutMs = 5000;
constexpr unsigned long kControlTimeoutMs = 1000;

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

const char* PipeTypeName(USBD_PIPE_TYPE type) {
  switch (type) {
    case UsbdPipeTypeControl:
      return "control";
    case UsbdPipeTypeIsochronous:
      return "isochronous";
    case UsbdPipeTypeBulk:
      return "bulk";
    case UsbdPipeTypeInterrupt:
      return "interrupt";
    default:
      return "unknown";
  }
}

}  // namespace

WinUsbTransport::~WinUsbTransport() {
  for (auto& slot : slots_) {
    if (slot.overlapped.hEvent) {
      CloseHandle(slot.overlapped.hEvent);
    }
  }
  if (winusb_handle_) {
    WinUsb_Free(static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_));
  }
  if (file_handle_ && file_handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(file_handle_));
  }
}

std::vector<DeviceLocation> WinUsbTransport::Enumerate() {
  std::vector<DeviceLocation> found;
  HDEVINFO info =
      SetupDiGetClassDevsW(&kDeviceInterfaceGuid, nullptr, nullptr,
                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (info == INVALID_HANDLE_VALUE) {
    return found;
  }

  SP_DEVICE_INTERFACE_DATA iface;
  iface.cbSize = sizeof(iface);
  for (DWORD index = 0; SetupDiEnumDeviceInterfaces(
           info, nullptr, &kDeviceInterfaceGuid, index, &iface);
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
    ParseVidPid(loc.path, &loc.vid, &loc.pid);
    loc.instance = loc.path;
    found.push_back(loc);
  }
  SetupDiDestroyDeviceInfoList(info);
  return found;
}

std::unique_ptr<WinUsbTransport> WinUsbTransport::Open(std::string* error) {
  std::vector<DeviceLocation> devices = Enumerate();
  if (devices.empty()) {
    if (error) {
      *error =
          "no WinUSB-bound MacroSilicon display interface found; install "
          "inf/ms912x_winusb.inf first";
    }
    return nullptr;
  }
  std::string last;
  for (const DeviceLocation& loc : devices) {
    std::unique_ptr<WinUsbTransport> transport = OpenPath(loc.path, &last);
    if (transport) {
      return transport;
    }
  }
  if (error) {
    *error = last;
  }
  return nullptr;
}

std::unique_ptr<WinUsbTransport> WinUsbTransport::OpenPath(
    const std::wstring& path, std::string* error) {
  HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error) {
      char buf[128];
      _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                  "CreateFileW on the WinUSB interface failed: 0x%08lX",
                  GetLastError());
      *error = buf;
    }
    return nullptr;
  }

  WINUSB_INTERFACE_HANDLE winusb = nullptr;
  if (!WinUsb_Initialize(file, &winusb)) {
    DWORD code = GetLastError();
    CloseHandle(file);
    if (error) {
      char buf[128];
      _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                  "WinUsb_Initialize failed: 0x%08lX", code);
      *error = buf;
    }
    return nullptr;
  }

  std::unique_ptr<WinUsbTransport> transport(new WinUsbTransport());
  transport->file_handle_ = file;
  transport->winusb_handle_ = winusb;
  transport->location_.path = path;
  ParseVidPid(path, &transport->location_.vid, &transport->location_.pid);

  std::string pipe_error;
  if (!transport->FindBulkOutPipe(&pipe_error)) {
    if (error) {
      *error = pipe_error;
    }
    return nullptr;
  }
  return transport;
}

bool WinUsbTransport::FindBulkOutPipe(std::string* error) {
  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_);
  USB_INTERFACE_DESCRIPTOR descriptor;
  if (!WinUsb_QueryInterfaceSettings(handle, 0, &descriptor)) {
    if (error) {
      *error = "WinUsb_QueryInterfaceSettings failed";
    }
    return false;
  }

  for (UCHAR i = 0; i < descriptor.bNumEndpoints; ++i) {
    WINUSB_PIPE_INFORMATION pipe;
    if (!WinUsb_QueryPipe(handle, 0, i, &pipe)) {
      continue;
    }
    if (pipe.PipeType != UsbdPipeTypeBulk ||
        (pipe.PipeId & 0x80) != 0 /* direction OUT */) {
      continue;
    }
    if ((pipe.PipeId & 0x0F) != kBulkOutEndpoint) {
      continue;
    }
    bulk_out_pipe_id_ = pipe.PipeId;
    bulk_max_packet_ = pipe.MaximumPacketSize;

    ULONG timeout = kBulkTimeoutMs;
    WinUsb_SetPipePolicy(handle, pipe.PipeId, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
    UCHAR off = FALSE;
    WinUsb_SetPipePolicy(handle, pipe.PipeId, SHORT_PACKET_TERMINATE,
                         sizeof(off), &off);
    UCHAR on = TRUE;
    WinUsb_SetPipePolicy(handle, pipe.PipeId, AUTO_CLEAR_STALL, sizeof(on),
                         &on);
    return true;
  }

  if (error) {
    char buf[128];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "no bulk OUT pipe on endpoint %u (interface has %u endpoints)",
                kBulkOutEndpoint, descriptor.bNumEndpoints);
    *error = buf;
  }
  return false;
}

std::string WinUsbTransport::Describe() const {
  char buf[160];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE,
              "WinUSB, %04X:%04X, bulk OUT pipe 0x%02X, max packet %u",
              location_.vid, location_.pid, bulk_out_pipe_id_,
              bulk_max_packet_);
  return buf;
}

bool WinUsbTransport::ControlSetReport(const uint8_t* data, size_t len) {
  if (len != kControlPayloadSize) {
    SetError("control payload must be 8 bytes");
    return false;
  }
  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_);
  ULONG timeout = kControlTimeoutMs;
  WinUsb_SetPipePolicy(handle, 0, PIPE_TRANSFER_TIMEOUT, sizeof(timeout),
                       &timeout);

  WINUSB_SETUP_PACKET setup;
  setup.RequestType = kRequestTypeClassInterfaceOut;
  setup.Request = kHidReqSetReport;
  setup.Value = kHidReportValue;
  setup.Index = kHidReportIndex;
  setup.Length = static_cast<USHORT>(len);

  uint8_t scratch[kControlPayloadSize];
  memcpy(scratch, data, len);

  ULONG transferred = 0;
  if (!WinUsb_ControlTransfer(handle, setup, scratch,
                              static_cast<ULONG>(len), &transferred, nullptr)) {
    SetWin32Error("WinUsb_ControlTransfer (SET_REPORT)", GetLastError());
    return false;
  }
  if (transferred != len) {
    SetError("short SET_REPORT control transfer");
    return false;
  }
  return true;
}

bool WinUsbTransport::ControlGetReport(uint8_t* data, size_t len) {
  if (len != kControlPayloadSize) {
    SetError("control payload must be 8 bytes");
    return false;
  }
  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_);

  WINUSB_SETUP_PACKET setup;
  setup.RequestType = kRequestTypeClassInterfaceIn;
  setup.Request = kHidReqGetReport;
  setup.Value = kHidReportValue;
  setup.Index = kHidReportIndex;
  setup.Length = static_cast<USHORT>(len);

  ULONG transferred = 0;
  if (!WinUsb_ControlTransfer(handle, setup, data, static_cast<ULONG>(len),
                              &transferred, nullptr)) {
    SetWin32Error("WinUsb_ControlTransfer (GET_REPORT)", GetLastError());
    return false;
  }
  if (transferred != len) {
    SetError("short GET_REPORT control transfer");
    return false;
  }
  return true;
}

bool WinUsbTransport::BulkWrite(const uint8_t* data, size_t len) {
  if (pipeline_depth_ > 1 && len > chunk_bytes_) {
    return BulkWritePipelined(data, len);
  }

  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_);
  ULONG transferred = 0;
  if (!WinUsb_WritePipe(handle, bulk_out_pipe_id_,
                        const_cast<uint8_t*>(data), static_cast<ULONG>(len),
                        &transferred, nullptr)) {
    SetWin32Error("WinUsb_WritePipe", GetLastError());
    /* A timed out or stalled pipe stays that way until it is reset, so
     * without this every subsequent write fails too and the display never
     * comes back. */
    WinUsb_AbortPipe(handle, bulk_out_pipe_id_);
    WinUsb_ResetPipe(handle, bulk_out_pipe_id_);
    return false;
  }
  if (transferred != len) {
    SetError("short bulk write");
    WinUsb_ResetPipe(handle, bulk_out_pipe_id_);
    return false;
  }
  return true;
}

bool WinUsbTransport::EnablePipelining(unsigned depth, size_t chunk_bytes) {
  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_);

  const size_t packet = bulk_max_packet_ ? bulk_max_packet_ : 512;
  /* RAW_IO demands whole packets. Round down so every chunk is legal. */
  chunk_bytes = (chunk_bytes / packet) * packet;
  if (depth < 1 || chunk_bytes == 0) {
    return false;
  }

  ULONG transfer_size = static_cast<ULONG>(chunk_bytes);
  WinUsb_SetPipePolicy(handle, bulk_out_pipe_id_, MAXIMUM_TRANSFER_SIZE,
                       sizeof(transfer_size), &transfer_size);

  UCHAR on = TRUE;
  if (!WinUsb_SetPipePolicy(handle, bulk_out_pipe_id_, RAW_IO, sizeof(on),
                            &on)) {
    SetWin32Error("WinUsb_SetPipePolicy(RAW_IO)", GetLastError());
    return false;
  }

  /* Under RAW_IO a timeout policy is not honoured, so clear it rather than
   * leave a value that silently does nothing. */
  ULONG timeout = 0;
  WinUsb_SetPipePolicy(handle, bulk_out_pipe_id_, PIPE_TRANSFER_TIMEOUT,
                       sizeof(timeout), &timeout);

  for (auto& slot : slots_) {
    if (slot.overlapped.hEvent) {
      CloseHandle(slot.overlapped.hEvent);
    }
  }
  slots_.assign(depth, PipelineSlot());
  for (auto& slot : slots_) {
    slot.overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!slot.overlapped.hEvent) {
      SetWin32Error("CreateEvent", GetLastError());
      return false;
    }
  }

  pipeline_depth_ = depth;
  chunk_bytes_ = chunk_bytes;
  return true;
}

bool WinUsbTransport::BulkWritePipelined(const uint8_t* data, size_t len) {
  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_);
  const size_t packet = bulk_max_packet_ ? bulk_max_packet_ : 512;

  size_t offset = 0;
  size_t next_slot = 0;
  bool failed = false;

  /* Submit chunks round robin, waiting on a slot only when it is needed
   * again. That keeps `pipeline_depth_` transfers queued in the host
   * controller at all times. */
  while (offset < len && !failed) {
    PipelineSlot& slot = slots_[next_slot];

    if (slot.busy) {
      ULONG done = 0;
      if (!WinUsb_GetOverlappedResult(handle, &slot.overlapped, &done, TRUE)) {
        SetWin32Error("WinUsb_GetOverlappedResult", GetLastError());
        failed = true;
        break;
      }
      slot.busy = false;
    }

    size_t chunk = len - offset;
    if (chunk > chunk_bytes_) {
      chunk = chunk_bytes_;
    }
    /* Every chunk but the final one must be whole packets. */
    if (offset + chunk < len) {
      chunk = (chunk / packet) * packet;
    }

    ResetEvent(slot.overlapped.hEvent);
    ULONG transferred = 0;
    if (!WinUsb_WritePipe(handle, bulk_out_pipe_id_,
                          const_cast<uint8_t*>(data) + offset,
                          static_cast<ULONG>(chunk), &transferred,
                          &slot.overlapped)) {
      if (GetLastError() != ERROR_IO_PENDING) {
        SetWin32Error("WinUsb_WritePipe", GetLastError());
        failed = true;
        break;
      }
    }
    slot.busy = true;
    offset += chunk;
    next_slot = (next_slot + 1) % slots_.size();
  }

  /* Always drain, even after a failure, so no transfer is left referencing
   * the caller's buffer. */
  for (auto& slot : slots_) {
    if (!slot.busy) {
      continue;
    }
    ULONG done = 0;
    if (!WinUsb_GetOverlappedResult(handle, &slot.overlapped, &done, TRUE)) {
      if (!failed) {
        SetWin32Error("WinUsb_GetOverlappedResult (drain)", GetLastError());
        failed = true;
      }
    }
    slot.busy = false;
  }

  if (failed) {
    WinUsb_ResetPipe(handle, bulk_out_pipe_id_);
    return false;
  }
  return true;
}

void WinUsbTransport::CancelTransfers() {
  if (!winusb_handle_) {
    return;
  }
  /* Aborts everything queued on the pipe and completes it with
   * ERROR_OPERATION_ABORTED, so a blocked writer returns at once. */
  WinUsb_AbortPipe(static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_),
                   bulk_out_pipe_id_);
}

bool WinUsbTransport::DumpBosDescriptor(std::string* out) {
  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_);
  constexpr UCHAR kBosDescriptorType = 0x0F;
  constexpr UCHAR kSuperSpeedCapability = 0x03;

  uint8_t header[5] = {0};
  ULONG transferred = 0;
  if (!WinUsb_GetDescriptor(handle, kBosDescriptorType, 0, 0, header,
                            sizeof(header), &transferred) ||
      transferred < sizeof(header)) {
    return false;
  }

  const uint16_t total = static_cast<uint16_t>(header[2] | (header[3] << 8));
  if (total < sizeof(header) || total > 512) {
    return false;
  }

  std::vector<uint8_t> bos(total);
  if (!WinUsb_GetDescriptor(handle, kBosDescriptorType, 0, 0, bos.data(),
                            total, &transferred) ||
      transferred < total) {
    return false;
  }

  char line[200];
  _snprintf_s(line, sizeof(line), _TRUNCATE,
              "bos:       present, %u byte(s), %u capability descriptor(s)\n",
              total, bos[4]);
  *out = line;

  for (size_t i = 5; i + 2 < bos.size(); i += bos[i]) {
    if (bos[i] == 0) {
      break;
    }
    if (bos[i + 1] == 0x10 && bos[i + 2] == kSuperSpeedCapability) {
      const uint16_t speeds =
          static_cast<uint16_t>(bos[i + 4] | (bos[i + 5] << 8));
      _snprintf_s(line, sizeof(line), _TRUNCATE,
                  "  SuperSpeed capability: speed mask 0x%04X%s\n", speeds,
                  (speeds & 0x08) ? " (5 Gbps supported)" : "");
      *out += line;
      *out +=
          "  -> the silicon is USB 3 capable. Connecting at high speed means\n"
          "     the port, cable or hub in the path is limiting it.\n";
    }
  }
  return true;
}

bool WinUsbTransport::DumpDescriptors(std::string* out) {
  auto handle = static_cast<WINUSB_INTERFACE_HANDLE>(winusb_handle_);
  char line[256];
  out->clear();

  USB_DEVICE_DESCRIPTOR device;
  ULONG transferred = 0;
  if (WinUsb_GetDescriptor(handle, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                           reinterpret_cast<PUCHAR>(&device), sizeof(device),
                           &transferred)) {
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "device %04X:%04X usb %X.%02X class %02X configurations %u\n",
                device.idVendor, device.idProduct, device.bcdUSB >> 8,
                device.bcdUSB & 0xFF, device.bDeviceClass,
                device.bNumConfigurations);
    *out += line;
  }

  for (UCHAR setting = 0;; ++setting) {
    USB_INTERFACE_DESCRIPTOR descriptor;
    if (!WinUsb_QueryInterfaceSettings(handle, setting, &descriptor)) {
      break;
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "interface %u alt %u class %02X subclass %02X protocol %02X "
                "endpoints %u\n",
                descriptor.bInterfaceNumber, descriptor.bAlternateSetting,
                descriptor.bInterfaceClass, descriptor.bInterfaceSubClass,
                descriptor.bInterfaceProtocol, descriptor.bNumEndpoints);
    *out += line;

    for (UCHAR i = 0; i < descriptor.bNumEndpoints; ++i) {
      WINUSB_PIPE_INFORMATION pipe;
      if (!WinUsb_QueryPipe(handle, setting, i, &pipe)) {
        continue;
      }
      _snprintf_s(line, sizeof(line), _TRUNCATE,
                  "  pipe 0x%02X %-11s dir %s max packet %u interval %u\n",
                  pipe.PipeId, PipeTypeName(pipe.PipeType),
                  (pipe.PipeId & 0x80) ? "IN " : "OUT",
                  pipe.MaximumPacketSize, pipe.Interval);
      *out += line;
    }
  }
  return !out->empty();
}

}  // namespace ms912x
