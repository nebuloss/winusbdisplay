/* SPDX-License-Identifier: GPL-2.0-only */

#include "pipeline.h"

#include <dxgi1_5.h>

#include <algorithm>
#include <cstring>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace usbhdmi {
namespace {

/* How long to wait for a free transfer buffer before abandoning the update.
 * Long enough to ride out a transfer that is nearly finished, short enough
 * that the acquire loop never looks stalled. */
constexpr unsigned kBufferWaitMs = 10;

/* The panel drops its signal after a second or two of silence, which is why
 * the repaint below exists at all. There is deliberately no interval: it runs
 * whenever the link is idle, so the spare capacity is used rather than
 * waited out. */
/* Rows per keepalive update. At 1920 wide this is about 490 KB, which is
 * just inside one of the adapter's slots, so it is as large as it can be
 * without costing a second one. */
constexpr int kIdleBandHeight = 128;

constexpr unsigned long long kSettingsPollMs = 500;

Rect FromRECT(const RECT& rect) {
  Rect out;
  out.x1 = rect.left;
  out.y1 = rect.top;
  out.x2 = rect.right;
  out.y2 = rect.bottom;
  return out;
}

}  // namespace

Pipeline::Pipeline(IDDCX_SWAPCHAIN swapchain, LUID render_adapter,
                   HANDLE new_frame_event, Chip* chip, FrameSender* sender,
                   const Mode& mode)
    : swapchain_(swapchain),
      render_adapter_(render_adapter),
      new_frame_event_(new_frame_event),
      chip_(chip),
      sender_(sender),
      mode_(mode) {
  terminate_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  damage_.Configure(mode_.width, mode_.height);
  scratch_.resize(static_cast<size_t>(mode_.width) * mode_.height * 2);
  onscreen_stride_ = static_cast<size_t>(mode_.width) * 2;
  onscreen_.assign(onscreen_stride_ * mode_.height, 0);
  onscreen_valid_ = false;
  settings_ = ReadSettings();
}

Pipeline::~Pipeline() {
  Stop();
  if (terminate_event_) {
    CloseHandle(terminate_event_);
  }
}

bool Pipeline::Start() {
  if (!CreateDevice()) {
    Log("pipeline: Direct3D setup failed, cannot drive this swapchain");
    return false;
  }

  gpu_usable_ = gpu_.Initialise(device_.Get(), context_.Get());
  Log("pipeline: %s, %u conversion thread(s), GPU path %s",
      gpu_usable_ ? "GPU and CPU conversion available" : "CPU conversion only",
      ConversionThreads(), gpu_usable_ ? "ready" : gpu_.error());

  thread_ = std::thread(&Pipeline::Run, this);
  return true;
}

void Pipeline::Stop() {
  if (terminate_event_) {
    SetEvent(terminate_event_);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool Pipeline::CreateDevice() {
  ComPtr<IDXGIFactory5> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    return false;
  }
  ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(
          factory->EnumAdapterByLuid(render_adapter_, IID_PPV_ARGS(&adapter)))) {
    return false;
  }

  D3D_FEATURE_LEVEL level;
  if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               0, nullptr, 0, D3D11_SDK_VERSION, &device_,
                               &level, &context_))) {
    return false;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(device_.As(&dxgi_device))) {
    return false;
  }
  /* The acquire loop is latency sensitive and loses frames if it is
   * scheduled behind ordinary graphics work. */
  dxgi_device->SetGPUThreadPriority(7);

  IDARG_IN_SWAPCHAINSETDEVICE set_device = {};
  set_device.pDevice = dxgi_device.Get();
  return NT_SUCCESS(IddCxSwapChainSetDevice(swapchain_, &set_device));
}

bool Pipeline::EnsureStaging(int width, int height) {
  if (staging_ && staging_width_ >= width && staging_height_ >= height) {
    return true;
  }

  staging_.Reset();
  D3D11_TEXTURE2D_DESC desc;
  memset(&desc, 0, sizeof(desc));
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) {
    return false;
  }
  staging_width_ = width;
  staging_height_ = height;
  return true;
}

bool Pipeline::ConvertOnCpu(ID3D11Texture2D* source, const Rect& rect) {
  if (!EnsureStaging(mode_.width, mode_.height)) {
    return false;
  }

  /* Copy only the damaged region, into the corner of the staging texture, so
   * the map that follows touches as little memory as possible. Reading mapped
   * graphics memory is far slower than ordinary memory and is most of the
   * cost of this path. */
  D3D11_BOX box;
  box.left = static_cast<UINT>(rect.x1);
  box.top = static_cast<UINT>(rect.y1);
  box.front = 0;
  box.right = static_cast<UINT>(rect.x2);
  box.bottom = static_cast<UINT>(rect.y2);
  box.back = 1;
  context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, source, 0, &box);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }

  /* The region now sits at the origin of the staging texture, so convert it
   * as if it were a whole image of that size. RowPitch is not width * 4. */
  Rect local;
  local.x1 = 0;
  local.y1 = 0;
  local.x2 = rect.width();
  local.y2 = rect.height();
  ConvertRegion(scratch_.data(), static_cast<const uint8_t*>(mapped.pData),
                mapped.RowPitch, local, settings_.picture());

  context_->Unmap(staging_.Get(), 0);
  ++cpu_conversions_;
  return true;
}

bool Pipeline::ConvertForSending(ID3D11Texture2D* source,
                                 const Rect& rect) {
  const bool prefer_gpu =
      gpu_usable_ && settings_.gpu_threshold_pixels > 0 &&
      ShouldConvertOnGpu(rect, settings_.gpu_threshold_pixels);

  if (prefer_gpu) {
    if (gpu_.Convert(source, rect, settings_.picture(), scratch_.data(),
                     scratch_.size())) {
      ++gpu_conversions_;
      return true;
    }
    /* One failure is enough to stop trying. The two paths produce identical
     * bytes, so falling back costs nothing but processor time, whereas
     * retrying a broken shader every frame costs a stall every frame. */
    Log("pipeline: GPU conversion failed (%s), using the processor from now on",
        gpu_.error());
    gpu_usable_ = false;
  }

  return ConvertOnCpu(source, rect);
}

bool Pipeline::SendRegion(ID3D11Texture2D* source, const Rect& rect,
                          bool force) {
  if (rect.empty()) {
    return true;
  }

  if (!ConvertForSending(source, rect)) {
    return false;
  }

  /* Send only what actually differs from what the panel is showing. The
   * compositor sometimes reports far more than it needs to, and a full
   * screen update costs eight refresh slots against one for a typical
   * region.
   *
   * `force` skips this, and exists for the idle repaint. That repaint's
   * whole purpose is to put traffic on the wire, because the panel drops its
   * signal after a second or two of silence. Letting the comparison decide
   * there is nothing to send would defeat it exactly when the desktop is
   * still, which is precisely when it is needed. */
  Rect changed = rect;
  if (onscreen_valid_ && !force) {
    changed = ShrinkChangedUyvy(rect, scratch_.data(), onscreen_.data(),
                                onscreen_stride_);
  }

  if (changed.empty()) {
    ++regions_skipped_;
    return true;
  }

  /* Widen to the granularity the adapter wants. It can only grow within the
   * already aligned region, so it stays inside the converted pixels. */
  changed = AlignDamageRect(changed, mode_.width, mode_.height);
  changed.x1 = std::max<int>(changed.x1, rect.x1);
  changed.y1 = std::max<int>(changed.y1, rect.y1);
  changed.x2 = std::min<int>(changed.x2, rect.x2);
  changed.y2 = std::min<int>(changed.y2, rect.y2);
  if (changed.empty()) {
    ++regions_skipped_;
    return true;
  }

  /* Twice, back to back, and both buffers are taken before either is sent.
   *
   * The adapter keeps two copies of the picture and alternates between them
   * on every transfer, so a region carried by a single transfer lands in one
   * copy and leaves the other holding what was there before. The two then
   * alternate on screen. With a moving mouse pointer that is unmistakable:
   * the pointer appears in two places at once, the old position refusing to
   * erase.
   *
   * Which is why the pair is all or nothing. Sending the first and then
   * discovering there is no buffer for the second produces exactly that
   * symptom, occasionally, under load. Taking both up front means the region
   * either reaches both copies or is not sent at all, and an unsent region
   * stays owed and goes out later.
   *
   * Sending the same bytes twice is also no more expensive than the obvious
   * alternative of repeating last frame's region on the next transfer: the
   * same two transfers either way, except that this way the region is
   * complete a frame sooner. */
  std::vector<uint8_t>* transfers[2] = {nullptr, nullptr};
  for (int i = 0; i < 2; ++i) {
    transfers[i] = sender_->Acquire(kBufferWaitMs);
    if (!transfers[i]) {
      for (int j = 0; j < i; ++j) {
        sender_->Release(transfers[j]);
      }
      /* The damage stays owed, so a later frame carries it, and the caller
       * abandons the rest of this frame so nothing arrives ahead of what
       * should have preceded it.
       *
       * Note what is deliberately not done here: the record of what the
       * panel is showing is left alone rather than thrown away. It still
       * describes this region correctly, because nothing was sent.
       * Discarding it would turn a moment of congestion into a lasting one,
       * since with no record to compare against every later region is sent
       * in full, which causes more congestion. */
      sender_->CountDropped();
      return false;
    }
  }

  for (int i = 0; i < 2; ++i) {
    const size_t length = FrameSubRegion(transfers[i]->data(),
                                         transfers[i]->size(),
                                         scratch_.data(), rect, changed);
    if (length == 0) {
      for (int j = i; j < 2; ++j) {
        sender_->Release(transfers[j]);
      }
      Log("pipeline: could not frame %d,%d %dx%d inside %d,%d %dx%d",
          changed.x1, changed.y1, changed.width(), changed.height(), rect.x1,
          rect.y1, rect.width(), rect.height());
      return false;
    }
    sender_->Submit(transfers[i], length);
    ++regions_sent_;
  }

  /* Both copies now hold this region, so it is safe to record it as what the
   * panel is showing. Recording it after only one transfer would make the
   * next comparison decide the second copy already had it, and the region
   * would be left stale in one of them forever. */
  StoreUyvyReference(rect, scratch_.data(), onscreen_.data(),
                     onscreen_stride_);
  if (rect.x1 == 0 && rect.y1 == 0 && rect.x2 >= mode_.width &&
      rect.y2 >= mode_.height) {
    onscreen_valid_ = true;
  }

  last_send_ms_ = GetTickCount64();
  return true;
}

void Pipeline::ProcessFrame(const IDARG_OUT_RELEASEANDACQUIREBUFFER& buffer) {
  const IDDCX_METADATA& meta = buffer.MetaData;

  ComPtr<ID3D11Texture2D> surface;
  if (FAILED(buffer.MetaData.pSurface->QueryInterface(
          IID_PPV_ARGS(surface.GetAddressOf())))) {
    return;
  }
  last_surface_ = surface;

  /* The metadata carries only the counts; the regions themselves have to be
   * fetched into buffers we provide. */
  bool have_damage = false;

  if (meta.DirtyRectCount > 0) {
    dirty_rects_.resize(meta.DirtyRectCount);
    IDARG_IN_GETDIRTYRECTS in = {};
    in.DirtyRectInCount = meta.DirtyRectCount;
    in.pDirtyRects = dirty_rects_.data();
    IDARG_OUT_GETDIRTYRECTS out = {};
    if (SUCCEEDED(IddCxSwapChainGetDirtyRects(swapchain_, &in, &out))) {
      for (UINT i = 0; i < out.DirtyRectOutCount; ++i) {
        damage_.Add(FromRECT(dirty_rects_[i]));
        have_damage = true;
      }
    }
  }

  if (meta.MoveRegionCount > 0) {
    move_regions_.resize(meta.MoveRegionCount);
    IDARG_IN_GETMOVEREGIONS in = {};
    in.MoveRegionInCount = meta.MoveRegionCount;
    in.pMoveRegions = move_regions_.data();
    IDARG_OUT_GETMOVEREGIONS out = {};
    if (SUCCEEDED(IddCxSwapChainGetMoveRegions(swapchain_, &in, &out))) {
      for (UINT i = 0; i < out.MoveRegionOutCount; ++i) {
        /* Both where the window was and where it went. Adding them
         * separately lets the planner decide whether keeping them apart is
         * worth it; merging them here would turn a small update into a
         * multi-megabyte one for the whole duration of a drag. */
        const RECT& destination = move_regions_[i].DestRect;
        damage_.Add(FromRECT(destination));

        RECT source;
        source.left = move_regions_[i].SourcePoint.x;
        source.top = move_regions_[i].SourcePoint.y;
        source.right = source.left + (destination.right - destination.left);
        source.bottom = source.top + (destination.bottom - destination.top);
        damage_.Add(FromRECT(source));
        have_damage = true;
      }
    }
  }

  /* No regions at all means the desktop did not change, which the compositor
   * documents explicitly. It emphatically does not mean "assume everything
   * changed": doing that repaints the whole screen on every such frame, and
   * a full repaint costs sixteen times an ordinary update and blocks the
   * wire while it goes out. The result is a display that lags behind by
   * hundreds of milliseconds precisely when there is nothing to draw. */
  if (!have_damage && damage_.Empty()) {
    return;
  }

  Rect planned[kMaxTransfersPerFrame];
  const size_t count = damage_.Plan(planned, kMaxTransfersPerFrame);

  for (size_t i = 0; i < count; ++i) {
    if (!SendRegion(surface.Get(), planned[i], false)) {
      /* Abandoning the rest of the frame keeps the order intact. Whatever
       * was not sent is still owed and goes out with the next frame. */
      for (size_t j = i; j < count; ++j) {
        damage_.Add(planned[j]);
      }
      break;
    }
  }
}

void Pipeline::RefreshIdle() {
  if (!last_surface_) {
    return;
  }

  /* The panel drops its signal after a second or two of silence, and a still
   * desktop means the compositor stops presenting entirely, so something has
   * to keep the wire busy.
   *
   * A band rather than the whole screen. A full repaint costs eight slots,
   * sixteen once it is sent to both of the adapter's copies, during which
   * nothing else can go out; a band costs one slot each. The band is safe
   * only because it is sent twice like everything else. Sent once it would
   * land in one copy and leave the other holding older content, and the two
   * would alternate visibly, which is exactly the trap a partial repaint
   * looks like a good idea right up until you fall into it.
   *
   * The band walks down the screen so that any region which somehow went
   * stale is eventually repainted anyway. */
  Rect band;
  band.x1 = 0;
  band.x2 = mode_.width;
  band.y1 = idle_band_row_;
  band.y2 = idle_band_row_ + kIdleBandHeight;
  if (band.y2 > mode_.height) {
    band.y2 = mode_.height;
  }
  band = AlignDamageRect(band, mode_.width, mode_.height);

  idle_band_row_ += kIdleBandHeight;
  if (idle_band_row_ >= mode_.height) {
    idle_band_row_ = 0;
  }

  if (!band.empty()) {
    SendRegion(last_surface_.Get(), band, true);
  }

  /* Restart the clock even if nothing went out, so a completely static
   * desktop does not retry on every pass through the loop. */
  last_send_ms_ = GetTickCount64();
}

void Pipeline::RefreshSettings() {
  const Settings updated = ReadSettings();
  const bool picture_changed = !(updated.picture() == settings_.picture());
  settings_ = updated;

  if (picture_changed) {
    /* Brightness is applied while converting, so everything already on the
     * panel was converted with the old values and none of it matches what a
     * comparison would now produce. */
    Log("pipeline: picture settings changed, repainting");
    onscreen_valid_ = false;
    damage_.MarkAll();
  }
}

void Pipeline::Run() {
  /* Slightly raised, because the compositor treats a monitor that falls too
   * far behind on the acquire loop as hung. */
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  Log("pipeline: running, mode %dx%d@%d", mode_.width, mode_.height, mode_.hz);

  /* Nothing on the panel can be trusted until the first full update lands. */
  onscreen_valid_ = false;

  HANDLE waits[] = {new_frame_event_, terminate_event_};
  unsigned long long last_report_ms = GetTickCount64();

  for (;;) {
    IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
    const NTSTATUS status =
        IddCxSwapChainReleaseAndAcquireBuffer(swapchain_, &buffer);

    if (status == E_PENDING) {
      const unsigned long long now = GetTickCount64();

      if (now - last_settings_poll_ms_ >= kSettingsPollMs) {
        last_settings_poll_ms_ = now;
        RefreshSettings();
      }

      /* Nothing new to draw. Rather than waiting for a timer, repaint a
       * band whenever the link has gone completely quiet: the capacity is
       * there and unused, and spending it walks a repaint down the screen
       * continuously, which keeps the panel's signal alive and repairs any
       * region that somehow went stale.
       *
       * The condition is "nothing queued and nothing on the wire", not "a
       * buffer is free". Queueing speculative work would keep the adapter
       * busy at the cost of making the next real update wait behind it,
       * which is the opposite of what spare capacity is for. */
      if (last_send_ms_ != 0 && settings_.idle_refresh && sender_->Idle()) {
        RefreshIdle();
      }

      const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 17);
      if (wait == WAIT_OBJECT_0 + 1) {
        break;
      }
      if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) {
        continue;
      }
      Log("pipeline: wait returned %lu, stopping", wait);
      break;
    }

    if (!NT_SUCCESS(status)) {
      Log("pipeline: acquire returned 0x%08X, stopping", status);
      break;
    }

    ProcessFrame(buffer);
    IddCxSwapChainFinishedProcessingFrame(swapchain_);

    const unsigned long long now = GetTickCount64();
    if (now - last_report_ms >= 10000) {
      last_report_ms = now;
      Log("pipeline: sent=%llu skipped=%llu dropped=%llu failed=%llu "
          "gpu=%llu cpu=%llu",
          regions_sent_, regions_skipped_, sender_->dropped(),
          sender_->failed(), gpu_conversions_, cpu_conversions_);
    }

    if (WaitForSingleObject(terminate_event_, 0) == WAIT_OBJECT_0) {
      break;
    }
  }

  Log("pipeline: stopped, sent=%llu skipped=%llu gpu=%llu cpu=%llu",
      regions_sent_, regions_skipped_, gpu_conversions_, cpu_conversions_);
}

}  // namespace usbhdmi
