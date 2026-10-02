/* SPDX-License-Identifier: GPL-2.0-only */

#include "cursor.h"

#include <string.h>

#include "log.h"

namespace usbdisplay {
namespace {

/* The largest pointer the compositor is told it may send. Windows pointers
 * are 32 or 48 pixels in practice, so this is sized once at a comfortable
 * ceiling and never reallocated on the frame path. */
constexpr UINT kMaxCursorSide = 128;

}  // namespace

CursorOverlay::~CursorOverlay() { Detach(); }

bool CursorOverlay::Attach(IDDCX_MONITOR monitor) {
  Detach();

  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event_) {
    Log("cursor: could not create the change event");
    return false;
  }

  /* What the compositor will accept is not documented in a way that can be
   * relied on, and getting it wrong returns one undifferentiated invalid
   * parameter. So the combinations are tried in order of preference and
   * the first that is accepted is used, which also records in the log what
   * this machine actually allows.
   *
   * Emulation is preferred over full support for the awkward formats. A
   * masked pointer carries its mask in the alpha channel with the opposite
   * meaning to an ordinary one, and the inverting kind needs the pixels
   * underneath, which have been converted and discarded by the time
   * anything here could use them. Letting the compositor convert both to
   * ordinary alpha removes two whole classes of wrong pointer. */
  struct Attempt {
    IDDCX_XOR_CURSOR_SUPPORT xor_support;
    UINT side;
    const char* describe;
  };
  static const Attempt kAttempts[] = {
      {IDDCX_XOR_CURSOR_SUPPORT_EMULATION, 128, "emulated xor, 128"},
      {IDDCX_XOR_CURSOR_SUPPORT_EMULATION, 256, "emulated xor, 256"},
      {IDDCX_XOR_CURSOR_SUPPORT_FULL, 512, "full xor, 512"},
      {IDDCX_XOR_CURSOR_SUPPORT_FULL, 128, "full xor, 128"},
      {IDDCX_XOR_CURSOR_SUPPORT_NONE, 128, "no xor, 128"},
  };

  NTSTATUS status = STATUS_INVALID_PARAMETER;
  UINT side = 0;
  for (const Attempt& attempt : kAttempts) {
    IDDCX_CURSOR_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.AlphaCursorSupport = TRUE;
    caps.ColorXorCursorSupport = attempt.xor_support;
    caps.MaxX = attempt.side;
    caps.MaxY = attempt.side;

    IDARG_IN_SETUP_HWCURSOR in = {};
    in.CursorInfo = caps;
    in.hNewCursorDataAvailable = event_;

    status = IddCxMonitorSetupHardwareCursor(monitor, &in);
    if (NT_SUCCESS(status)) {
      side = attempt.side;
      Log("cursor: attached with %s", attempt.describe);
      break;
    }
    Log("cursor: %s refused -> 0x%08X", attempt.describe, status);
  }

  if (!NT_SUCCESS(status)) {
    /* Not fatal. Without this the panel simply has no pointer on it, which
     * is worth a line in the log and nothing more. */
    Log("cursor: no combination was accepted, there will be no pointer");
    CloseHandle(event_);
    event_ = nullptr;
    return false;
  }

  monitor_ = monitor;
  shape_.resize(static_cast<size_t>(side) * side * 4);
  return true;
}

void CursorOverlay::Detach() {
  monitor_ = nullptr;
  if (event_) {
    CloseHandle(event_);
    event_ = nullptr;
  }
  image_.Clear();
  visible_ = false;
  last_shape_id_ = 0;
}

bool CursorOverlay::Fetch() {
  IDARG_IN_QUERY_HWCURSOR in = {};
  /* The shape is only copied when it differs from the one already held, so
   * the common case of a pointer moving without changing costs nothing but
   * the position. */
  in.LastShapeId = last_shape_id_;
  in.ShapeBufferSizeInBytes = static_cast<UINT>(shape_.size());
  in.pShapeBuffer = shape_.data();

  IDARG_OUT_QUERY_HWCURSOR out = {};
  const NTSTATUS status = IddCxMonitorQueryHardwareCursor(monitor_, &in, &out);
  if (!NT_SUCCESS(status)) {
    return false;
  }

  /* X and Y are the top left of the image, not the hotspot. The hotspot is
   * reported too and is deliberately not used: it describes where the
   * pointer points, which matters to whoever positions it and not to
   * whoever draws it. Subtracting it here would offset every pointer by
   * its own hotspot, which for the ordinary arrow is nothing and for a
   * crosshair or a resize arrow is half its width. */
  x_ = out.X;
  y_ = out.Y;
  visible_ = out.IsCursorVisible != FALSE;

  if (!out.IsCursorShapeUpdated) {
    return true;
  }

  const IDDCX_CURSOR_SHAPE_INFO& info = out.CursorShapeInfo;
  last_shape_id_ = info.ShapeId;

  switch (info.CursorType) {
    case IDDCX_CURSOR_SHAPE_TYPE_ALPHA:
      /* Ordinary coverage, which is what an arrow is. Not premultiplied:
       * the compositor hands over the colour unscaled. */
      image_.SetBgra(shape_.data(), info.Pitch, static_cast<int>(info.Width),
                     static_cast<int>(info.Height), false);
      break;
    case IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR:
      /* Every monochrome pointer becomes one of these, so this is the
       * text caret and most of the resize arrows rather than an exotic
       * case. Asking for emulation does not avoid it: that governs only
       * the inverting pixels within this format, not the format itself.
       *
       * Reading it as ordinary coverage inverts the mask, which hides the
       * pointer and paints the transparent border around it solid. */
      image_.SetMaskedColour(shape_.data(), info.Pitch,
                             static_cast<int>(info.Width),
                             static_cast<int>(info.Height));
      break;
    default:
      /* Clearing leaves whatever was drawn last sitting on the panel with
       * nothing to erase it, so the log says which format to add. */
      Log("cursor: unknown shape type %u, the pointer will not update",
          info.CursorType);
      image_.Clear();
      break;
  }
  return true;
}

Rect CursorOverlay::Bounds() const {
  if (!visible_ || image_.empty()) {
    return Rect();
  }
  Rect bounds;
  bounds.x1 = x_;
  bounds.y1 = y_;
  bounds.x2 = x_ + image_.width();
  bounds.y2 = y_ + image_.height();
  return bounds;
}

bool CursorOverlay::Poll(Rect* erase, Rect* draw) {
  *erase = Rect();
  *draw = Rect();
  if (!monitor_) {
    return false;
  }

  const Rect before = Bounds();
  if (!Fetch()) {
    return false;
  }
  const Rect after = Bounds();

  if (before.x1 == after.x1 && before.y1 == after.y1 &&
      before.x2 == after.x2 && before.y2 == after.y2) {
    return false;
  }

  /* Both areas are owed a repaint and the compositor will report neither:
   * as far as it is concerned the desktop did not change. The old area has
   * to be redrawn from the desktop underneath to erase the pointer, the
   * new one to draw it. Miss the first and the panel fills with copies of
   * the pointer. */
  *erase = before;
  *draw = after;
  return true;
}

void CursorOverlay::Blend(uint8_t* uyvy, const Rect& region) const {
  const Rect bounds = Bounds();
  if (bounds.empty()) {
    return;
  }
  image_.Blend(uyvy, region, bounds.x1, bounds.y1);
}

}  // namespace usbdisplay
