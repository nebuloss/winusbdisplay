/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Drawing the mouse pointer into a converted region.
 *
 * The pointer has to be drawn by this driver. An indirect display does not
 * get it for free: the compositor hands over the desktop without it and
 * expects the driver either to own a hardware cursor or to put the pointer
 * in itself. That is why the pointer was missing entirely.
 *
 * Blending happens after conversion, in the adapter's own format, which
 * looks wrong and is exactly right. The transform from colour to luma and
 * chroma is a matrix, so it is linear, so blending afterwards gives the
 * same answer as blending before and converting. Doing it this way means
 * the pointer costs one pass over a few thousand pixels instead of a
 * second copy of whatever region it happens to land on, and it works the
 * same for both conversion paths rather than needing the shader to know
 * about it.
 *
 * The one place it is an approximation is chroma. Two pixels share a pair
 * of chroma samples, so a pointer edge that splits a pair blends chroma
 * with the average of the two coverages. At the size a pointer is drawn
 * this is not visible, and the alternative is carrying a second full
 * resolution buffer.
 *
 * This file is deliberately free of Windows: it is where the arithmetic
 * lives, so it can be tested without a machine that has a pointer. The
 * plumbing that gets the shape and position out of the operating system is
 * in driver/cursor.*.
 */

#pragma once

#include <stdint.h>

#include <vector>

#include "rect.h"

namespace usbdisplay {

/* A pointer image, already converted to the adapter's colour space.
 *
 * Converted once when the shape changes rather than on every frame: the
 * shape changes when the pointer crosses a window edge, the position
 * changes sixty times a second. */
class CursorImage {
 public:
  /* Takes a straight BGRA image, premultiplied or not, and keeps it as
   * luma, chroma and coverage. `stride` is bytes per row of `bgra`. */
  void SetBgra(const uint8_t* bgra, size_t stride, int width, int height,
               bool premultiplied);

  /* Takes a masked colour image, where the alpha channel is an AND mask
   * and therefore means the opposite of what it usually does.
   *
   * Zero is opaque: the colour replaces what is underneath. 255 asks for
   * the colour to be exclusive-ored with the pixel below, which cannot be
   * done once that pixel has been converted and discarded, so those are
   * left transparent. Asking the compositor to emulate the inverting kind
   * means in practice they do not arrive.
   *
   * This is the format every monochrome pointer becomes, which is to say
   * the text caret and most of the resize arrows. Treating it as ordinary
   * alpha inverts the mask: the pointer itself vanishes and the
   * transparent border around it is painted solid. */
  void SetMaskedColour(const uint8_t* bgra, size_t stride, int width,
                       int height);

  void Clear() { width_ = height_ = 0; }
  bool empty() const { return width_ <= 0 || height_ <= 0; }
  int width() const { return width_; }
  int height() const { return height_; }

  /* Draws the pointer into a converted region.
   *
   * `uyvy` covers `region` exactly, packed, two bytes a pixel, which is
   * how the frame path holds a converted region. `at` is where the
   * pointer's top left corner belongs in the same coordinates as
   * `region`. Anything outside the region is clipped. */
  void Blend(uint8_t* uyvy, const Rect& region, int at_x, int at_y) const;

 private:
  struct Texel {
    uint8_t y, u, v, a;
  };
  std::vector<Texel> texels_;
  int width_ = 0;
  int height_ = 0;
};

}  // namespace usbdisplay
