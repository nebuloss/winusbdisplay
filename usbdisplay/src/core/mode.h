/* SPDX-License-Identifier: GPL-2.0-only
 *
 * A display mode and a connector type. Device independent on purpose: these
 * appear in the DisplayDevice interface, which must not drag in any
 * particular chip's wire constants.
 */

#pragma once

#include <stdint.h>

#include <stddef.h>

namespace usbdisplay {

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

/* One entry of the chip's output timing table. `index` is the byte the chip
 * wants; for the low values it coincides with a CEA-861 VIC but the table
 * runs past anything CEA defines, so treat it as an opaque table index. */
struct Mode {
  int width;
  int height;
  int hz;
  uint8_t index;
};

extern const Mode kModes[];
extern const size_t kModeCount;

const Mode* FindMode(int width, int height, int hz);

}  // namespace usbdisplay
