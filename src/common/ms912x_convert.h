/* SPDX-License-Identifier: GPL-2.0-only
 *
 * XRGB8888 to UYVY 4:2:2 conversion and bulk transfer framing.
 */

#pragma once

#include <stdint.h>

#include <vector>

#include "ms912x_proto.h"

namespace ms912x {

struct Rect {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;

  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
  bool empty() const { return x2 <= x1 || y2 <= y1; }
};

/* UYVY encodes pixels in pairs, so a damage rect whose left edge lands on an
 * odd column produces colour fringing. Expand to a whole pair and clamp to the
 * framebuffer. */
Rect AlignDamageRect(const Rect& rect, int fb_width, int fb_height);

Rect MergeRects(const Rect& a, const Rect& b);
Rect EmptyRect();

/* Bytes a framed transfer for `rect` will occupy. */
inline size_t TransferLength(const Rect& rect) {
  return static_cast<size_t>(rect.width()) * 2 * rect.height() + kFrameOverhead;
}

/* Picture adjustment applied during conversion, expressed the way DDC/CI
 * reports them: brightness and contrast each 0..100, with 100 and 50 meaning
 * "leave the image alone". The dongle has no hardware controls, so this is
 * how a brightness slider produces a visible change.
 *
 * Y is limited range (16..235) and gets scaled about its black point; U and V
 * are scaled about neutral (128) by contrast so colours do not shift. */
struct PictureAdjust {
  int brightness = 100;
  int contrast = 50;

  bool IsIdentity() const { return brightness == 100 && contrast == 50; }
};

void ApplyPictureAdjust(uint8_t* uyvy_row, int width,
                        const PictureAdjust& adjust);

/* Writes header, converted pixels and footer into `dst`, which must hold at
 * least TransferLength(rect) bytes. `src` points at the top left pixel of the
 * whole framebuffer in XRGB8888 with `src_stride` bytes per row. Returns the
 * number of bytes written, or 0 if the arguments are inconsistent.
 *
 * The rect must already be aligned; call AlignDamageRect first. */
size_t FrameRect(uint8_t* dst, size_t dst_capacity, const uint8_t* src,
                 size_t src_stride, int fb_width, int fb_height,
                 const Rect& rect,
                 const PictureAdjust& adjust = PictureAdjust());

/* Converts one row. Dispatches to the SIMD path when the CPU supports it.
 *
 * A scalar loop cannot keep up at 1080p: it costs tens of milliseconds per
 * full frame on the IddCx processing thread, which shows up directly as lag. */
void ConvertRowXrgbToUyvy(uint8_t* dst, const uint8_t* src, int width);

/* Scalar reference implementation, kept so the optimised path can be diffed
 * against it. See ConvertSelfTest. */
void ConvertRowXrgbToUyvyScalar(uint8_t* dst, const uint8_t* src, int width);

/* SSE2 implementation. Uses coefficients halved to fit the 16-bit multiply,
 * so results may differ from the scalar path by one least significant bit. */
void ConvertRowXrgbToUyvySimd(uint8_t* dst, const uint8_t* src, int width);

/* Converts rows across several threads. Worth doing because most of the cost
 * is not arithmetic but reading the source, which lives in mapped GPU memory
 * and is far slower than ordinary RAM; several threads keep more cache misses
 * outstanding at once. Falls back to the calling thread for small areas,
 * where the synchronisation would cost more than it saves. */
void SetConversionThreads(unsigned threads);
unsigned ConversionThreads();

/* Compares the two paths over pseudo-random pixels. Returns the largest
 * absolute difference found, and 0 when they agree exactly. */
int ConvertSelfTest(int width, int iterations);

/* Fills a full-frame XRGB8888 buffer with SMPTE-ish colour bars. */
void FillColourBars(uint8_t* dst, size_t stride, int width, int height);
void FillSolid(uint8_t* dst, size_t stride, int width, int height,
               uint8_t r, uint8_t g, uint8_t b);

}  // namespace ms912x
