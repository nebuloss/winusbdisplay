/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Everything the chip can be asked to do, independent of how it is reached.
 *
 * One rule governs this whole class: **the control plane is a single
 * conversation**. A register read is a write followed by a read, and two of
 * those interleaved return each other's answers. Every method here takes
 * `control_lock_`, and nothing outside this class should ever touch a Link's
 * control methods. Symptoms of getting it wrong are not subtle in hindsight
 * but are baffling at the time: plausible looking EDIDs with bad checksums,
 * chip ids that change between runs.
 */

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "proto.h"
#include "usb.h"

namespace usbhdmi {

enum class ChipModel {
  kUnknown,
  kMs9120,
  kMs912A,
  kMs912C,
  kMs9132,
};

const char* ChipModelName(ChipModel model);

struct ChipId {
  ChipModel model = ChipModel::kUnknown;
  uint8_t signature[3] = {0, 0, 0};
  uint32_t flash_timing_base = 0;
};

/* A timing programmed into the dongle's flash by whoever built it, which
 * overrides the corresponding entry of the built-in table. */
struct CustomTiming {
  Mode mode;
  uint16_t htotal, vtotal, hactive, vactive;
  uint16_t pixel_clock_10khz;
  uint16_t vfreq_centihz;
  uint16_t hoffset, voffset, hsync_width, vsync_width;
  bool progressive;
  bool positive_hsync;
  bool positive_vsync;
};

class Chip {
 public:
  explicit Chip(std::unique_ptr<Link> link);

  Link* link() { return link_.get(); }

  /* Raw access. Reads fetch up to kMaxReadBytes consecutive bytes per round
   * trip, which is what makes reading an EDID take 32 exchanges rather than
   * 128. */
  bool Read(uint16_t address, uint8_t* data, size_t len);
  bool ReadByte(uint16_t address, uint8_t* value);
  bool Command(uint8_t sub_op, const void* six_bytes);
  bool ReadFlash(uint32_t address, void* data, size_t len);

  bool PowerOn();
  bool PowerOff();

  /* Programs a mode. The order of the steps is a captured sequence and is not
   * arbitrary; see docs/protocol-notes.md before rearranging it.
   *
   * Deliberately leaves the output disabled. The vendor driver does the same,
   * and the reason is visible the moment you do not: the chip's memory still
   * holds the previous session's picture, and enabling output before a frame
   * has landed puts that on the panel. SendFrame turns it on once there is
   * something real to show. */
  bool SetMode(const Mode& mode);
  bool EnableOutput(bool enable);

  /* Counts how many times the adapter has been programmed.
   *
   * Reprogramming clears the adapter's picture memory and disables the
   * output, so anything a caller believed was on screen no longer is. It can
   * happen without the caller asking: a run of failed transfers triggers a
   * reset from whichever thread was sending. Watching this value is how the
   * frame pipeline learns that its record of the panel's contents is void
   * and it must repaint rather than send differences against it. */
  uint64_t generation() const { return generation_; }

  /* Re-runs power on and the last mode. The way back from a chip that has
   * stopped accepting transfers, which otherwise stays dark until the dongle
   * is physically unplugged. */
  bool Reset();

  bool ReadVideoPort(VideoPort* port);
  bool ReadDisplayStatus(uint8_t* status);
  bool ReadEdid(std::vector<uint8_t>* edid, int blocks, bool* checksum_ok);
  bool ReadChipId(ChipId* id);
  bool ReadCustomTimings(std::vector<CustomTiming>* timings);

  /* Which of the chip's two internal images the vendor's register says is on
   * screen. A probe: the pipeline does not use it, because two attempts to
   * build partial updates on top of it drifted out of step with the chip and
   * wrote every update to the image that was not being displayed. Reading it
   * is how anyone revisiting that decision would start. */
  bool ReadLiveImageIndex(int* index);

  /* Asks the chip to show one of its two images after a delay. Present in the
   * vendor's Linux driver but commented out there, so treat a failure as
   * informational. The unexplained text shimmer may live here. */
  bool TriggerFrame(uint8_t index, uint8_t delay);

  /* Sends one already framed transfer and the zero length packet that ends
   * it. Enables the output on the first success. */
  bool SendFrame(const uint8_t* data, size_t len);

  void Cancel() { link_->Cancel(); }

  const std::string& error() const { return error_; }

 private:
  bool Fail(const std::string& message);
  bool FailLink(const char* what);

  /* Variants that assume device_lock_ is already held, so the paths that
   * need to reprogram from inside a transfer do not deadlock on it. */
  bool SetModeLocked(const Mode& mode);
  bool EnableOutputLocked(bool enable);
  bool ResetLocked();

  std::unique_ptr<Link> link_;

  /* Serialises one control exchange against another. A register read is a
   * write followed by a read, and two interleaved return each other's
   * answers. */
  std::mutex control_lock_;

  /* Serialises programming the adapter against sending it pixels, and
   * guards the state below.
   *
   * Without this, a mode change on the OS thread interleaves command by
   * command with a transfer already in flight describing the old geometry,
   * and the panel ends up garbled in a way only a replug clears. Held for
   * the duration of a transfer, which is why programming a new mode can
   * block for as long as one: that is the correct thing for it to do. */
  std::mutex device_lock_;

  /* Written under device_lock_ but read without it for error reporting, so
   * it is deliberately not a std::string: a torn read of one of those from
   * another thread is a crash rather than a garbled message. */
  std::string error_;

  bool output_enabled_ = false;
  Mode last_mode_ = {};
  bool have_last_mode_ = false;
  unsigned consecutive_failures_ = 0;
  std::atomic<uint64_t> generation_{0};
};

bool EdidBlockChecksumOk(const uint8_t* block);

}  // namespace usbhdmi
