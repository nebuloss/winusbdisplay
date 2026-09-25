/* SPDX-License-Identifier: GPL-2.0-only */

#include "ms912x_convert.h"

#include <emmintrin.h>

#include <vector>

#include <climits>
#include <cstring>

namespace ms912x {
namespace {

inline unsigned RgbToY(unsigned r, unsigned g, unsigned b) {
  return ((16u << 16) + 16763u * r + 32904u * g + 6391u * b) >> 16;
}

inline unsigned RgbToU(unsigned r, unsigned g, unsigned b) {
  return ((128u << 16) - 9676u * r - 18996u * g + 28672u * b) >> 16;
}

inline unsigned RgbToV(unsigned r, unsigned g, unsigned b) {
  return ((128u << 16) + 28672u * r - 24009u * g - 4663u * b) >> 16;
}

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

void ConvertRowXrgbToUyvyScalar(uint8_t* dst, const uint8_t* src,
                                int width) {
  for (int i = 0; i + 1 < width; i += 2) {
    const uint8_t* p1 = src + static_cast<size_t>(i) * 4;
    const uint8_t* p2 = p1 + 4;

    /* XRGB8888 is little endian in memory: B, G, R, X. */
    unsigned b1 = p1[0], g1 = p1[1], r1 = p1[2];
    unsigned b2 = p2[0], g2 = p2[1], r2 = p2[2];

    unsigned y1 = RgbToY(r1, g1, b1);
    unsigned y2 = RgbToY(r2, g2, b2);
    unsigned u = (RgbToU(r1, g1, b1) + RgbToU(r2, g2, b2)) / 2;
    unsigned v = (RgbToV(r1, g1, b1) + RgbToV(r2, g2, b2)) / 2;

    *dst++ = static_cast<uint8_t>(u);
    *dst++ = static_cast<uint8_t>(y1);
    *dst++ = static_cast<uint8_t>(v);
    *dst++ = static_cast<uint8_t>(y2);
  }
}

void ApplyPictureAdjust(uint8_t* row, int width, const PictureAdjust& adjust) {
  if (adjust.IsIdentity()) {
    return;
  }
  /* Brightness scales luma above black; contrast scales chroma about neutral
   * and luma about mid grey. Both are fixed point over 256. */
  const int luma_gain = (adjust.brightness * 256) / 100;
  const int chroma_gain = (adjust.contrast * 256) / 50;

  for (int i = 0; i < width * 2; i += 4) {
    for (int luma_offset : {1, 3}) {
      int y = row[i + luma_offset] - 16;
      y = (y * luma_gain) >> 8;
      y += 16;
      row[i + luma_offset] = static_cast<uint8_t>(y < 16 ? 16
                                                  : (y > 235 ? 235 : y));
    }
    for (int chroma_offset : {0, 2}) {
      int c = row[i + chroma_offset] - 128;
      c = (c * chroma_gain) >> 8;
      c += 128;
      row[i + chroma_offset] = static_cast<uint8_t>(c < 16 ? 16
                                                    : (c > 240 ? 240 : c));
    }
  }
}

size_t FrameRect(uint8_t* dst, size_t dst_capacity, const uint8_t* src,
                 size_t src_stride, int fb_width, int fb_height,
                 const Rect& rect, const PictureAdjust& adjust) {
  if (rect.empty() || rect.x1 < 0 || rect.y1 < 0 || rect.x2 > fb_width ||
      rect.y2 > fb_height || (rect.x1 & 1) || (rect.width() & 1)) {
    return 0;
  }
  const size_t needed = TransferLength(rect);
  if (dst_capacity < needed) {
    return 0;
  }

  FrameUpdateHeader header;
  PutBe16(header.marker_be, kFrameMarker);
  PutBe24(header.position,
          ((static_cast<uint32_t>(rect.x1) & 0xFFF) << 12) |
              (static_cast<uint32_t>(rect.y1) & 0xFFF));
  PutBe24(header.dimensions,
          ((static_cast<uint32_t>(rect.width()) & 0xFFF) << 12) |
              (static_cast<uint32_t>(rect.height()) & 0xFFF));
  memcpy(dst, &header, sizeof(header));

  uint8_t* out = dst + sizeof(header);
  const size_t row_bytes = static_cast<size_t>(rect.width()) * 2;
  for (int y = rect.y1; y < rect.y2; ++y) {
    const uint8_t* row =
        src + static_cast<size_t>(y) * src_stride +
        static_cast<size_t>(rect.x1) * 4;
    ConvertRowXrgbToUyvy(out, row, rect.width());
    ApplyPictureAdjust(out, rect.width(), adjust);
    out += row_bytes;
  }

  memcpy(out, kFrameFooter, kFrameFooterSize);
  return needed;
}

void FillSolid(uint8_t* dst, size_t stride, int width, int height, uint8_t r,
               uint8_t g, uint8_t b) {
  for (int y = 0; y < height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      row[x * 4 + 0] = b;
      row[x * 4 + 1] = g;
      row[x * 4 + 2] = r;
      row[x * 4 + 3] = 0xFF;
    }
  }
}

void FillColourBars(uint8_t* dst, size_t stride, int width, int height) {
  static const uint8_t kBars[8][3] = {
      {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
      {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
  };
  for (int y = 0; y < height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      int bar = (x * 8) / (width > 0 ? width : 1);
      if (bar > 7) {
        bar = 7;
      }
      row[x * 4 + 0] = kBars[bar][2];
      row[x * 4 + 1] = kBars[bar][1];
      row[x * 4 + 2] = kBars[bar][0];
      row[x * 4 + 3] = 0xFF;
    }
  }
}


namespace {

/* The scalar path uses 16-bit fixed point, but 32904 does not fit in the
 * signed 16-bit lanes _mm_madd_epi16 needs, so the SIMD path halves every
 * coefficient and shifts by 15 instead of 16. */
constexpr int16_t kYb = 3196, kYg = 16452, kYr = 8382;
constexpr int16_t kUb = 14336, kUg = -9498, kUr = -4838;
constexpr int16_t kVb = -2332, kVg = -12005, kVr = 14336;

/* Horizontally adds the two dot products in a madd result and returns them in
 * lanes 0 and 1. Staying in registers matters: an earlier version wrote the
 * lanes to the stack and read them back, and the resulting store forwarding
 * stall dominated the whole conversion. */
inline __m128i PairDots(__m128i madd_result) {
  const __m128i summed = _mm_add_epi32(
      madd_result, _mm_shuffle_epi32(madd_result, _MM_SHUFFLE(3, 3, 1, 1)));
  return _mm_shuffle_epi32(summed, _MM_SHUFFLE(3, 1, 2, 0));
}

/* Four consecutive dot products, from the low and high halves of four
 * unpacked pixels. */
inline __m128i QuadDots(__m128i lo, __m128i hi, __m128i coeff) {
  return _mm_unpacklo_epi64(PairDots(_mm_madd_epi16(lo, coeff)),
                            PairDots(_mm_madd_epi16(hi, coeff)));
}

inline __m128i ScaleAndBias(__m128i value, int bias) {
  return _mm_add_epi32(_mm_srai_epi32(value, 15), _mm_set1_epi32(bias));
}

}  // namespace

void ConvertRowXrgbToUyvySimd(uint8_t* dst, const uint8_t* src, int width) {
  /* Memory order is B, G, R, X, so the coefficient lanes follow that order. */
  const __m128i y_coeff = _mm_setr_epi16(kYb, kYg, kYr, 0, kYb, kYg, kYr, 0);
  const __m128i u_coeff = _mm_setr_epi16(kUb, kUg, kUr, 0, kUb, kUg, kUr, 0);
  const __m128i v_coeff = _mm_setr_epi16(kVb, kVg, kVr, 0, kVb, kVg, kVr, 0);
  const __m128i zero = _mm_setzero_si128();
  const __m128i ones = _mm_set1_epi16(1);

  int i = 0;
  /* Eight pixels per iteration produces exactly one 16 byte UYVY store. */
  for (; i + 7 < width; i += 8) {
    const uint8_t* p = src + static_cast<size_t>(i) * 4;
    const __m128i raw0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m128i raw1 =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));

    const __m128i lo0 = _mm_unpacklo_epi8(raw0, zero);
    const __m128i hi0 = _mm_unpackhi_epi8(raw0, zero);
    const __m128i lo1 = _mm_unpacklo_epi8(raw1, zero);
    const __m128i hi1 = _mm_unpackhi_epi8(raw1, zero);

    /* Luma for all eight pixels, packed to 16 bit then to bytes. */
    const __m128i y_lo = ScaleAndBias(QuadDots(lo0, hi0, y_coeff), 16);
    const __m128i y_hi = ScaleAndBias(QuadDots(lo1, hi1, y_coeff), 16);
    const __m128i y_bytes =
        _mm_packus_epi16(_mm_packs_epi32(y_lo, y_hi), zero);

    /* Chroma for all eight, then averaged across each pixel pair.
     * _mm_madd_epi16 against all ones is a pairwise horizontal add. */
    const __m128i u_all = _mm_packs_epi32(
        ScaleAndBias(QuadDots(lo0, hi0, u_coeff), 128),
        ScaleAndBias(QuadDots(lo1, hi1, u_coeff), 128));
    const __m128i v_all = _mm_packs_epi32(
        ScaleAndBias(QuadDots(lo0, hi0, v_coeff), 128),
        ScaleAndBias(QuadDots(lo1, hi1, v_coeff), 128));

    const __m128i u_pairs = _mm_srai_epi32(_mm_madd_epi16(u_all, ones), 1);
    const __m128i v_pairs = _mm_srai_epi32(_mm_madd_epi16(v_all, ones), 1);

    /* Interleave to U V U V ..., then with luma to U Y V Y ... */
    const __m128i uv =
        _mm_unpacklo_epi16(_mm_packs_epi32(u_pairs, zero),
                           _mm_packs_epi32(v_pairs, zero));
    const __m128i uv_bytes = _mm_packus_epi16(uv, zero);

    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst),
                     _mm_unpacklo_epi8(uv_bytes, y_bytes));
    dst += 16;
  }

  if (i < width) {
    ConvertRowXrgbToUyvyScalar(dst, src + static_cast<size_t>(i) * 4,
                               width - i);
  }
}

void ConvertRowXrgbToUyvy(uint8_t* dst, const uint8_t* src, int width) {
  /* SSE2 is part of the x64 baseline, so no runtime check is needed. */
  ConvertRowXrgbToUyvySimd(dst, src, width);
}

int ConvertSelfTest(int width, int iterations) {
  std::vector<uint8_t> source(static_cast<size_t>(width) * 4);
  std::vector<uint8_t> a(static_cast<size_t>(width) * 2);
  std::vector<uint8_t> b(static_cast<size_t>(width) * 2);

  uint32_t seed = 12345;
  int worst = 0;
  for (int iter = 0; iter < iterations; ++iter) {
    for (size_t i = 0; i < source.size(); ++i) {
      seed = seed * 1664525u + 1013904223u;
      source[i] = static_cast<uint8_t>(seed >> 24);
    }
    ConvertRowXrgbToUyvyScalar(a.data(), source.data(), width);
    ConvertRowXrgbToUyvySimd(b.data(), source.data(), width);
    for (size_t i = 0; i < a.size(); ++i) {
      int diff = static_cast<int>(a[i]) - static_cast<int>(b[i]);
      if (diff < 0) {
        diff = -diff;
      }
      if (diff > worst) {
        worst = diff;
      }
    }
  }
  return worst;
}

}  // namespace ms912x
