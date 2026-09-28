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

/* The panel drops its signal after a second or two of silence. */
constexpr unsigned long long kIdleRefreshMs = 1200;

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

bool Pipeline::SendRegion(ID3D11Texture2D* source, const Rect& rect) {
  if (rect.empty()) {
    return true;
  }

  if (!ConvertForSending(source, rect)) {
    return false;
  }

  /* Send only what actually differs from what the panel is showing. The
   * compositor sometimes reports far more than it needs to, and a full
   * screen update costs eight refresh periods against one for a typical
   * region. */
  Rect changed = rect;
  if (onscreen_valid_) {
    changed = ShrinkChangedUyvy(rect, scratch_.data(), onscreen_.data(),
                                onscreen_stride_);
  }

  /* Either way the panel's copy is now known for the whole converted region,
   * including any part that turned out not to need sending. Once a region
   * covering the entire screen has been stored, comparisons everywhere are
   * meaningful and refinement can start. */
  StoreUyvyReference(rect, scratch_.data(), onscreen_.data(),
                     onscreen_stride_);
  if (rect.x1 == 0 && rect.y1 == 0 && rect.x2 >= mode_.width &&
      rect.y2 >= mode_.height) {
    onscreen_valid_ = true;
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

  std::vector<uint8_t>* transfer = sender_->Acquire(kBufferWaitMs);
  if (!transfer) {
    /* No buffer free. The damage stays owed, so a later frame carries it.
     * The caller must abandon the rest of this frame as well, or a region
     * would reach the adapter without the one that should have preceded it.
     *
     * The panel's copy is deliberately invalidated here rather than left
     * claiming this region was sent, since it was not. */
    onscreen_valid_ = false;
    sender_->CountDropped();
    return false;
  }

  const size_t length = FrameSubRegion(transfer->data(), transfer->size(),
                                       scratch_.data(), rect, changed);
  if (length == 0) {
    sender_->Release(transfer);
    Log("pipeline: could not frame %d,%d %dx%d inside %d,%d %dx%d",
        changed.x1, changed.y1, changed.width(), changed.height(), rect.x1,
        rect.y1, rect.width(), rect.height());
    return false;
  }

  sender_->Submit(transfer, length);
  ++regions_sent_;
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

  /* No damage information at all means the compositor is not telling us what
   * changed, so the whole screen has to be assumed dirty. The comparison in
   * SendRegion usually rescues this. */
  if (!have_damage) {
    damage_.MarkAll();
  }

  Rect planned[kMaxTransfersPerFrame];
  const size_t count = damage_.Plan(planned, kMaxTransfersPerFrame);

  for (size_t i = 0; i < count; ++i) {
    if (!SendRegion(surface.Get(), planned[i])) {
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
  if (!last_surface_ || !settings_.idle_refresh) {
    return;
  }

  /* The whole screen, not a band. A partial update lands in only one of the
   * adapter's two internal images and leaves the other holding older
   * content, which alternates visibly; a repaint whose entire purpose is to
   * resynchronise has to cover everything. */
  damage_.MarkAll();

  Rect planned[kMaxTransfersPerFrame];
  const size_t count = damage_.Plan(planned, kMaxTransfersPerFrame);
  for (size_t i = 0; i < count; ++i) {
    if (!SendRegion(last_surface_.Get(), planned[i])) {
      for (size_t j = i; j < count; ++j) {
        damage_.Add(planned[j]);
      }
      break;
    }
  }

  /* Even if the comparison found nothing to send, the panel has now been
   * checked and the clock restarts. Otherwise a completely static desktop
   * would retry every frame. */
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

      if (last_send_ms_ != 0 && now - last_send_ms_ >= kIdleRefreshMs) {
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
