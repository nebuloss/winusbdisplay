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

/* How long the link may stay silent before the panel is repainted.
 *
 * The vendor driver uses 2500 ms, but measured here the panel drops its
 * signal after roughly one to two seconds of silence, so at that interval it
 * spends much of its time re-acquiring, which reads as the whole screen
 * flickering. Overridable while the right value is established. */
constexpr unsigned long long kDefaultIdleRefreshMs = 900;

/* Where GPU conversion starts paying off, measured on this hardware by timing
 * both paths on the same rectangle:
 *
 *    160k pixels   gpu 663us   cpu 268us    CPU 2.5x faster
 *    240k pixels   gpu 758us   cpu 474us    CPU 1.6x faster
 *    570k pixels   gpu 1332us  cpu 784us    CPU 1.7x faster
 *  2.07M pixels   gpu 4190us  cpu 4392us    about equal
 *
 * A dispatch and readback costs roughly half a millisecond no matter how
 * small the region is, so for ordinary desktop damage the CPU path is simply
 * faster. It only stops being faster near full screen, where the GPU does the
 * same work for a fraction of the processor time.
 *
 * So: CPU below the threshold, GPU above it. */
constexpr size_t kGpuConversionMinPixels = 1000000;

/* Modes offered per connector type, mirroring the Linux driver's choices. */
const uint16_t kCvbsModes[][3] = {{720, 480, 60}, {720, 576, 50}};
const uint16_t kYPbPrModes[][3] = {
    {1280, 720, 60}, {1920, 1080, 60}, {720, 480, 60}, {720, 576, 50}};

/* Blanking intervals, roughly CVT reduced blanking. The exact numbers do not
 * have to match the panel, but the whole structure has to be internally
 * consistent: pixelRate must equal totalSize.cx * totalSize.cy * vSyncFreq,
 * and hSyncFreq must equal pixelRate / totalSize.cx. Reporting totalSize
 * equal to activeSize, as an earlier version did, makes those identities
 * false and the OS rejects the resulting topology. */
constexpr UINT32 kHorizontalBlanking = 160;
constexpr UINT32 kVerticalBlanking = 45;

/* Small settings read shared by the driver. Kept next to the brightness
 * values so there is one place to look for runtime configuration. */
DWORD ReadPolicyDword(const wchar_t* name, DWORD fallback) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\winusbdisplay", 0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return fallback;
  }
  DWORD value = 0;
  DWORD size = sizeof(value);
  DWORD type = 0;
  DWORD result = fallback;
  if (RegQueryValueExW(key, name, nullptr, &type,
                       reinterpret_cast<LPBYTE>(&value), &size) ==
          ERROR_SUCCESS &&
      type == REG_DWORD) {
    result = value;
  }
  RegCloseKey(key);
  return result;
}

DISPLAYCONFIG_VIDEO_SIGNAL_INFO MakeSignalInfo(uint16_t width, uint16_t height,
                                               uint16_t hz) {
  const UINT32 h_total = width + kHorizontalBlanking;
  const UINT32 v_total = height + kVerticalBlanking;

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO info = {};
  info.activeSize.cx = width;
  info.activeSize.cy = height;
  info.totalSize.cx = static_cast<UINT32>(h_total);
  info.totalSize.cy = static_cast<UINT32>(v_total);

  info.vSyncFreq.Numerator = hz;
  info.vSyncFreq.Denominator = 1;
  /* Lines per second. */
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
  /* Monitor modes require a zero divider. */
  out.MonitorVideoSignalInfo.AdditionalSignalInfo.vSyncFreqDivider = 0;
  return out;
}

IDDCX_TARGET_MODE MakeTargetMode(const Mode& mode) {
  IDDCX_TARGET_MODE out = {};
  out.Size = sizeof(out);
  out.TargetVideoSignalInfo.targetVideoSignalInfo =
      MakeSignalInfo(mode.width, mode.height, mode.hz);
  /* The panel runs at vSyncFreq while the OS composes the desktop at
   * vSyncFreq / vSyncFreqDivider, which is how a link that cannot carry full
   * rate frames asks for fewer of them. Only valid on target modes; monitor
   * modes require zero here. */
  out.TargetVideoSignalInfo.targetVideoSignalInfo.AdditionalSignalInfo
      .vSyncFreqDivider =
      ReadPolicyDword(L"SyncDivider", 0) != 0 ? SyncDividerForMode(mode) : 1;
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
  /* Abort first: the worker may be a hundred milliseconds into a transfer,
   * and joining without cancelling makes teardown slow enough that the
   * framework reports the driver as hung. */
  if (device_) {
    device_->CancelTransfers();
  }
  work_cv_.notify_all();
  free_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

std::vector<uint8_t>* FrameSender::AcquireBuffer(DWORD wait_ms,
                                                 Rect* superseded) {
  std::unique_lock<std::mutex> lock(mutex_);
  *superseded = EmptyRect();

  /* Prefer a slot that is completely free. */
  for (Slot& slot : slots_) {
    if (!slot.in_flight && !slot.queued) {
      return &slot.data;
    }
  }

  /* Otherwise take back a slot that is queued but has not started yet. Its
   * contents are already out of date, and the caller is about to write
   * something newer over them.
   *
   * This is what keeps the cursor feeling attached to the mouse. The chip
   * completes a transfer on its own 60 Hz boundary, so allowing a second
   * frame to sit in the queue behind the one on the wire puts two whole
   * periods, about 33 ms, between a movement and it appearing. Replacing the
   * waiting frame instead keeps that to a single period and means what is
   * sent is always the most recent picture rather than a stale one. */
  for (Slot& slot : slots_) {
    if (slot.queued && !slot.in_flight) {
      slot.queued = false;
      *superseded = slot.damage;
      ++frames_superseded_;
      return &slot.data;
    }
  }

  /* Everything is genuinely on the wire. Wait briefly for one to land, then
   * give up: queueing without limit would stall the compositor's acquire
   * loop, which Windows treats as a hung display. */
  if (!free_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] {
        return !slots_[0].in_flight || !slots_[1].in_flight;
      })) {
    ++frames_dropped_;
    return nullptr;
  }
  for (Slot& slot : slots_) {
    if (!slot.in_flight && !slot.queued) {
      return &slot.data;
    }
  }
  ++frames_dropped_;
  return nullptr;
}

void FrameSender::Submit(std::vector<uint8_t>* buffer, size_t length,
                         const Rect& damage, bool twice) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Slot& slot : slots_) {
      if (&slot.data == buffer) {
        slot.length = length;
        slot.damage = damage;
        slot.queued = true;
        slot.twice = twice;
        slot.queued_at = GetTickCount64();
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
    const ULONGLONG waited = start - slot->queued_at;
    bool ok = device_->SendFrame(slot->data.data(), slot->length);
    if (ok && slot->twice) {
      /* Same bytes again so the chip's other frame buffer matches. */
      ok = device_->SendFrame(slot->data.data(), slot->length);
    }
    ULONGLONG cost = GetTickCount64() - start;
    {
      /* Split the delay a frame sees into time spent waiting behind the
       * previous transfer and time on the wire. */
      static ULONGLONG last_latency_log = 0;
      const ULONGLONG now = GetTickCount64();
      if (now - last_latency_log >= 2000) {
        last_latency_log = now;
        Log("latency: queued %llums, on the wire %llums, %zu bytes", waited,
            cost, slot->length);
      }
    }
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
                                       FrameSender* sender, const Mode& mode,
                                       DdcCiSlave* ddc,
                                       IDDCX_MONITOR monitor)
    : swapchain_(swapchain),
      render_adapter_(render_adapter),
      new_frame_event_(new_frame_event),
      device_(device),
      sender_(sender),
      mode_(mode),
      ddc_(ddc),
      monitor_(monitor),
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
  /* Looking Glass does the same: the capture path is latency sensitive and
   * loses frames if it is scheduled behind ordinary GPU work. */
  dxgi_device->SetGPUThreadPriority(7);
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
    bool query_failed = false;

    if (meta.DirtyRectCount > 0) {
      std::vector<RECT> rects(meta.DirtyRectCount);
      IDARG_IN_GETDIRTYRECTS in = {};
      in.DirtyRectInCount = meta.DirtyRectCount;
      in.pDirtyRects = rects.data();
      IDARG_OUT_GETDIRTYRECTS out = {};
      if (NT_SUCCESS(IddCxSwapChainGetDirtyRects(swapchain_, &in, &out))) {
        for (UINT i = 0; i < out.DirtyRectOutCount; ++i) {
          damage = MergeRects(damage, FromRECT(rects[i]));
        }
      } else {
        query_failed = true;
      }
    }

    if (meta.MoveRegionCount > 0) {
      /* A move region says "this block of pixels moved from here to there".
       * Both ends change: the destination gains the content and the source
       * is repainted with whatever was behind it. Blasting the whole screen
       * instead, which is the obvious shortcut, turns every window drag into
       * a full 4 MB transfer and is the main cause of drag lag on USB 2. */
      std::vector<IDDCX_MOVEREGION> moves(meta.MoveRegionCount);
      for (auto& move : moves) {
        move.Size = sizeof(move);
      }
      IDARG_IN_GETMOVEREGIONS in = {};
      in.MoveRegionInCount = meta.MoveRegionCount;
      in.pMoveRegions = moves.data();
      IDARG_OUT_GETMOVEREGIONS out = {};
      if (NT_SUCCESS(IddCxSwapChainGetMoveRegions(swapchain_, &in, &out))) {
        for (UINT i = 0; i < out.MoveRegionOutCount; ++i) {
          /* Only the destination. A move says content relocated from one
           * place to another; a driver that can blit would copy it, and one
           * that cannot, like this chip, repaints the destination instead.
           * Anything the move uncovered arrives separately as a dirty rect.
           *
           * Merging the source as well looks safer but is badly wrong here,
           * because everything is reduced to a single bounding rectangle:
           * dragging a window across the screen puts the source and the
           * destination far apart and the union swells to most of the
           * display. Measured, that turned a 75 KB update into 3.2 MB, and
           * 15 ms on the wire into 109 ms, which is precisely the lag that
           * shows up while dragging. */
          damage = MergeRects(damage, FromRECT(moves[i].DestRect));
        }
      } else {
        query_failed = true;
      }
    }

    if (query_failed) {
      /* Only fall back to a full repaint when the OS would not tell us what
       * actually changed. */
      damage.x1 = 0;
      damage.y1 = 0;
      damage.x2 = fb_width;
      damage.y2 = fb_height;
    }
    have_new_damage = !damage.empty();
  }

  Rect to_send;
  Rect covered;
  {
    std::lock_guard<std::mutex> damage_lock(damage_mutex_);
    if (have_new_damage) {
      pending_damage_[0] = MergeRects(pending_damage_[0], damage);
    }

    /* Send this frame's damage together with the previous frame's, which is
     * what the Linux driver does and what the chip's double buffering
     * requires. Each transfer lands in one of two frame buffers and they
     * alternate on screen, so a region touched by only one transfer shows the
     * new content on one refresh and the old on the next. Covering two
     * consecutive frames' damage every time guarantees both buffers receive
     * every change. Dropping this is what makes redrawn text shimmer. */
    covered = pending_damage_[0];
    to_send = AlignDamageRect(MergeRects(covered, pending_damage_[1]),
                              fb_width, fb_height);
  }
  if (to_send.empty()) {
    return true;
  }

  Rect superseded = EmptyRect();
  std::vector<uint8_t>* transfer =
      sender_->AcquireBuffer(kBufferWaitMs, &superseded);
  if (!transfer) {
    /* Dropped, but the damage stays pending so a later frame still sends it. */
    return true;
  }

  if (!superseded.empty()) {
    /* A queued frame was taken back before it reached the chip, so the region
     * it covered is still stale there and this frame has to carry it too. */
    std::lock_guard<std::mutex> damage_lock(damage_mutex_);
    covered = MergeRects(covered, superseded);
    to_send = AlignDamageRect(MergeRects(to_send, superseded), fb_width,
                              fb_height);
    if (to_send.empty()) {
      sender_->Cancel(transfer);
      return true;
    }
  }

  LARGE_INTEGER t_begin, t_converted, qpc_freq;
  QueryPerformanceFrequency(&qpc_freq);
  QueryPerformanceCounter(&t_begin);

  PictureAdjust adjust;
  if (ddc_) {
    adjust.brightness = ddc_->brightness();
    adjust.contrast = ddc_->contrast();
  }

  const size_t row_bytes = static_cast<size_t>(to_send.width()) * 2;
  const size_t pixel_bytes = row_bytes * to_send.height();
  const size_t length = pixel_bytes + kFrameOverhead;
  if (transfer->size() < length) {
    sender_->Cancel(transfer);
    return false;
  }

  /* One conversion path only. A GPU compute shader was implemented and
   * measured: it is slower than the threaded SIMD path for ordinary damage,
   * about equal at full screen, and its output differs from the CPU path by
   * one least significant bit. Choosing between them by damage size meant a
   * region could be converted one way and then the other, which alternates
   * the picture and is visible on antialiased text. Not worth the CPU saving.
   */
  {
    /* Copy the damaged region out of the GPU, then convert while reading the
     * mapped staging texture. */
    if (!EnsureStaging(source_desc.Width, source_desc.Height)) {
      sender_->Cancel(transfer);
      return false;
    }
    D3D11_BOX box = {};
    box.left = static_cast<UINT>(to_send.x1);
    box.top = static_cast<UINT>(to_send.y1);
    box.front = 0;
    box.right = static_cast<UINT>(to_send.x2);
    box.bottom = static_cast<UINT>(to_send.y2);
    box.back = 1;
    d3d_context_->CopySubresourceRegion(staging_.Get(), 0, box.left, box.top,
                                        0, source.Get(), 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(d3d_context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0,
                                 &mapped))) {
      sender_->Cancel(transfer);
      return false;
    }
    const size_t produced =
        FrameRect(transfer->data(), transfer->size(),
                  static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                  fb_width, fb_height, to_send, adjust);
    d3d_context_->Unmap(staging_.Get(), 0);
    if (produced == 0) {
      sender_->Cancel(transfer);
      return false;
    }
  }

  QueryPerformanceCounter(&t_converted);
  {
    static ULONGLONG last_phase_log = 0;
    const ULONGLONG phase_now = GetTickCount64();
    if (phase_now - last_phase_log >= 30000) {
      last_phase_log = phase_now;
      const double to_us = 1000000.0 / qpc_freq.QuadPart;
      Log("phases: convert=%.0fus  %dx%d (%zu bytes)",
          (t_converted.QuadPart - t_begin.QuadPart) * to_us, to_send.width(),
          to_send.height(), length);
    }
  }

  /* Once is enough. Sending every update twice, so both of the chip's frame
   * buffers hold identical content, was tried and did not remove the text
   * shimmer, so whatever the chip does with its second buffer is not the
   * cause and the extra transfer was pure cost. */
  sender_->Submit(transfer, length, to_send);

  {
    /* [1] remembers what this frame covered, so the next transfer repeats it
     * for the other chip buffer. [0] starts accumulating again from empty. */
    std::lock_guard<std::mutex> damage_lock(damage_mutex_);
    pending_damage_[1] = covered;
    pending_damage_[0] = EmptyRect();
  }
  last_send_ms_ = now_ms;

  /* Report how much of the screen each transfer actually covers: if damage
   * tracking is working this should be far smaller than the full frame. */
  static ULONGLONG last_rect_log = 0;
  if (now_ms - last_rect_log >= 30000) {
    Log("damage: %dx%d at (%d,%d) rects=%u moves=%u -> %zu bytes",
        to_send.width(), to_send.height(), to_send.x1, to_send.y1,
        meta.DirtyRectCount, meta.MoveRegionCount, length);
    last_rect_log = now_ms;
  }

  force_full_frame_ = false;
  return true;
}

bool SwapChainProcessor::SendRefresh(bool whole_screen) {
  (void)whole_screen;
  if (!last_source_ || last_width_ <= 0 || last_height_ <= 0) {
    return false;
  }

  Rect full;
  full.x1 = 0;
  full.y1 = 0;
  full.x2 = last_width_;
  full.y2 = last_height_;
  full = AlignDamageRect(full, last_width_, last_height_);
  if (full.empty()) {
    return false;
  }

  Rect superseded = EmptyRect();
  std::vector<uint8_t>* transfer =
      sender_->AcquireBuffer(kBufferWaitMs, &superseded);
  if (!transfer) {
    return false;
  }

  if (!EnsureStaging(static_cast<UINT>(last_width_),
                     static_cast<UINT>(last_height_))) {
    sender_->Cancel(transfer);
    return false;
  }
  d3d_context_->CopyResource(staging_.Get(), last_source_.Get());

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(d3d_context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0,
                               &mapped))) {
    sender_->Cancel(transfer);
    return false;
  }

  PictureAdjust adjust;
  if (ddc_) {
    adjust.brightness = ddc_->brightness();
    adjust.contrast = ddc_->contrast();
  }
  const size_t length =
      FrameRect(transfer->data(), transfer->size(),
                static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                last_width_, last_height_, full, adjust);
  d3d_context_->Unmap(staging_.Get(), 0);
  if (length == 0) {
    sender_->Cancel(transfer);
    return false;
  }

  sender_->Submit(transfer, length, full);
  {
    /* A full repaint leaves nothing owed. */
    std::lock_guard<std::mutex> damage_lock(damage_mutex_);
    pending_damage_[0] = EmptyRect();
    pending_damage_[1] = EmptyRect();
  }
  last_send_ms_ = GetTickCount64();
  return true;
}

void SwapChainProcessor::Run() {
  /* Ask for a slightly raised priority: the compositor considers the monitor
   * hung if we fall too far behind on the acquire loop. */
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

  Log("SwapChain: processing started, %u conversion thread(s)",
      ConversionThreads());

  unsigned logged = 0;
  ULONGLONG last_report = GetTickCount64();

  HANDLE waits[] = {new_frame_event_, terminate_event_};
  for (;;) {
    IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
    NTSTATUS status =
        IddCxSwapChainReleaseAndAcquireBuffer(swapchain_, &buffer);

    if (status == E_PENDING) {
      const ULONGLONG idle_now = GetTickCount64();

      /* Picking up a brightness change needs a repaint even though the
       * desktop itself has not changed. */
      if (ddc_ && idle_now - last_settings_poll_ms_ >= 500) {
        last_settings_poll_ms_ = idle_now;
        if (ddc_->RefreshFromRegistry()) {
          SendRefresh(true);
        }
      }

      /* Nothing new to draw. The panel blanks without traffic, so it still
       * needs something periodically, but a full repaint is the wrong tool:
       * two full frames cost 250 ms during which no real update can go out,
       * and it repaints from a copy built up region by region, so anywhere
       * the copy never received shows stale content. Only refresh once the
       * link has genuinely been quiet. */
      const unsigned long long idle_limit =
          ReadPolicyDword(L"IdleRefreshMs",
                          static_cast<DWORD>(kDefaultIdleRefreshMs));
      if (idle_limit != 0 && last_send_ms_ != 0 &&
          idle_now - last_send_ms_ >= idle_limit) {
        SendRefresh(false);
      }
      DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 17);
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

  /* Experimental: the chip's double buffering is the leading suspect for the
   * text shimmer, and the bypass transfer modes may avoid it. Default stays
   * on the mode the vendor driver uses. */
  const uint8_t transfer_mode =
      static_cast<uint8_t>(ReadPolicyDword(L"TransferMode",
                                           kTransModeManualBlock));
  ms_device_->SetTransferMode(transfer_mode);
  if (transfer_mode != kTransModeManualBlock) {
    Log("PrepareHardware: transfer mode %u", transfer_mode);
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
  const ULONGLONG release_start = GetTickCount64();
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
  Log("ReleaseHardware: done at +%llums", GetTickCount64() - release_start);
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
      /* 1080p30 is a real chip mode and is a far better match for the
       * available bandwidth than 1080p60, so offer it first. */
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
  /* Connector type reported to the OS. HDMI is the safe default; INTERNAL is
   * selectable because brightness tools decide which control path to use from
   * this value, and the internal path is WMI rather than DDC/CI. Overridable
   * so the tradeoff can be tested without a rebuild. */
  info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  if (ReadPolicyDword(L"ReportAsInternal", 0) != 0) {
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
    Log("CreateMonitor: reporting connector as INTERNAL");
  }
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
    Log("CommitModes: %ux%u@%u -> chip mode 0x%02X, sync divider %u",
        width, height, hz, mode->mode_id, SyncDividerForMode(*mode));
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
      ms_device_.get(), sender_.get(), active_mode_, &ddc_, monitor_));

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
