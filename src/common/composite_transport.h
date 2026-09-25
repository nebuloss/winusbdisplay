/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Control plane on one transport, data plane on another.
 *
 * This exists because of a Windows-specific constraint that does not apply to
 * the Linux drivers this protocol was documented from. The control transfers
 * are class requests with recipient=Interface and wIndex=0, so they target
 * interface 0 -- which is a HID collection owned by hidusb.sys. The bulk pixel
 * endpoint lives on a different interface (MI_03 on the USB 3 parts) owned by
 * WinUSB.
 *
 * On Linux the driver holds the whole usb_device and can address either
 * interface freely. On Windows each interface is owned by a different driver,
 * and WinUSB will not forward an interface-recipient control request to an
 * interface it does not own -- it fails with ERROR_GEN_FAILURE (0x1F).
 *
 * So: talk to interface 0 through the HID stack, and to the bulk endpoint
 * through WinUSB. Both handles are open at once and they do not conflict.
 */

#pragma once

#include <memory>
#include <string>

#include "transport.h"

namespace ms912x {

class CompositeTransport : public Transport {
 public:
  CompositeTransport(std::unique_ptr<Transport> control,
                     std::unique_ptr<Transport> data);

  std::string Describe() const override;
  bool HasDataPlane() const override;
  bool ControlSetReport(const uint8_t* data, size_t len) override;
  bool ControlGetReport(uint8_t* data, size_t len) override;
  bool BulkWrite(const uint8_t* data, size_t len) override;

 private:
  std::unique_ptr<Transport> control_;
  std::unique_ptr<Transport> data_;
};

}  // namespace ms912x
