/* SPDX-License-Identifier: GPL-2.0-only */

#include "damage.h"

#include <emmintrin.h>

#include <cstring>

namespace usbhdmi {
namespace {

/* Cost of sending `a` and `b` as one transfer rather than two. Negative is a
 * saving. Overlapping rectangles always merge: sending a pixel twice in one
 * frame is pure waste whatever the arithmetic says. */
int MergeDelta(const Rect& a, const Rect& b) {
  if (RectsIntersect(a, b)) {
    return -1000;
  }
  return TransferPeriods(MergeRects(a, b)) - TransferPeriods(a) -
         TransferPeriods(b);
}

}  // namespace

void RectSet::Add(const Rect& rect) {
  if (rect.empty()) {
    return;
  }

  /* Fold into an existing rectangle when that is free or better. */
  for (size_t i = 0; i < count_; ++i) {
    if (MergeDelta(rects_[i], rect) <= 0) {
      rects_[i] = MergeRects(rects_[i], rect);
      return;
    }
  }

  if (count_ < kMaxTrackedRects) {
    rects_[count_++] = rect;
    return;
  }

  /* Full, and nothing merged for free. Take the cheapest merge available,
   * including merging the newcomer into something. */
  size_t best_a = 0, best_b = 1;
  int best = MergeDelta(rects_[0], rects_[1]);
  for (size_t i = 0; i < count_; ++i) {
    for (size_t j = i + 1; j < count_; ++j) {
      const int delta = MergeDelta(rects_[i], rects_[j]);
      if (delta < best) {
        best = delta;
        best_a = i;
        best_b = j;
      }
    }
  }

  size_t victim = count_;  /* means "merge the newcomer instead" */
  for (size_t i = 0; i < count_; ++i) {
    const int delta = MergeDelta(rects_[i], rect);
    if (delta < best) {
      best = delta;
      victim = i;
    }
  }

  if (victim != count_) {
    rects_[victim] = MergeRects(rects_[victim], rect);
    return;
  }

  rects_[best_a] = MergeRects(rects_[best_a], rects_[best_b]);
  rects_[best_b] = rects_[count_ - 1];
  --count_;
  rects_[count_++] = rect;
}

void RectSet::AddAll(const RectSet& other) {
  for (size_t i = 0; i < other.count_; ++i) {
    Add(other.rects_[i]);
  }
}

int RectSet::Periods() const { return TransferPeriods(rects_, count_); }

size_t PlanTransfers(const RectSet& in, Rect* out, size_t capacity) {
  if (capacity == 0) {
    return 0;
  }

  Rect work[kMaxTrackedRects];
  size_t count = 0;
  for (size_t i = 0; i < in.size() && count < kMaxTrackedRects; ++i) {
    if (!in[i].empty()) {
      work[count++] = in[i];
    }
  }
  if (count == 0) {
    return 0;
  }

  for (;;) {
    if (count <= 1) {
      break;
    }

    size_t best_a = 0, best_b = 1;
    int best = MergeDelta(work[0], work[1]);
    for (size_t i = 0; i < count; ++i) {
      for (size_t j = i + 1; j < count; ++j) {
        const int delta = MergeDelta(work[i], work[j]);
        if (delta < best) {
          best = delta;
          best_a = i;
          best_b = j;
        }
      }
    }

    /* Merge while it is free or better. Once nothing is free, keep merging
     * only while there are more transfers than allowed, taking the least bad
     * pair each time. */
    const bool must_reduce = count > capacity;
    if (best > 0 && !must_reduce) {
      break;
    }

    work[best_a] = MergeRects(work[best_a], work[best_b]);
    work[best_b] = work[count - 1];
    --count;
  }

  if (count > capacity) {
    count = capacity;
  }
  memcpy(out, work, count * sizeof(Rect));
  return count;
}

void DamageTracker::Configure(int width, int height) {
  width_ = width;
  height_ = height;
  pending_.Clear();
  MarkAll();
}

void DamageTracker::MarkAll() {
  Rect all;
  all.x1 = 0;
  all.y1 = 0;
  all.x2 = width_;
  all.y2 = height_;
  if (all.empty()) {
    return;
  }
  /* Deliberately replaces rather than adds: nothing smaller matters once the
   * whole screen is owed. */
  pending_.Clear();
  pending_.Add(all);
}

void DamageTracker::Add(const Rect& rect) { pending_.Add(rect); }

size_t DamageTracker::Plan(Rect* out, size_t capacity) {
  if (capacity > kMaxTransfersPerFrame) {
    capacity = kMaxTransfersPerFrame;
  }
  if (pending_.empty()) {
    return 0;
  }

  Rect planned[kMaxTransfersPerFrame];
  const size_t count = PlanTransfers(pending_, planned, capacity);

  size_t emitted = 0;
  for (size_t i = 0; i < count; ++i) {
    const Rect aligned = AlignDamageRect(planned[i], width_, height_);
    if (!aligned.empty()) {
      out[emitted++] = aligned;
    }
  }

  pending_.Clear();
  return emitted;
}

namespace {

/* True if any byte in the range differs. UYVY is compared in whole four byte
 * groups, which is one pixel pair and also the granularity the chip wants. */
bool BytesDiffer(const uint8_t* a, const uint8_t* b, size_t bytes) {
  size_t i = 0;
  for (; i + 16 <= bytes; i += 16) {
    const __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
    const __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(va, vb)) != 0xFFFF) {
      return true;
    }
  }
  return i < bytes && memcmp(a + i, b + i, bytes - i) != 0;
}

}  // namespace

Rect ShrinkChangedUyvy(const Rect& rect, const uint8_t* converted,
                       const uint8_t* reference, size_t reference_stride) {
  if (rect.empty()) {
    return rect;
  }

  const size_t row_bytes = static_cast<size_t>(rect.width()) * 2;
  const size_t x_offset = static_cast<size_t>(rect.x1) * 2;

  const auto row_of = [&](int y) {
    return converted + static_cast<size_t>(y - rect.y1) * row_bytes;
  };
  const auto reference_row = [&](int y) {
    return reference + static_cast<size_t>(y) * reference_stride + x_offset;
  };

  int top = rect.y2;
  for (int y = rect.y1; y < rect.y2; ++y) {
    if (BytesDiffer(row_of(y), reference_row(y), row_bytes)) {
      top = y;
      break;
    }
  }
  if (top == rect.y2) {
    Rect none;
    return none; /* the claimed region is exactly what is already on screen */
  }

  int bottom = top + 1;
  for (int y = rect.y2 - 1; y >= bottom; --y) {
    if (BytesDiffer(row_of(y), reference_row(y), row_bytes)) {
      bottom = y + 1;
      break;
    }
  }

  /* Now the columns, over the rows known to contain a change. Walking a
   * column touches four bytes per row and is cache hostile, so this comes in
   * from each edge and stops at the first difference rather than scanning
   * every column. */
  const int pairs = rect.width() / 2;
  int first_pair = 0;
  for (; first_pair < pairs; ++first_pair) {
    bool differs = false;
    for (int y = top; y < bottom && !differs; ++y) {
      const size_t at = static_cast<size_t>(first_pair) * 4;
      differs = memcmp(row_of(y) + at, reference_row(y) + at, 4) != 0;
    }
    if (differs) {
      break;
    }
  }

  int last_pair = pairs;
  for (; last_pair > first_pair; --last_pair) {
    bool differs = false;
    for (int y = top; y < bottom && !differs; ++y) {
      const size_t at = static_cast<size_t>(last_pair - 1) * 4;
      differs = memcmp(row_of(y) + at, reference_row(y) + at, 4) != 0;
    }
    if (differs) {
      break;
    }
  }

  Rect out;
  out.x1 = rect.x1 + first_pair * 2;
  out.x2 = rect.x1 + last_pair * 2;
  out.y1 = top;
  out.y2 = bottom;
  return out;
}

void StoreUyvyReference(const Rect& rect, const uint8_t* converted,
                        uint8_t* reference, size_t reference_stride) {
  if (rect.empty()) {
    return;
  }
  const size_t row_bytes = static_cast<size_t>(rect.width()) * 2;
  for (int y = rect.y1; y < rect.y2; ++y) {
    memcpy(reference + static_cast<size_t>(y) * reference_stride +
               static_cast<size_t>(rect.x1) * 2,
           converted + static_cast<size_t>(y - rect.y1) * row_bytes,
           row_bytes);
  }
}

}  // namespace usbhdmi
