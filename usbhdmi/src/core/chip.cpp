/* SPDX-License-Identifier: GPL-2.0-only */

#include "chip.h"

#include <cstring>

namespace usbhdmi {
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

Chip::Chip(std::unique_ptr<Link> link) : link_(std::move(link)) {}

bool Chip::Fail(const std::string& message) {
  error_ = message;
  return false;
}

bool Chip::FailLink(const char* what) {
  error_ = std::string(what) + ": " + link_->error();
  return false;
}

bool Chip::Read(uint16_t address, uint8_t* data, size_t len) {
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

bool Chip::ReadByte(uint16_t address, uint8_t* value) {
  return Read(address, value, 1);
}

bool Chip::Command(uint8_t sub_op, const void* six_bytes) {
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

bool Chip::ReadFlash(uint32_t address, void* data, size_t len) {
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

bool Chip::PowerOn() {
  const uint8_t payload[6] = {0x01, 0x02, 0, 0, 0, 0};
  return Command(kVideoPower, payload);
}

bool Chip::PowerOff() {
  const uint8_t payload[6] = {0, 0, 0, 0, 0, 0};
  return Command(kVideoPower, payload);
}

bool Chip::SetMode(const Mode& mode) {
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
  return true;
}

bool Chip::EnableOutput(bool enable) {
  uint8_t payload[6];
  memset(payload, 0, sizeof(payload));
  payload[0] = enable ? 1 : 0;
  if (!Command(kVideoEnable, payload)) {
    return false;
  }
  output_enabled_ = enable;
  return true;
}

bool Chip::Reset() {
  if (!have_last_mode_) {
    return Fail("no mode has been programmed yet, nothing to reset to");
  }
  output_enabled_ = false;
  if (!PowerOn()) {
    return false;
  }
  return SetMode(last_mode_);
}

bool Chip::ReadVideoPort(VideoPort* port) {
  uint8_t value = 0;
  if (!ReadByte(kRegVideoPort, &value)) {
    return false;
  }
  *port = value > static_cast<uint8_t>(VideoPort::kDigital)
              ? VideoPort::kUnknown
              : static_cast<VideoPort>(value);
  return true;
}

bool Chip::ReadDisplayStatus(uint8_t* status) {
  return ReadByte(kRegDisplayStatus, status);
}

bool Chip::ReadLiveImageIndex(int* index) {
  uint8_t value = 0;
  if (!ReadByte(kRegLiveImageIndex, &value)) {
    return false;
  }
  /* The vendor treats this as a flag rather than a counter. */
  *index = value ? 1 : 0;
  return true;
}

bool Chip::TriggerFrame(uint8_t index, uint8_t delay) {
  uint8_t payload[6];
  memset(payload, 0, sizeof(payload));
  payload[0] = index;
  payload[1] = delay;
  return Command(kVideoTriggerFrame, payload);
}

bool Chip::ReadEdid(std::vector<uint8_t>* edid, int blocks,
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

bool Chip::ReadChipId(ChipId* id) {
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

bool Chip::ReadCustomTimings(std::vector<CustomTiming>* timings) {
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

bool Chip::SendFrame(const uint8_t* data, size_t len) {
  if (!link_->HasPanel()) {
    return Fail("this link has no data plane, so pixels have nowhere to go");
  }

  if (!link_->BulkWrite(data, len)) {
    /* Several failures in a row means the chip has stopped accepting data
     * rather than that one transfer was unlucky, and reprogramming is the
     * only way back. Without this the panel stays dark until the dongle is
     * physically replugged. */
    if (++consecutive_failures_ >= 3) {
      consecutive_failures_ = 0;
      Reset();
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
    output_enabled_ = EnableOutput(true);
  }
  return true;
}

}  // namespace usbhdmi
