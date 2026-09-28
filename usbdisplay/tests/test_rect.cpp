/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The cost model and rectangle arithmetic.
 *
 * These numbers are not arbitrary and not a matter of taste. They come from
 * timing transfers of increasing size on an MS912C, which showed that the
 * adapter completes a transfer on its own 60 Hz boundary: every measurement
 * was a multiple of 16.7 ms. Everything the driver does about damage follows
 * from that, so if these tests start failing, the reasoning behind the
 * planner has been broken rather than merely a function.
 */

#include "../src/render/rect.h"
#include "testing.h"

using namespace usbdisplay;

namespace {

Rect Make(int x, int y, int w, int h) {
  Rect rect;
  rect.x1 = x;
  rect.y1 = y;
  rect.x2 = x + w;
  rect.y2 = y + h;
  return rect;
}

}  // namespace

TEST(rect, empty_rect_has_no_cost) {
  Rect nothing;
  CHECK(nothing.empty());
  CHECK_EQ(DefaultCostModel().TransferCost(nothing), 0);
  CHECK_EQ(nothing.area(), static_cast<int64_t>(0));
}

TEST(rect, transfer_length_is_two_bytes_a_pixel_plus_framing) {
  const Rect rect = Make(0, 0, 100, 50);
  CHECK_EQ_BECAUSE(TransferLength(rect),
                   static_cast<size_t>(100 * 50 * 2 + 16),
                   "UYVY is 16 bits a pixel and the framing is an 8 byte "
                   "header plus an 8 byte footer, with no padding between "
                   "the header and the pixels");
}

TEST(rect, a_tiny_update_still_costs_a_whole_period) {
  CHECK_EQ_BECAUSE(DefaultCostModel().TransferCost(Make(0, 0, 4, 2)), 1,
                   "the adapter finishes on its own refresh boundary, so "
                   "there is no such thing as a cheaper than one period "
                   "transfer and splitting an update in two doubles its cost");
}

TEST(rect, size_is_free_below_the_threshold) {
  /* Measured: 1920x128 is 491 KB and took 16.7 ms, one period, the same as
   * a few kilobytes. */
  const int small = DefaultCostModel().TransferCost(Make(0, 0, 64, 32));
  const int large = DefaultCostModel().TransferCost(Make(0, 0, 1920, 128));
  CHECK_EQ_BECAUSE(small, large,
                   "below roughly 520 KB an update costs one period whatever "
                   "its size, which is why coalescing nearby damage is free");
}

TEST(rect, cost_jumps_a_whole_period_past_the_threshold) {
  /* Measured: 1920x136 is 522 KB and took 17.9 ms; 1920x144 is 553 KB and
   * took 33.2 ms, a whole extra period for six percent more data. */
  CHECK_EQ(DefaultCostModel().TransferCost(Make(0, 0, 1920, 128)), 1);
  CHECK_BECAUSE(DefaultCostModel().TransferCost(Make(0, 0, 1920, 160)) >= 2,
                "just past the threshold the next byte costs an entire "
                "extra period");
}

TEST(rect, a_full_1080p_frame_costs_eight_periods) {
  CHECK_EQ_BECAUSE(DefaultCostModel().TransferCost(Make(0, 0, 1920, 1080)), 8,
                   "a full repaint takes 133 ms, which is 7.5 a second; this "
                   "is the number the driver exists to avoid paying");
}

TEST(rect, merge_takes_the_bounding_box) {
  const Rect merged = MergeRects(Make(10, 10, 10, 10), Make(100, 200, 5, 5));
  CHECK_EQ(merged.x1, 10);
  CHECK_EQ(merged.y1, 10);
  CHECK_EQ(merged.x2, 105);
  CHECK_EQ(merged.y2, 205);
}

TEST(rect, merging_with_an_empty_rect_changes_nothing) {
  const Rect real = Make(10, 10, 20, 20);
  Rect nothing;
  CHECK(MergeRects(real, nothing) == real);
  CHECK(MergeRects(nothing, real) == real);
}

TEST(rect, intersection_is_exclusive_at_the_edges) {
  CHECK(RectsIntersect(Make(0, 0, 10, 10), Make(5, 5, 10, 10)));
  CHECK_BECAUSE(!RectsIntersect(Make(0, 0, 10, 10), Make(10, 0, 10, 10)),
                "rectangles that merely touch do not overlap, so there is no "
                "duplicated pixel to avoid");
}

TEST(alignment, odd_left_edge_is_pulled_back_to_a_pixel_pair) {
  const Rect aligned = AlignDamageRect(Make(101, 51, 7, 3), 1920, 1080);
  CHECK_EQ_BECAUSE(aligned.x1 % 4, 0,
                   "an odd left edge splits a UYVY pixel pair and shows as "
                   "colour fringing down the left of the update");
  CHECK_EQ(aligned.width() % 4, 0);
  CHECK_EQ(aligned.y1 % 2, 0);
  CHECK_EQ(aligned.height() % 2, 0);
  CHECK_BECAUSE(aligned.x1 <= 101 && aligned.x2 >= 108,
                "alignment must grow the region, never crop it, or the "
                "uncovered pixels are simply never sent");
  CHECK(aligned.y1 <= 51 && aligned.y2 >= 54);
}

TEST(alignment, an_already_aligned_rect_is_left_alone) {
  const Rect rect = Make(100, 50, 8, 4);
  CHECK(AlignDamageRect(rect, 1920, 1080) == rect);
}

TEST(alignment, clipped_to_the_screen) {
  const Rect aligned = AlignDamageRect(Make(1900, 1070, 200, 200), 1920, 1080);
  CHECK(aligned.x2 <= 1920);
  CHECK(aligned.y2 <= 1080);
  CHECK(!aligned.empty());
}

TEST(alignment, a_rect_entirely_off_screen_becomes_empty) {
  CHECK(AlignDamageRect(Make(3000, 3000, 10, 10), 1920, 1080).empty());
  CHECK(AlignDamageRect(Make(-100, -100, 50, 50), 1920, 1080).empty());
}
