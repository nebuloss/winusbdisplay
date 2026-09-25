/* SPDX-License-Identifier: GPL-2.0-only */

#include "device.h"

#include <objbase.h>

#include "convert_cs.h"
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
  cursor_previous_ = EmptyRect();
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
  if (cursor_event_) {
    /* Wake the cursor thread so it notices the terminate event. */
    SetEvent(cursor_event_);
  }
  if (cursor_thread_.joinable()) {
    cursor_thread_.join();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (cursor_event_) {
    CloseHandle(cursor_event_);
    cursor_event_ = nullptr;
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

namespace {

/* Mirrors the cbuffer in convert_cs.hlsl. */
struct ConvertParams {
  UINT origin_x;
  UINT origin_y;
  UINT size_x;
  UINT size_y;
  UINT row_bytes;
  UINT luma_gain;
  UINT chroma_gain;
  UINT padding;
};

}  // namespace

bool SwapChainProcessor::EnsureCompute() {
  if (compute_ready_) {
    return true;
  }
  if (compute_failed_) {
    return false;
  }
  compute_failed_ = true;  /* cleared on success, so failure is not retried */

  if (ReadPolicyDword(L"UseComputeShader", 1) == 0) {
    Log("compute: disabled by policy, using the CPU path");
    return false;
  }

  if (FAILED(d3d_device_->CreateComputeShader(kConvertComputeShader,
                                              sizeof(kConvertComputeShader),
                                              nullptr, &compute_shader_))) {
    Log("compute: CreateComputeShader failed, using the CPU path");
    return false;
  }

  /* One buffer sized for the largest mode, so nothing is reallocated per
   * frame. A byte address buffer needs the raw views flag. */
  D3D11_BUFFER_DESC output = {};
  output.ByteWidth = static_cast<UINT>(kMaxWidth * kMaxHeight * 2);
  output.Usage = D3D11_USAGE_DEFAULT;
  output.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  output.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  output.StructureByteStride = 0;
  if (FAILED(d3d_device_->CreateBuffer(&output, nullptr, &compute_output_))) {
    Log("compute: output buffer allocation failed");
    return false;
  }

  D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav.Buffer.FirstElement = 0;
  uav.Buffer.NumElements = output.ByteWidth / 4;
  uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(d3d_device_->CreateUnorderedAccessView(
          compute_output_.Get(), &uav, &compute_output_uav_))) {
    Log("compute: UAV creation failed");
    return false;
  }

  D3D11_BUFFER_DESC readback = {};
  readback.ByteWidth = output.ByteWidth;
  readback.Usage = D3D11_USAGE_STAGING;
  readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(d3d_device_->CreateBuffer(&readback, nullptr,
                                       &compute_readback_))) {
    Log("compute: readback buffer allocation failed");
    return false;
  }

  D3D11_BUFFER_DESC params = {};
  params.ByteWidth = sizeof(ConvertParams);
  params.Usage = D3D11_USAGE_DYNAMIC;
  params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  params.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(d3d_device_->CreateBuffer(&params, nullptr, &compute_params_))) {
    Log("compute: constant buffer allocation failed");
    return false;
  }

  compute_failed_ = false;
  compute_ready_ = true;
  Log("compute: GPU conversion enabled");
  return true;
}

bool SwapChainProcessor::ConvertOnGpu(ID3D11Texture2D* source,
                                      const Rect& rect,
                                      const PictureAdjust& adjust,
                                      uint8_t* dst, size_t dst_capacity) {
  const size_t row_bytes = static_cast<size_t>(rect.width()) * 2;
  const size_t needed = row_bytes * rect.height();
  if (needed > dst_capacity) {
    return false;
  }

  if (compute_source_.Get() != source) {
    compute_source_srv_.Reset();
    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(d3d_device_->CreateShaderResourceView(source, &srv,
                                                     &compute_source_srv_))) {
      return false;
    }
    compute_source_ = source;
  }

  D3D11_MAPPED_SUBRESOURCE mapped_params = {};
  if (FAILED(d3d_context_->Map(compute_params_.Get(), 0,
                               D3D11_MAP_WRITE_DISCARD, 0, &mapped_params))) {
    return false;
  }
  ConvertParams* params = static_cast<ConvertParams*>(mapped_params.pData);
  params->origin_x = static_cast<UINT>(rect.x1);
  params->origin_y = static_cast<UINT>(rect.y1);
  params->size_x = static_cast<UINT>(rect.width());
  params->size_y = static_cast<UINT>(rect.height());
  params->row_bytes = static_cast<UINT>(row_bytes);
  params->luma_gain = static_cast<UINT>((adjust.brightness * 256) / 100);
  params->chroma_gain = static_cast<UINT>((adjust.contrast * 256) / 50);
  params->padding = 0;
  d3d_context_->Unmap(compute_params_.Get(), 0);

  ID3D11ShaderResourceView* srvs[] = {compute_source_srv_.Get()};
  ID3D11UnorderedAccessView* uavs[] = {compute_output_uav_.Get()};
  ID3D11Buffer* buffers[] = {compute_params_.Get()};

  d3d_context_->CSSetShader(compute_shader_.Get(), nullptr, 0);
  d3d_context_->CSSetShaderResources(0, 1, srvs);
  d3d_context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
  d3d_context_->CSSetConstantBuffers(0, 1, buffers);

  /* One thread per pixel pair, groups of 8x8. */
  const UINT groups_x = (static_cast<UINT>(rect.width() / 2) + 7) / 8;
  const UINT groups_y = (static_cast<UINT>(rect.height()) + 7) / 8;
  d3d_context_->Dispatch(groups_x, groups_y, 1);

  /* Unbind so the buffer can be read; leaving a UAV bound blocks the copy. */
  ID3D11ShaderResourceView* no_srv[] = {nullptr};
  ID3D11UnorderedAccessView* no_uav[] = {nullptr};
  d3d_context_->CSSetShaderResources(0, 1, no_srv);
  d3d_context_->CSSetUnorderedAccessViews(0, 1, no_uav, nullptr);

  /* Copy only the bytes actually produced, not the whole buffer. */
  D3D11_BOX box = {};
  box.left = 0;
  box.right = static_cast<UINT>(needed);
  box.top = 0;
  box.bottom = 1;
  box.front = 0;
  box.back = 1;
  d3d_context_->CopySubresourceRegion(compute_readback_.Get(), 0, 0, 0, 0,
                                      compute_output_.Get(), 0, &box);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(d3d_context_->Map(compute_readback_.Get(), 0, D3D11_MAP_READ, 0,
                               &mapped))) {
    return false;
  }
  memcpy(dst, mapped.pData, needed);
  d3d_context_->Unmap(compute_readback_.Get(), 0);
  return true;
}

void SwapChainProcessor::VerifyGpuAgainstCpu(ID3D11Texture2D* source,
                                             const Rect& rect,
                                             const PictureAdjust& adjust) {
  const size_t row_bytes = static_cast<size_t>(rect.width()) * 2;
  const size_t needed = row_bytes * rect.height();

  std::vector<uint8_t> gpu(needed);
  if (!ConvertOnGpu(source, rect, adjust, gpu.data(), gpu.size())) {
    Log("verify: GPU conversion failed for %dx%d at (%d,%d)", rect.width(),
        rect.height(), rect.x1, rect.y1);
    return;
  }

  /* The staging texture has to cover the whole surface, not just the rect,
   * because the CPU path indexes it with absolute coordinates. */
  if (!EnsureStaging(static_cast<UINT>(verify_fb_width_),
                     static_cast<UINT>(verify_fb_height_))) {
    return;
  }
  D3D11_BOX box = {};
  box.left = static_cast<UINT>(rect.x1);
  box.top = static_cast<UINT>(rect.y1);
  box.right = static_cast<UINT>(rect.x2);
  box.bottom = static_cast<UINT>(rect.y2);
  box.back = 1;
  d3d_context_->CopySubresourceRegion(staging_.Get(), 0, box.left, box.top, 0,
                                      source, 0, &box);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(d3d_context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0,
                               &mapped))) {
    return;
  }
  std::vector<uint8_t> cpu(needed + kFrameOverhead);
  const size_t produced = FrameRect(
      cpu.data(), cpu.size(), static_cast<const uint8_t*>(mapped.pData),
      mapped.RowPitch, verify_fb_width_, verify_fb_height_, rect, adjust);
  d3d_context_->Unmap(staging_.Get(), 0);
  if (produced == 0) {
    return;
  }

  /* Ignore uniform frames: a black desktop matches trivially and would make
   * a broken path look correct, which is exactly what happened before. */
  bool uniform = true;
  for (size_t i = 4; i < needed && uniform; i += 4) {
    if (memcmp(&cpu[kFrameHeaderSize + i], &cpu[kFrameHeaderSize], 4) != 0) {
      uniform = false;
    }
  }

  int worst = 0;
  size_t worst_at = 0;
  for (size_t i = 0; i < needed; ++i) {
    int diff = static_cast<int>(gpu[i]) -
               static_cast<int>(cpu[kFrameHeaderSize + i]);
    if (diff < 0) {
      diff = -diff;
    }
    if (diff > worst) {
      worst = diff;
      worst_at = i;
    }
  }

  if (!uniform && worst > 2) {
    Log("verify: %dx%d at (%d,%d) worst=%d at byte %zu", rect.width(),
        rect.height(), rect.x1, rect.y1, worst, worst_at);
  }

  if (!uniform) {
    ++verify_content_frames_;
    if (worst > 2) {
      Log("verify: MISMATCH, falling back to the CPU path");
      compute_ready_ = false;
      compute_failed_ = true;
      compute_verified_ = true;
      return;
    }
    /* Only trust the GPU path once several frames with real content, at
     * different offsets and sizes, have matched. */
    if (verify_content_frames_ >= 8) {
      Log("verify: GPU path matches the CPU reference over %u frames",
          verify_content_frames_);
      compute_verified_ = true;
    }
  }
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
  {
    std::lock_guard<std::mutex> damage_lock(damage_mutex_);
    if (have_new_damage) {
      pending_damage_[0] = MergeRects(pending_damage_[0], damage);
      pending_damage_[1] = MergeRects(pending_damage_[1], damage);
    }
    to_send =
        AlignDamageRect(pending_damage_[frame_index_], fb_width, fb_height);
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
    /* A queued frame was taken back before it reached the chip. Two things
     * follow, and missing either shows up as the picture alternating between
     * two versions of itself.
     *
     * The parity was advanced when that frame was submitted, on the
     * assumption it would be transmitted. It never was, so the chip's next
     * buffer is still the one that frame was meant to fill: step back.
     *
     * And the damage it carried was cleared from that buffer's pending set at
     * the same time, so restore it, otherwise the region stays stale there
     * forever. */
    std::lock_guard<std::mutex> damage_lock(damage_mutex_);
    frame_index_ = 1 - frame_index_;
    pending_damage_[frame_index_] =
        MergeRects(pending_damage_[frame_index_], superseded);
    to_send =
        AlignDamageRect(pending_damage_[frame_index_], fb_width, fb_height);
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

  bool converted = false;
  bool used_gpu = false;

  verify_fb_width_ = fb_width;
  verify_fb_height_ = fb_height;

  const size_t rect_pixels =
      static_cast<size_t>(to_send.width()) * to_send.height();
  const bool prefer_gpu = rect_pixels >= kGpuConversionMinPixels;

  /* IddCx keeps the acquired surface valid until the next acquire, so holding
   * it lets an idle refresh re-convert the current picture instead of
   * resending something stale. */
  last_source_ = source;
  last_width_ = fb_width;
  last_height_ = fb_height;

  if (EnsureCompute()) {
    /* Check the two paths agree before trusting the GPU one. */
    if (!compute_verified_) {
      VerifyGpuAgainstCpu(source.Get(), to_send, adjust);
    }
    if (compute_ready_ && prefer_gpu) {
      converted = ConvertOnGpu(source.Get(), to_send, adjust,
                               transfer->data() + kFrameHeaderSize,
                               transfer->size() - kFrameOverhead);
      used_gpu = converted;
      if (!converted) {
        Log("compute: conversion failed, falling back to the CPU path");
        compute_ready_ = false;
        compute_failed_ = true;
      }
    }
  }

  if (cursor_active_) {
    /* The cursor thread composites the pointer over a clean copy of the
     * desktop, so it needs one that never has the pointer baked in. Keeping
     * it here costs a copy of the damaged region per frame, which is far
     * cheaper than routing every mouse movement through the compositor. */
    if (EnsureStaging(source_desc.Width, source_desc.Height)) {
      D3D11_BOX box = {};
      box.left = static_cast<UINT>(to_send.x1);
      box.top = static_cast<UINT>(to_send.y1);
      box.front = 0;
      box.right = static_cast<UINT>(to_send.x2);
      box.bottom = static_cast<UINT>(to_send.y2);
      box.back = 1;
      d3d_context_->CopySubresourceRegion(staging_.Get(), 0, box.left, box.top,
                                          0, source.Get(), 0, &box);

      D3D11_MAPPED_SUBRESOURCE snapshot = {};
      if (SUCCEEDED(d3d_context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0,
                                      &snapshot))) {
        std::lock_guard<std::mutex> lock(cursor_mutex_);
        if (desktop_width_ != fb_width || desktop_height_ != fb_height) {
          desktop_width_ = fb_width;
          desktop_height_ = fb_height;
          desktop_stride_ = static_cast<size_t>(fb_width) * 4;
          desktop_copy_.assign(desktop_stride_ * fb_height, 0);
        }
        for (int y = to_send.y1; y < to_send.y2; ++y) {
          memcpy(desktop_copy_.data() + static_cast<size_t>(y) *
                                            desktop_stride_ +
                     static_cast<size_t>(to_send.x1) * 4,
                 static_cast<const uint8_t*>(snapshot.pData) +
                     static_cast<size_t>(y) * snapshot.RowPitch +
                     static_cast<size_t>(to_send.x1) * 4,
                 static_cast<size_t>(to_send.width()) * 4);
        }
        d3d_context_->Unmap(staging_.Get(), 0);
      }
    }
  }

  if (!converted) {
    /* CPU path: copy the damage out of the GPU, then convert while reading
     * the mapped staging texture. */
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
  } else {
    /* The GPU wrote the pixels straight into the transfer buffer; the header
     * and footer are still ours to add. */
    FrameUpdateHeader header;
    PutBe16(header.marker_be, kFrameMarker);
    PutBe24(header.position,
            ((static_cast<uint32_t>(to_send.x1) & 0xFFF) << 12) |
                (static_cast<uint32_t>(to_send.y1) & 0xFFF));
    PutBe24(header.dimensions,
            ((static_cast<uint32_t>(to_send.width()) & 0xFFF) << 12) |
                (static_cast<uint32_t>(to_send.height()) & 0xFFF));
    memcpy(transfer->data(), &header, sizeof(header));
    memcpy(transfer->data() + kFrameHeaderSize + pixel_bytes, kFrameFooter,
           kFrameFooterSize);
  }

  QueryPerformanceCounter(&t_converted);
  {
    static ULONGLONG last_phase_log = 0;
    const ULONGLONG phase_now = GetTickCount64();
    if (phase_now - last_phase_log >= 30000) {
      last_phase_log = phase_now;
      const double to_us = 1000000.0 / qpc_freq.QuadPart;
      Log("phases: %s convert=%.0fus  %dx%d (%zu bytes)",
          used_gpu ? "gpu" : "cpu",
          (t_converted.QuadPart - t_begin.QuadPart) * to_us, to_send.width(),
          to_send.height(), length);
    }
  }

  sender_->Submit(transfer, length, to_send);

  {
    /* This buffer is now up to date; the other still owes the same damage. */
    std::lock_guard<std::mutex> damage_lock(damage_mutex_);
    pending_damage_[frame_index_] = EmptyRect();
    frame_index_ = 1 - frame_index_;
  }
  last_send_ms_ = now_ms;

  /* Report how much of the screen each transfer actually covers: if damage
   * tracking is working this should be far smaller than the full frame. */
  static ULONGLONG last_rect_log = 0;
  if (now_ms - last_rect_log >= 2000) {
    Log("damage: %dx%d at (%d,%d) rects=%u moves=%u -> %zu bytes",
        to_send.width(), to_send.height(), to_send.x1, to_send.y1,
        meta.DirtyRectCount, meta.MoveRegionCount, length);
    last_rect_log = now_ms;
  }

  force_full_frame_ = false;
  return true;
}

bool SwapChainProcessor::SendRefresh(bool whole_screen) {
  if (!last_source_ || last_width_ <= 0 || last_height_ <= 0) {
    return false;
  }

  Rect full;
  full.x1 = 0;
  full.y1 = 0;
  full.x2 = last_width_;
  full.y2 = last_height_;

  /* Tempting idea that does not work: send only a band, since keeping the
   * panel awake just needs traffic and a full repaint costs eight vsync
   * periods. The chip alternates between two frame buffers on every
   * transfer, so a partial update lands in one of them and leaves the other
   * holding older content for that region. The two then alternate on screen
   * and the picture visibly flickers. Any partial update has to be tracked
   * per buffer, which is what pending_damage_ is for, so a refresh that is
   * meant to resynchronise everything sends the whole screen. */
  (void)whole_screen;

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
  if (!superseded.empty()) {
    /* The displaced frame's content is covered by a full repaint, but its
     * transfer never happened, so the chip's buffer parity did not advance. */
    std::lock_guard<std::mutex> damage_lock(damage_mutex_);
    frame_index_ = 1 - frame_index_;
  }

  PictureAdjust adjust;
  if (ddc_) {
    adjust.brightness = ddc_->brightness();
    adjust.contrast = ddc_->contrast();
  }

  const size_t row_bytes = static_cast<size_t>(full.width()) * 2;
  const size_t pixel_bytes = row_bytes * full.height();
  const size_t length = pixel_bytes + kFrameOverhead;
  if (transfer->size() < length) {
    sender_->Cancel(transfer);
    return false;
  }

  bool converted = false;
  if (compute_ready_) {
    converted = ConvertOnGpu(last_source_.Get(), full, adjust,
                             transfer->data() + kFrameHeaderSize,
                             transfer->size() - kFrameOverhead);
    if (converted) {
      FrameUpdateHeader header;
      PutBe16(header.marker_be, kFrameMarker);
      PutBe24(header.position, 0);
      PutBe24(header.dimensions,
              ((static_cast<uint32_t>(full.width()) & 0xFFF) << 12) |
                  (static_cast<uint32_t>(full.height()) & 0xFFF));
      memcpy(transfer->data(), &header, sizeof(header));
      memcpy(transfer->data() + kFrameHeaderSize + pixel_bytes, kFrameFooter,
             kFrameFooterSize);
    }
  }

  if (!converted) {
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
    const size_t produced =
        FrameRect(transfer->data(), transfer->size(),
                  static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                  last_width_, last_height_, full, adjust);
    d3d_context_->Unmap(staging_.Get(), 0);
    if (produced == 0) {
      sender_->Cancel(transfer);
      return false;
    }
  }

  sender_->Submit(transfer, length, full);

  std::lock_guard<std::mutex> damage_lock(damage_mutex_);
  /* Only the buffer that was just written is up to date. Clearing both, as an
   * earlier version did, left the other one stale and it would reappear on
   * the next flip. Advance the index so successive refreshes bring both
   * buffers current. */
  pending_damage_[frame_index_] = EmptyRect();
  frame_index_ = 1 - frame_index_;
  last_send_ms_ = GetTickCount64();
  return true;
}

bool SwapChainProcessor::SetupCursor() {
  if (!monitor_) {
    return false;
  }
  if (ReadPolicyDword(L"HardwareCursor", 1) == 0) {
    Log("cursor: hardware cursor disabled by policy");
    return false;
  }

  cursor_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!cursor_event_) {
    return false;
  }

  IDARG_IN_SETUP_HWCURSOR setup = {};
  setup.CursorInfo.Size = sizeof(setup.CursorInfo);
  setup.CursorInfo.AlphaCursorSupport = TRUE;
  /* No XOR support: emulation asks the OS to resolve those shapes for us,
   * which is what we want given there is no real cursor plane here. */
  setup.CursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_EMULATION;
  setup.CursorInfo.MaxX = 128;
  setup.CursorInfo.MaxY = 128;
  setup.hNewCursorDataAvailable = cursor_event_;

  const NTSTATUS status = IddCxMonitorSetupHardwareCursor(monitor_, &setup);
  if (!NT_SUCCESS(status)) {
    Log("cursor: IddCxMonitorSetupHardwareCursor -> 0x%08X, leaving the "
        "pointer to the compositor", status);
    CloseHandle(cursor_event_);
    cursor_event_ = nullptr;
    return false;
  }

  cursor_shape_.resize(128 * 128 * 4);
  cursor_active_ = true;
  Log("cursor: hardware cursor active");
  return true;
}

bool SwapChainProcessor::DrawCursor() {
  IDARG_IN_QUERY_HWCURSOR in = {};
  in.LastShapeId = cursor_shape_id_;
  in.ShapeBufferSizeInBytes = static_cast<UINT>(cursor_shape_.size());
  in.pShapeBuffer = cursor_shape_.data();

  IDARG_OUT_QUERY_HWCURSOR out = {};
  if (!NT_SUCCESS(IddCxMonitorQueryHardwareCursor(monitor_, &in, &out))) {
    return false;
  }

  if (out.IsCursorShapeUpdated) {
    cursor_shape_id_ = out.CursorShapeInfo.ShapeId;
    cursor_width_ = static_cast<int>(out.CursorShapeInfo.Width);
    cursor_height_ = static_cast<int>(out.CursorShapeInfo.Height);
    cursor_is_alpha_ =
        out.CursorShapeInfo.CursorType == IDDCX_CURSOR_SHAPE_TYPE_ALPHA;
  }

  std::lock_guard<std::mutex> lock(cursor_mutex_);
  if (desktop_copy_.empty() || cursor_width_ <= 0 || cursor_height_ <= 0) {
    return false;
  }

  /* Repaint where the pointer was, so it is erased, and where it now is. */
  Rect now = EmptyRect();
  if (out.IsCursorVisible) {
    now.x1 = out.X;
    now.y1 = out.Y;
    now.x2 = out.X + cursor_width_;
    now.y2 = out.Y + cursor_height_;
  }

  Rect region = MergeRects(cursor_previous_, now);
  cursor_previous_ = now;
  region = AlignDamageRect(region, desktop_width_, desktop_height_);
  if (region.empty()) {
    return false;
  }

  /* Start from the clean desktop, then lay the pointer over it. Working on a
   * scratch copy keeps desktop_copy_ free of the cursor, so the next move can
   * erase it simply by repainting from the copy. */
  const size_t region_stride = static_cast<size_t>(region.width()) * 4;
  cursor_scratch_.resize(region_stride * region.height());
  for (int y = 0; y < region.height(); ++y) {
    memcpy(cursor_scratch_.data() + static_cast<size_t>(y) * region_stride,
           desktop_copy_.data() +
               static_cast<size_t>(region.y1 + y) * desktop_stride_ +
               static_cast<size_t>(region.x1) * 4,
           region_stride);
  }

  if (out.IsCursorVisible) {
    const uint8_t* shape = cursor_shape_.data();
    for (int y = 0; y < cursor_height_; ++y) {
      const int target_y = out.Y + y - region.y1;
      if (target_y < 0 || target_y >= region.height()) {
        continue;
      }
      for (int x = 0; x < cursor_width_; ++x) {
        const int target_x = out.X + x - region.x1;
        if (target_x < 0 || target_x >= region.width()) {
          continue;
        }
        const uint8_t* src =
            shape + (static_cast<size_t>(y) * cursor_width_ + x) * 4;
        uint8_t* dst = cursor_scratch_.data() +
                       static_cast<size_t>(target_y) * region_stride +
                       static_cast<size_t>(target_x) * 4;
        const unsigned alpha = cursor_is_alpha_ ? src[3] : 255u;
        if (alpha == 0) {
          continue;
        }
        if (alpha == 255) {
          dst[0] = src[0];
          dst[1] = src[1];
          dst[2] = src[2];
          continue;
        }
        /* Straight source-over blend; the shape is premultiplied. */
        for (int c = 0; c < 3; ++c) {
          dst[c] = static_cast<uint8_t>(src[c] + (dst[c] * (255 - alpha)) / 255);
        }
      }
    }
  }

  Rect superseded = EmptyRect();
  std::vector<uint8_t>* transfer =
      sender_->AcquireBuffer(kBufferWaitMs, &superseded);
  if (!transfer) {
    /* Keep the region pending so the next move still erases the old pointer. */
    cursor_previous_ = MergeRects(cursor_previous_, region);
    return false;
  }
  if (!superseded.empty()) {
    std::lock_guard<std::mutex> damage_lock(damage_mutex_);
    frame_index_ = 1 - frame_index_;
    pending_damage_[frame_index_] =
        MergeRects(pending_damage_[frame_index_], superseded);
  }

  PictureAdjust adjust;
  if (ddc_) {
    adjust.brightness = ddc_->brightness();
    adjust.contrast = ddc_->contrast();
  }

  Rect local;
  local.x1 = 0;
  local.y1 = 0;
  local.x2 = region.width();
  local.y2 = region.height();

  const size_t length = FrameRect(transfer->data(), transfer->size(),
                                  cursor_scratch_.data(), region_stride,
                                  region.width(), region.height(), local,
                                  adjust);
  if (length == 0) {
    sender_->Cancel(transfer);
    return false;
  }

  /* FrameRect writes a header for a rect at the origin, so patch in the real
   * position on screen. */
  FrameUpdateHeader header;
  PutBe16(header.marker_be, kFrameMarker);
  PutBe24(header.position,
          ((static_cast<uint32_t>(region.x1) & 0xFFF) << 12) |
              (static_cast<uint32_t>(region.y1) & 0xFFF));
  PutBe24(header.dimensions,
          ((static_cast<uint32_t>(region.width()) & 0xFFF) << 12) |
              (static_cast<uint32_t>(region.height()) & 0xFFF));
  memcpy(transfer->data(), &header, sizeof(header));

  /* Sent twice, so both chip buffers receive it and neither is left owing
   * this region. The parity is unchanged by an even number of transfers, so
   * the frame path's tracking stays correct. */
  sender_->Submit(transfer, length, region, true);
  last_send_ms_ = GetTickCount64();
  return true;
}

void SwapChainProcessor::CursorLoop() {
  HANDLE waits[] = {cursor_event_, terminate_event_};
  for (;;) {
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait != WAIT_OBJECT_0) {
      break;
    }
    DrawCursor();
  }
}

void SwapChainProcessor::Run() {
  /* Ask for a slightly raised priority: the compositor considers the monitor
   * hung if we fall too far behind on the acquire loop. */
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

  Log("SwapChain: processing started, %u conversion thread(s)",
      ConversionThreads());

  if (SetupCursor()) {
    cursor_thread_ = std::thread(&SwapChainProcessor::CursorLoop, this);
  }

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

      /* Nothing new to draw. Keep the panel awake anyway. */
      if (last_send_ms_ != 0 &&
          idle_now - last_send_ms_ >= kIdleRefreshMs) {
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
