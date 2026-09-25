/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Device logic: register access, commands, modeset, EDID, custom timings.
 * Everything here is transport agnostic.
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ms912x_proto.h"
#include "transport.h"

namespace ms912x {

enum class ChipFamily {
  kUnknown,
  kMs9120,
  kMs912A,
  kMs912C,
  kMs9132,
};

const char* ChipFamilyName(ChipFamily family);

struct ChipInfo {
  ChipFamily family = ChipFamily::kUnknown;
  uint8_t signature[3] = {0, 0, 0};
  uint32_t custom_timing_base = 0;
};

struct CustomMode {
  Mode mode;
  uint16_t htotal, vtotal, hactive, vactive;
  uint16_t pixclk_10khz;
  uint16_t vfreq_centihz;
  uint16_t hoffset, voffset, hsyncwidth, vsyncwidth;
  bool progressive;
  bool positive_hsync;
  bool positive_vsync;
};

class Device {
 public:
  explicit Device(std::unique_ptr<Transport> transport);

  Transport* transport() { return transport_.get(); }

  /* Control plane. All of these serialize on ctrl_mutex_: the SET/GET pair is
   * a stateful sequence and interleaving two of them returns garbage. */
  bool ReadByte(uint16_t address, uint8_t* value);
  /* Reads up to kMaxRegisterReadCount consecutive bytes in one round trip. */
  bool ReadRegisters(uint16_t address, uint8_t* data, size_t len);
  bool WriteCommand(uint8_t cmd, const void* six_bytes);
  bool ReadFlash(uint32_t address, void* data, size_t len);

  bool PowerOn();
  bool PowerOff();

  /* The full section 4.3 sequence. Do not reorder; see docs/protocol-notes.md
   * for what each step actually is. Output stays disabled afterwards so the
   * panel does not show garbage before the first frame lands: call
   * EnableOutput() once a frame has been sent successfully. */
  bool SetResolution(const Mode& mode);
  bool EnableOutput(bool enable);

  /* Transfer mode the chip is put into during the modeset. Manual block is
   * what the vendor driver uses and what makes damage rectangles possible.
   * The bypass variants are undocumented beyond their names, but the name
   * suggests they write through rather than into a frame buffer that is then
   * swapped, which would avoid the chip's double buffering entirely. Exposed
   * so the alternatives can be tried against real content. */
  void SetTransferMode(uint8_t mode) { transfer_mode_ = mode; }

  /* Tells the chip which of its frame buffers to display. The vendor driver
   * tracks an alternating index and has a call for this, left commented out
   * in the source they publish. */
  bool TriggerFrame(uint8_t index, uint8_t delay);

  bool ReadVideoPort(VideoPort* port);
  bool ReadDisplayStatus(uint8_t* status);

  /* Reads `blocks` * 128 bytes of EDID. Returns false only on I/O failure;
   * a bad checksum is reported through `checksum_ok`. */
  bool ReadEdid(std::vector<uint8_t>* edid, int blocks, bool* checksum_ok);

  bool ReadChipInfo(ChipInfo* info);
  bool ReadCustomTimings(std::vector<CustomMode>* modes);

  /* Data plane. Sends one already-framed transfer, terminates it with the
   * zero length packet the chip expects, and enables output on first use. */
  bool SendFrame(const uint8_t* data, size_t len);

  /* Re-runs power on and the modeset. Used to recover a chip that has stopped
   * accepting transfers, which otherwise leaves the panel dark for good. */
  bool Reinitialise();

  /* Aborts a transfer in flight so teardown does not block on the bus. */
  void CancelTransfers() { transport_->CancelTransfers(); }

  const std::string& last_error() const { return last_error_; }

 private:
  bool Fail(const std::string& message);
  bool FailTransport(const char* what);

  std::unique_ptr<Transport> transport_;
  std::mutex ctrl_mutex_;
  std::string last_error_;
  bool output_enabled_ = false;
  Mode last_mode_ = {};
  bool have_last_mode_ = false;
  unsigned consecutive_failures_ = 0;
  uint8_t transfer_mode_ = kTransModeManualBlock;
};

bool EdidBlockChecksumOk(const uint8_t* block);

}  // namespace ms912x
