/* SPDX-License-Identifier: GPL-2.0-only */

#include "macrosilicon.h"

#include <cstring>
#include <string>
#include <vector>

#include "../render/convert.h"

namespace usbdisplay {
namespace {

uint16_t ReadLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

const char* ChipModelName(ChipModel model) {
  switch (model) {
    case ChipModel::kMs9120:
      return "MS9120";
    case ChipModel::kMs912A:
      return "MS912A";
    case ChipModel::kMs912C:
      return "MS912C";
    case ChipModel::kMs9132:
      return "MS9132";
    default:
      return "unrecognised";
  }
}

bool EdidBlockChecksumOk(const uint8_t* block) {
  uint8_t sum = 0;
  for (int i = 0; i < 128; ++i) {
    sum = static_cast<uint8_t>(sum + block[i]);
  }
  return sum == 0;
}

MacroSiliconDevice::MacroSiliconDevice(std::unique_ptr<Link> link)
    : link_(std::move(link)) {}

bool MacroSiliconDevice::Fail(const std::string& message) {
  error_ = message;
  return false;
}

bool MacroSiliconDevice::FailLink(const char* what) {
  error_ = std::string(what) + ": " + link_->error();
  return false;
}

bool MacroSiliconDevice::Read(uint16_t address, uint8_t* data, size_t len) {
  if (len == 0 || len > kMaxReadBytes) {
    return Fail("register read length out of range");
  }
  std::lock_guard<std::mutex> lock(control_lock_);

  ReadRequest request;
  memset(&request, 0, sizeof(request));
  request.op = kOpReadXdata;
  PutBe16(request.address_be, address);

  if (!link_->ControlWrite(reinterpret_cast<uint8_t*>(&request))) {
    return FailLink("register read request");
  }
  if (!link_->ControlRead(reinterpret_cast<uint8_t*>(&request))) {
    return FailLink("register read response");
  }
  memcpy(data, request.data, len);
  return true;
}

bool MacroSiliconDevice::ReadByte(uint16_t address, uint8_t* value) {
  return Read(address, value, 1);
}

bool MacroSiliconDevice::WriteByte(uint16_t address, uint8_t value) {
  std::lock_guard<std::mutex> lock(control_lock_);

  WriteByteRequest request;
  memset(&request, 0, sizeof(request));
  request.op = kOpWriteXdataByte;
  PutBe16(request.address_be, address);
  request.value = value;

  if (!link_->ControlWrite(reinterpret_cast<uint8_t*>(&request))) {
    return FailLink("register write");
  }
  return true;
}

bool MacroSiliconDevice::Command(uint8_t sub_op, const void* six_bytes) {
  std::lock_guard<std::mutex> lock(control_lock_);

  VideoRequest request;
  memset(&request, 0, sizeof(request));
  request.op = kOpVideo;
  request.sub_op = sub_op;
  memcpy(request.payload, six_bytes, sizeof(request.payload));

  if (!link_->ControlWrite(reinterpret_cast<uint8_t*>(&request))) {
    return FailLink("command");
  }
  return true;
}

bool MacroSiliconDevice::ReadFlash(uint32_t address, void* data, size_t len) {
  if (address > 0xFFFFFF || len > 0x1000000u - address) {
    return Fail("flash address out of range");
  }
  std::lock_guard<std::mutex> lock(control_lock_);

  auto* out = static_cast<uint8_t*>(data);
  for (size_t offset = 0; offset < len; offset += kControlSize) {
    size_t chunk = len - offset;
    if (chunk > kControlSize) {
      chunk = kControlSize;
    }

    FlashReadRequest request;
    memset(&request, 0, sizeof(request));
    request.op = kOpReadFlash;
    PutBe24(request.address_be24, static_cast<uint32_t>(address + offset));

    if (!link_->ControlWrite(reinterpret_cast<uint8_t*>(&request))) {
      return FailLink("flash read request");
    }
    if (!link_->ControlRead(reinterpret_cast<uint8_t*>(&request))) {
      return FailLink("flash read response");
    }
    memcpy(out + offset, &request, chunk);
  }
  return true;
}

bool MacroSiliconDevice::PowerOn() {
  const uint8_t payload[6] = {0x01, 0x02, 0, 0, 0, 0};
  return Command(kVideoPower, payload);
}

bool MacroSiliconDevice::PowerOff() {
  const uint8_t payload[6] = {0, 0, 0, 0, 0, 0};
  return Command(kVideoPower, payload);
}

bool MacroSiliconDevice::SetMode(const Mode& mode) {
  std::lock_guard<std::mutex> lock(device_lock_);
  return SetModeLocked(mode);
}

bool MacroSiliconDevice::SetModeLocked(const Mode& mode) {
  uint8_t payload[6];
  uint8_t discard = 0;

  /* Nothing can show a picture without first setting a mode, so this is the
   * one place every path is guaranteed to pass through before the output
   * matters. */
  EnsureIdentified();

  output_enabled_ = false;

  /* Stop whatever is in flight before reprogramming anything. */
  memset(payload, 0, sizeof(payload));
  if (!Command(kVideoTransferEnable, payload)) {
    return false;
  }

  /* Three register reads the capture shows here. The reverse engineered
   * notes call them required handshakes; they are not. They are the SDRAM
   * type and two registers whose purpose is still unknown, and the vendor
   * driver uses the first for real. They are kept because preserving the
   * captured ordering is free and diverging from it is the sort of thing
   * that costs a day. */
  if (!ReadByte(kRegSdramType, &discard) ||
      !ReadByte(kRegModesetProbeA, &discard) ||
      !ReadByte(kRegModesetProbeB, &discard)) {
    return false;
  }

  /* Manual block mode is what makes partial updates possible at all. In any
   * other mode the chip expects a whole frame every time, which at 1080p
   * means eight vsync periods per update instead of one. */
  memset(payload, 0, sizeof(payload));
  payload[0] = kTransferModeManualBlock;
  if (!Command(kVideoTransferMode, payload)) {
    return false;
  }

  InputInfo input;
  PutBe16(input.width_be, static_cast<uint16_t>(mode.width));
  PutBe16(input.height_be, static_cast<uint16_t>(mode.height));
  input.pixel_format = kPixelFormatUyvy;
  input.byte_select = kByteSelectUyvy;
  if (!Command(kVideoInputInfo, &input)) {
    return false;
  }

  OutputInfo output;
  output.index = mode.index;
  output.colour = 0x01;
  PutBe16(output.width_be, static_cast<uint16_t>(mode.width));
  PutBe16(output.height_be, static_cast<uint16_t>(mode.height));
  if (!Command(kVideoOutputInfo, &output)) {
    return false;
  }

  memset(payload, 0, sizeof(payload));
  payload[0] = 1;
  if (!Command(kVideoTransferEnable, payload)) {
    return false;
  }

  last_mode_ = mode;
  have_last_mode_ = true;
  /* Announce that the adapter's picture memory is now undefined, so anyone
   * sending differences against a remembered image starts over. */
  ++generation_;
  return true;
}

bool MacroSiliconDevice::EnableOutput(bool enable) {
  std::lock_guard<std::mutex> lock(device_lock_);
  return EnableOutputLocked(enable);
}

/* Clears the transmitter's own mute, which is separate from video enable.
 *
 * The enable command starts the pipeline; this is what actually lets the
 * picture out. The vendor driver does both, in this order, and the register
 * is not the same one on the two families:
 *
 *   MS912x HDMI   0xF507 bit 1
 *   MS9132 HDMI   0xFB07 bit 1
 *
 * Clear to show, set to mute, on both. Writing the 912x address on a 9132
 * lands somewhere harmless and the panel stays dark while every transfer
 * reports success, which is the worst way this hardware fails.
 *
 * Only HDMI is handled. The other connectors each have their own register
 * again, none of them is present on hardware here, and writing a guessed
 * address to an unknown chip is how you brick a dongle. They keep the
 * behaviour that shipped, which is to rely on the enable command alone.
 *
 * Failure is reported but not fatal: on a part where this register means
 * something else, or nothing, refusing to bring the output up at all would
 * be worse than the mute never having been touched. */
/* Makes sure the chip has been identified and the connector read, once.
 *
 * Both answers steer later decisions, and both used to depend on some
 * caller happening to have asked first: the cost model on SupportedModes,
 * the unmute on ReadConnector. The console tool calls neither before
 * putting a picture up, so on that path the unmute quietly did nothing and
 * the panel stayed dark. That is the second time this shape of bug has
 * appeared here, so the ordering is no longer left to callers.
 *
 * Two control exchanges, once per device. Failures are left alone
 * deliberately: the defaults are the conservative ones, and a chip that
 * will not answer is not a reason to refuse to drive it. */
void MacroSiliconDevice::EnsureIdentified() {
  if (identified_) {
    return;
  }
  identified_ = true;

  ChipId id;
  ReadChipId(&id);

  VideoPort port = VideoPort::kUnknown;
  ReadConnector(&port);
}

/* Clears the transmitter's own mute, which is separate from video enable.
 *
 * The enable command starts the pipeline; this is what lets the picture
 * out. The vendor driver does both, in this order, and the register is not
 * the same one on the two families, so writing the 912x address to a 9132
 * would land somewhere else entirely.
 *
 * Only HDMI is handled here. Each of the other connectors has its own
 * register again, none of them exists on hardware to try it against, and
 * writing a guessed address to a chip is how a dongle stops working for
 * good. They keep the behaviour that shipped, which is the enable command
 * alone.
 *
 * Nothing is written when the bit already says what it should, so on an
 * adapter that comes up unmuted this costs one register read and no write
 * at all. Failure is not fatal: refusing to bring the output up because
 * the mute could not be read would turn a working picture into none. */
bool MacroSiliconDevice::SetMuteLocked(bool muted) {
  if (port_.load(std::memory_order_relaxed) != VideoPort::kHdmi) {
    return true;
  }

  const uint16_t address =
      model_.load(std::memory_order_relaxed) == ChipModel::kMs9132
          ? kRegHdmiMute913x
          : kRegHdmiMute912x;

  /* Read and rewrite rather than assigning: the rest of this register
   * belongs to the transmitter and is none of our business. */
  uint8_t value = 0;
  if (!ReadByte(address, &value)) {
    return false;
  }
  const uint8_t updated = muted ? static_cast<uint8_t>(value | kHdmiMuteBit)
                                : static_cast<uint8_t>(value & ~kHdmiMuteBit);
  if (updated == value) {
    return true;
  }
  return WriteByte(address, updated);
}

bool MacroSiliconDevice::EnableOutputLocked(bool enable) {
  uint8_t payload[6];
  memset(payload, 0, sizeof(payload));
  payload[0] = enable ? 1 : 0;
  if (!Command(kVideoEnable, payload)) {
    return false;
  }
  /* After the enable, which is the order the vendor uses. */
  SetMuteLocked(!enable);
  output_enabled_ = enable;
  return true;
}

bool MacroSiliconDevice::Reset() {
  std::lock_guard<std::mutex> lock(device_lock_);
  return ResetLocked();
}

/* Whether a picture is reaching the panel.
 *
 * Reads a register found by dumping every one of them on an MS9132 that
 * was displaying and on one that was not, and comparing. The byte two
 * past kRegDisplayLive is 0x44 in the first case and 0x01 in the second,
 * and it agrees with what a person sees. It means nothing on the USB 2
 * parts; see below.
 *
 * What the register is for is unknown. That does not matter: the question
 * it answers cannot be answered any other way, because every transfer
 * succeeds whether or not anything is displayed, so the frame path has no
 * idea. Being unable to ask is what made a dark panel take a day to
 * understand.
 *
 * **Tests for the dark signature, not for an exact showing value.** This
 * used to require 0x44 exactly, and an adapter reading 0x43 while showing
 * a normal desktop was reported as dark. Only the high nibble separates
 * the two states; the low bits vary while displaying. See proto.h.
 *
 * A failed read returns true, and so does any value that is not a dark
 * reading. Reporting a dark panel wrongly is the expensive mistake: it
 * used to drive reprogramming, which blinked displays that were working. */
bool MacroSiliconDevice::DisplayingPicture() {
  /* Only the USB 3 parts. The register was found on an MS9132 by
   * comparing a displaying adapter against a dark one, and on the
   * MS912C the same address reads zero whether or not a picture is on
   * the glass. Measured, not assumed: a frame that visibly lit the
   * panel left it reading 00 00 00 00.
   *
   * Saying "yes" for a part that cannot answer is the only safe
   * reading. The alternative, treating an unanswerable question as a
   * fault, would have the driver reprogram that adapter every few
   * seconds forever, which is a self inflicted flicker in place of a
   * problem it does not have. */
  if (model_.load(std::memory_order_relaxed) != ChipModel::kMs9132) {
    return true;
  }

  uint8_t live[4] = {0, 0, 0, 0};
  if (!Read(kRegDisplayLive, live, sizeof(live))) {
    return true;
  }
  return (live[2] & kDisplayLiveMask) != kDisplayLiveDark;
}

/* Reprograms the adapter, which is how a dark one comes back.
 *
 * Measured rather than hoped for: an adapter that had stopped displaying,
 * with the driver running and every transfer succeeding, was revived by a
 * single frame from the console tool. The tool does nothing clever; it
 * powers the chip on and sets the mode before every frame, which the
 * driver does only once at startup. That difference is the whole of it. */
/* How long this adapter will hold a picture with nothing arriving.
 *
 * Both families need the repaint. That is settled by a failed
 * experiment worth recording, because the evidence for removing it
 * looked strong and was not.
 *
 * With the driver stopped and nothing sent at all, the USB 3 part
 * appeared to hold a test pattern for two minutes, and the register
 * that reports whether a picture is live agreed throughout. Removing
 * the repaint on the strength of that turned the panel black within
 * seconds, while the register still said a picture was live.
 *
 * So that register describes what the chip is transmitting, not what
 * the panel is showing, and the two come apart exactly here. It is
 * still the right thing to use for spotting the dark state, because
 * it was validated against that; it is not evidence that silence is
 * safe. */
unsigned MacroSiliconDevice::KeepaliveMs() const { return 500; }
bool MacroSiliconDevice::Revive() {
  std::lock_guard<std::mutex> lock(device_lock_);
  return ResetLocked();
}

bool MacroSiliconDevice::ResetLocked() {
  if (!have_last_mode_) {
    return Fail("no mode has been programmed yet, nothing to reset to");
  }
  output_enabled_ = false;
  if (!PowerOn()) {
    return false;
  }
  return SetModeLocked(last_mode_);
}

bool MacroSiliconDevice::ReadConnector(VideoPort* port) {
  uint8_t value = 0;
  if (!ReadByte(kRegVideoPort, &value)) {
    return false;
  }
  *port = value > static_cast<uint8_t>(VideoPort::kDigital)
              ? VideoPort::kUnknown
              : static_cast<VideoPort>(value);
  /* Kept because unmuting the output needs it, and that happens on the
   * frame path where asking the chip again would mean a control exchange
   * in the middle of sending pixels. */
  port_.store(*port, std::memory_order_relaxed);
  return true;
}

bool MacroSiliconDevice::ReadDisplayStatus(uint8_t* status) {
  return ReadByte(kRegDisplayStatus, status);
}

bool MacroSiliconDevice::ReadLiveImageIndex(int* index) {
  uint8_t value = 0;
  if (!ReadByte(kRegLiveImageIndex, &value)) {
    return false;
  }
  /* The vendor treats this as a flag rather than a counter. */
  *index = value ? 1 : 0;
  return true;
}

bool MacroSiliconDevice::TriggerFrame(uint8_t index, uint8_t delay) {
  uint8_t payload[6];
  memset(payload, 0, sizeof(payload));
  payload[0] = index;
  payload[1] = delay;
  return Command(kVideoTriggerFrame, payload);
}

bool MacroSiliconDevice::ReadEdid(std::vector<uint8_t>* edid, int blocks,
                    bool* checksum_ok) {
  edid->assign(static_cast<size_t>(blocks) * 128, 0);
  for (size_t i = 0; i < edid->size(); i += kMaxReadBytes) {
    size_t chunk = edid->size() - i;
    if (chunk > kMaxReadBytes) {
      chunk = kMaxReadBytes;
    }
    if (!Read(static_cast<uint16_t>(kRegEdidBase + i), edid->data() + i,
              chunk)) {
      return false;
    }
  }

  bool ok = true;
  for (int block = 0; block < blocks; ++block) {
    if (!EdidBlockChecksumOk(edid->data() + block * 128)) {
      ok = false;
    }
  }
  if (checksum_ok) {
    *checksum_ok = ok;
  }
  return true;
}

bool MacroSiliconDevice::ReadChipId(ChipId* id) {
  id->model = ChipModel::kUnknown;
  id->flash_timing_base = 0;

  /* Signature layout, from the vendor driver: byte 1 marks the family and
   * byte 2 is always 0x0A. Within the 912x family byte 0 names the part.
   *
   * Both addresses are tried because the product id is not a reliable guide:
   * the unit these notes were written against advertises a USB 3 product id
   * and answers at the 912x address. Getting this wrong is not cosmetic, it
   * selects which flash address custom timings are read from. */
  uint8_t signature[3] = {0, 0, 0};
  if (!Read(kRegChipId913x, signature, 3)) {
    return false;
  }
  memcpy(id->signature, signature, sizeof(signature));
  if (signature[1] == kSignatureFamily913x && signature[2] == kSignatureTail) {
    id->model = ChipModel::kMs9132;
    id->flash_timing_base = kFlashTimingBase913x;
    /* Deliberately not an early return. Everything that identifies the chip
     * has to go past RememberModel below, or the cost model keeps the
     * conservative default and the faster part is driven as though it were
     * the slower one. That happened. */
  } else {
    if (!Read(kRegChipId912x, signature, 3)) {
      return false;
    }
    memcpy(id->signature, signature, sizeof(signature));
    if (signature[1] == kSignatureFamily912x &&
        signature[2] == kSignatureTail) {
      switch (signature[0]) {
        case kSignaturePart912C:
          id->model = ChipModel::kMs912C;
          break;
        case kSignaturePart912A:
          id->model = ChipModel::kMs912A;
          break;
        default:
          id->model = ChipModel::kMs9120;
          break;
      }
      id->flash_timing_base = kFlashTimingBase912x;
    }
  }

  /* An unrecognised signature is not an I/O error, and id->signature holds
   * whatever the chip said so the caller can print it. */
  RememberModel(id->model);
  return true;
}

/* Records what the chip turned out to be, and with it the one number the
 * frame loop needs: how much fits in a 60 Hz period.
 *
 * Kept here rather than at the call sites because this is the only place the
 * chip is ever identified, so there is nowhere else for the two to drift
 * apart. An unrecognised chip leaves the conservative default alone. */
void MacroSiliconDevice::RememberModel(ChipModel model) {
  model_.store(model, std::memory_order_relaxed);
  if (model == ChipModel::kMs9132) {
    bytes_per_period_.store(kBytesPerPeriod913x, std::memory_order_relaxed);
  } else if (model != ChipModel::kUnknown) {
    bytes_per_period_.store(kBytesPerPeriod912x, std::memory_order_relaxed);
  }
}

bool MacroSiliconDevice::ReadSdramType(uint8_t* type) {
  uint8_t value = 0;
  if (!ReadByte(kRegSdramType, &value)) {
    return false;
  }
  /* The vendor rejects anything past the last known code rather than
   * guessing, and so does this: a value nobody has seen is not a size to
   * reason about. */
  if (value > kSdramNone) {
    return Fail("the adapter reported an unknown memory size");
  }
  *type = value;
  return true;
}

bool MacroSiliconDevice::ReadCustomTimings(std::vector<CustomTiming>* timings) {
  timings->clear();

  ChipId id;
  if (!ReadChipId(&id)) {
    return false;
  }
  if (id.model == ChipModel::kUnknown) {
    return Fail("chip signature not recognised, so the flash layout is unknown");
  }

  uint8_t marker[7] = {0};
  if (!ReadFlash(id.flash_timing_base, marker, sizeof(marker))) {
    return false;
  }

  unsigned records = 0;
  if (memcmp(marker, "modify1", sizeof(marker)) == 0) {
    records = 1;
  } else if (memcmp(marker, "modify2", sizeof(marker)) == 0) {
    records = 2;
  } else {
    return true; /* nothing programmed, which is the common case */
  }

  for (unsigned i = 0; i < records; ++i) {
    uint8_t raw[sizeof(FlashTiming)] = {0};
    const uint32_t address =
        id.flash_timing_base + kFlashTimingOffset + i * kFlashTimingStride;
    if (!ReadFlash(address, raw, sizeof(raw))) {
      return false;
    }

    CustomTiming timing;
    memset(&timing, 0, sizeof(timing));
    timing.mode.index = raw[0];
    const uint8_t polarity = raw[1];
    timing.htotal = ReadLe16(raw + 2);
    timing.vtotal = ReadLe16(raw + 4);
    timing.hactive = ReadLe16(raw + 6);
    timing.vactive = ReadLe16(raw + 8);
    timing.pixel_clock_10khz = ReadLe16(raw + 10);
    timing.vfreq_centihz = ReadLe16(raw + 12);
    timing.hoffset = ReadLe16(raw + 14);
    timing.voffset = ReadLe16(raw + 16);
    timing.hsync_width = ReadLe16(raw + 18);
    timing.vsync_width = ReadLe16(raw + 20);
    timing.progressive = (polarity & kTimingProgressive) != 0;
    timing.positive_hsync = (polarity & kTimingPositiveHSync) != 0;
    timing.positive_vsync = (polarity & kTimingPositiveVSync) != 0;

    /* Unprogrammed flash reads as plausible looking noise, so sanity check
     * rather than trust. */
    if (timing.hactive == 0 || timing.hactive > kMaxFrameWidth ||
        timing.vactive == 0 || timing.vactive > kMaxFrameHeight ||
        timing.htotal <= timing.hactive || timing.vtotal <= timing.vactive ||
        timing.pixel_clock_10khz == 0 || timing.vfreq_centihz == 0) {
      continue;
    }

    timing.mode.width = timing.hactive;
    timing.mode.height = timing.vactive;
    timing.mode.hz = (timing.vfreq_centihz + 50) / 100;
    if (timing.mode.hz == 0) {
      continue;
    }
    timings->push_back(timing);
  }
  return true;
}

bool MacroSiliconDevice::SendTransfer(const uint8_t* data, size_t len) {
  if (!link_->HasPanel()) {
    return Fail("this link has no data plane, so pixels have nowhere to go");
  }

  /* Held for the whole transfer, so the adapter cannot be reprogrammed out
   * from under a transfer describing the previous geometry. */
  std::lock_guard<std::mutex> lock(device_lock_);

  if (!link_->BulkWrite(data, len)) {
    /* Several failures in a row means the chip has stopped accepting data
     * rather than that one transfer was unlucky, and reprogramming is the
     * only way back. Without this the panel stays dark until the dongle is
     * physically replugged. */
    if (++consecutive_failures_ >= 3) {
      consecutive_failures_ = 0;
      ResetLocked();
    }
    return FailLink("bulk write");
  }
  consecutive_failures_ = 0;

  /* A zero length bulk packet is how the chip is told the transfer is
   * complete. Omitting it is the classic cause of a dark panel where every
   * single transfer reported success, because the chip sits waiting for more
   * data that never arrives. The vendor driver sends one after every frame. */
  if (!link_->BulkWrite(nullptr, 0)) {
    return FailLink("end of frame packet");
  }

  if (!output_enabled_) {
    output_enabled_ = EnableOutputLocked(true);
  }
  return true;
}

/* ---- DisplayDevice: the parts that describe this chip's behaviour ------- */

std::string MacroSiliconDevice::Describe() const { return link_->Describe(); }

/* Cost is quantised, not proportional.
 *
 * Both families complete a bulk transfer on their own 60 Hz boundary, so
 * what an update costs is the number of those periods it spans rather than
 * anything to do with its size directly.
 *
 * How much fits in a period is what separates them, and it is read here
 * rather than compiled in because the two answers imply opposite planning.
 * Measured on an MS912C, 491 KB took 16.7 ms and 553 KB took 33.2 ms, so a
 * full 1080p frame costs eight periods and merging distant regions is
 * ruinous. Measured on an MS9132, 30 KB took 14.5 ms and a 4.1 MB full frame
 * took 17.1 ms, both one period, so every transfer costs the same and
 * merging is always worth it.
 *
 * The planner needs to know nothing about either; it compares costs. */
int MacroSiliconDevice::TransferCost(const Rect& region) const {
  if (region.empty()) {
    return 0;
  }
  const size_t bytes = BytesPerRow(region.width()) *
                           static_cast<size_t>(region.height()) +
                       kFrameOverhead;
  const size_t per_period = bytes_per_period_.load(std::memory_order_relaxed);
  const size_t periods = (bytes + per_period - 1) / per_period;
  return periods < 1 ? 1 : static_cast<int>(periods);
}

/* Twice, because the chip alternates between two internal copies of the
 * picture on every transfer. A region sent once lands in one of them and
 * leaves the other holding what was there before, and the two then alternate
 * on screen: with a moving pointer it looks like two pointers. */
int MacroSiliconDevice::TransmissionsPerRegion() const { return 2; }

Rect MacroSiliconDevice::AlignRegion(const Rect& region, int width,
                                     int height) const {
  return AlignDamageRect(region, width, height);
}

size_t MacroSiliconDevice::BytesPerRow(int width) const {
  return static_cast<size_t>(width) * 2; /* UYVY, 16 bits a pixel */
}

void MacroSiliconDevice::ConvertRow(uint8_t* dst, const uint8_t* source,
                                    int width) const {
  ::usbdisplay::ConvertRow(dst, source, width);
}

size_t MacroSiliconDevice::MaxTransferBytes() const {
  return kMaxTransferBytes;
}

size_t MacroSiliconDevice::Frame(uint8_t* destination, size_t capacity,
                                 const uint8_t* pixels, const Rect& region,
                                 const Rect& sub) const {
  return FrameSubRegion(destination, capacity, pixels, region, sub);
}

std::vector<Mode> MacroSiliconDevice::SupportedModes(VideoPort port) {
  std::vector<Mode> modes;

  /* Identify the chip first, explicitly.
   *
   * ReadCustomTimings below happens to do this on its way to finding the
   * flash layout, and relying on that worked right up until it did not:
   * the cost model is set here too, and a chip left unidentified is driven
   * as the slower family. One accidental early return was enough. This is
   * one round trip at attach and it makes the dependency visible. */
  ChipId id;
  ReadChipId(&id);

  /* What the board's memory can hold, which is not the same question as
   * what the chip can scan out.
   *
   * The chip keeps two frames, so a mode needs width * height * 2 twice
   * over. 1080p wants 7.9 MB and a 4 MB board cannot do it however willing
   * the silicon is. Offering it anyway gets a corrupt picture rather than a
   * refusal, which is a much worse way to find out.
   *
   * A board that will not say is given the benefit of the doubt, because
   * this read failing is not evidence the memory is small, and the previous
   * behaviour was to offer everything. */
  size_t memory = 0;
  uint8_t sdram = 0;
  if (ReadSdramType(&sdram)) {
    memory = SdramBytes(sdram);
  }

  const auto push = [&modes, memory](int w, int h, int hz) {
    if (memory != 0 && SdramBytesNeeded(w, h) > memory) {
      return;
    }
    const Mode* mode = FindMode(w, h, hz);
    if (mode) {
      modes.push_back(*mode);
    }
  };

  /* Whether a whole frame of this size is one transfer's worth.
   *
   * The chip finishes a transfer on an output vsync boundary, so one slot
   * is one frame period at whatever rate the mode runs. A full frame that
   * fits in a slot can therefore be redrawn every frame; one that spans
   * eight cannot. */
  const auto FullFrameCostsOneSlot = [this](int w, int h) {
    Rect frame;
    frame.x2 = w;
    frame.y2 = h;
    return TransferCost(frame) <= 1;
  };

  switch (port) {
    case VideoPort::kCvbs:
    case VideoPort::kSVideo:
    case VideoPort::kCvbsSVideo:
      push(720, 480, 60);
      push(720, 576, 50);
      break;
    case VideoPort::kYPbPr:
      push(1920, 1080, 60);
      push(1280, 720, 60);
      push(720, 480, 60);
      break;
    default:
      /* Which of the two 1080p rates to offer first, which is to say what
       * a user who never opens the display settings ends up running.
       *
       * This used to be 30 unconditionally, on the grounds that 60 asks
       * for more than the link can carry. True of the USB 2 parts, where a
       * full frame costs eight slots, and wrong on the USB 3 parts, where
       * it costs one and 60 is comfortable. Leaving it at 30 there halves
       * the frame rate of an adapter that can manage the full rate.
       *
       * Asked of the device rather than decided here, so this stays a
       * question about measured cost rather than about which chip is
       * fitted. */
      if (FullFrameCostsOneSlot(1920, 1080)) {
        push(1920, 1080, 60);
        push(1920, 1080, 30);
      } else {
        push(1920, 1080, 30);
        push(1920, 1080, 60);
      }
      push(1600, 1200, 60);
      push(1680, 1050, 60);
      push(1440, 900, 60);
      push(1366, 768, 60);
      push(1280, 1024, 60);
      push(1280, 800, 60);
      push(1280, 720, 60);
      push(1024, 768, 60);
      push(800, 600, 60);
      push(640, 480, 60);
      break;
  }

  /* Timings programmed into the adapter's flash by whoever built it take
   * priority over the built-in table. */
  std::vector<CustomTiming> custom;
  if (ReadCustomTimings(&custom)) {
    for (auto it = custom.rbegin(); it != custom.rend(); ++it) {
      modes.insert(modes.begin(), it->mode);
    }
  }

  if (modes.empty()) {
    modes.push_back(*FindMode(1024, 768, 60));
  }
  return modes;
}

}  // namespace usbdisplay
