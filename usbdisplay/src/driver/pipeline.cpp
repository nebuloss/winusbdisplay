/* SPDX-License-Identifier: GPL-2.0-only */

#include "pipeline.h"

#include <dxgi1_5.h>

#include <algorithm>
#include <cstring>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace usbdisplay {
namespace {

/* How long to wait for a free transfer buffer before abandoning the update.
 *
 * A buffer can only come free when a transfer finishes, and a transfer cannot
 * finish faster than one of the adapter's 16.7 ms slots. Waiting less than
 * that therefore guarantees a timeout: the wait is dead time, the region is
 * dropped after paying its full conversion cost, and it has to be converted
 * again next frame. The previous value of 10 ms did exactly that.
 *
 * Slightly over two slots, so a wait can ride out a transfer that had only
 * just started, while still bounding how long the compositor's thread is
 * held. */
constexpr unsigned kBufferWaitMs = 36;

/* How often to repaint when there is nothing new to draw.
 *
 * "Whenever the link is idle" was the original rule, written for an adapter
 * where a keepalive band was a fraction of the screen. On the USB 3 parts a
 * band is the whole screen, and that rule turns into a full frame sent
 * twice as fast as the bus will carry it: measured at 250 MB/s against a
 * ceiling of 237, which is to say permanently saturated. The adapter
 * tolerates that for a while and then stops putting a picture out, with
 * every transfer still reporting success and its own registers still
 * reporting the output live. Unplugging it is the only way back.
 *
 * So there is an interval now. The vendor's driver uses 2500 ms for the
 * same job; this is well inside that and still leaves the link essentially
 * free for real updates. */
constexpr unsigned long long kKeepaliveMs = 500;

/* Smallest keepalive band worth sending, in rows. Only a floor: the real
 * height is worked out from the device, see ChooseIdleBandRows. */
constexpr int kMinIdleBandRows = 8;

/* Upper bound on how many times any device can ask for a region to be sent.
 * Two today; the array is sized from this so the frame path allocates
 * nothing. */
constexpr int kMaxTransmissions = 2;

constexpr unsigned long long kSettingsPollMs = 500;

/* How long the gamma table must hold still before the screen is repainted
 * for it.
 *
 * A gamma change affects every pixel, so it costs a full repaint: eight of
 * the adapter's slots, doubled because everything is sent twice, which is
 * about a quarter of a second of wire time. Dragging a brightness slider
 * produces a change per pixel of travel, tens per second, and repainting on
 * each one asks for several times the traffic the link can carry. The queue
 * backs up, real updates stop getting through, and the picture freezes:
 * moving the slider loses the display rather than dimming it.
 *
 * So changes are allowed to settle first. A quarter of a second is about
 * one repaint's worth of time, which is the fastest this can usefully go,
 * and during a drag the panel still follows in visible steps. */
constexpr unsigned long long kGammaSettleMs = 250;

Rect FromRECT(const RECT& rect) {
  Rect out;
  out.x1 = rect.left;
  out.y1 = rect.top;
  out.x2 = rect.right;
  out.y2 = rect.bottom;
  return out;
}

/* The tallest keepalive band that still costs a single slot.
 *
 * This used to be 128 rows, which is about 490 KB at 1920 wide and just
 * inside one slot on the USB 2 parts. It was a fact about one chip written
 * into the device independent half of the driver, and on the USB 3 parts it
 * was badly wrong: there a transfer costs one slot whatever its size, so
 * nine bands cost nine slots where the whole screen would have cost one.
 * Measured, that is 122 ms a pass against 14.6 ms, and it looks like the
 * picture being redrawn in visible stripes.
 *
 * Asking the device how much a band costs gets the right answer for both
 * without the pipeline knowing anything about either: the USB 2 parts land
 * back on about 134 rows, the USB 3 parts on the whole screen.
 *
 * Binary search because cost rises with height but in steps, so the largest
 * height at a given cost cannot be calculated directly. */
int ChooseIdleBandRows(const DisplayDevice* device, int width, int height) {
  Rect whole;
  whole.x2 = width;
  whole.y2 = height;
  if (device->TransferCost(whole) <= 1) {
    return height;
  }

  int low = kMinIdleBandRows;
  int high = height;
  while (low < high) {
    const int middle = low + (high - low + 1) / 2;
    Rect band;
    band.x2 = width;
    band.y2 = middle;
    if (device->TransferCost(band) <= 1) {
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  return low < kMinIdleBandRows ? kMinIdleBandRows : low;
}

}  // namespace

Pipeline::Pipeline(IDDCX_SWAPCHAIN swapchain, LUID render_adapter,
                   HANDLE new_frame_event, DisplayDevice* device, FrameSender* sender,
                   const Mode& mode)
    : swapchain_(swapchain),
      render_adapter_(render_adapter),
      new_frame_event_(new_frame_event),
      device_(device),
      sender_(sender),
      mode_(mode) {
  terminate_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  damage_.Configure(mode_.width, mode_.height, device_);
  scratch_.resize(device_->BytesPerRow(mode_.width) *
                  static_cast<size_t>(mode_.height));
  onscreen_stride_ = device_->BytesPerRow(mode_.width);
  onscreen_.assign(onscreen_stride_ * mode_.height, 0);
  onscreen_valid_ = false;
  settings_ = ReadSettings();
  gamma_.Reset();
  pending_gamma_.Reset();
}

void Pipeline::SetGammaRamp(const GammaRamp& gamma) {
  std::lock_guard<std::mutex> guard(gamma_lock_);
  if (gamma_changed_ && pending_gamma_ == gamma) {
    /* Same table again: leave the clock alone so a program that repeats
     * itself cannot hold off the repaint indefinitely. */
    return;
  }
  pending_gamma_ = gamma;
  gamma_changed_ = true;
  gamma_changed_at_ms_ = GetTickCount64();
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

  /* Graphics card first, so it gets the chance to claim large regions. */
  std::unique_ptr<GpuRegionConverter> gpu(new GpuRegionConverter());
  if (!gpu->Initialise(d3d_device_.Get(), d3d_context_.Get())) {
    Log("pipeline: no graphics conversion (%s), using the processor only",
        gpu->error());
  } else {
    converters_.Add(std::move(gpu));
  }
  converters_.Add(std::unique_ptr<RegionConverter>(
      new CpuRegionConverter(d3d_device_.Get(), d3d_context_.Get())));

  transmissions_ = device_->TransmissionsPerRegion();
  Log("pipeline: converters: %s, %u thread(s), %d transmission(s) per region",
      converters_.Describe().c_str(), ConversionThreads(), transmissions_);

  /* What a whole-screen update costs this adapter, asked through the
   * interface rather than assumed.
   *
   * It is the number that separates the two families: eight periods on the
   * USB 2 parts, one on the USB 3 parts. It also decides whether the
   * planner merges freely or keeps regions apart, so having it in the log
   * turns "why is this slow" into something answerable from a log file
   * rather than from a bench. */
  const Rect whole = {0, 0, mode_.width, mode_.height};
  idle_band_rows_ = ChooseIdleBandRows(device_, mode_.width, mode_.height);
  Log("pipeline: a full repaint costs %d period(s) on this adapter, "
      "keepalive band %d rows",
      device_->TransferCost(whole), idle_band_rows_);

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
                               0, nullptr, 0, D3D11_SDK_VERSION, &d3d_device_,
                               &level, &d3d_context_))) {
    return false;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(d3d_device_.As(&dxgi_device))) {
    return false;
  }
  /* The acquire loop is latency sensitive and loses frames if it is
   * scheduled behind ordinary graphics work. */
  dxgi_device->SetGPUThreadPriority(7);

  IDARG_IN_SWAPCHAINSETDEVICE set_device = {};
  set_device.pDevice = dxgi_device.Get();
  return NT_SUCCESS(IddCxSwapChainSetDevice(swapchain_, &set_device));
}


bool Pipeline::ConvertForSending(ID3D11Texture2D* source,
                                 const Rect& rect) {
  const char* used = nullptr;
  if (!converters_.Convert(source, rect, settings_.picture(), gamma_,
                           scratch_.data(), scratch_.size(),
                           settings_.gpu_threshold_pixels, &used)) {
    Log("pipeline: no converter could handle a %dx%d region", rect.width(),
        rect.height());
    return false;
  }
  return true;
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
  changed = device_->AlignRegion(changed, mode_.width, mode_.height);
  changed.x1 = std::max<int>(changed.x1, rect.x1);
  changed.y1 = std::max<int>(changed.y1, rect.y1);
  changed.x2 = std::min<int>(changed.x2, rect.x2);
  changed.y2 = std::min<int>(changed.y2, rect.y2);
  if (changed.empty()) {
    ++regions_skipped_;
    return true;
  }

  if (!SubmitConverted(rect, changed)) {
    return false;
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

/* Sends `sub` out of the converted pixels sitting in scratch_, which cover
 * `region`. Twice, and all or nothing: see the comment below. */
bool Pipeline::SubmitConverted(const Rect& region, const Rect& sub) {
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
  std::vector<uint8_t>* transfers[kMaxTransmissions] = {nullptr, nullptr};
  for (int i = 0; i < transmissions_; ++i) {
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

  /* Frame both before submitting either. Framing the first, submitting it,
   * and only then discovering the second cannot be framed would leave the
   * region in one copy and reintroduce the very alternation this pair
   * exists to prevent. The two are identical, so the second is a copy. */
  const size_t length = device_->Frame(transfers[0]->data(),
                                       transfers[0]->size(), scratch_.data(),
                                       region, sub);
  if (length == 0) {
    for (int i = 0; i < transmissions_; ++i) {
      sender_->Release(transfers[i]);
    }
    Log("pipeline: could not frame %d,%d %dx%d inside %d,%d %dx%d",
        sub.x1, sub.y1, sub.width(), sub.height(), region.x1,
        region.y1, region.width(), region.height());
    return false;
  }
  for (int i = 1; i < transmissions_; ++i) {
    memcpy(transfers[i]->data(), transfers[0]->data(), length);
  }
  for (int i = 0; i < transmissions_; ++i) {
    sender_->Submit(transfers[i], length);
    ++regions_sent_;
    bytes_sent_ += length;
  }

  /* The first few regions, with their geometry.
   *
   * What the driver puts on the wire is otherwise invisible: the counters
   * say transfers succeeded, not what was in them, and a chip will accept a
   * transfer describing the wrong rectangle without complaint. Four lines
   * once per session is a small price for being able to tell a full repaint
   * from a stream of empty updates.
   *
   * Not a running log: at sixty updates a second that would be useless and
   * would itself slow the frame thread. */
  if (regions_logged_ < 4) {
    ++regions_logged_;
    Log("pipeline: transfer %u: %d,%d %dx%d of region %dx%d, %zu bytes",
        regions_logged_, sub.x1, sub.y1, sub.width(), sub.height(),
        region.width(), region.height(), length);
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
  /* The panel drops its signal after a second or two of silence, and a still
   * desktop means the compositor stops presenting entirely, so something has
   * to keep the wire busy.
   *
   * A band rather than the whole screen: one slot against eight, doubled
   * because everything is sent twice. It walks down the screen, so a picture
   * that has gone stale is brought back a band at a time without ever
   * blocking the wire for a quarter of a second.
   *
   * The band is converted afresh from the last image the compositor gave us
   * rather than replayed from the record of what was sent. That matters when
   * the record has been thrown away, which is what happens when the gamma
   * table changes: replaying would have nothing to replay, so nothing would
   * be sent, and on a still desktop nothing else is coming either. The panel
   * would sit there receiving no data at all and go dark, which is precisely
   * what this exists to prevent. */
  if (!last_surface_) {
    return;
  }

  /* The band is as tall as a single slot allows, whichever job this is.
   *
   * Sending a short band when the picture is already correct was tried,
   * on the reasoning that a transfer costs a slot whatever it carries so
   * the bytes are free to save. It is not enough: the panel blanks and
   * flickers on a still desktop. Whatever the adapter is counting to
   * decide its output is still live, a few rows does not satisfy it.
   *
   * So the saving comes from how often this runs, not from how much it
   * sends. See the keepalive interval in Run. */
  const int rows = idle_band_rows_;

  Rect band;
  band.x1 = 0;
  band.x2 = mode_.width;
  band.y1 = idle_band_row_;
  band.y2 = idle_band_row_ + rows;
  if (band.y2 > mode_.height) {
    band.y2 = mode_.height;
  }
  band = device_->AlignRegion(band, mode_.width, mode_.height);

  idle_band_row_ += rows;
  if (idle_band_row_ >= mode_.height) {
    idle_band_row_ = 0;
    /* One full pass done, so everything on the panel has now been drawn
     * with the settings in force and the record can be trusted again. */
    onscreen_valid_ = true;
  }

  if (!band.empty()) {
    /* Forced: the point is to put traffic on the wire, so it must send even
     * where the pixels have not changed. */
    SendRegion(last_surface_.Get(), band, true);
  }

  last_send_ms_ = GetTickCount64();
}

/* Picks up a gamma table the operating system installed, and repaints if it
 * differs. Everything already on the panel was converted through the old
 * table, so none of it matches what a comparison would now produce. */
void Pipeline::CheckGammaRamp() {
  GammaRamp updated;
  {
    std::lock_guard<std::mutex> guard(gamma_lock_);
    if (!gamma_changed_) {
      return;
    }
    /* Wait for it to stop moving. See kGammaSettleMs: acting on every change
     * asks for many times the traffic the link can carry, and the display
     * stops responding altogether. */
    if (GetTickCount64() - gamma_changed_at_ms_ < kGammaSettleMs) {
      return;
    }
    gamma_changed_ = false;
    updated = pending_gamma_;
  }

  if (updated == gamma_) {
    return;
  }
  gamma_ = updated;
  Log("pipeline: gamma table settled (%s), repainting",
      gamma_.identity ? "back to neutral" : "adjusting the picture");
  onscreen_valid_ = false;
  damage_.MarkAll();
}

void Pipeline::CheckAdapterReprogrammed() {
  /* Reprogramming clears the adapter's picture memory, and it can happen
   * without this thread asking: a run of failed transfers makes the sender
   * reset the adapter, and a mode change comes from the OS thread. Either
   * way what is on the panel no longer resembles the record kept here, and
   * sending differences against that record would leave the screen showing
   * fragments of whatever came before. */
  const uint64_t generation = device_->generation();
  if (generation == adapter_generation_) {
    return;
  }
  Log("pipeline: adapter was reprogrammed, repainting everything");
  adapter_generation_ = generation;
  onscreen_valid_ = false;
  damage_.MarkAll();
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

      /* Checked every pass rather than on the slower poll, because a
       * brightness slider should follow the pointer, not lag half a second
       * behind it. */
      CheckGammaRamp();

      if (now - last_settings_poll_ms_ >= kSettingsPollMs) {
        last_settings_poll_ms_ = now;
        RefreshSettings();
        CheckAdapterReprogrammed();
      }

      /* Nothing new to draw.
       *
       * Repainting is still how the panel is kept alive and how a region
       * that went stale is repaired, but it is rationed rather than run
       * flat out: see kKeepaliveMs, where saturating the bus is what
       * stopped the adapter putting out a picture at all.
       *
       * While the record of what is on the panel cannot be trusted the
       * ration is lifted, because the picture is wrong until a full pass
       * has completed and finishing that quickly matters more than the
       * traffic it costs. On the USB 3 parts a pass is a single transfer.
       *
       * The condition is "nothing queued and nothing on the wire", not "a
       * buffer is free". Queueing speculative work would keep the adapter
       * busy at the cost of making the next real update wait behind it,
       * which is the opposite of what spare capacity is for. */
      if (last_send_ms_ != 0 && settings_.idle_refresh && sender_->Idle() &&
          (!onscreen_valid_ || now - last_send_ms_ >= kKeepaliveMs)) {
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

    CheckGammaRamp();
    CheckAdapterReprogrammed();
    ProcessFrame(buffer);
    IddCxSwapChainFinishedProcessingFrame(swapchain_);

    const unsigned long long now = GetTickCount64();
    if (now - last_report_ms >= 10000) {
      last_report_ms = now;
      Log("pipeline: sent=%llu skipped=%llu dropped=%llu failed=%llu, "
          "%llu MB total",
          regions_sent_, regions_skipped_, sender_->dropped(),
          sender_->failed(), bytes_sent_ / (1024ull * 1024ull));
    }

    if (WaitForSingleObject(terminate_event_, 0) == WAIT_OBJECT_0) {
      break;
    }
  }

  Log("pipeline: stopped, sent=%llu skipped=%llu", regions_sent_,
      regions_skipped_);
}

}  // namespace usbdisplay
