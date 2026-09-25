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

  const std::string& last_error() const { return last_error_; }

 private:
  bool Fail(const std::string& message);
  bool FailTransport(const char* what);

  std::unique_ptr<Transport> transport_;
  std::mutex ctrl_mutex_;
  std::string last_error_;
  bool output_enabled_ = false;
};

bool EdidBlockChecksumOk(const uint8_t* block);

}  // namespace ms912x
