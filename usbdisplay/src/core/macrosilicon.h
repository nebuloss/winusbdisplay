/* SPDX-License-Identifier: GPL-2.0-only
 *
 * MacroSilicon MS912x / MS913x, implementing DisplayDevice.
 *
 * The only file in the driver that knows this particular chip exists, apart
 * from the wire constants it uses and the factory that picks it. Everything
 * peculiar to it lives here: the quantised cost model, the requirement to
 * send each region twice, the pixel format, the framing and the order of the
 * mode programming sequence.
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

#include "display_device.h"
#include "proto.h"
#include "link.h"

namespace usbdisplay {

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

class MacroSiliconDevice : public DisplayDevice {
 public:
  explicit MacroSiliconDevice(std::unique_ptr<Link> link);

  /* ---- DisplayDevice ---------------------------------------------------- */

  std::string Describe() const override;
  bool ReadConnector(VideoPort* port) override;
  bool ReadEdid(std::vector<uint8_t>* edid, int blocks,
                bool* checksum_ok) override;
  std::vector<Mode> SupportedModes(VideoPort port) override;

  int TransferCost(const Rect& region) const override;
  int TransmissionsPerRegion() const override;

  Rect AlignRegion(const Rect& region, int width, int height) const override;
  size_t BytesPerRow(int width) const override;
  void ConvertRow(uint8_t* dst, const uint8_t* source,
                  int width) const override;
  size_t MaxTransferBytes() const override;
  size_t Frame(uint8_t* destination, size_t capacity, const uint8_t* pixels,
               const Rect& region, const Rect& sub) const override;

  bool PowerOn() override;
  bool PowerOff() override;
  bool SetMode(const Mode& mode) override;
  bool SendTransfer(const uint8_t* data, size_t length) override;
  void Cancel() override { link_->Cancel(); }
  bool StillPresent() const override { return link_->StillPresent(); }
  bool DisplayingPicture() override;
  unsigned KeepaliveMs() const override;
  bool Revive() override;
  uint64_t generation() const override { return generation_; }
  const std::string& error() const override { return error_; }

  /* ---- beyond the interface --------------------------------------------- */


  Link* link() { return link_.get(); }

  /* Raw access. Reads fetch up to kMaxReadBytes consecutive bytes per round
   * trip, which is what makes reading an EDID take 32 exchanges rather than
   * 128. */
  bool Read(uint16_t address, uint8_t* data, size_t len);
  bool ReadByte(uint16_t address, uint8_t* value);
  bool WriteByte(uint16_t address, uint8_t value);
  bool Command(uint8_t sub_op, const void* six_bytes);
  bool ReadFlash(uint32_t address, void* data, size_t len);


  bool EnableOutput(bool enable);


  /* Re-runs power on and the last mode. The way back from a chip that has
   * stopped accepting transfers, which otherwise stays dark until the dongle
   * is physically unplugged. */
  bool Reset();

  /* How long to let the chip settle at the three points in mode programming
   * where the vendor driver waits. Fifty milliseconds each, from the vendor
   * source, and the waits are the point rather than an accident: see
   * SetModeLocked.
   *
   * Settable only so the tests can zero it. Fifteen of them program a mode,
   * and three real sleeps each would add two seconds to a suite whose whole
   * value is that it runs in under a second. */
  void set_settle_ms(unsigned ms) { settle_ms_ = ms; }

  bool ReadDisplayStatus(uint8_t* status);
  bool ReadChipId(ChipId* id);

  /* How much memory the dongle has, as the chip reports it.
   *
   * A board property rather than a chip one, so it is read rather than
   * inferred from the model: the same silicon ships with different amounts,
   * and it decides which modes will fit. */
  bool ReadSdramType(uint8_t* type);

  bool ReadCustomTimings(std::vector<CustomTiming>* timings);

  /* What the chip was last identified as, without going near the wire.
   *
   * ReadChipId records it, so anything that has probed the adapter once has
   * set this. Unknown until then, which the cost model treats as the slower
   * family; see bytes_per_period_. */
  ChipModel model() const { return model_.load(std::memory_order_relaxed); }

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

 private:
  bool Fail(const std::string& message);
  bool FailLink(const char* what);

  /* Variants that assume device_lock_ is already held, so the paths that
   * need to reprogram from inside a transfer do not deadlock on it. */
  bool SetModeLocked(const Mode& mode);
  bool EnableOutputLocked(bool enable);
  bool SetMuteLocked(bool muted);
  bool ResetLocked();

  /* Reads the chip id and the connector once, so later decisions do not
   * depend on some caller having asked first. */
  void EnsureIdentified();
  bool identified_ = false;

  void RememberModel(ChipModel model);

  /* Sleeps, when programming a mode calls for it. A plain wait rather than
   * anything the Link knows about, because the chip needs time rather than
   * traffic. */
  void Settle() const;
  unsigned settle_ms_ = 50;

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

  /* What the chip is, and the one number its cost behaviour turns on.
   *
   * Atomic because the frame thread reads them through TransferCost while
   * the control plane may still be probing, and relaxed because there is
   * nothing to order against: each is a single value that only ever goes
   * from the conservative default to the measured truth, once.
   *
   * The default is the USB 2 figure deliberately. An adapter that has not
   * been identified yet, or that answers with a signature nobody has seen,
   * is then planned for as the slower part, which splits updates that could
   * have been merged. That is a little slower and always correct. The
   * opposite mistake, assuming a whole frame is free on a chip where it
   * costs eight periods, turns ordinary typing into full-screen repaints. */
  std::atomic<ChipModel> model_{ChipModel::kUnknown};
  std::atomic<size_t> bytes_per_period_{kBytesPerPeriod912x};

  /* The connector, cached by ReadConnector so anything on the frame path
   * can consult it without a control exchange. */
  std::atomic<VideoPort> port_{VideoPort::kUnknown};
};

bool EdidBlockChecksumOk(const uint8_t* block);

}  // namespace usbdisplay
