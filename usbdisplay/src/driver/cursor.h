/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Where the mouse pointer comes from.
 *
 * An indirect display driver does not get the pointer drawn for it. The
 * compositor hands over the desktop without it, on the assumption that the
 * hardware has a pointer of its own, and offers the shape and position
 * separately. A driver that asks for neither shows no pointer at all,
 * which is what this one did.
 *
 * This adapter has no pointer of its own, so the shape is fetched here and
 * drawn into the picture during conversion. See render/overlay.h for the
 * drawing, which is kept apart from this because it is arithmetic and can
 * be tested, while everything in this file can only be exercised against a
 * real compositor.
 *
 * The part that is easy to get wrong is not drawing the pointer, it is
 * erasing it. Moving the pointer changes two areas of the screen and the
 * compositor reports neither, because as far as it is concerned nothing
 * changed. Both have to be added to the damage by hand, or the old pointer
 * stays on the panel and the display fills up with copies of it.
 */

#pragma once

#include <windows.h>

/* Before IddCx, which refuses to compile otherwise and says so. */
#include <wdf.h>

#include <IddCx.h>

#include <vector>

#include "../render/overlay.h"
#include "../render/rect.h"

namespace usbdisplay {

class CursorOverlay {
 public:
  ~CursorOverlay();

  /* Asks the compositor for pointer updates on this monitor.
   *
   * Returns false if it declines, which is not fatal: the display works
   * without a pointer, and saying so in the log is more useful than
   * refusing to start. */
  bool Attach(IDDCX_MONITOR monitor);
  void Detach();

  /* Set when the pointer has moved or changed shape. The frame loop waits
   * on this alongside the frame event, so a pointer moving over a still
   * desktop still gets drawn. */
  HANDLE change_event() const { return event_; }

  /* Picks up whatever the compositor has waiting.
   *
   * Fills `erase` with where the pointer was and `draw` with where it is
   * now, either of which may be empty. Returns true if anything changed.
   * Both are in screen coordinates and are already padded to the alignment
   * the adapter wants, so the caller can hand them straight to the damage
   * tracker. */
  bool Poll(Rect* erase, Rect* draw);

  /* Draws the pointer into a converted region, if it lands there. */
  void Blend(uint8_t* uyvy, const Rect& region) const;

  /* Where the pointer is now, empty when it is hidden or absent. */
  Rect Bounds() const;

 private:
  bool Fetch();

  IDDCX_MONITOR monitor_ = nullptr;
  HANDLE event_ = nullptr;

  CursorImage image_;
  /* The buffer the compositor copies a new shape into. Sized once for the
   * largest pointer it is allowed to send. */
  std::vector<uint8_t> shape_;
  /* Which shape is already held, so the compositor only copies a new one
   * when it differs. A pointer moving without changing therefore costs
   * nothing but its position. */
  DWORD last_shape_id_ = 0;

  /* Top left of the pointer image in screen coordinates. Not the hotspot:
   * the compositor reports the corner, which is what drawing needs. */
  int x_ = 0;
  int y_ = 0;
  bool visible_ = false;
};

}  // namespace usbdisplay
