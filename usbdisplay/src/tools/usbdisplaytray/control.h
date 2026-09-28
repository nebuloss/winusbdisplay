/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Setting the brightness of a monitor, whatever kind of monitor it is.
 *
 * Windows has no single way to do this. It has three, they apply to
 * different hardware, and a monitor that answers none of them is simply not
 * adjustable. What each one needs:
 *
 *   DDC/CI      a protocol carried along the display cable itself, driven by
 *               the graphics card's I2C master. This is how a desktop
 *               monitor's own brightness setting is reached, and it is the
 *               one to prefer: it adjusts the backlight, so black stays
 *               black and no image quality is given up.
 *
 *   gamma ramp  a lookup table applied to the pixels on their way out. Not
 *               real brightness, and at low settings it visibly crushes the
 *               darker tones, but it needs nothing from the hardware and so
 *               works where the first does not.
 *
 *   WMI         the path laptop panels use, which needs a kernel driver to
 *               expose it. Not implemented here: this machine has none, so
 *               there would be no way to test it, and shipping an untested
 *               path is worse than not shipping one.
 *
 * The USB adapter this project drives answers the second. Its display is not
 * attached to a graphics card's signalling hardware, so the first cannot
 * reach it; the driver accepts the gamma table instead and applies it while
 * converting, which genuinely dims the panel rather than only appearing to.
 */

#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace usbdisplay {

class BrightnessControl {
 public:
  virtual ~BrightnessControl() = default;

  /* How this control reaches the monitor, for the interface to show. */
  virtual const wchar_t* Method() const = 0;

  /* True when this control adjusts the actual backlight rather than the
   * pixels. Worth surfacing, because the two behave differently at the dark
   * end and a user is entitled to know which one they have. */
  virtual bool IsHardware() const = 0;

  /* 0 to 100. Returns false if the monitor stopped responding, which
   * happens routinely when one is switched off or unplugged. */
  virtual bool Get(int* percent) = 0;
  virtual bool Set(int percent) = 0;

  /* Applies anything a rate-limited implementation is still holding back.
   * Called when a slider settles, so the final position always takes
   * effect. Does nothing where there is no rate limit. */
  virtual void Flush() {}
};

/* A monitor and the best way found to control it. */
struct Monitor {
  std::wstring name;        /* what the monitor calls itself */
  std::wstring adapter;     /* what drives it, which is how two identical
                             * monitors are told apart */
  std::wstring device;      /* \\.\DISPLAYn */
  std::wstring identity;    /* stable across restarts, for saved settings */
  bool primary = false;
  std::unique_ptr<BrightnessControl> control;
  int brightness = 100;
};

/* Finds every attached monitor and works out how to control each.
 *
 * Deliberately returns monitors it cannot control as well, with a null
 * control. Hiding them would leave a user wondering whether a screen was
 * missed; showing it as unavailable at least answers the question. */
std::vector<Monitor> FindMonitors();

}  // namespace usbdisplay
