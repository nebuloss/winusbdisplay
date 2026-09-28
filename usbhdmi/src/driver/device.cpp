/* SPDX-License-Identifier: GPL-2.0-only */

#include "device.h"

#include <algorithm>
#include <cstring>

#include "log.h"

namespace usbhdmi {
namespace {

/* Roughly CVT reduced blanking. The exact numbers matter less than the fact
 * that there are some: the timing fields have to satisfy
 *
 *     pixelRate = totalSize.cx * totalSize.cy * vSyncFreq
 *     hSyncFreq = pixelRate / totalSize.cx
 *
 * and reporting a total size equal to the active size, which is the obvious
 * thing to do for a device with no real blanking interval, makes those
 * identities false. Windows then rejects the whole display topology with a
 * generic failure that gives no hint as to why. */
constexpr int kHorizontalBlanking = 160;
constexpr int kVerticalBlanking = 45;

DISPLAYCONFIG_VIDEO_SIGNAL_INFO MakeSignalInfo(int width, int height, int hz) {
  const UINT32 h_total = static_cast<UINT32>(width + kHorizontalBlanking);
  const UINT32 v_total = static_cast<UINT32>(height + kVerticalBlanking);

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO info = {};
  info.activeSize.cx = static_cast<UINT32>(width);
  info.activeSize.cy = static_cast<UINT32>(height);
  info.totalSize.cx = h_total;
  info.totalSize.cy = v_total;

  info.vSyncFreq.Numerator = static_cast<UINT32>(hz);
  info.vSyncFreq.Denominator = 1;
  info.hSyncFreq.Numerator = static_cast<UINT32>(hz) * v_total;
  info.hSyncFreq.Denominator = 1;
  info.pixelRate = static_cast<UINT64>(h_total) * v_total * hz;

  info.AdditionalSignalInfo.videoStandard = 255; /* other */
  info.AdditionalSignalInfo.vSyncFreqDivider = 1;
  info.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  return info;
}

IDDCX_MONITOR_MODE MakeMonitorMode(const Mode& mode,
                                   IDDCX_MONITOR_MODE_ORIGIN origin) {
  IDDCX_MONITOR_MODE out = {};
  out.Size = sizeof(out);
  out.Origin = origin;
  out.MonitorVideoSignalInfo = MakeSignalInfo(mode.width, mode.height, mode.hz);
  /* Monitor modes require a zero divider; only target modes may set one. */
  out.MonitorVideoSignalInfo.AdditionalSignalInfo.vSyncFreqDivider = 0;
  return out;
}

IDDCX_TARGET_MODE MakeTargetMode(const Mode& mode) {
  IDDCX_TARGET_MODE out = {};
  out.Size = sizeof(out);
  out.TargetVideoSignalInfo.targetVideoSignalInfo =
      MakeSignalInfo(mode.width, mode.height, mode.hz);
  /* Asking the compositor to draw less often than the panel refreshes looks
   * like the designed answer for a link that cannot carry full rate frames.
   * Windows rejects any value above one outright, with correct timings and
   * everything else in order, so pacing is done by dropping updates instead. */
  return out;
}

}  // namespace

void BuildFallbackEdid(std::vector<uint8_t>* edid, int width, int height,
                       int hz) {
  edid->assign(128, 0);
  uint8_t* e = edid->data();

  static const uint8_t kMagic[8] = {0x00, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0x00};
  memcpy(e, kMagic, sizeof(kMagic));

  const uint16_t manufacturer = static_cast<uint16_t>(
      ((('M' - 'A' + 1) & 0x1F) << 10) | ((('S' - 'A' + 1) & 0x1F) << 5) |
      (('D' - 'A' + 1) & 0x1F));
  e[8] = static_cast<uint8_t>(manufacturer >> 8);
  e[9] = static_cast<uint8_t>(manufacturer);
  e[10] = 0x01; /* product code */
  e[16] = 0x01; /* week */
  e[17] = 36;   /* year */
  e[18] = 0x01; /* EDID 1.4 */
  e[19] = 0x04;
  e[20] = 0x80; /* digital input */
  e[21] = 52;   /* about 24 inches at 16:9 */
  e[22] = 29;
  e[23] = 120;  /* gamma 2.2 */
  e[24] = 0x06; /* RGB 4:4:4, preferred timing is the first descriptor */

  static const uint8_t kChroma[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C,
                                      0x99, 0x26, 0x0F, 0x50, 0x54};
  memcpy(e + 25, kChroma, sizeof(kChroma));

  const int hblank = kHorizontalBlanking;
  const int vblank = kVerticalBlanking;
  const uint32_t pixel_clock_10khz = static_cast<uint32_t>(
      (static_cast<long long>(width + hblank) * (height + vblank) * hz) /
      10000);
  uint8_t* dtd = e + 54;
  dtd[0] = static_cast<uint8_t>(pixel_clock_10khz & 0xFF);
  dtd[1] = static_cast<uint8_t>(pixel_clock_10khz >> 8);
  dtd[2] = static_cast<uint8_t>(width & 0xFF);
  dtd[3] = static_cast<uint8_t>(hblank & 0xFF);
  dtd[4] = static_cast<uint8_t>(((width >> 8) << 4) | (hblank >> 8));
  dtd[5] = static_cast<uint8_t>(height & 0xFF);
  dtd[6] = static_cast<uint8_t>(vblank & 0xFF);
  dtd[7] = static_cast<uint8_t>(((height >> 8) << 4) | (vblank >> 8));
  dtd[8] = 48;    /* hsync offset */
  dtd[9] = 32;    /* hsync width */
  dtd[10] = 0x58; /* vsync offset 5, width 8 */
  dtd[12] = 52;   /* 520 mm */
  dtd[13] = 29;   /* 290 mm */
  dtd[17] = 0x1E;

  uint8_t* name = e + 108;
  name[3] = 0xFC;
  const char* text = "USB HDMI Display\n";
  memcpy(name + 5, text, strlen(text));
  for (size_t i = 5 + strlen(text); i < 18; ++i) {
    name[i] = ' ';
  }

  uint8_t sum = 0;
  for (int i = 0; i < 127; ++i) {
    sum = static_cast<uint8_t>(sum + e[i]);
  }
  e[127] = static_cast<uint8_t>(256 - sum);
}

IndirectDevice::IndirectDevice(WDFDEVICE wdf_device)
    : wdf_device_(wdf_device) {}

IndirectDevice::~IndirectDevice() {
  pipeline_.reset();
  sender_.reset();
  chip_.reset();
}

NTSTATUS IndirectDevice::PrepareHardware() {
  /* Distinct status codes per failure, because this return value is one of
   * the few things that reaches the event log verbatim, and everything else
   * the framework reports collapses into a single generic error. */
  std::string error;
  std::unique_ptr<UsbLink> link = UsbLink::Open(true, &error);
  if (!link) {
    Log("PrepareHardware: %s", error.c_str());
    /* 0xC0000225: the other package is probably not installed. */
    return STATUS_NOT_FOUND;
  }
  Log("PrepareHardware: %s", link->Describe().c_str());

  chip_.reset(new Chip(std::move(link)));

  if (!chip_->ReadVideoPort(&port_)) {
    port_ = VideoPort::kUnknown;
  }

  /* Reading the EDID is 32 round trips, so do it once here and keep it. */
  edid_valid_ = false;
  if (port_ == VideoPort::kHdmi || port_ == VideoPort::kVga ||
      port_ == VideoPort::kDigital) {
    bool checksum_ok = false;
    if (chip_->ReadEdid(&edid_, 1, &checksum_ok) && checksum_ok) {
      edid_valid_ = true;
    }
  }
  if (!edid_valid_) {
    /* Never refuse to create the monitor over this. A user with no monitor
     * at all has nothing to work with; a user with a monitor showing the
     * wrong resolution can at least see that something is connected. */
    BuildFallbackEdid(&edid_, 1920, 1080, 60);
  }

  BuildModeList();
  Log("PrepareHardware: connector %s, EDID %s, %u modes",
      VideoPortName(port_), edid_valid_ ? "read from the panel" : "synthesised",
      static_cast<unsigned>(modes_.size()));

  sender_.reset(new FrameSender(chip_.get()));
  sender_->Start();
  return STATUS_SUCCESS;
}

void IndirectDevice::ReleaseHardware() {
  const ULONGLONG started = GetTickCount64();
  Log("ReleaseHardware: stopping");

  /* Stop producing before stopping the sender. The other order means waiting
   * on a multi-megabyte transfer while the system is trying to stop the
   * device, which it reports as a hung driver. */
  pipeline_.reset();
  if (sender_) {
    sender_->Stop();
    sender_.reset();
  }
  if (chip_) {
    chip_->PowerOff();
    chip_.reset();
  }
  Log("ReleaseHardware: done in %llu ms", GetTickCount64() - started);
}

void IndirectDevice::BuildModeList() {
  modes_.clear();

  const auto push = [this](int w, int h, int hz) {
    const Mode* mode = FindMode(w, h, hz);
    if (mode) {
      modes_.push_back(*mode);
    }
  };

  switch (port_) {
    case VideoPort::kCvbs:
    case VideoPort::kSVideo:
    case VideoPort::kCvbsSVideo:
      push(720, 480, 60);
      push(720, 576, 50);
      break;
    case VideoPort::kYPbPr:
      push(1920, 1080, 60);
      push(1280, 720, 60);
      push(720, 480, 60);
      break;
    default:
      /* 1080p30 first: it is a real mode of this adapter and a far better
       * match for the bandwidth available than 1080p60, so it is what a user
       * accepting the default gets. */
      push(1920, 1080, 30);
      push(1920, 1080, 60);
      push(1600, 1200, 60);
      push(1680, 1050, 60);
      push(1440, 900, 60);
      push(1366, 768, 60);
      push(1280, 1024, 60);
      push(1280, 800, 60);
      push(1280, 720, 60);
      push(1024, 768, 60);
      push(800, 600, 60);
      push(640, 480, 60);
      break;
  }

  /* Timings programmed into the adapter's flash by whoever built it take
   * priority over the built-in table. */
  std::vector<CustomTiming> custom;
  if (chip_ && chip_->ReadCustomTimings(&custom)) {
    for (auto it = custom.rbegin(); it != custom.rend(); ++it) {
      modes_.insert(modes_.begin(), it->mode);
    }
  }

  if (modes_.empty()) {
    modes_.push_back(*FindMode(1024, 768, 60));
  }
  active_mode_ = modes_.front();
}

void IndirectDevice::CreateMonitor() {
  IDDCX_MONITOR_INFO info = {};
  info.Size = sizeof(info);
  info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  info.ConnectorIndex = 0;
  info.MonitorDescription.Size = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = static_cast<UINT>(edid_.size());
  info.MonitorDescription.pData = edid_.data();

  /* An all-zero container id is rejected. */
  if (FAILED(CoCreateGuid(&info.MonitorContainerId))) {
    Log("CreateMonitor: could not generate a container id");
    return;
  }

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorContextWrapper);

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attributes;
  create.pMonitorInfo = &info;

  IDARG_OUT_MONITORCREATE created = {};
  NTSTATUS status = IddCxMonitorCreate(adapter_, &create, &created);
  Log("CreateMonitor: create -> 0x%08X", status);
  if (!NT_SUCCESS(status)) {
    return;
  }

  auto* wrapper = GetMonitorContext(created.MonitorObject);
  if (wrapper) {
    wrapper->device = this;
  }
  monitor_ = created.MonitorObject;

  IDARG_OUT_MONITORARRIVAL arrival = {};
  status = IddCxMonitorArrival(monitor_, &arrival);
  Log("CreateMonitor: arrival -> 0x%08X", status);
}

void IndirectDevice::OnAdapterReady(IDDCX_ADAPTER adapter) {
  adapter_ = adapter;
  /* Inline, as Windows' own indirect display drivers do. Deferring this to a
   * worker means the stop path has to join a sleeping thread, and the
   * framework reports that as a hang and takes the device offline. */
  CreateMonitor();
}

NTSTATUS IndirectDevice::CommitModes(const IDARG_IN_COMMITMODES* args) {
  if (!chip_) {
    return STATUS_DEVICE_NOT_READY;
  }

  for (UINT i = 0; i < args->PathCount; ++i) {
    const IDDCX_PATH& path = args->pPaths[i];
    if (!(path.Flags & IDDCX_PATH_FLAGS_ACTIVE)) {
      continue;
    }

    const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal = path.TargetVideoSignalInfo;
    const int width = static_cast<int>(signal.activeSize.cx);
    const int height = static_cast<int>(signal.activeSize.cy);
    int hz = 60;
    if (signal.vSyncFreq.Denominator) {
      hz = static_cast<int>(
          (signal.vSyncFreq.Numerator + signal.vSyncFreq.Denominator / 2) /
          signal.vSyncFreq.Denominator);
    }

    const Mode* mode = FindMode(width, height, hz);
    if (!mode) {
      Log("CommitModes: %dx%d@%d is not in the adapter's table", width, height,
          hz);
      return STATUS_INVALID_PARAMETER;
    }
    if (!chip_->PowerOn() || !chip_->SetMode(*mode)) {
      Log("CommitModes: programming failed: %s", chip_->error().c_str());
      return STATUS_DEVICE_DATA_ERROR;
    }
    Log("CommitModes: %dx%d@%d, adapter timing index 0x%02X", width, height, hz,
        mode->index);
    active_mode_ = *mode;
  }
  return STATUS_SUCCESS;
}

NTSTATUS IndirectDevice::AssignSwapChain(const IDARG_IN_SETSWAPCHAIN* args) {
  pipeline_.reset();
  if (!chip_ || !sender_) {
    return STATUS_DEVICE_NOT_READY;
  }

  std::unique_ptr<Pipeline> pipeline(
      new Pipeline(args->hSwapChain, args->RenderAdapterLuid,
                   args->hNextSurfaceAvailable, chip_.get(), sender_.get(),
                   active_mode_));

  if (!pipeline->Start()) {
    /* Hand the swapchain back so the OS builds another and tries again. It
     * has to happen here, on the OS's own thread, while the object is still
     * ours to delete. */
    Log("AssignSwapChain: releasing a swapchain we cannot drive");
    WdfObjectDelete(args->hSwapChain);
    return STATUS_SUCCESS;
  }

  pipeline_ = std::move(pipeline);
  return STATUS_SUCCESS;
}

void IndirectDevice::UnassignSwapChain() {
  if (sender_) {
    Log("UnassignSwapChain: sent=%llu dropped=%llu failed=%llu",
        sender_->sent(), sender_->dropped(), sender_->failed());
  }
  pipeline_.reset();
}

UINT FillMonitorModes(const std::vector<Mode>& modes, UINT capacity,
                      IDDCX_MONITOR_MODE* out,
                      IDDCX_MONITOR_MODE_ORIGIN origin) {
  if (out == nullptr || capacity == 0) {
    return static_cast<UINT>(modes.size());
  }
  const UINT count = std::min<UINT>(capacity, static_cast<UINT>(modes.size()));
  for (UINT i = 0; i < count; ++i) {
    out[i] = MakeMonitorMode(modes[i], origin);
  }
  return count;
}

UINT FillTargetModes(const std::vector<Mode>& modes, UINT capacity,
                     IDDCX_TARGET_MODE* out) {
  if (out == nullptr || capacity == 0) {
    return static_cast<UINT>(modes.size());
  }
  const UINT count = std::min<UINT>(capacity, static_cast<UINT>(modes.size()));
  for (UINT i = 0; i < count; ++i) {
    out[i] = MakeTargetMode(modes[i]);
  }
  return count;
}

}  // namespace usbhdmi
