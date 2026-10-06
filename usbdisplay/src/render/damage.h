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
 * The adapter's habit of keeping two copies of the picture and alternating
 * between them is handled elsewhere, by sending each region twice; see
 * Pipeline::SendRegion. It is worth knowing about here only because it means
 * every region planned costs two transfers, not one.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rect.h"

namespace usbdisplay {

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
  void Add(const Rect& rect, const TransferCostModel& cost);

  void AddAll(const RectSet& other, const TransferCostModel& cost);

 private:
  Rect rects_[kMaxTrackedRects];
  size_t count_ = 0;
};

/* Accumulates damage and turns it into transfers. */
class DamageTracker {
 public:
  void Configure(int width, int height, const TransferCostModel* cost);

  /* Marks the whole screen dirty. Used for the first frame after a modeset,
   * and for the idle repaint. */
  void MarkAll();

  void Add(const Rect& rect);

  /* Forgets what is owed, for a caller that has just satisfied it another
   * way. The idle repaint draws straight from the last image it was given
   * without consulting this, so once it has covered the whole screen the
   * accumulated damage describes work already done, and planning it later
   * would send a redundant full frame. */
  void Clear() { pending_.Clear(); }

  bool Empty() const { return pending_.empty(); }

  /* Fills `out` with the regions to send now, aligned to the adapter's pixel
   * pair requirements and clipped to the screen, and returns how many.
   * Consumes the accumulated damage.
   *
   * Each region still has to reach the adapter twice, once for each of its
   * two internal copies of the picture. That is the sender's business, not
   * this class's: see Pipeline::SendRegion. Doing it here instead, by
   * repeating the previous plan on the next call, works but delivers each
   * region a frame later for no saving. */
  size_t Plan(Rect* out, size_t capacity);

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  int width_ = 0;
  int height_ = 0;
  RectSet pending_;  /* changed since the last plan */
  const TransferCostModel* cost_ = nullptr;
};

/* Coalesces `in` into at most `capacity` rectangles, merging a pair whenever
 * the merged transfer costs no more than the two separate ones, and then
 * merging the least bad pairs until the count fits. Returns how many were
 * written to `out`.
 *
 * Exposed for the tool, which exercises it against synthetic damage patterns
 * so the cost reasoning can be checked without hardware.  */
size_t PlanTransfers(const RectSet& in, Rect* out, size_t capacity,
                     const TransferCostModel& cost);

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

}  // namespace usbdisplay
