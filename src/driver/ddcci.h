/* SPDX-License-Identifier: GPL-2.0-only
 *
 * A small DDC/CI (MCCS) slave, so the monitor this driver presents behaves
 * like a real one to the Windows Monitor Configuration API and therefore to
 * brightness utilities such as Twinkle Tray.
 *
 * The dongle has no hardware brightness control and the MacroSilicon protocol
 * exposes no DDC pass-through to the downstream panel, so brightness and
 * contrast are implemented in software: they scale the picture on its way
 * through the RGB to UYVY conversion. That is a real, visible effect rather
 * than a value we merely remember.
 *
 * Wire format, for reference, since getting the checksums wrong makes the
 * monitor look simply unsupported:
 *
 *   host -> display, written to 8-bit address 0x6E (7-bit 0x37)
 *     0x51, 0x80|len, message..., checksum
 *     checksum = XOR over {0x6E, 0x51, 0x80|len, message...}
 *
 *   display -> host, read from 8-bit address 0x6F
 *     0x6E, 0x80|len, message..., checksum
 *     checksum = XOR over {0x50, 0x6E, 0x80|len, message...}
 *
 * 0x50 is the "virtual host address" used only when checksumming replies.
 */

#pragma once

#include <stdint.h>

#include <mutex>
#include <string>
#include <vector>

namespace ms912x {

constexpr uint32_t kDdcI2cAddress7Bit = 0x37;

/* MCCS VCP codes we implement. */
constexpr uint8_t kVcpBrightness = 0x10;
constexpr uint8_t kVcpContrast = 0x12;

class DdcCiSlave {
 public:
  DdcCiSlave();

  /* Feed a host-to-display transaction. Returns false if the packet was not
   * addressed to us or failed its checksum. */
  bool Transmit(uint32_t address, const uint8_t* data, size_t len);

  /* Fill a display-to-host read. Returns false if there is nothing pending. */
  bool Receive(uint32_t address, uint8_t* data, size_t len);

  /* 0..100. Applied to the picture by the frame pipeline. */
  int brightness() const;
  int contrast() const;

  /* Windows does not route DDC/CI to indirect displays: Microsoft's own DDI
   * documentation states the OS does not call the I2C callbacks. The slave
   * above is therefore correct but never exercised, so brightness is also
   * exposed through a registry value that any user-mode tool can set.
   *
   * Polls HKLM\SOFTWARE\winusbdisplay for Brightness and Contrast and
   * returns true if either changed. Cheap enough to call once a second. */
  bool RefreshFromRegistry();

 private:
  void QueueReply(const uint8_t* message, size_t len);
  void HandleMessage(const uint8_t* message, size_t len);

  mutable std::mutex mutex_;
  int brightness_ = 100;
  int contrast_ = 50;

  /* Capabilities strings are returned in 32-byte windows, so the reader keeps
   * asking with a rising offset until it gets a short reply. */
  std::string capabilities_;

  std::vector<uint8_t> pending_;
};

}  // namespace ms912x
