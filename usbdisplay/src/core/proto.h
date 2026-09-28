/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Wire protocol for MacroSilicon MS912x / MS913x USB display chips.
 *
 * Constants only, no logic. Three sources were reconciled to produce this:
 * MacroSilicon's own GPL-2.0 Linux driver (which names everything), the
 * GPL-2.0 ms912x driver by rhgndf (reverse engineered from Windows captures),
 * and measurements on an MS912C. Where they disagreed, the hardware won.
 * ../../../docs/protocol-notes.md records each disagreement and its evidence.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mode.h"

namespace usbdisplay {

/* Both vendor ids are in use. The product id says nothing useful about the
 * silicon: the unit these notes were measured on reports a USB 3 product id
 * and contains a USB 2 die. Probe the chip id instead. */
constexpr uint16_t kVendorIdUsb2 = 0x534D;
constexpr uint16_t kVendorIdUsb3 = 0x345F;

/* Pixels leave on this bulk OUT endpoint, on the display interface. */
constexpr uint8_t kBulkOutEndpoint = 4;

/* Every control exchange is an 8 byte HID feature report. */
constexpr size_t kControlSize = 8;
constexpr uint16_t kHidFeatureReport = 0x0300; /* feature report, id 0 */
constexpr uint16_t kHidControlInterface = 0;   /* MI_00, not the display one */

/* First byte of a control payload: what kind of operation this is. */
constexpr uint8_t kOpVideo = 0xA6;     /* followed by a sub-operation */
constexpr uint8_t kOpReadXdata = 0xB5; /* returns up to four bytes */
constexpr uint8_t kOpReadFlash = 0xF5;

/* Sub-operations of kOpVideo, named by the vendor source. */
constexpr uint8_t kVideoTriggerFrame = 0x00;
constexpr uint8_t kVideoInputInfo = 0x01;  /* what we are sending */
constexpr uint8_t kVideoOutputInfo = 0x02; /* what the panel should show */
constexpr uint8_t kVideoTransferMode = 0x03;
constexpr uint8_t kVideoTransferEnable = 0x04;
constexpr uint8_t kVideoEnable = 0x05;
constexpr uint8_t kVideoPower = 0x07;

/* Transfer modes. Manual block is the one that makes partial updates work at
 * all; in any other mode the chip expects whole frames. Bypass manual block
 * looks like it would avoid the chip's internal double buffering and was
 * tried: the chip rejects every transfer and the panel stays dark. */
constexpr uint8_t kTransferModeFrame = 0;
constexpr uint8_t kTransferModeFixedBlockMN = 1;
constexpr uint8_t kTransferModeFixedBlockWH = 2;
constexpr uint8_t kTransferModeManualBlock = 3;
constexpr uint8_t kTransferModeBypassFrame = 4;
constexpr uint8_t kTransferModeBypassManualBlock = 5;

/* Registers in the chip's xdata space. */
constexpr uint16_t kRegSdramType = 0x0030;
constexpr uint16_t kRegVideoPort = 0x0031;
constexpr uint16_t kRegDisplayStatus = 0x0032; /* hot plug detect */
constexpr uint16_t kRegModesetProbeA = 0x0033; /* read during modeset */
constexpr uint16_t kRegModesetProbeB = 0xC620; /* read during modeset */
constexpr uint16_t kRegEdidBase = 0xC000;
/* The vendor reads this to learn which of the chip's two internal images is
 * on screen. Unused here on purpose; see Chip::ReadLiveImageIndex. */
constexpr uint16_t kRegLiveImageIndex = 0xD003;
constexpr uint16_t kRegChipId912x = 0xF000;
constexpr uint16_t kRegChipId913x = 0xFF00;

/* One read request returns four consecutive bytes, which is what makes a
 * 128 byte EDID cost 32 round trips instead of 128. The reverse engineered
 * driver only ever read one byte and nobody had noticed. */
constexpr size_t kMaxReadBytes = 4;

/* Chip id signatures. Byte 1 identifies the family, byte 0 the part. */
constexpr uint8_t kSignatureFamily912x = 0x16;
constexpr uint8_t kSignatureFamily913x = 0x13;
constexpr uint8_t kSignatureTail = 0x0A;
constexpr uint8_t kSignaturePart912C = 0xB7;
constexpr uint8_t kSignaturePart912A = 0xA7;

/* Custom output timings live in flash, at a base address that depends on the
 * chip family. Reading the wrong one yields plausible looking garbage, which
 * is the practical reason never to guess the family from the product id. */
constexpr uint32_t kFlashTimingBase912x = 0x1C00;
constexpr uint32_t kFlashTimingBase913x = 0xFC50;
constexpr uint32_t kFlashTimingOffset = 0x10;
constexpr uint32_t kFlashTimingStride = 0x20;

constexpr uint8_t kTimingProgressive = 1u << 0;
constexpr uint8_t kTimingPositiveHSync = 1u << 1;
constexpr uint8_t kTimingPositiveVSync = 1u << 2;

/* Pixel formats the chip accepts. RGB565, RGB888, YUV422 and YUV444 are all
 * available and 4:2:2 at 16 bpp is the cheapest, so there is no fallback to
 * reach for when bandwidth is tight. */
constexpr uint8_t kPixelFormatRgb888 = 0x11;
constexpr uint8_t kPixelFormatUyvy = 0x22;
constexpr uint8_t kByteSelectUyvy = 0x00;

/* Bulk transfer framing. Header, then packed UYVY rows with no padding
 * between them, then the footer. Total is width * 2 * height + 16. */
constexpr uint16_t kFrameMarker = 0xFF00;
constexpr size_t kFrameHeaderSize = 8;
constexpr size_t kFrameFooterSize = 8;
constexpr size_t kFrameOverhead = kFrameHeaderSize + kFrameFooterSize;
extern const uint8_t kFrameFooter[kFrameFooterSize];

constexpr int kMaxFrameWidth = 1920;
constexpr int kMaxFrameHeight = 1200;
constexpr size_t kMaxTransferBytes =
    static_cast<size_t>(kMaxFrameWidth) * kMaxFrameHeight * 2 + kFrameOverhead;

/* The chip finishes a bulk transfer on its own 60 Hz boundary rather than
 * streaming at a byte rate, so the cost of an update is quantised into these
 * periods. Everything in render/ is built around that; see render/rect.h. */
constexpr size_t kBytesPerPeriod = 520u * 1024u;
constexpr unsigned kPeriodMicroseconds = 16667;

/* Sustained bulk throughput measured on an MS912C at 1080p. The bus could
 * carry more; the chip cannot. Pipelining eight overlapped transfers moves
 * this by under one percent, so it is a device limit, not a host one. */
constexpr size_t kSustainedBytesPerSecond = 29u * 1024u * 1024u;

/* Modes and connector types live in mode.h: they are part of the device
 * independent interface rather than of this chip's wire format. */


#pragma pack(push, 1)

/* kOpReadXdata, and the response to it. */
struct ReadRequest {
  uint8_t op;
  uint8_t address_be[2];
  uint8_t data[4];
  uint8_t reserved;
};
static_assert(sizeof(ReadRequest) == kControlSize, "");

/* kOpVideo. The six payload bytes are one of the structures below. */
struct VideoRequest {
  uint8_t op;
  uint8_t sub_op;
  uint8_t payload[6];
};
static_assert(sizeof(VideoRequest) == kControlSize, "");

struct FlashReadRequest {
  uint8_t op;
  uint8_t address_be24[3];
  uint8_t reserved[4];
};
static_assert(sizeof(FlashReadRequest) == kControlSize, "");

/* Payload of kVideoInputInfo: the format of the pixels we will send. */
struct InputInfo {
  uint8_t width_be[2];
  uint8_t height_be[2];
  uint8_t pixel_format;
  uint8_t byte_select;
};
static_assert(sizeof(InputInfo) == 6, "");

/* Payload of kVideoOutputInfo: the timing the panel should run at. The
 * `colour` byte here is a different field from the pixel format above and is
 * always 1. */
struct OutputInfo {
  uint8_t index;
  uint8_t colour;
  uint8_t width_be[2];
  uint8_t height_be[2];
};
static_assert(sizeof(OutputInfo) == 6, "");

/* Bulk frame header. Pixel data starts at byte 8; there is no padding, which
 * is where the published description of this protocol is wrong. */
struct FrameHeader {
  uint8_t marker_be[2];
  uint8_t position_be24[3];   /* (x & 0xFFF) << 12 | (y & 0xFFF) */
  uint8_t dimensions_be24[3]; /* (w & 0xFFF) << 12 | (h & 0xFFF) */
};
static_assert(sizeof(FrameHeader) == kFrameHeaderSize, "");

/* One custom timing record as stored in flash, little endian. */
struct FlashTiming {
  uint8_t index;
  uint8_t polarity;
  uint16_t htotal;
  uint16_t vtotal;
  uint16_t hactive;
  uint16_t vactive;
  uint16_t pixel_clock_10khz;
  uint16_t vfreq_centihz;
  uint16_t hoffset;
  uint16_t voffset;
  uint16_t hsync_width;
  uint16_t vsync_width;
};
static_assert(sizeof(FlashTiming) == 22, "");

#pragma pack(pop)

inline void PutBe16(uint8_t* dst, uint16_t value) {
  dst[0] = static_cast<uint8_t>(value >> 8);
  dst[1] = static_cast<uint8_t>(value);
}

inline void PutBe24(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value >> 16);
  dst[1] = static_cast<uint8_t>(value >> 8);
  dst[2] = static_cast<uint8_t>(value);
}

}  // namespace usbdisplay
