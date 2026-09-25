/* SPDX-License-Identifier: GPL-2.0-only
 *
 * IddCx frontend: adapter, monitor, swapchain processing and the frame
 * pipeline that feeds the USB backend.
 */

#pragma once

#include <windows.h>

#include <wdf.h>

#include <iddcx.h>

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ms912x_convert.h"
#include "ms912x_device.h"
#include "usb_backend.h"

namespace ms912x {

/* Owns the USB transfer buffers and the worker thread that pushes them.
 *
 * Section 5: never block the IddCx processing thread on USB, and drop frames
 * rather than queueing them. There are two buffers; the producer takes the one
 * that is not in flight, and gives up after a short wait instead of building
 * unbounded latency. */
class FrameSender {
 public:
  explicit FrameSender(Device* device);
  ~FrameSender();

  void Start();
  void Stop();

  /* Returns the buffer to convert into, or nullptr if both are busy and the
   * caller should drop this frame. */
  std::vector<uint8_t>* AcquireBuffer(DWORD wait_ms);

  /* Hands the buffer previously returned by AcquireBuffer to the worker. */
  void Submit(std::vector<uint8_t>* buffer, size_t length);

  /* Returns a buffer to the pool without sending it. */
  void Cancel(std::vector<uint8_t>* buffer);

  uint64_t frames_sent() const { return frames_sent_; }
  uint64_t frames_dropped() const { return frames_dropped_; }

 private:
  void WorkerMain();

  struct Slot {
    std::vector<uint8_t> data;
    size_t length = 0;
    bool in_flight = false;
    bool queued = false;
  };

  Device* device_;
  Slot slots_[2];
  size_t next_slot_ = 0;

  std::mutex mutex_;
  std::condition_variable free_cv_;
  std::condition_variable work_cv_;
  std::thread worker_;
  bool running_ = false;

  std::atomic<uint64_t> frames_sent_{0};
  std::atomic<uint64_t> frames_dropped_{0};
};

/* Drives one IddCx swapchain on its own thread. */
class SwapChainProcessor {
 public:
  SwapChainProcessor(IDDCX_SWAPCHAIN swapchain, LUID render_adapter,
                     HANDLE new_frame_event, Device* device,
                     FrameSender* sender, const Mode& mode);
  ~SwapChainProcessor();

  void Terminate();

 private:
  void Run();
  bool EnsureD3D();
  bool EnsureStaging(UINT width, UINT height);
  bool ProcessFrame(const IDARG_OUT_RELEASEANDACQUIREBUFFER& buffer);

  IDDCX_SWAPCHAIN swapchain_;
  LUID render_adapter_;
  HANDLE new_frame_event_;
  Device* device_;
  FrameSender* sender_;
  Mode mode_;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
  UINT staging_width_ = 0;
  UINT staging_height_ = 0;

  /* The panel latches the previous update too, so each transfer must cover the
   * union of this frame's damage and the last frame's damage. */
  Rect previous_damage_;
  bool force_full_frame_ = true;

  std::thread thread_;
  HANDLE terminate_event_ = nullptr;
};

/* Per-WDFDEVICE state. */
class IndirectDevice {
 public:
  explicit IndirectDevice(WDFDEVICE wdf_device);
  ~IndirectDevice();

  NTSTATUS PrepareHardware();
  void ReleaseHardware();

  void OnAdapterInitFinished(IDDCX_ADAPTER adapter);
  NTSTATUS CommitModes(const IDARG_IN_COMMITMODES* args);
  NTSTATUS AssignSwapChain(const IDARG_IN_SETSWAPCHAIN* args);
  void UnassignSwapChain();

  /* Modes we are willing to expose, filtered by the connector type. */
  const std::vector<Mode>& modes() const { return modes_; }
  const std::vector<uint8_t>& edid() const { return edid_; }

  Device* device() { return ms_device_.get(); }
  IDDCX_ADAPTER adapter() const { return adapter_; }
  IDDCX_MONITOR monitor() const { return monitor_; }

 private:
  void BuildModeList();
  void CreateMonitor();

  WDFDEVICE wdf_device_;
  IDDCX_ADAPTER adapter_ = nullptr;
  IDDCX_MONITOR monitor_ = nullptr;

  std::unique_ptr<Device> ms_device_;
  std::unique_ptr<FrameSender> sender_;
  std::unique_ptr<SwapChainProcessor> processor_;

  VideoPort port_ = VideoPort::kUnknown;
  std::vector<uint8_t> edid_;
  bool edid_valid_ = false;
  std::vector<Mode> modes_;
  Mode active_mode_{1920, 1080, 60, 0x81};
};

struct IndirectDeviceContextWrapper {
  IndirectDevice* device;

  void Cleanup() {
    delete device;
    device = nullptr;
  }
};

/* Synthesized when the panel has no readable EDID, so the monitor still
 * appears and the user has something to diagnose with. */
void BuildFallbackEdid(std::vector<uint8_t>* edid, uint16_t width,
                       uint16_t height, uint16_t hz);

}  // namespace ms912x

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ms912x::IndirectDeviceContextWrapper,
                                   GetIndirectDeviceContext)
