/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Wire protocol constants for MacroSilicon MS912x / MS913x USB display
 * adapters. Transliterated from the GPL-2.0 Linux driver ms912x by rhgndf
 * (https://github.com/rhgndf/ms912x), which was reverse engineered from
 * USB captures of the vendor's Windows driver.
 */

#pragma once

#include <stdint.h>

namespace ms912x {

constexpr uint16_t kVidMacroSiliconUsb2 = 0x534D;
constexpr uint16_t kVidMacroSiliconUsb3 = 0x345F;

constexpr uint8_t kBulkOutEndpoint = 4;

/* First byte of every 8-byte control payload. */
constexpr uint8_t kReqTypeWrite6Bytes = 0xA6;
constexpr uint8_t kReqTypeReadByte = 0xB5;
constexpr uint8_t kReqTypeReadFlash = 0xF5;

/* HID class control transfer plumbing. */
constexpr uint8_t kHidReqGetReport = 0x01;
constexpr uint8_t kHidReqSetReport = 0x09;
constexpr uint16_t kHidReportValue = 0x0300; /* feature report, report id 0 */
constexpr uint16_t kHidReportIndex = 0;      /* interface 0, not the display one */
constexpr size_t kControlPayloadSize = 8;

/* Registers. */
constexpr uint16_t kRegVideoPort = 0x0031;
constexpr uint16_t kRegDisplayStatus = 0x0032;
constexpr uint16_t kRegEdidBase = 0xC000;
constexpr uint16_t kRegModeSequence0 = 0x0030;
constexpr uint16_t kRegModeSequence1 = 0x0033;
constexpr uint16_t kRegModeSequence2 = 0xC620;

/* Commands, written with kReqTypeWrite6Bytes. */
constexpr uint8_t kCmdResolution = 0x01;
constexpr uint8_t kCmdMode = 0x02;
constexpr uint8_t kCmdUnknown1 = 0x03;
constexpr uint8_t kCmdUnknown2 = 0x04;
constexpr uint8_t kCmdOutputEnable = 0x05;
constexpr uint8_t kCmdPower = 0x07;

/* Chip identification and custom timing storage in flash. */
constexpr uint16_t kReg913xChipId = 0xFF00;
constexpr uint16_t kReg912xChipId = 0xF000;
constexpr uint8_t kChipIdSignatureMsb913x = 0x13;
constexpr uint8_t kChipIdSignatureMsb912x = 0x16;
constexpr uint8_t kChipIdSignatureLsb = 0x0A;
constexpr uint32_t kCustomTimingBase913x = 0xFC50;
constexpr uint32_t kCustomTimingBase912x = 0x1C00;
constexpr uint32_t kCustomTimingOffset = 0x10;
constexpr uint32_t kCustomTimingStride = 0x20;

constexpr uint8_t kTimingProgressive = 1u << 0;
constexpr uint8_t kTimingPositiveHSync = 1u << 1;
constexpr uint8_t kTimingPositiveVSync = 1u << 2;

/* Pixel formats for the kCmdResolution payload. */
constexpr uint8_t kPixFmtRgb888 = 0x11;
constexpr uint8_t kPixFmtUyvy = 0x22;
constexpr uint8_t kByteSelectUyvy = 0x00;

/* Framebuffer transfer framing. */
constexpr uint16_t kFrameMarker = 0xFF00;
constexpr size_t kFrameHeaderSize = 8;
constexpr size_t kFrameFooterSize = 8;
constexpr size_t kFrameOverhead = kFrameHeaderSize + kFrameFooterSize; /* 16 */
constexpr uint32_t kMaxWidth = 1920;
constexpr uint32_t kMaxHeight = 1200;
constexpr size_t kMaxTransferLen =
    static_cast<size_t>(kMaxWidth) * kMaxHeight * 2 + kFrameOverhead;

enum class VideoPort : uint8_t {
  kCvbs = 0,
  kSVideo = 1,
  kVga = 2,
  kYPbPr = 3,
  kCvbsSVideo = 4,
  kHdmi = 5,
  kDigital = 6,
  kUnknown = 0xFF,
};

const char* VideoPortName(VideoPort port);

/* A mode the chip understands. `mode_id` is the magic byte the chip wants in
 * the kCmdMode payload. For the low values it happens to coincide with a
 * CEA-861 VIC, but from 0x40 up it is a vendor specific table index. */
struct Mode {
  uint16_t width;
  uint16_t height;
  uint16_t hz;
  uint8_t mode_id;
};

/* Mode table dumped from the device / captured from the Windows driver. */
extern const Mode kModeList[];
extern const size_t kModeListLen;

const Mode* FindMode(uint16_t width, uint16_t height, uint16_t hz);

#pragma pack(push, 1)

/* kReqTypeReadByte / response. addr is big endian. */
struct RegisterRequest {
  uint8_t type;
  uint8_t addr_be[2];
  uint8_t data[5];
};
static_assert(sizeof(RegisterRequest) == kControlPayloadSize, "");

/* kReqTypeWrite6Bytes. */
struct WriteRequest {
  uint8_t type;
  uint8_t cmd;
  uint8_t data[6];
};
static_assert(sizeof(WriteRequest) == kControlPayloadSize, "");

/* kReqTypeReadFlash. addr is a big endian 24 bit value. */
struct FlashReadRequest {
  uint8_t type;
  uint8_t addr[3];
  uint8_t reserved[4];
};
static_assert(sizeof(FlashReadRequest) == kControlPayloadSize, "");

/* Payload of kCmdResolution. */
struct ResolutionRequest {
  uint8_t width_be[2];
  uint8_t height_be[2];
  uint8_t pixel_format;
  uint8_t byte_select;
};
static_assert(sizeof(ResolutionRequest) == 6, "");

/* Payload of kCmdMode. Note pixel_format here is a different field from the
 * one in ResolutionRequest and is always 0x01. */
struct ModeRequest {
  uint8_t mode_id;
  uint8_t pixel_format;
  uint8_t width_be[2];
  uint8_t height_be[2];
};
static_assert(sizeof(ModeRequest) == 6, "");

/* Bulk OUT frame header. No padding: pixel data starts at byte 8. */
struct FrameUpdateHeader {
  uint8_t marker_be[2];
  uint8_t position[3];   /* be24: (x & 0xfff) << 12 | (y & 0xfff) */
  uint8_t dimensions[3]; /* be24: (w & 0xfff) << 12 | (h & 0xfff) */
};
static_assert(sizeof(FrameUpdateHeader) == kFrameHeaderSize, "");

/* One custom timing record in flash, little endian. */
struct CustomTimingRecord {
  uint8_t vic;
  uint8_t polarity;
  uint16_t htotal;
  uint16_t vtotal;
  uint16_t hactive;
  uint16_t vactive;
  uint16_t pixclk;
  uint16_t vfreq; /* centihertz */
  uint16_t hoffset;
  uint16_t voffset;
  uint16_t hsyncwidth;
  uint16_t vsyncwidth;
};
static_assert(sizeof(CustomTimingRecord) == 26, "");

#pragma pack(pop)

extern const uint8_t kFrameFooter[kFrameFooterSize];

inline void PutBe16(uint8_t* dst, uint16_t value) {
  dst[0] = static_cast<uint8_t>(value >> 8);
  dst[1] = static_cast<uint8_t>(value);
}

inline void PutBe24(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value >> 16);
  dst[1] = static_cast<uint8_t>(value >> 8);
  dst[2] = static_cast<uint8_t>(value);
}

}  // namespace ms912x
