/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Rectangles, and the cost model that decides what to do with them.
 *
 * The cost model is the reason this file exists separately from the damage
 * tracker. The chip does not stream at a byte rate: it finishes a bulk
 * transfer on its own 60 Hz boundary, so the price of an update is the number
 * of 16.67 ms periods it spans, not its size. Measured on an MS912C:
 *
 *   491 KB -> 16.7 ms   1 period
 *   552 KB -> 33.2 ms   2 periods
 *   4.1 MB -> 133  ms   8 periods
 *
 * Two consequences drive the whole design of the pipeline.
 *
 * Below roughly 520 KB, **size is free**. A 2 KB update and a 490 KB update
 * cost exactly the same. So there is no point shaving bytes off a small
 * update, and coalescing several small rectangles into one is free as long as
 * the result stays under the threshold.
 *
 * But **every transfer costs at least one period**, and a merge that crosses
 * the threshold is expensive. Two 50 KB rectangles at opposite corners of the
 * screen cost two periods sent separately and eight merged, because their
 * bounding box is the whole screen. The previous implementation always merged
 * into a single bounding box and paid that eight-period price routinely, for
 * instance whenever a window was dragged.
 */

#pragma once

#include <stdint.h>

#include <stddef.h>

#include "../core/proto.h"

namespace usbdisplay {

struct Rect {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;

  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
  bool empty() const { return x2 <= x1 || y2 <= y1; }
  int64_t area() const {
    return empty() ? 0
                   : static_cast<int64_t>(width()) * static_cast<int64_t>(height());
  }

  bool operator==(const Rect& other) const {
    return x1 == other.x1 && y1 == other.y1 && x2 == other.x2 &&
           y2 == other.y2;
  }
};

Rect EmptyRect();
Rect MergeRects(const Rect& a, const Rect& b);
bool RectsIntersect(const Rect& a, const Rect& b);

/* UYVY encodes pixels in pairs, and the vendor driver masks the extent with
 * 0xFFC, so the horizontal edges go to a multiple of four and the vertical
 * ones to a multiple of two. Getting this wrong shows up as colour fringing
 * on the left edge of an update, or as torn blocks. Always run a rectangle
 * through this before framing it. */
Rect AlignDamageRect(const Rect& rect, int fb_width, int fb_height);

/* Bytes a framed transfer for `rect` occupies on the wire. */
inline size_t TransferLength(const Rect& rect) {
  return static_cast<size_t>(rect.width()) * 2 * rect.height() + kFrameOverhead;
}

/* What sending a region costs, as far as the planner is concerned.
 *
 * The planner merges two regions when the merged cost does not exceed the
 * separate costs. It never needs to know what the unit is, only that the
 * numbers are comparable, which is exactly why this is an interface: the
 * unit is a property of the hardware. On the adapter this driver was written
 * for, cost is quantised into refresh periods rather than proportional to
 * size, and building that assumption into the planner would have tied it to
 * one chip for no reason.
 *
 * Implementations must return zero for an empty region, non-zero otherwise,
 * and must never return less for a region that contains another. */
class TransferCostModel {
 public:
  virtual ~TransferCostModel() = default;
  virtual int TransferCost(const Rect& region) const = 0;

  /* Cost of sending each region as its own transfer. */
  int TransferCost(const Rect* regions, size_t count) const;
};

/* The cost model used when there is no device to ask: one unit per region,
 * plus one per whole 512 KB. Good enough for the tests and for reasoning
 * about the planner on its own. */
const TransferCostModel& DefaultCostModel();

}  // namespace usbdisplay
