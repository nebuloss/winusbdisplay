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

bool MacroSiliconDevice::EnableOutputLocked(bool enable) {
  uint8_t payload[6];
  memset(payload, 0, sizeof(payload));
  payload[0] = enable ? 1 : 0;
  if (!Command(kVideoEnable, payload)) {
    return false;
  }
  output_enabled_ = enable;
  return true;
}

bool MacroSiliconDevice::Reset() {
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
    return true;
  }

  if (!Read(kRegChipId912x, signature, 3)) {
    return false;
  }
  memcpy(id->signature, signature, sizeof(signature));
  if (signature[1] == kSignatureFamily912x && signature[2] == kSignatureTail) {
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

  /* An unrecognised signature is not an I/O error, and id->signature holds
   * whatever the chip said so the caller can print it. */
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
 * This chip completes a bulk transfer on its own 60 Hz boundary, so what an
 * update costs is the number of those periods it spans. Measured on an
 * MS912C: 491 KB took 16.7 ms, 553 KB took 33.2 ms. Below the threshold size
 * is free, which is why the damage planner can merge nearby regions at no
 * cost, and above it the next byte costs an entire extra period, which is
 * why merging distant ones is ruinous. */
int MacroSiliconDevice::TransferCost(const Rect& region) const {
  if (region.empty()) {
    return 0;
  }
  const size_t bytes = BytesPerRow(region.width()) *
                           static_cast<size_t>(region.height()) +
                       kFrameOverhead;
  const size_t periods = (bytes + kBytesPerPeriod - 1) / kBytesPerPeriod;
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
  const auto push = [&modes](int w, int h, int hz) {
    const Mode* mode = FindMode(w, h, hz);
    if (mode) {
      modes.push_back(*mode);
    }
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
      /* 1080p30 first: a real mode of this chip and a far better match for
       * the bandwidth available than 1080p60, so it is what a user accepting
       * the default gets. */
      push(1920, 1080, 30);
      push(1920, 1080, 60);
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
