/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The per-device state: the adapter, the monitor and its mode list.
 *
 * ## Why this driver is not attached to the USB device
 *
 * It would be natural to bind this to the adapter's display interface and
 * talk to it through the framework's USB support. That does not start. The
 * indirect display stack requires an upper filter that turns out to be
 * incompatible with the framework's WinUSB dispatcher, and neither of
 * Windows' own indirect display drivers declares one either.
 *
 * So this is a root enumerated software device that reaches the hardware
 * through ordinary user mode handles, which works because these drivers run
 * in a user mode host process. The consequence is that two packages have to
 * be installed: this one, and the one that binds WinUSB to the display
 * interface.
 *
 * It also means **nothing tells this driver when the adapter is plugged in
 * or pulled out**. A root enumerated node is not a child of the USB device,
 * so PnP has no reason to start or stop it when the hardware comes and goes.
 * Without something to bridge that gap, unplugging the adapter leaves a
 * phantom monitor in Windows holding the user's windows and showing a frozen
 * desktop, and plugging it back in does nothing at all. That is what the
 * watcher thread below is for: it looks for the hardware, brings the monitor
 * up when it appears, and takes it away when it goes.
 */

#pragma once

#include <windows.h>

#include <wdf.h>

#include <iddcx.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/display_device.h"
#include "pipeline.h"
#include "sender.h"

namespace usbdisplay {

class IndirectDevice {
 public:
  explicit IndirectDevice(WDFDEVICE wdf_device);
  ~IndirectDevice();

  NTSTATUS PrepareHardware();
  void ReleaseHardware();

  void OnAdapterReady(IDDCX_ADAPTER adapter);
  NTSTATUS CommitModes(const IDARG_IN_COMMITMODES* args);
  NTSTATUS AssignSwapChain(const IDARG_IN_SETSWAPCHAIN* args);
  void UnassignSwapChain();

  /* The mode list is read by callbacks on the OS thread while the watcher
   * thread may be rebuilding it after a hot-plug, so it is copied out under
   * the lock rather than handed out by reference. */
  std::vector<Mode> modes();

  /* Applies a gamma table from the operating system. This is how ordinary
   * Windows brightness tools reach this monitor: the paths they normally
   * use need either a graphics card's I2C master or a kernel driver, and an
   * indirect display has neither, but the display stack will hand the gamma
   * table to a driver that declares it applies one. */
  NTSTATUS SetGammaRamp(const IDARG_IN_SET_GAMMARAMP* args);

 private:
  void CreateMonitor();
  void AnnounceMonitor();
  void RemoveMonitor();

  /* Opens the adapter and brings the monitor up. Returns false when the
   * hardware is not there, which is an ordinary state and not an error. */
  bool TryAttach();
  void Detach();

  void WatcherLoop();

  /* Kept although nothing reads it: it is the handle this object belongs
   * to, and the first thing anyone adding a framework call here will need.
   * Named in the constructor so the ownership is plain. */
  WDFDEVICE wdf_device_;
  IDDCX_ADAPTER adapter_ = nullptr;
  IDDCX_MONITOR monitor_ = nullptr;

  std::unique_ptr<DisplayDevice> device_;
  std::unique_ptr<FrameSender> sender_;
  std::unique_ptr<Pipeline> pipeline_;

  /* Guards everything above and below that the watcher thread and the OS
   * callbacks both touch. */
  std::mutex lock_;

  std::thread watcher_;
  HANDLE watcher_stop_ = nullptr;
  bool adapter_ready_ = false;
  bool pending_arrival_ = false;
  std::string last_attach_error_;

  /* The gamma table in force, kept here so it survives a swapchain being
   * torn down and rebuilt, which happens on every mode change. */
  GammaRamp gamma_;

  VideoPort port_ = VideoPort::kUnknown;
  std::vector<uint8_t> edid_;
  bool edid_valid_ = false;
  std::vector<Mode> modes_;
  Mode active_mode_ = {1920, 1080, 60, 0x81};
};

/* Made up when the panel has no readable EDID, so the monitor still appears
 * and the user has something to work with rather than nothing at all. */
void BuildFallbackEdid(std::vector<uint8_t>* edid, int width, int height,
                       int hz);

/* The mode enumeration callbacks use a two pass convention: called once with
 * no buffer to learn the count, then again to fill it. */
UINT FillMonitorModes(const std::vector<Mode>& modes, UINT capacity,
                      IDDCX_MONITOR_MODE* out,
                      IDDCX_MONITOR_MODE_ORIGIN origin);
UINT FillTargetModes(const std::vector<Mode>& modes, UINT capacity,
                     IDDCX_TARGET_MODE* out);

}  // namespace usbdisplay

/* Outside the namespace deliberately: the framework's context macros paste
 * the type name into new identifiers, which a qualified name breaks. */
struct DeviceContextWrapper {
  usbdisplay::IndirectDevice* device;

  void Cleanup() {
    delete device;
    device = nullptr;
  }
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContextWrapper, GetDeviceContext)

/* Creating a monitor with null object attributes succeeds, and then
 * announcing its arrival fails with a status that means nothing at all
 * ("device not ready"). Real attributes with a context type are required. */
struct MonitorContextWrapper {
  usbdisplay::IndirectDevice* device;

  void Cleanup() { device = nullptr; }
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContextWrapper, GetMonitorContext)
