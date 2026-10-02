/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Drawing the pointer, which is arithmetic and therefore testable without
 * a machine that has one.
 */

#include <string.h>

#include <vector>

#include "../src/render/convert.h"
#include "../src/render/overlay.h"
#include "testing.h"

namespace usbdisplay {
namespace {

/* A region of flat colour, converted the way the frame path converts it,
 * so a test starts from exactly what the pointer would be drawn onto. */
std::vector<uint8_t> Background(const Rect& region, uint8_t r, uint8_t g,
                                uint8_t b) {
  const int width = region.width();
  const int height = region.height();
  std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
  for (size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = b;
    bgra[i + 1] = g;
    bgra[i + 2] = r;
    bgra[i + 3] = 255;
  }

  std::vector<uint8_t> uyvy(static_cast<size_t>(width) * height * 2);
  for (int y = 0; y < height; ++y) {
    ConvertRow(uyvy.data() + static_cast<size_t>(y) * width * 2,
               bgra.data() + static_cast<size_t>(y) * width * 4, width);
  }
  return uyvy;
}

/* A square pointer of one colour and one coverage, which is enough to
 * check placement and blending without depending on any real shape. */
CursorImage Square(int side, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  std::vector<uint8_t> bgra(static_cast<size_t>(side) * side * 4);
  for (size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = b;
    bgra[i + 1] = g;
    bgra[i + 2] = r;
    bgra[i + 3] = a;
  }
  CursorImage image;
  image.SetBgra(bgra.data(), static_cast<size_t>(side) * 4, side, side, false);
  return image;
}

uint8_t LumaAt(const std::vector<uint8_t>& uyvy, const Rect& region, int x,
               int y) {
  const size_t row = static_cast<size_t>(y - region.y1) * region.width() * 2;
  const int in_region = x - region.x1;
  return uyvy[row + static_cast<size_t>(in_region / 2) * 4 + 1 +
              (in_region & 1) * 2];
}

}  // namespace

TEST(cursor, an_opaque_pointer_replaces_what_is_under_it) {
  const Rect region = {0, 0, 64, 64};
  std::vector<uint8_t> screen = Background(region, 0, 0, 0);
  const uint8_t before = LumaAt(screen, region, 10, 10);

  Square(8, 255, 255, 255, 255).Blend(screen.data(), region, 8, 8);

  CHECK_BECAUSE(LumaAt(screen, region, 10, 10) > before,
                "a white pointer on a black desktop has to be brighter than "
                "the desktop, or it is not being drawn at all");
}

TEST(cursor, a_transparent_pointer_changes_nothing) {
  const Rect region = {0, 0, 64, 64};
  const std::vector<uint8_t> original = Background(region, 90, 90, 90);
  std::vector<uint8_t> screen = original;

  Square(8, 255, 255, 255, 0).Blend(screen.data(), region, 8, 8);

  CHECK_BECAUSE(screen == original,
                "fully transparent pixels must leave the desktop untouched, "
                "or the pointer leaves a visible square around itself");
}

TEST(cursor, nothing_outside_the_pointer_is_touched) {
  const Rect region = {0, 0, 64, 64};
  std::vector<uint8_t> screen = Background(region, 0, 0, 0);
  const uint8_t outside = LumaAt(screen, region, 40, 40);

  Square(8, 255, 255, 255, 255).Blend(screen.data(), region, 8, 8);

  CHECK_BECAUSE(LumaAt(screen, region, 40, 40) == outside,
                "a pointer that writes beyond its own bounds would smear "
                "across the screen as it moved");
  CHECK_EQ(LumaAt(screen, region, 7, 8), outside);
  CHECK_EQ(LumaAt(screen, region, 16, 8), outside);
}

TEST(cursor, a_pointer_half_off_the_region_is_clipped_not_wrapped) {
  const Rect region = {0, 0, 64, 64};
  std::vector<uint8_t> screen = Background(region, 0, 0, 0);

  /* Straddling the right edge. The half that fits must be drawn and the
   * half that does not must not reappear on the opposite side, which is
   * what an unclipped row offset would do. */
  Square(8, 255, 255, 255, 255).Blend(screen.data(), region, 60, 30);

  CHECK_BECAUSE(LumaAt(screen, region, 62, 30) > LumaAt(screen, region, 2, 30),
                "the part inside the region is drawn and the part outside "
                "does not wrap round to the start of the row");
}

TEST(cursor, a_pointer_is_drawn_at_the_same_place_in_any_region) {
  /* The same pointer at the same screen position, once in a region that
   * starts at the origin and once in a region that does not. Both have to
   * put it on the same screen pixel, or a pointer would jump whenever the
   * damage around it happened to be shaped differently. */
  const Rect whole = {0, 0, 64, 64};
  std::vector<uint8_t> a = Background(whole, 0, 0, 0);
  Square(8, 255, 255, 255, 255).Blend(a.data(), whole, 20, 20);

  const Rect part = {16, 16, 48, 48};
  std::vector<uint8_t> b = Background(part, 0, 0, 0);
  Square(8, 255, 255, 255, 255).Blend(b.data(), part, 20, 20);

  CHECK_EQ(LumaAt(a, whole, 22, 22), LumaAt(b, part, 22, 22));
  CHECK_BECAUSE(LumaAt(b, part, 22, 22) > LumaAt(b, part, 40, 40),
                "the pointer belongs at screen coordinates, not at an "
                "offset into whichever region is being converted");
}

TEST(cursor, partial_coverage_lands_between_the_two) {
  const Rect region = {0, 0, 64, 64};
  std::vector<uint8_t> screen = Background(region, 0, 0, 0);
  const uint8_t dark = LumaAt(screen, region, 10, 10);

  std::vector<uint8_t> opaque = Background(region, 0, 0, 0);
  Square(8, 255, 255, 255, 255).Blend(opaque.data(), region, 8, 8);
  const uint8_t bright = LumaAt(opaque, region, 10, 10);

  Square(8, 255, 255, 255, 128).Blend(screen.data(), region, 8, 8);
  const uint8_t half = LumaAt(screen, region, 10, 10);

  CHECK_BECAUSE(half > dark && half < bright,
                "half coverage has to land between the desktop and the "
                "pointer, which is what makes an antialiased edge smooth");
}

/* ---- the masked colour format, whose mask runs the other way ----------- */

namespace {

/* A masked colour pointer: a white square whose left half is drawn and
 * whose right half is transparent. The mask is an AND mask, so zero is
 * the part that gets painted. */
CursorImage MaskedHalf(int side) {
  std::vector<uint8_t> bgra(static_cast<size_t>(side) * side * 4);
  for (int y = 0; y < side; ++y) {
    for (int x = 0; x < side; ++x) {
      uint8_t* p = &bgra[(static_cast<size_t>(y) * side + x) * 4];
      p[0] = p[1] = p[2] = 255;
      p[3] = (x < side / 2) ? 0 : 255;
    }
  }
  CursorImage image;
  image.SetMaskedColour(bgra.data(), static_cast<size_t>(side) * 4, side,
                        side);
  return image;
}

}  // namespace

TEST(cursor, a_masked_pointer_draws_where_its_mask_is_zero) {
  const Rect region = {0, 0, 64, 64};
  std::vector<uint8_t> screen = Background(region, 0, 0, 0);
  const uint8_t before = LumaAt(screen, region, 10, 10);

  MaskedHalf(8).Blend(screen.data(), region, 8, 8);

  CHECK_BECAUSE(LumaAt(screen, region, 10, 10) > before,
                "zero in an AND mask means paint the colour, the opposite "
                "of what zero means in ordinary coverage");
}

TEST(cursor, a_masked_pointer_leaves_the_rest_alone) {
  const Rect region = {0, 0, 64, 64};
  std::vector<uint8_t> screen = Background(region, 255, 255, 255);
  const uint8_t before = LumaAt(screen, region, 14, 10);

  MaskedHalf(8).Blend(screen.data(), region, 8, 8);

  CHECK_BECAUSE(LumaAt(screen, region, 14, 10) == before,
                "reading the mask the wrong way round paints a solid block "
                "where the pointer should be transparent, which on a white "
                "page shows up as a dark rectangle around the caret");
}

}  // namespace usbdisplay
