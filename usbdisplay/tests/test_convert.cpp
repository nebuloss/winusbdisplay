/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Colour conversion and wire framing.
 *
 * The exactness tests are the important ones. Two conversion paths run in
 * this driver, chosen per update by size, so a region redrawn at slightly
 * different sizes takes different paths on consecutive frames. If the paths
 * disagree by even one least significant bit, that region alternates between
 * two values, and on small text it is visible as a shimmer. This is not
 * hypothetical: it happened, and unifying the arithmetic is what fixed it.
 */

#include "../src/render/convert.h"
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

/* One row of a single colour, in the XRGB8888 memory order the compositor
 * hands over: blue, green, red, unused. */
std::vector<uint8_t> SolidRow(int width, uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> row(static_cast<size_t>(width) * 4);
  for (int i = 0; i < width; ++i) {
    row[i * 4 + 0] = b;
    row[i * 4 + 1] = g;
    row[i * 4 + 2] = r;
    row[i * 4 + 3] = 0xFF;
  }
  return row;
}

}  // namespace

TEST(conversion, simd_and_scalar_agree_exactly_at_every_width) {
  /* Widths chosen to exercise the SIMD body, the scalar remainder, and the
   * boundary between them, since which pixels fall to which path is what
   * made an earlier mismatch visible. */
  for (int width : {2, 4, 6, 8, 10, 14, 16, 18, 62, 64, 66, 130, 512, 1920}) {
    CHECK_EQ_BECAUSE(SelfTest(width, 16), 0,
                     "the paths must agree exactly, not closely, or text "
                     "shimmers as regions are redrawn at different widths");
  }
}

TEST(conversion, known_colours_match_the_reference_values) {
  /* Spot checked against the reference implementation and confirmed on the
   * panel: these are the bytes the adapter is known to render correctly. */
  struct Sample {
    const char* name;
    uint8_t r, g, b;
    uint8_t expected[4];
  };
  const Sample samples[] = {
      {"white", 255, 255, 255, {0x80, 0xEA, 0x80, 0xEA}},
      {"black", 0, 0, 0, {0x80, 0x10, 0x80, 0x10}},
      {"cyan", 0, 255, 255, {0xA5, 0xA8, 0x10, 0xA8}},
      {"blue", 0, 0, 255, {0xEF, 0x28, 0x6D, 0x28}},
  };

  for (const Sample& sample : samples) {
    const std::vector<uint8_t> row = SolidRow(2, sample.r, sample.g, sample.b);
    uint8_t out[4] = {0, 0, 0, 0};
    ConvertRow(out, row.data(), 2);
    for (int i = 0; i < 4; ++i) {
      CHECK_EQ_BECAUSE(static_cast<int>(out[i]),
                       static_cast<int>(sample.expected[i]),
                       std::string("colour ") + sample.name +
                           " no longer converts to the values verified "
                           "against the reference driver and the panel");
    }
  }
}

TEST(conversion, luma_stays_in_broadcast_range) {
  /* The adapter expects limited range, so black is 16 and white sits at the
   * top of the range rather than at 255. Sending full range washes the
   * picture out at both ends. White lands one below the nominal 235 because
   * the published coefficients themselves undershoot slightly; that is the
   * value the reference driver produces and the value confirmed on the
   * panel, so it is what we match. */
  const std::vector<uint8_t> black = SolidRow(2, 0, 0, 0);
  const std::vector<uint8_t> white = SolidRow(2, 255, 255, 255);
  uint8_t out[4];

  ConvertRow(out, black.data(), 2);
  CHECK_EQ(static_cast<int>(out[1]), 16);
  ConvertRow(out, white.data(), 2);
  CHECK_EQ(static_cast<int>(out[3]), 234);
  CHECK(out[3] <= 235);
}

TEST(conversion, grey_has_neutral_chroma) {
  const std::vector<uint8_t> grey = SolidRow(2, 128, 128, 128);
  uint8_t out[4];
  ConvertRow(out, grey.data(), 2);
  CHECK_EQ_BECAUSE(static_cast<int>(out[0]), 128,
                   "a grey pixel with a colour cast means the coefficients "
                   "or the channel order are wrong");
  CHECK_EQ(static_cast<int>(out[2]), 128);
}

TEST(brightness, the_default_leaves_the_picture_untouched) {
  uint8_t row[8] = {0x70, 0x40, 0x90, 0x50, 0x80, 0xEA, 0x80, 0xEA};
  uint8_t expected[8];
  memcpy(expected, row, sizeof(row));

  PictureAdjust neutral;
  CHECK(neutral.IsIdentity());
  ApplyPictureAdjust(row, 4, neutral);
  CHECK_EQ(memcmp(row, expected, sizeof(row)), 0);
}

TEST(brightness, dimming_lowers_luma_and_leaves_chroma_neutral) {
  /* The adapter has no hardware brightness control, so a slider has to work
   * by changing the pixels. Dimming must not tint the picture. */
  uint8_t row[4] = {128, 200, 128, 200};

  PictureAdjust dim;
  dim.brightness = 50;
  ApplyPictureAdjust(row, 2, dim);

  CHECK_BECAUSE(row[1] < 200, "brightness 50 should visibly darken");
  CHECK_BECAUSE(row[1] >= 16, "luma must not fall below broadcast black");
  CHECK_EQ_BECAUSE(static_cast<int>(row[0]), 128,
                   "chroma is scaled about neutral, so a neutral pixel stays "
                   "neutral and dimming does not introduce a colour cast");
}

TEST(brightness, extremes_stay_inside_the_legal_range) {
  uint8_t row[4] = {16, 235, 240, 16};
  PictureAdjust extreme;
  extreme.brightness = 100;
  extreme.contrast = 100;
  ApplyPictureAdjust(row, 2, extreme);

  CHECK(row[1] >= 16 && row[1] <= 235);
  CHECK(row[3] >= 16 && row[3] <= 235);
  CHECK(row[0] >= 16 && row[0] <= 240);
  CHECK(row[2] >= 16 && row[2] <= 240);
}

TEST(framing, header_and_footer_are_where_the_adapter_expects) {
  std::vector<uint8_t> out(kMaxTransferBytes);
  const Rect rect = Make(64, 32, 128, 64);

  const size_t written = FrameExisting(out.data(), out.size(), rect);
  CHECK_EQ(written, TransferLength(rect));

  CHECK_EQ_BECAUSE(static_cast<int>(out[0]), 0xFF, "the marker is FF 00");
  CHECK_EQ(static_cast<int>(out[1]), 0x00);

  /* Position and size are packed twelve bits each into three bytes. */
  const uint32_t position = (out[2] << 16) | (out[3] << 8) | out[4];
  const uint32_t dimensions = (out[5] << 16) | (out[6] << 8) | out[7];
  CHECK_EQ(static_cast<int>(position >> 12), 64);
  CHECK_EQ(static_cast<int>(position & 0xFFF), 32);
  CHECK_EQ(static_cast<int>(dimensions >> 12), 128);
  CHECK_EQ(static_cast<int>(dimensions & 0xFFF), 64);

  CHECK_EQ_BECAUSE(
      memcmp(out.data() + written - kFrameFooterSize, kFrameFooter,
             kFrameFooterSize),
      0,
      "the footer sits immediately after the pixels; the published "
      "description of this protocol wrongly claims eight bytes of padding "
      "before the pixel data as well");
}

TEST(framing, pixels_start_at_byte_eight_with_no_padding) {
  const int width = 1920, height = 64;
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> source(stride * height);
  FillSolid(source.data(), stride, width, height, 255, 255, 255);

  std::vector<uint8_t> out(kMaxTransferBytes);
  const Rect rect = Make(0, 0, 8, 2);
  const size_t written = FrameRect(out.data(), out.size(), source.data(),
                                   stride, width, height, rect);
  CHECK_EQ(written, TransferLength(rect));
  CHECK_EQ_BECAUSE(static_cast<int>(out[kFrameHeaderSize]), 0x80,
                   "white converts to U=0x80, and it has to appear at byte "
                   "eight exactly");
  CHECK_EQ(static_cast<int>(out[kFrameHeaderSize + 1]), 0xEA);
}

TEST(framing, rows_are_packed_with_no_stride_padding) {
  const int width = 64, height = 4;
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> source(stride * height);
  /* Each row a different shade, so row boundaries are detectable. */
  for (int y = 0; y < height; ++y) {
    FillSolid(source.data() + static_cast<size_t>(y) * stride, stride, width, 1,
              static_cast<uint8_t>(y * 40), static_cast<uint8_t>(y * 40),
              static_cast<uint8_t>(y * 40));
  }

  std::vector<uint8_t> out(kMaxTransferBytes);
  const Rect rect = Make(0, 0, 8, 4);
  FrameRect(out.data(), out.size(), source.data(), stride, width, height,
            rect);

  const size_t row_bytes = static_cast<size_t>(rect.width()) * 2;
  for (int y = 1; y < 4; ++y) {
    const uint8_t previous =
        out[kFrameHeaderSize + (y - 1) * row_bytes + 1];
    const uint8_t current = out[kFrameHeaderSize + y * row_bytes + 1];
    CHECK_BECAUSE(current > previous,
                  "rows are width * 2 bytes with nothing between them; a "
                  "stride mistake here shears the picture");
  }
}

TEST(framing, refuses_a_misaligned_rect) {
  const int width = 64, height = 8;
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> source(stride * height);
  std::vector<uint8_t> out(kMaxTransferBytes);

  CHECK_EQ_BECAUSE(FrameRect(out.data(), out.size(), source.data(), stride,
                             width, height, Make(1, 0, 8, 2)),
                   static_cast<size_t>(0),
                   "an odd left edge would split a UYVY pair, so it is "
                   "refused rather than silently rendered with fringing");
  CHECK_EQ(FrameRect(out.data(), out.size(), source.data(), stride, width,
                     height, Make(0, 0, 7, 2)),
           static_cast<size_t>(0));
}

TEST(framing, refuses_a_rect_outside_the_image) {
  const int width = 64, height = 8;
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> source(stride * height);
  std::vector<uint8_t> out(kMaxTransferBytes);

  CHECK_EQ(FrameRect(out.data(), out.size(), source.data(), stride, width,
                     height, Make(0, 0, 128, 2)),
           static_cast<size_t>(0));
}

TEST(framing, refuses_a_buffer_that_is_too_small) {
  const int width = 64, height = 8;
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> source(stride * height);
  std::vector<uint8_t> tiny(16);

  CHECK_EQ(FrameRect(tiny.data(), tiny.size(), source.data(), stride, width,
                     height, Make(0, 0, 64, 8)),
           static_cast<size_t>(0));
}

TEST(modes, the_table_covers_the_common_desktop_resolutions) {
  CHECK(FindMode(1920, 1080, 60) != nullptr);
  CHECK(FindMode(1280, 720, 60) != nullptr);
  CHECK(FindMode(1024, 768, 60) != nullptr);
  CHECK_BECAUSE(FindMode(1920, 1080, 30) != nullptr,
                "1080p30 is a real mode of this adapter and suits the "
                "available bandwidth far better than 1080p60");
  CHECK(FindMode(1234, 567, 60) == nullptr);
}

TEST(modes, no_mode_exceeds_the_maximum_frame_size) {
  for (size_t i = 0; i < kModeCount; ++i) {
    CHECK(kModes[i].width <= kMaxFrameWidth);
    CHECK(kModes[i].height <= kMaxFrameHeight);
    const size_t bytes =
        static_cast<size_t>(kModes[i].width) * kModes[i].height * 2 +
        kFrameOverhead;
    CHECK_BECAUSE(bytes <= kMaxTransferBytes,
                  "transfer buffers are sized from these constants, so a "
                  "mode that does not fit would overrun them");
  }
}

/* Gamma tables are how ordinary Windows brightness tools reach this monitor,
 * so they have to behave exactly like the rest of the conversion: applied on
 * the source colour, and identically whichever path runs. */

TEST(gamma, the_default_table_changes_nothing) {
  GammaRamp ramp;
  ramp.Reset();
  CHECK_BECAUSE(ramp.identity,
                "a table where output equals input must be recognised as "
                "such, or every frame pays for a pointless pass over the "
                "pixels");

  std::vector<uint8_t> row = SolidRow(4, 200, 100, 50);
  const std::vector<uint8_t> before = row;
  ApplyGammaRow(row.data(), 4, ramp);
  CHECK_EQ(memcmp(row.data(), before.data(), row.size()), 0);
}

TEST(gamma, a_dimming_table_darkens_every_channel) {
  uint16_t table[768];
  for (int i = 0; i < 256; ++i) {
    const uint16_t half = static_cast<uint16_t>(i * 257 / 2);
    table[i] = half;
    table[256 + i] = half;
    table[512 + i] = half;
  }

  GammaRamp ramp;
  ramp.Set(table);
  CHECK_BECAUSE(!ramp.identity, "a table that halves output is not neutral");

  std::vector<uint8_t> row = SolidRow(2, 200, 160, 80);
  ApplyGammaRow(row.data(), 2, ramp);

  /* Source order is blue, green, red. */
  CHECK_EQ(static_cast<int>(row[0]), 80 / 2);
  CHECK_EQ(static_cast<int>(row[1]), 160 / 2);
  CHECK_EQ(static_cast<int>(row[2]), 200 / 2);
}

TEST(gamma, a_table_that_only_moves_low_bits_counts_as_neutral) {
  uint16_t table[768];
  for (int i = 0; i < 256; ++i) {
    /* Differs from the identity, but only below the eighth bit, which
     * conversion discards. Built by placing the level in the high byte
     * directly: the obvious i * 257 + n overflows at the top of the range
     * and changes the high byte after all, which is what this test is
     * supposed to rule out. */
    const uint16_t nudged = static_cast<uint16_t>((i << 8) | (i % 7));
    table[i] = nudged;
    table[256 + i] = nudged;
    table[512 + i] = nudged;
  }
  GammaRamp ramp;
  ramp.Set(table);
  CHECK_BECAUSE(ramp.identity,
                "only the top eight bits survive conversion, so a table that "
                "moves nothing above them is not worth applying");
}

TEST(gamma, applied_before_conversion_not_after) {
  /* Gamma acts on colour, so dimming the source and converting must give the
   * same answer as converting a dimmed source. If it were applied to the
   * encoded result instead, the chroma would shift and colours would drift
   * as brightness changed. */
  uint16_t table[768];
  for (int i = 0; i < 256; ++i) {
    const uint16_t half = static_cast<uint16_t>(i * 257 / 2);
    table[i] = half;
    table[256 + i] = half;
    table[512 + i] = half;
  }
  GammaRamp ramp;
  ramp.Set(table);

  const std::vector<uint8_t> source = SolidRow(2, 200, 160, 80);
  const std::vector<uint8_t> halved = SolidRow(2, 100, 80, 40);

  std::vector<uint8_t> viaGamma(4);
  std::vector<uint8_t> direct(4);
  {
    std::vector<uint8_t> work = source;
    ApplyGammaRow(work.data(), 2, ramp);
    ConvertRow(viaGamma.data(), work.data(), 2);
  }
  ConvertRow(direct.data(), halved.data(), 2);

  CHECK_EQ_BECAUSE(memcmp(viaGamma.data(), direct.data(), 4), 0,
                   "a gamma table must act on the colour, exactly as if the "
                   "desktop itself had been drawn darker");
}

TEST(gamma, region_conversion_honours_the_table) {
  const int width = 64, height = 8;
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> source(stride * height);
  FillSolid(source.data(), stride, width, height, 200, 200, 200);

  uint16_t table[768];
  for (int i = 0; i < 256; ++i) {
    const uint16_t half = static_cast<uint16_t>(i * 257 / 2);
    table[i] = half;
    table[256 + i] = half;
    table[512 + i] = half;
  }
  GammaRamp ramp;
  ramp.Set(table);

  const Rect region = Make(0, 0, width, height);
  std::vector<uint8_t> plain(static_cast<size_t>(width) * 2 * height);
  std::vector<uint8_t> dimmed = plain;

  CHECK(ConvertRegion(plain.data(), plain.size(), source.data(), stride,
                      region, PictureAdjust(), nullptr));
  CHECK(ConvertRegion(dimmed.data(), dimmed.size(), source.data(), stride,
                      region, PictureAdjust(), &ramp));

  /* Luma sits at odd byte offsets in the encoded output. */
  CHECK_BECAUSE(dimmed[1] < plain[1],
                "a dimming table must actually dim the encoded result, or "
                "the brightness slider moves and nothing happens");
}

/* Brightness has to track what a monitor's own control does, or a display
 * driven by this project looks wrong beside an ordinary one set to the same
 * number. That is not a cosmetic concern: it is the first thing anyone
 * notices with two screens side by side. */

TEST(brightness, full_brightness_is_exactly_unchanged) {
  PictureAdjust full;
  CHECK_EQ_BECAUSE(full.LumaGain(), 256,
                   "anything else would dim a picture nobody asked to dim");
}

TEST(brightness, follows_emitted_light_rather_than_stored_value) {
  /* A backlight at half emits half the light. Luma is gamma encoded, so the
   * stored value that emits half is about 0.73, not 0.5. Scaling by the
   * percentage instead lands near a fifth of the light and the display
   * looks far darker than the monitor beside it. */
  PictureAdjust half;
  half.brightness = 50;

  const int gain = half.LumaGain();
  CHECK_BECAUSE(gain > 170 && gain < 200,
                "half brightness should scale stored luma by roughly 0.73, "
                "which is what emits half the light");

  /* Confirm it really is half the light once decoded. */
  const double encoded = gain / 256.0;
  const double light = pow(encoded, PictureAdjust::kDisplayGamma);
  CHECK_BECAUSE(light > 0.45 && light < 0.55,
                "decoding the scaled value must give half the original "
                "light, or this display and a normal one disagree");
}

TEST(brightness, never_brightens) {
  for (int percent = 0; percent <= 100; ++percent) {
    PictureAdjust adjust;
    adjust.brightness = percent;
    CHECK(adjust.LumaGain() <= 256);
  }
}

TEST(brightness, rises_with_the_setting) {
  int previous = -1;
  for (int percent = 0; percent <= 100; percent += 5) {
    PictureAdjust adjust;
    adjust.brightness = percent;
    const int gain = adjust.LumaGain();
    CHECK_BECAUSE(gain >= previous,
                  "a higher setting must never produce a darker picture");
    previous = gain;
  }
}
