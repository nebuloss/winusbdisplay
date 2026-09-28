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
 */

#pragma once

#include <windows.h>

#include <wdf.h>

#include <iddcx.h>

#include <memory>
#include <vector>

#include "../core/chip.h"
#include "pipeline.h"
#include "sender.h"

namespace usbhdmi {

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

  const std::vector<Mode>& modes() const { return modes_; }
  const std::vector<uint8_t>& edid() const { return edid_; }

 private:
  void BuildModeList();
  void CreateMonitor();

  WDFDEVICE wdf_device_;
  IDDCX_ADAPTER adapter_ = nullptr;
  IDDCX_MONITOR monitor_ = nullptr;

  std::unique_ptr<Chip> chip_;
  std::unique_ptr<FrameSender> sender_;
  std::unique_ptr<Pipeline> pipeline_;

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

}  // namespace usbhdmi

/* Outside the namespace deliberately: the framework's context macros paste
 * the type name into new identifiers, which a qualified name breaks. */
struct DeviceContextWrapper {
  usbhdmi::IndirectDevice* device;

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
  usbhdmi::IndirectDevice* device;

  void Cleanup() { device = nullptr; }
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContextWrapper, GetMonitorContext)
