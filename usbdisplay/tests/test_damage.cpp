/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The damage planner, which is where the performance of this driver is
 * actually decided.
 *
 * Every scenario here is a real thing a desktop does. The window drag case
 * in particular is the one the previous driver got wrong: it merged
 * everything into one bounding box, so dragging a window across the screen
 * cost a full repaint on every frame.
 */

#include "../src/render/damage.h"
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

struct Plan {
  Rect rects[kMaxTransfersPerFrame];
  size_t count = 0;

  int periods() const { return DefaultCostModel().TransferCost(rects, count); }
  size_t bytes() const {
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
      total += TransferLength(rects[i]);
    }
    return total;
  }
};

Plan PlanFor(const std::vector<Rect>& damage) {
  RectSet set;
  for (const Rect& rect : damage) {
    set.Add(rect, DefaultCostModel());
  }
  Plan plan;
  plan.count = PlanTransfers(set, plan.rects, kMaxTransfersPerFrame, DefaultCostModel());
  return plan;
}

bool PlanCovers(const Plan& plan, const Rect& rect) {
  for (size_t i = 0; i < plan.count; ++i) {
    const Rect& out = plan.rects[i];
    if (out.x1 <= rect.x1 && out.y1 <= rect.y1 && out.x2 >= rect.x2 &&
        out.y2 >= rect.y2) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(planner, nearby_regions_merge_because_one_transfer_beats_two) {
  const Plan plan =
      PlanFor({Make(100, 100, 200, 200), Make(300, 100, 200, 200)});
  CHECK_EQ_BECAUSE(plan.count, static_cast<size_t>(1),
                   "two adjacent regions both fit inside one period, so "
                   "sending them together halves the cost");
  CHECK_EQ(plan.periods(), 1);
}

TEST(planner, far_apart_regions_stay_split) {
  const Plan plan =
      PlanFor({Make(0, 0, 200, 200), Make(1700, 880, 200, 200)});
  CHECK_EQ_BECAUSE(plan.periods(), 2,
                   "their bounding box is the whole screen, which costs "
                   "eight periods; keeping them apart costs two");
  CHECK_EQ(plan.count, static_cast<size_t>(2));
}

TEST(planner, a_window_drag_does_not_become_a_full_repaint) {
  /* The compositor reports where the window was and where it went. */
  const Plan plan =
      PlanFor({Make(100, 100, 600, 400), Make(1200, 600, 600, 400)});
  CHECK_BECAUSE(plan.periods() <= 4,
                "merging a move's source and destination swells a 75 KB "
                "update into megabytes for the whole duration of a drag");
  CHECK_BECAUSE(plan.bytes() < static_cast<size_t>(1920) * 1080 * 2,
                "the plan must send less than a full screen, or there was "
                "no point tracking damage at all");
}

TEST(planner, overlapping_regions_always_merge) {
  const Plan plan =
      PlanFor({Make(100, 100, 400, 400), Make(300, 300, 400, 400)});
  CHECK_EQ_BECAUSE(plan.count, static_cast<size_t>(1),
                   "sending an overlapping pixel twice in one frame is pure "
                   "waste whatever the cost arithmetic says");
}

TEST(planner, every_input_region_is_covered_by_the_plan) {
  const std::vector<Rect> damage = {
      Make(0, 0, 100, 100), Make(900, 500, 50, 50), Make(1800, 1000, 40, 40),
      Make(400, 200, 120, 90)};
  const Plan plan = PlanFor(damage);
  for (const Rect& rect : damage) {
    CHECK_BECAUSE(PlanCovers(plan, rect),
                  "a region left out of the plan is a region that never "
                  "reaches the panel, which shows as stale content");
  }
}

TEST(planner, never_emits_more_transfers_than_allowed) {
  std::vector<Rect> damage;
  for (int i = 0; i < 40; ++i) {
    damage.push_back(Make((i * 97) % 1800, (i * 53) % 1000, 30, 30));
  }
  const Plan plan = PlanFor(damage);
  CHECK_BECAUSE(plan.count <= kMaxTransfersPerFrame,
                "each transfer costs at least a whole period, so past a "
                "handful, splitting makes updates slower not faster");
}

TEST(planner, a_full_screen_update_is_a_single_transfer) {
  const Plan plan = PlanFor({Make(0, 0, 1920, 1080)});
  CHECK_EQ(plan.count, static_cast<size_t>(1));
  CHECK_EQ(plan.periods(), 8);
}

TEST(planner, empty_damage_plans_nothing) {
  const Plan plan = PlanFor({});
  CHECK_EQ(plan.count, static_cast<size_t>(0));
}

TEST(rectset, bounded_regardless_of_how_much_is_added) {
  RectSet set;
  for (int i = 0; i < 500; ++i) {
    set.Add(Make((i * 37) % 1900, (i * 71) % 1060, 20, 20), DefaultCostModel());
  }
  CHECK_BECAUSE(set.size() <= kMaxTrackedRects,
                "tracking is bounded so a pathological frame cannot make "
                "the bookkeeping cost more than the bytes it saves");
}

TEST(rectset, ignores_empty_additions) {
  RectSet set;
  Rect nothing;
  set.Add(nothing, DefaultCostModel());
  CHECK(set.empty());
}

TEST(tracker, damage_is_consumed_by_planning) {
  DamageTracker tracker;
  tracker.Configure(1920, 1080, nullptr);

  Rect planned[kMaxTransfersPerFrame];
  tracker.Plan(planned, kMaxTransfersPerFrame);  /* the initial full repaint */

  tracker.Add(Make(100, 100, 200, 200));
  CHECK(tracker.Plan(planned, kMaxTransfersPerFrame) > 0);

  CHECK_EQ_BECAUSE(
      tracker.Plan(planned, kMaxTransfersPerFrame), static_cast<size_t>(0),
      "a region is planned once and then sent twice by the pipeline, once "
      "for each of the adapter's two internal copies of the picture; "
      "planning it again as well would send it four times and the "
      "connection would never go quiet");
  CHECK(tracker.Empty());
}

TEST(tracker, configure_marks_the_whole_screen) {
  DamageTracker tracker;
  tracker.Configure(1280, 720, nullptr);

  Rect planned[kMaxTransfersPerFrame];
  const size_t count = tracker.Plan(planned, kMaxTransfersPerFrame);
  CHECK_EQ(count, static_cast<size_t>(1));
  CHECK_EQ_BECAUSE(planned[0].width(), 1280,
                   "after a mode change nothing on the panel can be trusted, "
                   "so the first update has to be everything");
  CHECK_EQ(planned[0].height(), 720);
}

TEST(tracker, plans_are_aligned_and_clipped) {
  DamageTracker tracker;
  tracker.Configure(1920, 1080, nullptr);
  Rect planned[kMaxTransfersPerFrame];
  tracker.Plan(planned, kMaxTransfersPerFrame);

  tracker.Add(Make(101, 51, 7, 3));
  const size_t count = tracker.Plan(planned, kMaxTransfersPerFrame);
  CHECK_EQ(count, static_cast<size_t>(1));
  CHECK_EQ(planned[0].x1 % 4, 0);
  CHECK_EQ(planned[0].width() % 4, 0);
  CHECK(planned[0].x2 <= 1920 && planned[0].y2 <= 1080);
}

TEST(refinement, an_unchanged_region_shrinks_to_nothing) {
  const int width = 640, height = 64;
  const size_t stride = static_cast<size_t>(width) * 2;
  std::vector<uint8_t> screen(stride * height, 0x80);
  std::vector<uint8_t> converted(stride * height, 0x80);

  Rect whole = Make(0, 0, width, height);
  CHECK_BECAUSE(
      ShrinkChangedUyvy(whole, converted.data(), screen.data(), stride)
          .empty(),
      "when nothing differs there is nothing to send, and sending it anyway "
      "wastes a period the next real update needs");
}

TEST(refinement, finds_the_changed_island_in_a_whole_screen_claim) {
  const int width = 1920, height = 1080;
  const size_t stride = static_cast<size_t>(width) * 2;
  std::vector<uint8_t> screen(stride * height, 0);
  std::vector<uint8_t> converted = screen;

  for (int y = 500; y < 520; ++y) {
    for (int x = 800; x < 840; ++x) {
      converted[y * stride + static_cast<size_t>(x) * 2] = 0xFF;
    }
  }

  const Rect whole = Make(0, 0, width, height);
  const Rect actual =
      ShrinkChangedUyvy(whole, converted.data(), screen.data(), stride);

  CHECK_EQ(actual.y1, 500);
  CHECK_EQ(actual.y2, 520);
  CHECK_EQ(actual.x1, 800);
  CHECK_EQ(actual.x2, 840);
  CHECK_EQ_BECAUSE(DefaultCostModel().TransferCost(actual), 1,
                   "the compositor sometimes calls the whole screen dirty "
                   "when almost nothing changed; believing it costs eight "
                   "periods and shows as a sweep down the panel");
}

TEST(refinement, result_lands_on_a_pixel_pair) {
  const int width = 64, height = 8;
  const size_t stride = static_cast<size_t>(width) * 2;
  std::vector<uint8_t> screen(stride * height, 0);
  std::vector<uint8_t> converted = screen;

  /* Change a single pixel at an odd column. */
  converted[3 * stride + 11 * 2] = 0xFF;

  const Rect actual = ShrinkChangedUyvy(Make(0, 0, width, height),
                                        converted.data(), screen.data(),
                                        stride);
  CHECK_EQ_BECAUSE(actual.x1 % 2, 0,
                   "UYVY encodes pixels in pairs, so a region cannot start "
                   "half way through one");
  CHECK_EQ(actual.width() % 2, 0);
  CHECK(actual.x1 <= 11 && actual.x2 >= 12);
}

TEST(refinement, storing_then_comparing_reports_no_change) {
  const int width = 128, height = 16;
  const size_t stride = static_cast<size_t>(width) * 2;
  std::vector<uint8_t> screen(stride * height, 0);

  const Rect region = Make(16, 4, 32, 8);
  std::vector<uint8_t> converted(
      static_cast<size_t>(region.width()) * 2 * region.height(), 0x5A);

  StoreUyvyReference(region, converted.data(), screen.data(), stride);
  CHECK_BECAUSE(
      ShrinkChangedUyvy(region, converted.data(), screen.data(), stride)
          .empty(),
      "what was just sent is what the panel is showing, so an identical "
      "region must compare equal or every frame resends everything");
}

/* The planner must work off whatever cost model the device supplies, not a
 * built-in assumption about one chip. These use a deliberately different
 * model from the real adapter's, so a regression that reintroduces the
 * hardcoded one shows up here rather than on somebody's screen. */
namespace {

/* A device that streams: cost proportional to area, nothing quantised. */
class ProportionalCost : public TransferCostModel {
 public:
  int TransferCost(const Rect& region) const override {
    return region.empty() ? 0
                          : 1 + static_cast<int>(region.area() / 100000);
  }
};

/* A device where every transfer costs the same regardless of size. */
class FlatCost : public TransferCostModel {
 public:
  int TransferCost(const Rect& region) const override {
    return region.empty() ? 0 : 1;
  }
};

}  // namespace

TEST(costmodel, a_flat_model_merges_everything) {
  const FlatCost flat;
  RectSet set;
  set.Add(Make(0, 0, 100, 100), flat);
  set.Add(Make(1800, 980, 100, 100), flat);

  Rect planned[kMaxTransfersPerFrame];
  const size_t count = PlanTransfers(set, planned, kMaxTransfersPerFrame, flat);
  CHECK_EQ_BECAUSE(count, static_cast<size_t>(1),
                   "when every transfer costs the same, one covering both "
                   "regions is always better than two");
}

TEST(costmodel, a_proportional_model_keeps_distant_regions_apart) {
  const ProportionalCost proportional;
  RectSet set;
  set.Add(Make(0, 0, 100, 100), proportional);
  set.Add(Make(1800, 980, 100, 100), proportional);

  Rect planned[kMaxTransfersPerFrame];
  const size_t count =
      PlanTransfers(set, planned, kMaxTransfersPerFrame, proportional);
  CHECK_EQ_BECAUSE(count, static_cast<size_t>(2),
                   "when cost follows area, merging two corners into a full "
                   "screen bounding box is plainly worse");
}

TEST(costmodel, the_tracker_uses_the_model_it_was_given) {
  const FlatCost flat;
  DamageTracker tracker;
  tracker.Configure(1920, 1080, &flat);

  Rect planned[kMaxTransfersPerFrame];
  tracker.Plan(planned, kMaxTransfersPerFrame);  /* the opening repaint */

  tracker.Add(Make(0, 0, 100, 100));
  tracker.Add(Make(1800, 980, 100, 100));
  CHECK_EQ_BECAUSE(tracker.Plan(planned, kMaxTransfersPerFrame),
                   static_cast<size_t>(1),
                   "the tracker must plan with the supplied model rather "
                   "than falling back to a built-in one");
}
