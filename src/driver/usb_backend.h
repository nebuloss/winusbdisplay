/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Transport implementation for use inside the UMDF2 driver. Same wire
 * behaviour as the standalone WinUSB transport, but driven through the WDF
 * USB target so that PnP and power are handled by the framework.
 */

#pragma once

#include <windows.h>

/* wdfusb.h needs the USB type definitions (URB, USBD_STATUS, the standard
 * request codes) but does not pull them in itself. Including these in the
 * wrong order produces a wall of syntax errors inside wdfusb.h. */
#include <usb.h>
#include <usbspec.h>

#include <wdf.h>
#include <wdfusb.h>

#include <memory>
#include <string>
#include <vector>

#include "transport.h"

namespace ms912x {

class WdfUsbBackend : public Transport {
 public:
  /* Creates the USB target for `device` and locates the bulk OUT pipe.
   * Call from EvtDevicePrepareHardware. */
  static std::unique_ptr<WdfUsbBackend> Create(WDFDEVICE device,
                                               std::string* error);

  std::string Describe() const override;
  bool HasDataPlane() const override { return bulk_pipe_ != nullptr; }
  bool ControlSetReport(const uint8_t* data, size_t len) override;
  bool ControlGetReport(uint8_t* data, size_t len) override;
  bool BulkWrite(const uint8_t* data, size_t len) override;

 private:
  bool ControlTransfer(bool device_to_host, uint8_t request, uint8_t* data,
                       size_t len);

  WDFUSBDEVICE usb_device_ = nullptr;
  WDFUSBINTERFACE usb_interface_ = nullptr;
  WDFUSBPIPE bulk_pipe_ = nullptr;
  uint8_t bulk_endpoint_ = 0;
};

}  // namespace ms912x
