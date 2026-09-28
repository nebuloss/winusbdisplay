/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Runtime knobs, read from HKLM\SOFTWARE\usbhdmi.
 *
 * HKLM rather than HKCU, and that is not a style choice: the driver runs
 * inside WUDFHost as LOCAL SERVICE and has no user hive to read. The
 * installer widens the ACL on this one key so a brightness control can write
 * it without needing elevation every time somebody moves a slider.
 */

#pragma once

#include <stdint.h>

#include "../render/convert.h"

namespace usbhdmi {

struct Settings {
  /* 0..100, applied while converting, because the adapter has no hardware
   * brightness control of any kind. */
  int brightness = 100;
  /* 0..100, 50 is neutral. */
  int contrast = 50;

  /* Repaint periodically when the desktop is static. The panel drops its
   * signal after a second or two without traffic, and a still desktop means
   * the compositor stops presenting entirely, so nothing else would keep it
   * alive. */
  bool idle_refresh = true;

  /* Updates at least this large convert on the GPU. Exposed because the
   * crossover depends on the machine: an integrated GPU sharing system
   * memory reads back cheaply, a discrete one does not. Zero forces the CPU
   * path, which is also what happens automatically if the GPU path cannot
   * initialise. */
  int64_t gpu_threshold_pixels = kGpuThresholdPixels;

  PictureAdjust picture() const {
    PictureAdjust adjust;
    adjust.brightness = brightness;
    adjust.contrast = contrast;
    return adjust;
  }
};

/* Re-reads the key. Cheap enough to call every couple of seconds from the
 * frame loop, which is how a brightness change takes effect without the
 * monitor having to be restarted. */
Settings ReadSettings();

}  // namespace usbhdmi
