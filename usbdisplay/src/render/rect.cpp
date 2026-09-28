/* SPDX-License-Identifier: GPL-2.0-only */

#include "rect.h"

#include <climits>

namespace usbdisplay {
namespace {

/* The chip wants the horizontal extent on a multiple of four pixels and the
 * vertical extent on a multiple of two. UYVY pixel pairs alone would only
 * demand two horizontally, but the vendor driver masks with 0xFFC and getting
 * this wrong shows up as torn or shifted blocks. */
inline int AlignDown4(int value) { return value & ~3; }
inline int AlignUp4(int value) { return (value + 3) & ~3; }
inline int AlignDown2(int value) { return value & ~1; }
inline int AlignUp2(int value) { return (value + 1) & ~1; }

}  // namespace

Rect EmptyRect() {
  Rect rect;
  rect.x1 = INT_MAX;
  rect.y1 = INT_MAX;
  rect.x2 = 0;
  rect.y2 = 0;
  return rect;
}

Rect MergeRects(const Rect& a, const Rect& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  Rect out;
  out.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
  out.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
  out.x2 = a.x2 > b.x2 ? a.x2 : b.x2;
  out.y2 = a.y2 > b.y2 ? a.y2 : b.y2;
  return out;
}

bool RectsIntersect(const Rect& a, const Rect& b) {
  if (a.empty() || b.empty()) {
    return false;
  }
  return a.x1 < b.x2 && b.x1 < a.x2 && a.y1 < b.y2 && b.y1 < a.y2;
}

Rect AlignDamageRect(const Rect& rect, int fb_width, int fb_height) {
  Rect out = rect;
  if (out.x1 < 0) {
    out.x1 = 0;
  }
  if (out.y1 < 0) {
    out.y1 = 0;
  }
  if (out.x2 > fb_width) {
    out.x2 = fb_width;
  }
  if (out.y2 > fb_height) {
    out.y2 = fb_height;
  }
  if (out.empty()) {
    out.x1 = out.y1 = out.x2 = out.y2 = 0;
    return out;
  }
  out.x1 = AlignDown4(out.x1);
  out.x2 = AlignUp4(out.x2);
  if (out.x2 > fb_width) {
    out.x2 = AlignDown4(fb_width);
  }
  out.y1 = AlignDown2(out.y1);
  out.y2 = AlignUp2(out.y2);
  if (out.y2 > fb_height) {
    out.y2 = AlignDown2(fb_height);
  }
  if (out.x2 <= out.x1 || out.y2 <= out.y1) {
    out.x1 = out.y1 = out.x2 = out.y2 = 0;
  }
  return out;
}

int TransferCostModel::TransferCost(const Rect* regions, size_t count) const {
  int total = 0;
  for (size_t i = 0; i < count; ++i) {
    total += TransferCost(regions[i]);
  }
  return total;
}

namespace {

class QuantisedCostModel : public TransferCostModel {
 public:
  int TransferCost(const Rect& region) const override {
    if (region.empty()) {
      return 0;
    }
    const size_t bytes = TransferLength(region);
    const size_t periods = (bytes + kBytesPerPeriod - 1) / kBytesPerPeriod;
    return periods < 1 ? 1 : static_cast<int>(periods);
  }
};

}  // namespace

const TransferCostModel& DefaultCostModel() {
  static const QuantisedCostModel model;
  return model;
}

}  // namespace usbdisplay
