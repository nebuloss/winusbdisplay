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

#include "composite_transport.h"
#include "ddcci.h"
#include "hid_transport.h"
#include "ms912x_convert.h"
#include "ms912x_device.h"
#include "winusb_transport.h"

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

  /* Returns the buffer to convert into, or nullptr if both are on the wire
   * and the caller should drop this frame.
   *
   * If a frame that had been queued but not yet started is taken back, the
   * damage it was carrying is written to `superseded` so the caller can fold
   * it back into what still needs sending. That damage never reached the
   * chip, so forgetting it would leave the region stale. */
  std::vector<uint8_t>* AcquireBuffer(DWORD wait_ms, Rect* superseded);

  /* Hands the buffer previously returned by AcquireBuffer to the worker,
   * along with the damage it covers. */
  /* `twice` transmits the same bytes back to back. The chip alternates
   * between two frame buffers on every transfer, so a standalone partial
   * update otherwise lands in one of them and the other keeps older content,
   * which alternates visibly on screen. Sending it twice puts it in both. */
  void Submit(std::vector<uint8_t>* buffer, size_t length, const Rect& damage,
              bool twice = false);

  /* Returns a buffer to the pool without sending it. */
  void Cancel(std::vector<uint8_t>* buffer);

  uint64_t frames_sent() const { return frames_sent_; }
  uint64_t frames_dropped() const { return frames_dropped_; }
  /* Frames replaced before they reached the wire. Not a loss: it means a
   * newer picture went out in place of a stale one. */
  uint64_t frames_superseded() const { return frames_superseded_; }

 private:
  void WorkerMain();

  struct Slot {
    std::vector<uint8_t> data;
    size_t length = 0;
    bool in_flight = false;
    bool queued = false;
    /* What this frame repaints, so it can be recovered if superseded. */
    Rect damage;
    unsigned long long queued_at = 0;
    bool twice = false;
  };

  Device* device_;
  Slot slots_[2];

  std::mutex mutex_;
  std::condition_variable free_cv_;
  std::condition_variable work_cv_;
  std::thread worker_;
  bool running_ = false;

  std::atomic<uint64_t> frames_sent_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint64_t> frames_superseded_{0};
};

/* Drives one IddCx swapchain on its own thread. */
class SwapChainProcessor {
 public:
  SwapChainProcessor(IDDCX_SWAPCHAIN swapchain, LUID render_adapter,
                     HANDLE new_frame_event, Device* device,
                     FrameSender* sender, const Mode& mode, DdcCiSlave* ddc,
                     IDDCX_MONITOR monitor);
  ~SwapChainProcessor();

  /* D3D is initialised on the calling thread, so a failure can be reported
   * before any worker exists. Doing it on the worker instead means the worker
   * may still be running when the OS reclaims the swapchain, and deleting it
   * from there is a use-after-free that takes the whole UMDF host down. */
  bool Start();

  void Terminate();

 private:
  void Run();
  bool EnsureD3D();
  bool EnsureStaging(UINT width, UINT height);

  /* GPU conversion. Most of the CPU cost of a frame is reading the acquired
   * surface back over the bus, so converting on the GPU and reading back UYVY
   * instead of RGBA both removes the conversion and halves the readback.
   * Returns false if the device cannot support it, in which case the CPU path
   * is used and stays used. */
  bool EnsureCompute();
  bool ConvertOnGpu(ID3D11Texture2D* source, const Rect& rect,
                    const PictureAdjust& adjust, uint8_t* dst,
                    size_t dst_capacity);
  /* Converts one frame both ways and logs the largest difference, so a silent
   * mismatch between the two paths cannot go unnoticed. Runs once. */
  void VerifyGpuAgainstCpu(ID3D11Texture2D* source, const Rect& rect,
                           const PictureAdjust& adjust);
  bool ProcessFrame(const IDARG_OUT_RELEASEANDACQUIREBUFFER& buffer);

  /* Re-sends the whole picture from the staging copy. Needed because the
   * panel blanks if it stops receiving data, and when the desktop is static
   * the OS stops presenting entirely, so no frame ever arrives to trigger a
   * normal update. */
  /* Cursor handling.
   *
   * By default Windows draws the pointer into the desktop image, so every
   * mouse movement has to travel the whole path: composite, present, acquire,
   * convert, transmit. Taking the hardware cursor path stops that. Windows
   * then leaves the pointer out of the image and reports its position on a
   * separate event, which a dedicated thread can act on immediately.
   *
   * The chip has no cursor overlay, so the pointer is still composited here,
   * but only over the small region it occupies, and without waiting for the
   * compositor to produce a new desktop frame. */
  bool SetupCursor();
  void CursorLoop();
  bool DrawCursor();

  /* Re-sends the picture from the last acquired surface. The panel blanks if
   * it stops receiving data, and when the desktop is static the OS stops
   * presenting, so nothing else would keep it alive. Always sends the whole
   * screen: see the comment in the implementation for why a partial refresh
   * flickers. */
  bool SendRefresh(bool whole_screen);

  IDDCX_SWAPCHAIN swapchain_;
  LUID render_adapter_;
  HANDLE new_frame_event_;
  Device* device_;
  FrameSender* sender_;
  Mode mode_;
  DdcCiSlave* ddc_ = nullptr;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
  UINT staging_width_ = 0;
  UINT staging_height_ = 0;

  Microsoft::WRL::ComPtr<ID3D11ComputeShader> compute_shader_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> compute_output_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> compute_output_uav_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> compute_readback_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> compute_params_;
  /* The acquired surface is a different texture most frames, so the view is
   * cached against the texture it was made for. */
  Microsoft::WRL::ComPtr<ID3D11Texture2D> compute_source_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> compute_source_srv_;
  bool compute_ready_ = false;
  bool compute_failed_ = false;
  bool compute_verified_ = false;
  unsigned verify_content_frames_ = 0;
  /* Last acquired surface, held so the idle refresh has something current to
   * convert from. */
  Microsoft::WRL::ComPtr<ID3D11Texture2D> last_source_;
  int last_width_ = 0;
  int last_height_ = 0;
  int verify_fb_width_ = 0;
  int verify_fb_height_ = 0;

  /* The chip holds two frame buffers and alternates between them, so damage
   * has to be tracked per buffer: new damage accumulates into both, and only
   * the buffer actually written is cleared. Tracking a single rectangle makes
   * each buffer miss half the updates, which shows up as ghosting and as the
   * picture flickering between two different images. */
  /* Damage owed by each chip buffer, and which buffer the next transfer will
   * land in. Touched by both the swapchain thread and the cursor thread, so
   * everything that reads or writes them holds damage_mutex_. */
  std::mutex damage_mutex_;
  Rect pending_damage_[2];
  int frame_index_ = 0;
  bool force_full_frame_ = true;

  /* The panel drops its signal if left idle, so refresh it periodically even
   * when the desktop has not changed. */
  unsigned long long last_send_ms_ = 0;
  unsigned long long last_settings_poll_ms_ = 0;

  std::thread thread_;
  HANDLE terminate_event_ = nullptr;

  IDDCX_MONITOR monitor_ = nullptr;
  std::thread cursor_thread_;
  HANDLE cursor_event_ = nullptr;
  std::vector<uint8_t> cursor_shape_;
  std::vector<uint8_t> cursor_scratch_;
  /* Guards the cached desktop copy the cursor thread composites over. */
  std::mutex cursor_mutex_;
  std::vector<uint8_t> desktop_copy_;
  int desktop_width_ = 0;
  int desktop_height_ = 0;
  size_t desktop_stride_ = 0;
  UINT cursor_shape_id_ = 0;
  int cursor_width_ = 0;
  int cursor_height_ = 0;
  bool cursor_is_alpha_ = false;
  /* Where the pointer was last drawn, so it can be erased. */
  Rect cursor_previous_;
  bool cursor_active_ = false;
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
  DdcCiSlave* ddc() { return &ddc_; }
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

  /* Makes the monitor answer DDC/CI, so the Windows Monitor Configuration
   * API (and tools built on it, such as Twinkle Tray) can drive brightness. */
  DdcCiSlave ddc_;

  VideoPort port_ = VideoPort::kUnknown;
  std::vector<uint8_t> edid_;
  bool edid_valid_ = false;
  std::vector<Mode> modes_;
  Mode active_mode_{1920, 1080, 60, 0x81};
};

/* Synthesized when the panel has no readable EDID, so the monitor still
 * appears and the user has something to diagnose with. */
void BuildFallbackEdid(std::vector<uint8_t>* edid, uint16_t width,
                       uint16_t height, uint16_t hz);

/* Copy helpers for the IddCx mode enumeration callbacks. Passing a null or
 * zero capacity buffer returns the number of entries the caller must allocate,
 * which is the two-pass convention IddCx uses. */
UINT FillMonitorModes(const std::vector<Mode>& modes, UINT capacity,
                      IDDCX_MONITOR_MODE* out,
                      IDDCX_MONITOR_MODE_ORIGIN origin);
UINT FillTargetModes(const std::vector<Mode>& modes, UINT capacity,
                     IDDCX_TARGET_MODE* out);

}  // namespace ms912x

/* This lives outside the namespace on purpose: the WDF context macros paste
 * the type name into new identifiers, so a qualified name does not compile. */
struct IndirectDeviceContextWrapper {
  ms912x::IndirectDevice* device;

  void Cleanup() {
    delete device;
    device = nullptr;
  }
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IndirectDeviceContextWrapper,
                                   GetIndirectDeviceContext)

/* IddCxMonitorCreate needs real object attributes with a context type. Passing
 * null attributes lets the create succeed but then IddCxMonitorArrival fails
 * with STATUS_DEVICE_NOT_READY, which is a thoroughly misleading symptom. */
struct MonitorContextWrapper {
  ms912x::IndirectDevice* device;

  void Cleanup() { device = nullptr; }
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContextWrapper, GetMonitorContext)
