/* SPDX-License-Identifier: GPL-2.0-only */

#include "proto.h"

namespace usbhdmi {

const uint8_t kFrameFooter[kFrameFooterSize] = {0xFF, 0xC0, 0x00, 0x00,
                                                0x00, 0x00, 0x00, 0x00};

/* The chip's output timing table. Entries below 0x40 happen to line up with
 * CEA-861 video identification codes; the rest are the vendor's own. */
const Mode kModes[] = {
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

const size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

const Mode* FindMode(int width, int height, int hz) {
  for (size_t i = 0; i < kModeCount; ++i) {
    if (kModes[i].width == width && kModes[i].height == height &&
        kModes[i].hz == hz) {
      return &kModes[i];
    }
  }
  return nullptr;
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

}  // namespace usbhdmi
