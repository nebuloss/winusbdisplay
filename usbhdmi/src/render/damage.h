/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Deciding what to put on the wire.
 *
 * This is where most of the performance of the driver is decided, and it is
 * worth being explicit about why, because the obvious approach is wrong.
 *
 * The obvious approach is to merge everything the compositor reports into one
 * bounding rectangle and send that. It is simple, it is what the reference
 * drivers do, and on this chip it is expensive. Cost is quantised into 16.67
 * ms periods (see rect.h), so two 50 KB updates in opposite corners cost two
 * periods sent separately and **eight** merged, because their bounding box is
 * the whole screen. Dragging a window produces exactly that shape, which is
 * why the old driver felt slow whenever anything moved.
 *
 * So this file keeps damage as a small set of rectangles and coalesces by
 * cost rather than by geometry: merge two rectangles only when the merged
 * transfer is not more expensive than the two separate ones. The planner then
 * emits up to a handful of transfers per frame.
 *
 * The second thing going on here is the chip's double buffering. The chip
 * alternates between two internal images on every transfer, so a region
 * carried by only one transfer lands in one image and leaves the other
 * holding older content, and the two alternate visibly on screen. Every
 * transfer therefore carries this frame's damage *and* the previous
 * transfer's.
 *
 * Tracking which image is next and writing them one at a time is the
 * efficient answer and it does not work. It was implemented twice. It is
 * correct only while the driver's idea of the next image stays in step with
 * the chip's, nothing enforces that, and once they drift every update is
 * written to the image that is not on screen. Sending the union needs no
 * agreement with the chip at all.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rect.h"

namespace usbhdmi {

/* How many disjoint regions are tracked before the cheapest pair is merged.
 * Desktop damage is usually one or two regions; more than this and the
 * bookkeeping costs more than the bytes it saves. */
constexpr size_t kMaxTrackedRects = 8;

/* Transfers emitted for one frame. Each costs at least a whole period, so
 * splitting past this point makes updates slower, not faster. */
constexpr size_t kMaxTransfersPerFrame = 4;

/* An unordered, bounded set of dirty rectangles that merges by cost when it
 * runs out of room. */
class RectSet {
 public:
  void Clear() { count_ = 0; }
  bool empty() const { return count_ == 0; }
  size_t size() const { return count_; }
  const Rect& operator[](size_t index) const { return rects_[index]; }

  /* Adds a rectangle, merging it into an existing one when they overlap or
   * when merging is free. Never grows past kMaxTrackedRects. */
  void Add(const Rect& rect);

  void AddAll(const RectSet& other);

  /* Total cost in chip periods if every rectangle were sent separately. */
  int Periods() const;

 private:
  Rect rects_[kMaxTrackedRects];
  size_t count_ = 0;
};

/* Accumulates damage and turns it into transfers. */
class DamageTracker {
 public:
  void Configure(int width, int height);

  /* Marks the whole screen dirty. Used for the first frame after a modeset,
   * and for the idle repaint. */
  void MarkAll();

  void Add(const Rect& rect);

  bool Empty() const { return pending_.empty() && previous_.empty(); }

  /* Fills `out` with the transfers to send now, aligned to the chip's pixel
   * pair requirements and clipped to the screen, and returns how many.
   *
   * Consumes the accumulated damage: what was planned becomes the "previous
   * transfer" that the next call will repeat for the chip's other image. */
  size_t Plan(Rect* out, size_t capacity);

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  int width_ = 0;
  int height_ = 0;
  RectSet pending_;   /* changed since the last transfer */
  RectSet previous_;  /* what the last transfer carried */
};

/* Coalesces `in` into at most `capacity` rectangles, merging a pair whenever
 * the merged transfer costs no more than the two separate ones, and then
 * merging the least bad pairs until the count fits. Returns how many were
 * written to `out`.
 *
 * Exposed for the tool, which exercises it against synthetic damage patterns
 * so the cost reasoning can be checked without hardware.  */
size_t PlanTransfers(const RectSet& in, Rect* out, size_t capacity);

/* Shrinks `rect` to the part that actually differs from the last thing sent,
 * or returns an empty rectangle if nothing does.
 *
 * The compositor sometimes reports a single dirty rectangle covering the
 * whole screen when very little has changed. Believing it costs eight periods
 * and is visible as a sweep down the panel, so it is worth checking.
 *
 * The comparison is deliberately made on **converted** pixels rather than on
 * the source image, which sounds backwards and is not. What this saves is
 * transfer time, and a transfer costs twenty to fifty times what a conversion
 * does, so converting the claimed region first and then discovering most of
 * it was unnecessary is still a large win. Doing it this way also means it
 * works identically whichever conversion path ran, which the obvious
 * arrangement does not: comparing source pixels would need a copy of the
 * desktop that the GPU path never reads and therefore could never keep
 * current.
 *
 * `converted` holds rect.height() packed rows of rect.width() * 2 bytes.
 * `reference` is the whole screen in UYVY with `reference_stride` bytes per
 * row. Results are on a two pixel boundary, which is the granularity UYVY
 * has anyway. */
Rect ShrinkChangedUyvy(const Rect& rect, const uint8_t* converted,
                       const uint8_t* reference, size_t reference_stride);

/* Copies `rect` of `converted` into the whole-screen `reference` buffer, so
 * the next comparison has something current to work against. */
void StoreUyvyReference(const Rect& rect, const uint8_t* converted,
                        uint8_t* reference, size_t reference_stride);

}  // namespace usbhdmi
