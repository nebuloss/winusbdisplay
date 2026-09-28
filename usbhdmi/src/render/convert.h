/* SPDX-License-Identifier: GPL-2.0-only
 *
 * XRGB8888 to UYVY 4:2:2, and the framing that wraps it for the wire.
 *
 * There are two conversion paths in this driver, one here on the CPU and one
 * on the GPU in gpu_convert.h, and which one is used depends on how big the
 * update is. That split is deliberate and measured: a compute shader has a
 * fixed dispatch and readback cost that a small update cannot amortise, while
 * a large update is dominated by dragging the source across the bus, which is
 * exactly what the GPU path avoids.
 *
 * Because both paths run, they must agree **exactly**. Not approximately: a
 * region redrawn at slightly different sizes would otherwise flip between the
 * two paths and between two different byte values, and on small text that is
 * visible as a shimmer. Every path here, scalar, SIMD and shader, uses the
 * same 15 bit fixed point constants for that reason, and `SelfTest` proves
 * it. Do not introduce a third path without doing the same.
 */

#pragma once

#include <stdint.h>

#include <stddef.h>

#include "../core/proto.h"
#include "rect.h"

namespace usbhdmi {

/* BT.601 limited range in 15 bit fixed point.
 *
 * The published reference uses 16 bit constants. They cannot be used here:
 * _mm_madd_epi16 needs coefficients that fit a signed 16 bit lane and 32904
 * does not, so the SIMD path must halve them and shift by 15. Leaving the
 * scalar path on the 16 bit constants makes the two disagree by one least
 * significant bit, and since SIMD handles whole groups of eight pixels while
 * scalar handles the remainder, which pixels get which arithmetic depends on
 * the width of the rectangle. Everything uses 15 bit. */
constexpr int kCoeffYr = 8382, kCoeffYg = 16452, kCoeffYb = 3196;
constexpr int kCoeffUr = -4838, kCoeffUg = -9498, kCoeffUb = 14336;
constexpr int kCoeffVr = 14336, kCoeffVg = -12005, kCoeffVb = -2331;

/* Both chroma rows must sum to exactly zero, or a grey pixel acquires a
 * colour cast and the whole picture is faintly tinted. Halving the published
 * 16 bit constants and rounding each independently left the V row summing to
 * -1, which is where the last of those digits comes from: it is rounded the
 * other way on purpose. */
static_assert(kCoeffUr + kCoeffUg + kCoeffUb == 0, "U must be neutral on grey");
static_assert(kCoeffVr + kCoeffVg + kCoeffVb == 0, "V must be neutral on grey");

/* Updates at least this many pixels convert on the GPU, the rest on the
 * processor. Roughly a quarter of a 1080p screen.
 *
 * A compute shader has a fixed dispatch and readback cost that a small update
 * cannot repay, while a large one is dominated by dragging the source across
 * the bus, which is what the GPU path avoids. This is a starting point rather
 * than a law: on a discrete card the crossover should move down, since the
 * processor path has to drag four bytes per pixel back across the bus where
 * the GPU path drags two. It is overridable at runtime for that reason. */
constexpr int64_t kGpuThresholdPixels = 1920 * 1080 / 4;

/* Picture adjustment applied while converting, expressed the way a monitor
 * reports it: brightness and contrast each 0..100, with 100 and 50 meaning
 * "leave the image alone".
 *
 * The dongle has no hardware brightness control of any kind, so this is the
 * only way a brightness slider can produce a visible change. Luma is scaled
 * about its black point and chroma about neutral, so dimming does not tint
 * the picture. */
struct PictureAdjust {
  int brightness = 100;
  int contrast = 50;

  bool IsIdentity() const { return brightness == 100 && contrast == 50; }
  bool operator==(const PictureAdjust& other) const {
    return brightness == other.brightness && contrast == other.contrast;
  }
};

/* Writes header, converted pixels and footer into `dst`, which must hold at
 * least TransferLength(rect) bytes. `src` points at the top left of the whole
 * image in XRGB8888 with `stride` bytes per row, which is not necessarily
 * width * 4. Returns bytes written, or 0 if the arguments do not agree.
 *
 * `rect` must already have been through AlignDamageRect. */
size_t FrameRect(uint8_t* dst, size_t dst_capacity, const uint8_t* src,
                 size_t stride, int image_width, int image_height,
                 const Rect& rect,
                 const PictureAdjust& adjust = PictureAdjust());

/* Writes just the header and footer around pixel data that something else,
 * in practice the GPU path, has already produced. */
size_t FrameExisting(uint8_t* dst, size_t dst_capacity, const Rect& rect);

/* Converts a region into packed UYVY with no framing: `rect.height()` rows of
 * `rect.width() * 2` bytes, one after another. This is what the pipeline
 * works in, because a region has to be converted before it can be compared
 * against what is already on screen, and only the part that differs is then
 * wrapped up and sent. */
void ConvertRegion(uint8_t* dst, const uint8_t* src, size_t stride,
                   const Rect& rect, const PictureAdjust& adjust);

/* Copies a sub-region out of a buffer produced by ConvertRegion and frames
 * it for the wire. `sub` must lie inside `region`. Returns bytes written. */
size_t FrameSubRegion(uint8_t* dst, size_t dst_capacity,
                      const uint8_t* converted, const Rect& region,
                      const Rect& sub);

/* One row. Dispatches to SIMD; SSE2 is part of the x64 baseline so there is
 * no runtime check. */
void ConvertRow(uint8_t* dst, const uint8_t* src, int width);
void ConvertRowScalar(uint8_t* dst, const uint8_t* src, int width);
void ConvertRowSimd(uint8_t* dst, const uint8_t* src, int width);

void ApplyPictureAdjust(uint8_t* uyvy_row, int width,
                        const PictureAdjust& adjust);

/* Rows are converted across a small persistent thread pool. This helps more
 * than the arithmetic saving suggests, because most of the cost is reading
 * the source, which lives in mapped GPU memory and is far slower than
 * ordinary RAM; several threads keep more cache misses outstanding at once.
 * Small rectangles run on the calling thread, where synchronising would cost
 * more than it saves. */
void SetConversionThreads(unsigned threads);
unsigned ConversionThreads();

/* Compares the scalar and SIMD paths over pseudo-random pixels and returns
 * the largest absolute difference, which must be zero. This is the guard on
 * any change to the conversion kernels. */
int SelfTest(int width, int iterations);

/* Test images, for driving a panel without a compositor. */
void FillColourBars(uint8_t* dst, size_t stride, int width, int height);
void FillSolid(uint8_t* dst, size_t stride, int width, int height, uint8_t r,
               uint8_t g, uint8_t b);

}  // namespace usbhdmi
