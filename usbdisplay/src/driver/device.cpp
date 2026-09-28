/* SPDX-License-Identifier: GPL-2.0-only */

#include "device.h"

#include <algorithm>
#include <cstring>

#include "log.h"

namespace usbdisplay {
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
  const char* text = "USB Display\n";
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
  device_.reset();
}

/* How often to look for the adapter.
 *
 * Nothing notifies a root enumerated device that USB hardware has come or
 * gone, so this polls. Once a second is far below what a person notices when
 * plugging something in, and the check itself is a cheap enumeration that
 * touches no hardware when there is none. */
constexpr unsigned kWatchIntervalMs = 1000;

NTSTATUS IndirectDevice::PrepareHardware() {
  /* Deliberately succeeds whether or not the adapter is plugged in.
   *
   * Failing here would be the obvious thing and it is wrong: the device node
   * is created once, at install time, and never restarted, so a driver that
   * refuses to start without hardware present stays broken for good. The
   * user plugs the adapter in, nothing happens, and the only way out is to
   * reinstall. Starting regardless and watching for the hardware is what
   * makes plugging it in work. */
  watcher_stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!watcher_stop_) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (TryAttach()) {
    Log("PrepareHardware: adapter present");
  } else {
    Log("PrepareHardware: no adapter yet, watching for one");
  }

  watcher_ = std::thread(&IndirectDevice::WatcherLoop, this);
  return STATUS_SUCCESS;
}

bool IndirectDevice::TryAttach() {
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (device_) {
      return true;
    }

    std::string error;
    std::unique_ptr<DisplayDevice> device = OpenDisplayDevice(&error);
    if (!device) {
      /* Ordinary: the adapter is simply not plugged in. Logged only when the
       * reason changes, so a machine sitting for hours without one does not
       * fill the log with the same line. */
      if (error != last_attach_error_) {
        last_attach_error_ = error;
        Log("attach: waiting (%s)", error.c_str());
      }
      return false;
    }
    last_attach_error_.clear();
    Log("attach: %s", device->Describe().c_str());

    if (!device->ReadConnector(&port_)) {
      port_ = VideoPort::kUnknown;
    }

    /* Reading the panel's capabilities is 32 round trips, so do it once here
     * and keep it. It has to be re-read on every attach, because the user
     * may have moved the adapter to a different screen while it was
     * unplugged. */
    edid_valid_ = false;
    if (port_ == VideoPort::kHdmi || port_ == VideoPort::kVga ||
        port_ == VideoPort::kDigital) {
      bool checksum_ok = false;
      if (device->ReadEdid(&edid_, 1, &checksum_ok) && checksum_ok) {
        edid_valid_ = true;
      }
    }
    if (!edid_valid_) {
      /* Never refuse to show a monitor over this. A user with no monitor at
       * all has nothing to work with; a user with a monitor at the wrong
       * resolution can at least see that something is connected. */
      BuildFallbackEdid(&edid_, 1920, 1080, 60);
    }

    modes_ = device->SupportedModes(port_);
    if (modes_.empty()) {
      modes_.push_back(*FindMode(1024, 768, 60));
    }
    active_mode_ = modes_.front();

    device_ = std::move(device);
    Log("attach: connector %s, capabilities %s, %u modes",
        VideoPortName(port_),
        edid_valid_ ? "read from the panel" : "synthesised",
        static_cast<unsigned>(modes_.size()));

    sender_.reset(new FrameSender(device_.get()));
    sender_->Start();

    if (adapter_ready_) {
      CreateMonitor();
    }
  }

  /* Outside the lock: announcing calls straight back into this driver. */
  AnnounceMonitor();
  return true;
}

void IndirectDevice::Detach() {
  std::lock_guard<std::mutex> guard(lock_);
  if (!device_) {
    return;
  }
  Log("detach: adapter is gone, removing the monitor");

  /* Order matters. Take the monitor away from Windows first, so it moves
   * the user's windows back to a real screen rather than leaving them on a
   * display that no longer exists. Then stop producing, then stop sending:
   * the other way round means waiting on a transfer to hardware that has
   * been unplugged. */
  RemoveMonitor();
  pipeline_.reset();
  if (sender_) {
    sender_->Stop();
    sender_.reset();
  }
  device_.reset();
}

void IndirectDevice::WatcherLoop() {
  for (;;) {
    if (WaitForSingleObject(watcher_stop_, kWatchIntervalMs) ==
        WAIT_OBJECT_0) {
      return;
    }

    bool attached;
    bool still_present = true;
    {
      std::lock_guard<std::mutex> guard(lock_);
      attached = device_ != nullptr;
      if (attached) {
        /* Asked of the device, which answers from what the system already
         * knows rather than by talking to hardware that may have just been
         * pulled out. */
        still_present = device_->StillPresent();
      }
    }

    if (!attached) {
      TryAttach();
    } else if (!still_present) {
      Detach();
    }
  }
}

void IndirectDevice::ReleaseHardware() {
  const ULONGLONG started = GetTickCount64();
  Log("ReleaseHardware: stopping");

  if (watcher_stop_) {
    SetEvent(watcher_stop_);
  }
  if (watcher_.joinable()) {
    watcher_.join();
  }

  Detach();

  if (watcher_stop_) {
    CloseHandle(watcher_stop_);
    watcher_stop_ = nullptr;
  }
  Log("ReleaseHardware: done in %llu ms", GetTickCount64() - started);
}

std::vector<Mode> IndirectDevice::modes() {
  std::lock_guard<std::mutex> guard(lock_);
  return modes_;
}

void IndirectDevice::CreateMonitor() {
  if (monitor_ || !adapter_) {
    return;
  }
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
  pending_arrival_ = true;
}

/* Announces the monitor, and must be called with lock_ **released**.
 *
 * Announcing arrival makes Windows turn round and call straight back into
 * this driver, on this thread, to assign a swapchain. Holding the lock
 * across that call deadlocks the driver against itself: the monitor never
 * appears and the log stops mid-sentence after the create succeeded, which
 * is a memorably unhelpful symptom. */
void IndirectDevice::AnnounceMonitor() {
  IDDCX_MONITOR monitor = nullptr;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!pending_arrival_ || !monitor_) {
      return;
    }
    pending_arrival_ = false;
    monitor = monitor_;
  }

  IDARG_OUT_MONITORARRIVAL arrival = {};
  const NTSTATUS status = IddCxMonitorArrival(monitor, &arrival);
  Log("AnnounceMonitor: arrival -> 0x%08X", status);
}

void IndirectDevice::RemoveMonitor() {
  if (!monitor_) {
    return;
  }
  const NTSTATUS status = IddCxMonitorDeparture(monitor_);
  Log("RemoveMonitor: departure -> 0x%08X", status);
  monitor_ = nullptr;
}

void IndirectDevice::OnAdapterReady(IDDCX_ADAPTER adapter) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    adapter_ = adapter;
    adapter_ready_ = true;

    /* Only create a monitor if there is hardware behind it. If the adapter
     * is not plugged in yet, the watcher does this when it arrives. */
    if (device_) {
      CreateMonitor();
    }
  }
  AnnounceMonitor();
}

NTSTATUS IndirectDevice::CommitModes(const IDARG_IN_COMMITMODES* args) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!device_) {
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
    if (!device_->PowerOn() || !device_->SetMode(*mode)) {
      Log("CommitModes: programming failed: %s", device_->error().c_str());
      return STATUS_DEVICE_DATA_ERROR;
    }
    Log("CommitModes: %dx%d@%d, adapter timing index 0x%02X", width, height, hz,
        mode->index);
    active_mode_ = *mode;
  }
  return STATUS_SUCCESS;
}

NTSTATUS IndirectDevice::AssignSwapChain(const IDARG_IN_SETSWAPCHAIN* args) {
  std::lock_guard<std::mutex> guard(lock_);
  pipeline_.reset();
  if (!device_ || !sender_) {
    return STATUS_DEVICE_NOT_READY;
  }

  std::unique_ptr<Pipeline> pipeline(
      new Pipeline(args->hSwapChain, args->RenderAdapterLuid,
                   args->hNextSurfaceAvailable, device_.get(), sender_.get(),
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
  std::lock_guard<std::mutex> guard(lock_);
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
