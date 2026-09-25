/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Data plane for the driver: the bulk OUT pixel pipe, reached through the WDF
 * USB target so PnP and power are handled by the framework.
 *
 * This deliberately does NOT implement the control plane. The control
 * transfers are class requests addressed to interface 0, which is a HID
 * collection owned by hidusb.sys, while this driver is bound to the vendor
 * specific display interface. WinUSB (and therefore the WDF USB target layered
 * on it) refuses to forward an interface-recipient request to an interface it
 * does not own, failing with ERROR_GEN_FAILURE.
 *
 * Because UMDF runs in user mode, the driver can simply open the sibling HID
 * interface itself. See device.cpp, which pairs this with a HidTransport
 * inside a CompositeTransport.
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

  /* Always fail: see the comment at the top of this file. */
  bool ControlSetReport(const uint8_t* data, size_t len) override;
  bool ControlGetReport(uint8_t* data, size_t len) override;

  bool BulkWrite(const uint8_t* data, size_t len) override;

 private:
  bool FindBulkOutPipe(std::string* error);

  WDFUSBDEVICE usb_device_ = nullptr;
  WDFUSBINTERFACE usb_interface_ = nullptr;
  WDFUSBPIPE bulk_pipe_ = nullptr;
  uint8_t bulk_endpoint_ = 0;
};

}  // namespace ms912x
