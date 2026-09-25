/* SPDX-License-Identifier: GPL-2.0-only */

#include "device.h"

#include <objbase.h>

#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ms912x {
namespace {

constexpr DWORD kBufferWaitMs = 10;

/* The vendor driver repaints the whole screen if nothing has been sent for
 * this long, which is what stops the panel deciding there is no signal. */
constexpr unsigned long long kIdleRefreshMs = 2500;

/* Modes offered per connector type, mirroring the Linux driver's choices. */
const uint16_t kCvbsModes[][3] = {{720, 480, 60}, {720, 576, 50}};
const uint16_t kYPbPrModes[][3] = {
    {1280, 720, 60}, {1920, 1080, 60}, {720, 480, 60}, {720, 576, 50}};

DISPLAYCONFIG_VIDEO_SIGNAL_INFO MakeSignalInfo(uint16_t width, uint16_t height,
                                               uint16_t hz) {
  DISPLAYCONFIG_VIDEO_SIGNAL_INFO info = {};
  info.totalSize.cx = info.activeSize.cx = width;
  info.totalSize.cy = info.activeSize.cy = height;
  info.AdditionalSignalInfo.vSyncFreqDivider = 1;
  info.AdditionalSignalInfo.videoStandard = 255;
  info.vSyncFreq.Numerator = hz;
  info.vSyncFreq.Denominator = 1;
  info.hSyncFreq.Numerator = static_cast<UINT32>(hz) * height;
  info.hSyncFreq.Denominator = 1;
  info.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  info.pixelRate = static_cast<UINT64>(hz) * width * height;
  return info;
}

IDDCX_MONITOR_MODE MakeMonitorMode(const Mode& mode,
                                   IDDCX_MONITOR_MODE_ORIGIN origin) {
  IDDCX_MONITOR_MODE out = {};
  out.Size = sizeof(out);
  out.Origin = origin;
  out.MonitorVideoSignalInfo = MakeSignalInfo(mode.width, mode.height, mode.hz);
  /* Monitor modes require a zero divider. */
  out.MonitorVideoSignalInfo.AdditionalSignalInfo.vSyncFreqDivider = 0;
  return out;
}

IDDCX_TARGET_MODE MakeTargetMode(const Mode& mode) {
  IDDCX_TARGET_MODE out = {};
  out.Size = sizeof(out);
  out.TargetVideoSignalInfo.targetVideoSignalInfo =
      MakeSignalInfo(mode.width, mode.height, mode.hz);
  return out;
}

Rect FromRECT(const RECT& r) {
  Rect out;
  out.x1 = r.left;
  out.y1 = r.top;
  out.x2 = r.right;
  out.y2 = r.bottom;
  return out;
}

}  // namespace

/* -------------------------------------------------------------------------
 * Fallback EDID
 * ---------------------------------------------------------------------- */

void BuildFallbackEdid(std::vector<uint8_t>* edid, uint16_t width,
                       uint16_t height, uint16_t hz) {
  edid->assign(128, 0);
  uint8_t* e = edid->data();

  static const uint8_t kMagic[8] = {0x00, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0x00};
  memcpy(e, kMagic, sizeof(kMagic));

  /* Manufacturer "MSI" is taken; use "MSD" for MacroSilicon Display. */
  uint16_t manufacturer = static_cast<uint16_t>(((('M' - 'A' + 1) & 0x1F) << 10) |
                                                ((('S' - 'A' + 1) & 0x1F) << 5) |
                                                (('D' - 'A' + 1) & 0x1F));
  e[8] = static_cast<uint8_t>(manufacturer >> 8);
  e[9] = static_cast<uint8_t>(manufacturer);
  e[10] = 0x01;  /* product code */
  e[11] = 0x00;
  e[16] = 0x01;  /* week */
  e[17] = 36;    /* year 2026 */
  e[18] = 0x01;  /* EDID 1.4 */
  e[19] = 0x04;
  e[20] = 0x80;  /* digital input */
  e[21] = 52;    /* ~16:9 at 24 inch */
  e[22] = 29;
  e[23] = 120;   /* gamma 2.2 */
  e[24] = 0x06;  /* no DPMS, RGB 4:4:4, preferred timing in DTD 1 */

  /* Neutral chromaticity block. */
  static const uint8_t kChroma[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C,
                                      0x99, 0x26, 0x0F, 0x50, 0x54};
  memcpy(e + 25, kChroma, sizeof(kChroma));

  /* Detailed timing descriptor 1, CVT-ish reduced blanking. */
  const uint32_t hblank = 160;
  const uint32_t vblank = 45;
  const uint32_t pixel_clock_10khz =
      (static_cast<uint32_t>(width + hblank) * (height + vblank) * hz) / 10000;
  uint8_t* dtd = e + 54;
  dtd[0] = static_cast<uint8_t>(pixel_clock_10khz & 0xFF);
  dtd[1] = static_cast<uint8_t>(pixel_clock_10khz >> 8);
  dtd[2] = static_cast<uint8_t>(width & 0xFF);
  dtd[3] = static_cast<uint8_t>(hblank & 0xFF);
  dtd[4] = static_cast<uint8_t>(((width >> 8) << 4) | (hblank >> 8));
  dtd[5] = static_cast<uint8_t>(height & 0xFF);
  dtd[6] = static_cast<uint8_t>(vblank & 0xFF);
  dtd[7] = static_cast<uint8_t>(((height >> 8) << 4) | (vblank >> 8));
  dtd[8] = 48;   /* hsync offset */
  dtd[9] = 32;   /* hsync width */
  dtd[10] = 0x58; /* vsync offset 5, width 8 */
  dtd[11] = 0x00;
  dtd[12] = 52;  /* 520 mm */
  dtd[13] = 29;  /* 290 mm */
  dtd[14] = 0x00;
  dtd[17] = 0x1E;

  /* Descriptor 4: monitor name. */
  uint8_t* name = e + 108;
  name[3] = 0xFC;
  const char* text = "MacroSilicon USB\n";
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

/* -------------------------------------------------------------------------
 * FrameSender
 * ---------------------------------------------------------------------- */

FrameSender::FrameSender(Device* device) : device_(device) {
  for (Slot& slot : slots_) {
    slot.data.resize(kMaxTransferLen);
  }
}

FrameSender::~FrameSender() { Stop(); }

void FrameSender::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_) {
    return;
  }
  running_ = true;
  worker_ = std::thread(&FrameSender::WorkerMain, this);
}

void FrameSender::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return;
    }
    running_ = false;
  }
  work_cv_.notify_all();
  free_cv_.notify_all();
  if (worker_.joinable()) {
    Log("FrameSender: stopping");
    worker_.join();
    Log("FrameSender: stopped");
  }
}

std::vector<uint8_t>* FrameSender::AcquireBuffer(DWORD wait_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  Slot& slot = slots_[next_slot_];

  if (slot.in_flight || slot.queued) {
    /* Wait briefly, then give up. Queueing would build unbounded latency and
     * eventually stall the compositor's acquire loop. */
    if (!free_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                           [&] { return !slot.in_flight && !slot.queued; })) {
      ++frames_dropped_;
      return nullptr;
    }
  }
  return &slot.data;
}

void FrameSender::Submit(std::vector<uint8_t>* buffer, size_t length) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < 2; ++i) {
      if (&slots_[i].data == buffer) {
        slots_[i].length = length;
        slots_[i].queued = true;
        next_slot_ = 1 - i;
        break;
      }
    }
  }
  work_cv_.notify_one();
}

void FrameSender::Cancel(std::vector<uint8_t>* buffer) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Slot& slot : slots_) {
      if (&slot.data == buffer) {
        slot.queued = false;
        slot.in_flight = false;
      }
    }
  }
  free_cv_.notify_all();
}

void FrameSender::WorkerMain() {
  for (;;) {
    Slot* slot = nullptr;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_cv_.wait(lock, [&] {
        return !running_ || slots_[0].queued || slots_[1].queued;
      });
      if (!running_) {
        return;
      }
      for (Slot& candidate : slots_) {
        if (candidate.queued) {
          candidate.queued = false;
          candidate.in_flight = true;
          slot = &candidate;
          break;
        }
      }
    }
    if (!slot) {
      continue;
    }

    ULONGLONG start = GetTickCount64();
    bool ok = device_->SendFrame(slot->data.data(), slot->length);
    ULONGLONG cost = GetTickCount64() - start;
    if (!ok) {
      Log("FrameSender: send failed after %llums: %s", cost,
          device_->last_error().c_str());
    } else if (cost > 400 || frames_sent_ < 3) {
      Log("FrameSender: sent %zu bytes in %llums", slot->length, cost);
    }
    ++frames_sent_;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      slot->in_flight = false;
    }
    free_cv_.notify_all();
  }
}

/* -------------------------------------------------------------------------
 * SwapChainProcessor
 * ---------------------------------------------------------------------- */

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapchain,
                                       LUID render_adapter,
                                       HANDLE new_frame_event, Device* device,
                                       FrameSender* sender, const Mode& mode)
    : swapchain_(swapchain),
      render_adapter_(render_adapter),
      new_frame_event_(new_frame_event),
      device_(device),
      sender_(sender),
      mode_(mode),
      frame_index_(0) {
  pending_damage_[0] = EmptyRect();
  pending_damage_[1] = EmptyRect();
  terminate_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

bool SwapChainProcessor::Start() {
  if (!EnsureD3D()) {
    Log("SwapChain: D3D init failed");
    return false;
  }
  thread_ = std::thread(&SwapChainProcessor::Run, this);
  return true;
}

SwapChainProcessor::~SwapChainProcessor() {
  Terminate();
  if (terminate_event_) {
    CloseHandle(terminate_event_);
  }
}

void SwapChainProcessor::Terminate() {
  if (terminate_event_) {
    SetEvent(terminate_event_);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool SwapChainProcessor::EnsureD3D() {
  ComPtr<IDXGIFactory5> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    return false;
  }
  ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(factory->EnumAdapterByLuid(render_adapter_,
                                        IID_PPV_ARGS(&adapter)))) {
    return false;
  }

  D3D_FEATURE_LEVEL level;
  HRESULT hr = D3D11CreateDevice(
      adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, &d3d_device_, &level, &d3d_context_);
  if (FAILED(hr)) {
    return false;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(d3d_device_.As(&dxgi_device))) {
    return false;
  }
  IDARG_IN_SWAPCHAINSETDEVICE set_device = {};
  set_device.pDevice = dxgi_device.Get();
  return NT_SUCCESS(IddCxSwapChainSetDevice(swapchain_, &set_device));
}

bool SwapChainProcessor::EnsureStaging(UINT width, UINT height) {
  if (staging_ && staging_width_ == width && staging_height_ == height) {
    return true;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  staging_.Reset();
  if (FAILED(d3d_device_->CreateTexture2D(&desc, nullptr, &staging_))) {
    return false;
  }
  staging_width_ = width;
  staging_height_ = height;
  return true;
}

bool SwapChainProcessor::ProcessFrame(
    const IDARG_OUT_RELEASEANDACQUIREBUFFER& buffer) {
  const IDDCX_METADATA& meta = buffer.MetaData;
  if (!meta.pSurface) {
    return true;
  }

  ComPtr<ID3D11Texture2D> source;
  if (FAILED(meta.pSurface->QueryInterface(IID_PPV_ARGS(&source)))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC source_desc = {};
  source->GetDesc(&source_desc);
  if (!EnsureStaging(source_desc.Width, source_desc.Height)) {
    return false;
  }

  const int fb_width = static_cast<int>(source_desc.Width);
  const int fb_height = static_cast<int>(source_desc.Height);

  const unsigned long long now_ms = GetTickCount64();

  /* Work out what changed this frame and fold it into both buffers' pending
   * damage. A zero dirty rect count together with a zero move region count
   * means nothing changed at all. */
  bool have_new_damage = false;
  Rect damage = EmptyRect();

  if (force_full_frame_) {
    damage.x1 = 0;
    damage.y1 = 0;
    damage.x2 = fb_width;
    damage.y2 = fb_height;
    have_new_damage = true;
  } else if (meta.DirtyRectCount > 0 || meta.MoveRegionCount > 0) {
    std::vector<RECT> rects(meta.DirtyRectCount);
    IDARG_IN_GETDIRTYRECTS in = {};
    in.DirtyRectInCount = meta.DirtyRectCount;
    in.pDirtyRects = rects.data();
    IDARG_OUT_GETDIRTYRECTS out = {};
    if (meta.MoveRegionCount > 0 ||
        !NT_SUCCESS(IddCxSwapChainGetDirtyRects(swapchain_, &in, &out))) {
      /* Move regions relocate content, so the source area needs repainting
       * too. Rather than track both ends, repaint everything. */
      damage.x1 = 0;
      damage.y1 = 0;
      damage.x2 = fb_width;
      damage.y2 = fb_height;
    } else {
      for (UINT i = 0; i < out.DirtyRectOutCount; ++i) {
        damage = MergeRects(damage, FromRECT(rects[i]));
      }
    }
    have_new_damage = !damage.empty();
  }

  if (have_new_damage) {
    pending_damage_[0] = MergeRects(pending_damage_[0], damage);
    pending_damage_[1] = MergeRects(pending_damage_[1], damage);
  } else if (last_send_ms_ != 0 && now_ms - last_send_ms_ >= kIdleRefreshMs) {
    /* Keep the link and the panel awake. */
    Rect full;
    full.x1 = 0;
    full.y1 = 0;
    full.x2 = fb_width;
    full.y2 = fb_height;
    pending_damage_[0] = full;
    pending_damage_[1] = full;
  }

  Rect to_send =
      AlignDamageRect(pending_damage_[frame_index_], fb_width, fb_height);
  if (to_send.empty()) {
    return true;
  }

  std::vector<uint8_t>* transfer = sender_->AcquireBuffer(kBufferWaitMs);
  if (!transfer) {
    /* Dropped, but the damage stays pending so a later frame still sends it. */
    return true;
  }

  /* Copy only the damaged region out of the GPU. */
  D3D11_BOX box = {};
  box.left = static_cast<UINT>(to_send.x1);
  box.top = static_cast<UINT>(to_send.y1);
  box.front = 0;
  box.right = static_cast<UINT>(to_send.x2);
  box.bottom = static_cast<UINT>(to_send.y2);
  box.back = 1;
  d3d_context_->CopySubresourceRegion(staging_.Get(), 0, box.left, box.top, 0,
                                      source.Get(), 0, &box);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(d3d_context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0,
                               &mapped))) {
    sender_->Cancel(transfer);
    return false;
  }

  const size_t length =
      FrameRect(transfer->data(), transfer->size(),
                static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                fb_width, fb_height, to_send);
  d3d_context_->Unmap(staging_.Get(), 0);

  if (length == 0) {
    sender_->Cancel(transfer);
    return false;
  }

  sender_->Submit(transfer, length);

  /* This buffer is now up to date; the other one still owes the same damage. */
  pending_damage_[frame_index_] = EmptyRect();
  frame_index_ = 1 - frame_index_;
  last_send_ms_ = now_ms;

  /* Report how much of the screen each transfer actually covers: if damage
   * tracking is working this should be far smaller than the full frame. */
  static ULONGLONG last_rect_log = 0;
  ULONGLONG now_ms = GetTickCount64();
  if (now_ms - last_rect_log >= 2000) {
    Log("damage: %dx%d at (%d,%d) rects=%u moves=%u -> %zu bytes",
        to_send.width(), to_send.height(), to_send.x1, to_send.y1,
        meta.DirtyRectCount, meta.MoveRegionCount, length);
    last_rect_log = now_ms;
  }

  force_full_frame_ = false;
  return true;
}

void SwapChainProcessor::Run() {
  /* Ask for a slightly raised priority: the compositor considers the monitor
   * hung if we fall too far behind on the acquire loop. */
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

  Log("SwapChain: processing started");

  unsigned logged = 0;
  ULONGLONG last_report = GetTickCount64();

  HANDLE waits[] = {new_frame_event_, terminate_event_};
  for (;;) {
    IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
    NTSTATUS status =
        IddCxSwapChainReleaseAndAcquireBuffer(swapchain_, &buffer);

    if (status == E_PENDING) {
      DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 16);
      if (wait == WAIT_OBJECT_0 + 1) {
        Log("SwapChain: terminate signalled");
        break;
      }
      if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) {
        continue;
      }
      Log("SwapChain: wait -> %lu, leaving loop", wait);
      break;
    }
    if (!NT_SUCCESS(status)) {
      Log("SwapChain: acquire -> 0x%08X, leaving loop", status);
      break;
    }

    ULONGLONG frame_start = GetTickCount64();
    bool ok = ProcessFrame(buffer);
    ULONGLONG frame_ms = GetTickCount64() - frame_start;
    IddCxSwapChainFinishedProcessingFrame(swapchain_);

    /* First few frames, then once a second, so the log stays readable. */
    ULONGLONG now = GetTickCount64();
    if (logged < 3 || now - last_report >= 10000) {
      Log("SwapChain: frame ok=%d cost=%llums sent=%llu dropped=%llu",
          ok ? 1 : 0, frame_ms, sender_->frames_sent(),
          sender_->frames_dropped());
      ++logged;
      last_report = now;
    }

    if (WaitForSingleObject(terminate_event_, 0) == WAIT_OBJECT_0) {
      Log("SwapChain: terminate requested");
      break;
    }
  }
  Log("SwapChain: processing stopped");
}

/* -------------------------------------------------------------------------
 * IndirectDevice
 * ---------------------------------------------------------------------- */

IndirectDevice::IndirectDevice(WDFDEVICE wdf_device)
    : wdf_device_(wdf_device) {}

IndirectDevice::~IndirectDevice() {
  processor_.reset();
  sender_.reset();
  ms_device_.reset();
}

NTSTATUS IndirectDevice::PrepareHardware() {
  /* Both planes are opened from user mode, which is possible because UMDF
   * hosts are user mode processes. This is the same pairing the msdisp tool
   * uses and is known to drive the panel:
   *   control -> the dongle's HID interface, owned by hidusb
   *   data    -> the WinUSB interface bound by inf/ms912x_winusb.inf
   *
   * Distinct failure codes per step: PrepareHardware's return value appears
   * verbatim in the DriverFrameworks-UserMode event log, and is otherwise the
   * only visibility into why a UMDF device refuses to start. */
  std::string error;

  std::unique_ptr<HidTransport> control = HidTransport::Open(&error);
  if (!control) {
    Log("PrepareHardware: HID open failed: %s", error.c_str());
    return STATUS_ACCESS_DENIED; /* 0xC0000022: no HID control interface */
  }
  Log("PrepareHardware: control = %s", control->Describe().c_str());

  std::unique_ptr<WinUsbTransport> data = WinUsbTransport::Open(&error);
  if (!data) {
    Log("PrepareHardware: WinUSB open failed: %s", error.c_str());
    /* 0xC0000225: the WinUSB package is probably not installed. */
    return STATUS_NOT_FOUND;
  }
  Log("PrepareHardware: data = %s", data->Describe().c_str());

  ms_device_.reset(new Device(std::unique_ptr<Transport>(
      new CompositeTransport(std::move(control), std::move(data)))));

  if (!ms_device_->ReadVideoPort(&port_)) {
    port_ = VideoPort::kUnknown;
  }

  /* Reading EDID is 32 control round trips, so do it once here and cache. */
  edid_valid_ = false;
  if (port_ == VideoPort::kHdmi || port_ == VideoPort::kVga ||
      port_ == VideoPort::kDigital) {
    bool checksum_ok = false;
    if (ms_device_->ReadEdid(&edid_, 1, &checksum_ok) && checksum_ok) {
      edid_valid_ = true;
    }
  }
  if (!edid_valid_) {
    /* Never refuse to create the monitor: the user would see nothing at all
     * and have no way to diagnose it. */
    BuildFallbackEdid(&edid_, 1920, 1080, 60);
  }

  BuildModeList();
  Log("PrepareHardware: port=%s edid_valid=%d modes=%u",
      VideoPortName(port_), edid_valid_ ? 1 : 0,
      static_cast<unsigned>(modes_.size()));

  sender_.reset(new FrameSender(ms_device_.get()));
  sender_->Start();
  return STATUS_SUCCESS;
}

void IndirectDevice::ReleaseHardware() {
  Log("ReleaseHardware: enter");
  /* Stop producing frames first, then stop the sender. Doing it the other way
   * round means waiting on a multi-megabyte USB transfer while WDF is trying
   * to stop the device, which it reports as a driver hang. */
  processor_.reset();
  if (sender_) {
    sender_->Stop();
    sender_.reset();
  }
  if (ms_device_) {
    ms_device_->PowerOff();
    ms_device_.reset();
  }
  Log("ReleaseHardware: done");
}

void IndirectDevice::BuildModeList() {
  modes_.clear();

  auto push = [this](uint16_t w, uint16_t h, uint16_t hz) {
    const Mode* mode = FindMode(w, h, hz);
    if (mode) {
      modes_.push_back(*mode);
    }
  };

  switch (port_) {
    case VideoPort::kCvbs:
    case VideoPort::kSVideo:
    case VideoPort::kCvbsSVideo:
      for (const auto& entry : kCvbsModes) {
        push(entry[0], entry[1], entry[2]);
      }
      break;
    case VideoPort::kYPbPr:
      for (const auto& entry : kYPbPrModes) {
        push(entry[0], entry[1], entry[2]);
      }
      break;
    default:
      /* HDMI, VGA, digital and unknown get the full table, most useful first. */
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

  /* Custom timings from flash take priority when present. */
  std::vector<CustomMode> custom;
  if (ms_device_ && ms_device_->ReadCustomTimings(&custom)) {
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
  /* An indirect monitor is reported as an externally connected target. */
  info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  info.ConnectorIndex = 0;
  info.MonitorDescription.Size = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = static_cast<UINT>(edid_.size());
  info.MonitorDescription.pData = edid_.data();
  /* Mandatory: an all-zero container id is rejected. */
  if (FAILED(CoCreateGuid(&info.MonitorContainerId))) {
    Log("CreateMonitor: CoCreateGuid failed");
    return;
  }

  /* Mandatory: real object attributes with a context type. */
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorContextWrapper);

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attributes;
  create.pMonitorInfo = &info;

  IDARG_OUT_MONITORCREATE created = {};
  NTSTATUS status = IddCxMonitorCreate(adapter_, &create, &created);
  Log("CreateMonitor: IddCxMonitorCreate -> 0x%08X", status);
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
  Log("CreateMonitor: IddCxMonitorArrival -> 0x%08X", status);
}

void IndirectDevice::OnAdapterInitFinished(IDDCX_ADAPTER adapter) {
  adapter_ = adapter;

  /* Create the monitor inline, exactly as the in-box drivers do. Deferring it
   * to a worker thread means the PnP stop path has to join a sleeping thread,
   * and WDF then reports the driver as hung and takes the device offline. */
  CreateMonitor();
}

NTSTATUS IndirectDevice::CommitModes(const IDARG_IN_COMMITMODES* args) {
  if (!ms_device_) {
    return STATUS_DEVICE_NOT_READY;
  }
  for (UINT i = 0; i < args->PathCount; ++i) {
    const IDDCX_PATH& path = args->pPaths[i];
    if (!(path.Flags & IDDCX_PATH_FLAGS_ACTIVE)) {
      continue;
    }
    const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal = path.TargetVideoSignalInfo;
    const uint16_t width = static_cast<uint16_t>(signal.activeSize.cx);
    const uint16_t height = static_cast<uint16_t>(signal.activeSize.cy);
    uint16_t hz = 60;
    if (signal.vSyncFreq.Denominator) {
      hz = static_cast<uint16_t>(
          (signal.vSyncFreq.Numerator + signal.vSyncFreq.Denominator / 2) /
          signal.vSyncFreq.Denominator);
    }

    const Mode* mode = FindMode(width, height, hz);
    if (!mode) {
      Log("CommitModes: %ux%u@%u not in the chip mode table", width, height, hz);
      return STATUS_INVALID_PARAMETER;
    }
    if (!ms_device_->PowerOn() || !ms_device_->SetResolution(*mode)) {
      Log("CommitModes: modeset failed: %s", ms_device_->last_error().c_str());
      return STATUS_DEVICE_DATA_ERROR;
    }
    Log("CommitModes: %ux%u@%u -> chip mode 0x%02X", width, height, hz,
        mode->mode_id);
    active_mode_ = *mode;
  }
  return STATUS_SUCCESS;
}

NTSTATUS IndirectDevice::AssignSwapChain(const IDARG_IN_SETSWAPCHAIN* args) {
  Log("AssignSwapChain: mode %ux%u@%u", active_mode_.width, active_mode_.height,
      active_mode_.hz);
  processor_.reset();
  if (!ms_device_ || !sender_) {
    Log("AssignSwapChain: device not ready");
    return STATUS_DEVICE_NOT_READY;
  }
  std::unique_ptr<SwapChainProcessor> processor(new SwapChainProcessor(
      args->hSwapChain, args->RenderAdapterLuid, args->hNextSurfaceAvailable,
      ms_device_.get(), sender_.get(), active_mode_));

  if (!processor->Start()) {
    /* Delete the swapchain so the OS builds a new one and tries again. This
     * has to happen here, on the OS's own thread, while the object is still
     * ours to delete. */
    Log("AssignSwapChain: releasing unusable swapchain");
    WdfObjectDelete(args->hSwapChain);
    return STATUS_SUCCESS;
  }

  processor_ = std::move(processor);
  return STATUS_SUCCESS;
}

void IndirectDevice::UnassignSwapChain() {
  Log("UnassignSwapChain: sent=%llu dropped=%llu",
      sender_ ? sender_->frames_sent() : 0,
      sender_ ? sender_->frames_dropped() : 0);
  processor_.reset();
}

/* Helpers used by the callbacks in driver.cpp. */

UINT FillMonitorModes(const std::vector<Mode>& modes, UINT capacity,
                      IDDCX_MONITOR_MODE* out,
                      IDDCX_MONITOR_MODE_ORIGIN origin) {
  if (out == nullptr || capacity == 0) {
    return static_cast<UINT>(modes.size());
  }
  UINT count = std::min<UINT>(capacity, static_cast<UINT>(modes.size()));
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
  UINT count = std::min<UINT>(capacity, static_cast<UINT>(modes.size()));
  for (UINT i = 0; i < count; ++i) {
    out[i] = MakeTargetMode(modes[i]);
  }
  return count;
}

}  // namespace ms912x
