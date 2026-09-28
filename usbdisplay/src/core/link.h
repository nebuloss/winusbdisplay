/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Somewhere to send control exchanges and pixels.
 *
 * Deliberately free of any operating system: this header is the reason the
 * protocol layer and its tests build anywhere, which in turn is what lets
 * the whole test suite run on a Linux machine in a few seconds rather than
 * needing Windows and a driver kit. The implementations, which do need an
 * operating system, live in usb.h.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace usbdisplay {

class Link {
 public:
  virtual ~Link() = default;

  virtual std::string Describe() const = 0;

  /* False when pixels go nowhere real, so callers can say so rather than
   * reporting success into a void. */
  virtual bool HasPanel() const = 0;

  /* Exactly kControlSize bytes each way. Callers must hold the control lock;
   * Chip owns it, and nothing else should be calling these. */
  virtual bool ControlWrite(const uint8_t* payload) = 0;
  virtual bool ControlRead(uint8_t* payload) = 0;

  virtual bool BulkWrite(const uint8_t* data, size_t len) = 0;

  /* Terminates the transfer in flight without waiting for it.
   *
   * A full frame owns the bus for over a hundred milliseconds. PnP stop and
   * surprise removal will not wait that long: a driver that blocks gets
   * reported as hung and its device taken offline. Safe from another thread,
   * which is the whole point. */
  virtual void Cancel() {}

  /* Whether the underlying hardware is still attached. Answer from what the
   * system already knows: a request to a device that has just been unplugged
   * can block until it times out. */
  virtual bool StillPresent() const = 0;

  const std::string& error() const { return error_; }

 protected:
  void SetError(const std::string& message) { error_ = message; }

  /* Formats a platform error code into `error_`. Declared here because the
   * implementations below share it; defined alongside them, since only they
   * know what an error code means. */
  void SetPlatformError(const char* what, unsigned long code);

  std::string error_;
};

}  // namespace usbdisplay
