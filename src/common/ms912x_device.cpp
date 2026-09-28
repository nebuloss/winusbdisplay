/* SPDX-License-Identifier: GPL-2.0-only */

#include "ms912x_device.h"

#include <cstdio>
#include <cstring>

namespace ms912x {
namespace {

uint16_t ReadLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

Device::Device(std::unique_ptr<Transport> transport)
    : transport_(std::move(transport)) {}

bool Device::Fail(const std::string& message) {
  last_error_ = message;
  return false;
}

bool Device::FailTransport(const char* what) {
  last_error_ = std::string(what) + ": " + transport_->last_error();
  return false;
}

bool Device::ReadRegisters(uint16_t address, uint8_t* data, size_t len) {
  if (len == 0 || len > kMaxRegisterReadCount) {
    return Fail("register read count out of range");
  }
  std::lock_guard<std::mutex> lock(ctrl_mutex_);

  RegisterRequest request;
  memset(&request, 0, sizeof(request));
  request.type = kReqTypeReadByte;
  PutBe16(request.addr_be, address);

  if (!transport_->ControlSetReport(reinterpret_cast<uint8_t*>(&request),
                                    sizeof(request))) {
    return FailTransport("register read request");
  }
  if (!transport_->ControlGetReport(reinterpret_cast<uint8_t*>(&request),
                                    sizeof(request))) {
    return FailTransport("register read response");
  }
  memcpy(data, request.data, len);
  return true;
}

bool Device::ReadByte(uint16_t address, uint8_t* value) {
  return ReadRegisters(address, value, 1);
}

bool Device::WriteCommand(uint8_t cmd, const void* six_bytes) {
  std::lock_guard<std::mutex> lock(ctrl_mutex_);

  WriteRequest request;
  memset(&request, 0, sizeof(request));
  request.type = kReqTypeWrite6Bytes;
  request.cmd = cmd;
  memcpy(request.data, six_bytes, sizeof(request.data));

  if (!transport_->ControlSetReport(reinterpret_cast<uint8_t*>(&request),
                                    sizeof(request))) {
    return FailTransport("command write");
  }
  return true;
}

bool Device::ReadFlash(uint32_t address, void* data, size_t len) {
  if (address > 0xFFFFFF || len > 0x1000000u - address) {
    return Fail("flash address out of range");
  }

  std::lock_guard<std::mutex> lock(ctrl_mutex_);
  auto* dst = static_cast<uint8_t*>(data);

  for (size_t offset = 0; offset < len; offset += kControlPayloadSize) {
    size_t chunk = len - offset;
    if (chunk > kControlPayloadSize) {
      chunk = kControlPayloadSize;
    }

    FlashReadRequest request;
    memset(&request, 0, sizeof(request));
    request.type = kReqTypeReadFlash;
    PutBe24(request.addr, static_cast<uint32_t>(address + offset));

    if (!transport_->ControlSetReport(reinterpret_cast<uint8_t*>(&request),
                                      sizeof(request))) {
      return FailTransport("flash read request");
    }
    if (!transport_->ControlGetReport(reinterpret_cast<uint8_t*>(&request),
                                      sizeof(request))) {
      return FailTransport("flash read response");
    }
    memcpy(dst + offset, &request, chunk);
  }
  return true;
}

bool Device::PowerOn() {
  uint8_t data[6] = {0x01, 0x02, 0, 0, 0, 0};
  return WriteCommand(kCmdPower, data);
}

bool Device::PowerOff() {
  uint8_t data[6] = {0, 0, 0, 0, 0, 0};
  return WriteCommand(kCmdPower, data);
}

bool Device::SetResolution(const Mode& mode) {
  uint8_t data[6];
  uint8_t discard;

  output_enabled_ = false;

  /* Stop any transfer in progress before reprogramming. */
  memset(data, 0, sizeof(data));
  if (!WriteCommand(kCmdTransferEnable, data)) {
    return false;
  }

  /* These reads are not handshakes, whatever the reverse engineered notes
   * say: 0x0030 is the SDRAM type and 0x0031 the video port. The vendor
   * driver reads them for real. Keeping them preserves the captured ordering
   * and costs nothing. */
  if (!ReadByte(kRegSdramType, &discard) ||
      !ReadByte(kRegModeSequence1, &discard) ||
      !ReadByte(kRegModeSequence2, &discard)) {
    return false;
  }

  memset(data, 0, sizeof(data));
  data[0] = kTransModeManualBlock;
  if (!WriteCommand(kCmdSetTransMode, data)) {
    return false;
  }

  ResolutionRequest resolution;
  PutBe16(resolution.width_be, mode.width);
  PutBe16(resolution.height_be, mode.height);
  resolution.pixel_format = kPixFmtUyvy;
  resolution.byte_select = kByteSelectUyvy;
  if (!WriteCommand(kCmdVideoInInfo, &resolution)) {
    return false;
  }

  ModeRequest mode_request;
  mode_request.mode_id = mode.mode_id;
  mode_request.pixel_format = 0x01;
  PutBe16(mode_request.width_be, mode.width);
  PutBe16(mode_request.height_be, mode.height);
  if (!WriteCommand(kCmdVideoOutInfo, &mode_request)) {
    return false;
  }

  memset(data, 0, sizeof(data));
  data[0] = 1;
  if (!WriteCommand(kCmdTransferEnable, data)) {
    return false;
  }
  last_mode_ = mode;
  have_last_mode_ = true;
  return true;
}

bool Device::Reinitialise() {
  if (!have_last_mode_) {
    return false;
  }
  output_enabled_ = false;
  if (!PowerOn()) {
    return false;
  }
  return SetResolution(last_mode_);
}

bool Device::EnableOutput(bool enable) {
  uint8_t data[6];
  memset(data, 0, sizeof(data));
  data[0] = enable ? 1 : 0;
  if (!WriteCommand(kCmdVideoEnable, data)) {
    return false;
  }
  output_enabled_ = enable;
  return true;
}

bool Device::ReadVideoPort(VideoPort* port) {
  uint8_t value = 0;
  if (!ReadByte(kRegVideoPort, &value)) {
    return false;
  }
  if (value > static_cast<uint8_t>(VideoPort::kDigital)) {
    *port = VideoPort::kUnknown;
  } else {
    *port = static_cast<VideoPort>(value);
  }
  return true;
}

bool Device::ReadDisplayStatus(uint8_t* status) {
  return ReadByte(kRegDisplayStatus, status);
}

bool Device::ReadCurrentFrameIndex(int* index) {
  uint8_t value = 0;
  if (!ReadByte(kRegFrameSwitch, &value)) {
    return false;
  }
  /* The vendor HAL treats this as a flag rather than a counter: any
   * non-zero value means buffer 1. */
  *index = value ? 1 : 0;
  return true;
}

bool Device::TriggerFrame(uint8_t index, uint8_t delay) {
  uint8_t data[6];
  memset(data, 0, sizeof(data));
  data[0] = index;
  data[1] = delay;
  return WriteCommand(kCmdTriggerFrame, data);
}

bool EdidBlockChecksumOk(const uint8_t* block) {
  uint8_t sum = 0;
  for (int i = 0; i < 128; ++i) {
    sum = static_cast<uint8_t>(sum + block[i]);
  }
  return sum == 0;
}

bool Device::ReadEdid(std::vector<uint8_t>* edid, int blocks,
                      bool* checksum_ok) {
  edid->assign(static_cast<size_t>(blocks) * 128, 0);
  /* Four bytes per round trip, so one block costs 32 transfers not 128. */
  for (size_t i = 0; i < edid->size(); i += kMaxRegisterReadCount) {
    size_t chunk = edid->size() - i;
    if (chunk > kMaxRegisterReadCount) {
      chunk = kMaxRegisterReadCount;
    }
    if (!ReadRegisters(static_cast<uint16_t>(kRegEdidBase + i),
                       edid->data() + i, chunk)) {
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

const char* ChipFamilyName(ChipFamily family) {
  switch (family) {
    case ChipFamily::kMs9120:
      return "MS9120";
    case ChipFamily::kMs912A:
      return "MS912A";
    case ChipFamily::kMs912C:
      return "MS912C";
    case ChipFamily::kMs9132:
      return "MS9132";
    default:
      return "unrecognised";
  }
}

bool Device::ReadChipInfo(ChipInfo* info) {
  info->family = ChipFamily::kUnknown;
  info->custom_timing_base = 0;

  /* Signature layout, from the vendor HAL: byte 1 is the family marker and
   * byte 2 is always 0x0A. For the 912x family byte 0 distinguishes the
   * variant. Note that a dongle sold with a USB 3 product id can still carry
   * a 912x die, so never infer the chip from the USB product id. */
  uint8_t signature[3] = {0, 0, 0};
  if (!ReadRegisters(kReg913xChipId, signature, 3)) {
    return false;
  }
  memcpy(info->signature, signature, sizeof(signature));
  if (signature[1] == kChipIdSignatureMsb913x &&
      signature[2] == kChipIdSignatureLsb) {
    info->family = ChipFamily::kMs9132;
    info->custom_timing_base = kCustomTimingBase913x;
    return true;
  }

  if (!ReadRegisters(kReg912xChipId, signature, 3)) {
    return false;
  }
  memcpy(info->signature, signature, sizeof(signature));
  if (signature[1] == kChipIdSignatureMsb912x &&
      signature[2] == kChipIdSignatureLsb) {
    switch (signature[0]) {
      case 0xB7:
        info->family = ChipFamily::kMs912C;
        break;
      case 0xA7:
        info->family = ChipFamily::kMs912A;
        break;
      default:
        info->family = ChipFamily::kMs9120;
        break;
    }
    info->custom_timing_base = kCustomTimingBase912x;
  }

  /* An unrecognised signature is not an I/O failure. info->signature holds
   * what the chip actually said so the caller can report it. */
  return true;
}

bool Device::ReadCustomTimings(std::vector<CustomMode>* modes) {
  modes->clear();

  ChipInfo info;
  if (!ReadChipInfo(&info)) {
    return false;
  }
  if (info.family == ChipFamily::kUnknown) {
    return Fail("chip signature not recognised, cannot locate custom timings");
  }

  uint8_t marker[7] = {0};
  if (!ReadFlash(info.custom_timing_base, marker, sizeof(marker))) {
    return false;
  }

  unsigned records;
  if (memcmp(marker, "modify1", sizeof(marker)) == 0) {
    records = 1;
  } else if (memcmp(marker, "modify2", sizeof(marker)) == 0) {
    records = 2;
  } else {
    return true; /* no custom timings programmed, not an error */
  }

  for (unsigned i = 0; i < records; ++i) {
    uint8_t raw[sizeof(CustomTimingRecord)] = {0};
    uint32_t address = info.custom_timing_base + kCustomTimingOffset +
                       i * kCustomTimingStride;
    if (!ReadFlash(address, raw, sizeof(raw))) {
      return false;
    }

    CustomMode custom;
    memset(&custom, 0, sizeof(custom));
    custom.mode.mode_id = raw[0];
    uint8_t polarity = raw[1];
    custom.htotal = ReadLe16(raw + 2);
    custom.vtotal = ReadLe16(raw + 4);
    custom.hactive = ReadLe16(raw + 6);
    custom.vactive = ReadLe16(raw + 8);
    custom.pixclk_10khz = ReadLe16(raw + 10);
    custom.vfreq_centihz = ReadLe16(raw + 12);
    custom.hoffset = ReadLe16(raw + 14);
    custom.voffset = ReadLe16(raw + 16);
    custom.hsyncwidth = ReadLe16(raw + 18);
    custom.vsyncwidth = ReadLe16(raw + 20);
    custom.progressive = (polarity & kTimingProgressive) != 0;
    custom.positive_hsync = (polarity & kTimingPositiveHSync) != 0;
    custom.positive_vsync = (polarity & kTimingPositiveVSync) != 0;

    if (custom.hactive == 0 || custom.hactive > kMaxWidth ||
        custom.vactive == 0 || custom.vactive > kMaxHeight ||
        custom.htotal <= custom.hactive || custom.vtotal <= custom.vactive ||
        custom.pixclk_10khz == 0 || custom.vfreq_centihz == 0) {
      continue;
    }

    custom.mode.width = custom.hactive;
    custom.mode.height = custom.vactive;
    custom.mode.hz =
        static_cast<uint16_t>((custom.vfreq_centihz + 50) / 100);
    if (custom.mode.hz == 0) {
      continue;
    }
    modes->push_back(custom);
  }
  return true;
}

bool Device::SendFrame(const uint8_t* data, size_t len) {
  if (!transport_->HasDataPlane()) {
    return Fail("current transport has no data plane");
  }
  if (!transport_->BulkWrite(data, len)) {
    /* A few failures in a row means the chip has stopped accepting data, not
     * that one transfer was unlucky. Reprogramming it is the only way back;
     * without this the panel stays dark until the device is replugged. */
    if (++consecutive_failures_ >= 3) {
      consecutive_failures_ = 0;
      Reinitialise();
    }
    return FailTransport("bulk write");
  }
  consecutive_failures_ = 0;
  /* The vendor driver terminates every frame with a zero length bulk packet.
   * Without it the chip can sit waiting for more data and the panel stays
   * dark even though every transfer reported success. */
  if (!transport_->BulkWrite(nullptr, 0)) {
    return FailTransport("end of frame packet");
  }
  if (!output_enabled_) {
    /* Only light the panel once a frame has actually landed, otherwise the
     * user sees whatever garbage was left in the chip's memory. */
    output_enabled_ = EnableOutput(true);
  }
  return true;
}

}  // namespace ms912x
