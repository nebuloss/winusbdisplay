/* SPDX-License-Identifier: GPL-2.0-only */

#include "ms912x_proto.h"

namespace ms912x {

const uint8_t kFrameFooter[kFrameFooterSize] = {0xFF, 0xC0, 0x00, 0x00,
                                                0x00, 0x00, 0x00, 0x00};

const Mode kModeList[] = {
    {720, 480, 60, 0x02},   {720, 576, 50, 0x11},   {1280, 720, 50, 0x13},
    {1920, 1080, 50, 0x1F}, {1920, 1080, 30, 0x22}, {640, 480, 60, 0x40},
    {800, 600, 60, 0x42},   {800, 600, 75, 0x44},   {1024, 768, 60, 0x47},
    {1024, 768, 75, 0x49},  {1152, 864, 60, 0x4C},  {1280, 600, 60, 0x4E},
    {1280, 720, 60, 0x4F},  {1280, 768, 60, 0x54},  {1280, 768, 75, 0x56},
    {1280, 800, 60, 0x57},  {1280, 960, 60, 0x5B},  {1280, 1024, 60, 0x60},
    {1280, 1024, 75, 0x61}, {1360, 768, 60, 0x64},  {1366, 768, 60, 0x66},
    {1400, 1050, 60, 0x67}, {1440, 900, 60, 0x6B},  {1600, 1200, 60, 0x73},
    {1680, 1050, 60, 0x78}, {1920, 1080, 60, 0x81},
};

const size_t kModeListLen = sizeof(kModeList) / sizeof(kModeList[0]);

const Mode* FindMode(uint16_t width, uint16_t height, uint16_t hz) {
  for (size_t i = 0; i < kModeListLen; ++i) {
    if (kModeList[i].width == width && kModeList[i].height == height &&
        kModeList[i].hz == hz) {
      return &kModeList[i];
    }
  }
  return nullptr;
}

uint32_t SyncDividerForMode(const Mode& mode) {
  /* Bytes one full frame costs on the wire, at two bytes per pixel. */
  const uint64_t frame_bytes =
      static_cast<uint64_t>(mode.width) * mode.height * 2;
  if (frame_bytes == 0 || mode.hz == 0) {
    return 1;
  }

  /* Frames per second the link can sustain for this mode. */
  const uint64_t sustainable = kSustainedBytesPerSecond / frame_bytes;
  if (sustainable >= mode.hz) {
    return 1;
  }
  if (sustainable == 0) {
    return mode.hz;
  }

  uint32_t divider = static_cast<uint32_t>((mode.hz + sustainable - 1) /
                                           sustainable);
  /* Windows rejects a zero divider, and anything past 8 makes the desktop
   * feel broken rather than merely slow. */
  if (divider < 1) {
    divider = 1;
  }
  if (divider > 8) {
    divider = 8;
  }
  return divider;
}

const char* VideoPortName(VideoPort port) {
  switch (port) {
    case VideoPort::kCvbs:
      return "CVBS";
    case VideoPort::kSVideo:
      return "S-Video";
    case VideoPort::kVga:
      return "VGA";
    case VideoPort::kYPbPr:
      return "YPbPr";
    case VideoPort::kCvbsSVideo:
      return "CVBS+S-Video";
    case VideoPort::kHdmi:
      return "HDMI";
    case VideoPort::kDigital:
      return "Digital";
    default:
      return "unknown";
  }
}

}  // namespace ms912x
