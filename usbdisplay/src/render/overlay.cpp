/* SPDX-License-Identifier: GPL-2.0-only */

#include "overlay.h"

#include <string.h>

#include "convert.h"

namespace usbdisplay {
namespace {

/* The same arithmetic the conversion path uses, deliberately.
 *
 * Not shared with convert.cpp because those are in an anonymous namespace
 * there and exposing them would widen that file's interface for one
 * caller. The constants are shared, which is the part that matters: a
 * pointer converted with different coefficients from the desktop it sits
 * on would have a visible colour cast against it. */
inline int RgbToY(int r, int g, int b) {
  return 16 + ((kCoeffYr * r + kCoeffYg * g + kCoeffYb * b) >> 15);
}
inline int RgbToU(int r, int g, int b) {
  return 128 + ((kCoeffUr * r + kCoeffUg * g + kCoeffUb * b) >> 15);
}
inline int RgbToV(int r, int g, int b) {
  return 128 + ((kCoeffVr * r + kCoeffVg * g + kCoeffVb * b) >> 15);
}

inline uint8_t Mix(uint8_t under, uint8_t over, int alpha) {
  return static_cast<uint8_t>(under + (((over - under) * alpha + 127) / 255));
}

}  // namespace

void CursorImage::SetBgra(const uint8_t* bgra, size_t stride, int width,
                          int height, bool premultiplied) {
  if (!bgra || width <= 0 || height <= 0) {
    Clear();
    return;
  }
  width_ = width;
  height_ = height;
  texels_.resize(static_cast<size_t>(width) * height);

  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      const uint8_t* p = row + static_cast<size_t>(x) * 4;
      int b = p[0], g = p[1], r = p[2];
      const int a = p[3];

      /* Premultiplied pixels carry colour already scaled by coverage, and
       * blending would scale it a second time. Undoing it here keeps the
       * blend below ignorant of which kind arrived. */
      if (premultiplied && a != 0 && a != 255) {
        b = b * 255 / a;
        g = g * 255 / a;
        r = r * 255 / a;
        if (b > 255) b = 255;
        if (g > 255) g = 255;
        if (r > 255) r = 255;
      }

      Texel& texel = texels_[static_cast<size_t>(y) * width + x];
      texel.y = static_cast<uint8_t>(RgbToY(r, g, b));
      texel.u = static_cast<uint8_t>(RgbToU(r, g, b));
      texel.v = static_cast<uint8_t>(RgbToV(r, g, b));
      texel.a = static_cast<uint8_t>(a);
    }
  }
}

void CursorImage::SetMaskedColour(const uint8_t* bgra, size_t stride,
                                  int width, int height) {
  if (!bgra || width <= 0 || height <= 0) {
    Clear();
    return;
  }
  width_ = width;
  height_ = height;
  texels_.resize(static_cast<size_t>(width) * height);

  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      const uint8_t* p = row + static_cast<size_t>(x) * 4;
      const int b = p[0], g = p[1], r = p[2];

      Texel& texel = texels_[static_cast<size_t>(y) * width + x];
      /* The mask is inverted against ordinary alpha, which is the whole
       * reason this is a separate entry point. Zero means draw the
       * colour; anything else asks for an exclusive-or against pixels
       * that no longer exist here, and is left alone instead. */
      if (p[3] != 0) {
        texel.a = 0;
        continue;
      }
      texel.y = static_cast<uint8_t>(RgbToY(r, g, b));
      texel.u = static_cast<uint8_t>(RgbToU(r, g, b));
      texel.v = static_cast<uint8_t>(RgbToV(r, g, b));
      texel.a = 255;
    }
  }
}

void CursorImage::Blend(uint8_t* uyvy, const Rect& region, int at_x,
                        int at_y) const {
  if (empty() || !uyvy || region.empty()) {
    return;
  }

  /* Where the pointer and the region actually overlap, in screen
   * coordinates. */
  int x1 = at_x > region.x1 ? at_x : region.x1;
  int y1 = at_y > region.y1 ? at_y : region.y1;
  int x2 = (at_x + width_) < region.x2 ? (at_x + width_) : region.x2;
  int y2 = (at_y + height_) < region.y2 ? (at_y + height_) : region.y2;
  if (x1 >= x2 || y1 >= y2) {
    return;
  }

  const size_t row_bytes = static_cast<size_t>(region.width()) * 2;

  for (int y = y1; y < y2; ++y) {
    uint8_t* row =
        uyvy + static_cast<size_t>(y - region.y1) * row_bytes;
    const Texel* source =
        &texels_[static_cast<size_t>(y - at_y) * width_];

    for (int x = x1; x < x2; ++x) {
      const Texel& texel = source[x - at_x];
      if (texel.a == 0) {
        continue;
      }

      /* Two pixels share a chroma pair, so the byte offsets depend on
       * which half of the pair this pixel is. */
      const int in_region = x - region.x1;
      uint8_t* pair = row + static_cast<size_t>(in_region / 2) * 4;
      uint8_t* luma = pair + 1 + (in_region & 1) * 2;

      *luma = Mix(*luma, texel.y, texel.a);
      /* Chroma is blended at half strength because only one of the two
       * pixels sharing it is being covered by this step. A pointer edge
       * that splits a pair therefore gets the average of the two, which
       * is the correct thing and is why this is not conditional on
       * coverage. */
      pair[0] = Mix(pair[0], texel.u, texel.a / 2);
      pair[2] = Mix(pair[2], texel.v, texel.a / 2);
    }
  }
}

}  // namespace usbdisplay
