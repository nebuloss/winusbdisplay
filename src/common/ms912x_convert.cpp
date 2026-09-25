/* SPDX-License-Identifier: GPL-2.0-only */

#include "ms912x_convert.h"

#include <emmintrin.h>

#include <atomic>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <climits>
#include <cstring>

namespace ms912x {
namespace {

/* Fixed point BT.601 limited range, in 15 bit rather than the 16 bit the
 * reference implementations use.
 *
 * The reason is consistency rather than precision. _mm_madd_epi16 needs
 * coefficients to fit in a signed 16 bit lane and 32904 does not, so the SIMD
 * path has to halve them and shift by 15. Leaving the scalar path on the
 * original 16 bit constants makes the two disagree by one least significant
 * bit, and since the SIMD path handles whole groups of eight pixels while the
 * scalar path handles the remainder, which pixels fall to which depends on the
 * width of the damage rectangle. A region redrawn at slightly different widths
 * then alternates between two values, and on text that is visible as a shimmer.
 *
 * Every path here, scalar, SIMD and the compute shader, now uses exactly these
 * constants, so the same pixel always converts to the same bytes. */
constexpr int kYr = 8382, kYg = 16452, kYb = 3196;
constexpr int kUr = -4838, kUg = -9498, kUb = 14336;
constexpr int kVr = 14336, kVg = -12005, kVb = -2332;

inline unsigned RgbToY(unsigned r, unsigned g, unsigned b) {
  return static_cast<unsigned>(
      16 + ((kYr * static_cast<int>(r) + kYg * static_cast<int>(g) +
             kYb * static_cast<int>(b)) >> 15));
}

inline unsigned RgbToU(unsigned r, unsigned g, unsigned b) {
  return static_cast<unsigned>(
      128 + ((kUr * static_cast<int>(r) + kUg * static_cast<int>(g) +
              kUb * static_cast<int>(b)) >> 15));
}

inline unsigned RgbToV(unsigned r, unsigned g, unsigned b) {
  return static_cast<unsigned>(
      128 + ((kVr * static_cast<int>(r) + kVg * static_cast<int>(g) +
              kVb * static_cast<int>(b)) >> 15));
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

namespace {

/* Minimal persistent worker pool. Threads are created once: spawning them per
 * frame would cost more than the work being parallelised. */
class RowPool {
 public:
  static RowPool& Instance() {
    static RowPool pool;
    return pool;
  }

  unsigned workers() const { return static_cast<unsigned>(threads_.size()) + 1; }

  void Resize(unsigned total) {
    if (total < 1) {
      total = 1;
    }
    Shutdown();
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
    for (unsigned i = 0; i + 1 < total; ++i) {
      threads_.emplace_back(&RowPool::Worker, this);
    }
  }

  /* Runs body(first_row, last_row) over [0, rows) split across the pool,
   * including the calling thread, and returns once every chunk is done. */
  void Run(int rows, const std::function<void(int, int)>& body) {
    const unsigned total = workers();
    if (total <= 1 || rows < 64) {
      body(0, rows);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      body_ = &body;
      rows_ = rows;
      chunks_ = total;
      next_chunk_ = 1;  /* chunk 0 belongs to the caller */
      outstanding_ = total - 1;
      ++generation_;
    }
    work_cv_.notify_all();

    RunChunk(0, rows, total, body);

    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock, [this] { return outstanding_ == 0; });
    body_ = nullptr;
  }

 private:
  RowPool() {
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
      hardware = 2;
    }
    /* Leave headroom: this runs on the IddCx processing thread while the USB
     * worker and the rest of the system are also active. */
    unsigned total = hardware > 4 ? 4 : hardware;
    Resize(total);
  }

  ~RowPool() { Shutdown(); }

  static void RunChunk(unsigned index, int rows, unsigned chunks,
                       const std::function<void(int, int)>& body) {
    const int first = static_cast<int>(
        (static_cast<long long>(rows) * index) / chunks);
    const int last = static_cast<int>(
        (static_cast<long long>(rows) * (index + 1)) / chunks);
    if (last > first) {
      body(first, last);
    }
  }

  void Worker() {
    unsigned seen = 0;
    for (;;) {
      unsigned index = 0;
      int rows = 0;
      unsigned chunks = 0;
      const std::function<void(int, int)>* body = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        work_cv_.wait(lock, [&] { return !running_ || generation_ != seen; });
        if (!running_) {
          return;
        }
        seen = generation_;
        if (next_chunk_ >= chunks_) {
          continue;
        }
        index = next_chunk_++;
        rows = rows_;
        chunks = chunks_;
        body = body_;
      }

      if (body) {
        RunChunk(index, rows, chunks, *body);
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        --outstanding_;
      }
      done_cv_.notify_one();
    }
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ && threads_.empty()) {
        return;
      }
      running_ = false;
      ++generation_;
    }
    work_cv_.notify_all();
    for (std::thread& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  bool running_ = false;
  unsigned generation_ = 0;
  unsigned chunks_ = 0;
  unsigned next_chunk_ = 0;
  unsigned outstanding_ = 0;
  int rows_ = 0;
  const std::function<void(int, int)>* body_ = nullptr;
};

}  // namespace

void SetConversionThreads(unsigned threads) {
  RowPool::Instance().Resize(threads);
}

unsigned ConversionThreads() { return RowPool::Instance().workers(); }

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

  uint8_t* const out = dst + sizeof(header);
  const size_t row_bytes = static_cast<size_t>(rect.width()) * 2;
  const int width = rect.width();

  RowPool::Instance().Run(
      rect.height(), [&](int first, int last) {
        for (int i = first; i < last; ++i) {
          const uint8_t* row = src +
                               static_cast<size_t>(rect.y1 + i) * src_stride +
                               static_cast<size_t>(rect.x1) * 4;
          uint8_t* target = out + static_cast<size_t>(i) * row_bytes;
          ConvertRowXrgbToUyvy(target, row, width);
          ApplyPictureAdjust(target, width, adjust);
        }
      });

  /* The footer sits immediately after the pixel data. `out` is deliberately
   * const now that rows are written by index, so compute the end explicitly
   * rather than relying on a pointer the loop used to advance. */
  memcpy(out + row_bytes * static_cast<size_t>(rect.height()), kFrameFooter,
         kFrameFooterSize);
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
/* Same constants as the scalar path above, narrowed for the 16 bit lanes. */

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
  const __m128i y_coeff = _mm_setr_epi16(
      static_cast<int16_t>(kYb), static_cast<int16_t>(kYg),
      static_cast<int16_t>(kYr), 0, static_cast<int16_t>(kYb),
      static_cast<int16_t>(kYg), static_cast<int16_t>(kYr), 0);
  const __m128i u_coeff = _mm_setr_epi16(
      static_cast<int16_t>(kUb), static_cast<int16_t>(kUg),
      static_cast<int16_t>(kUr), 0, static_cast<int16_t>(kUb),
      static_cast<int16_t>(kUg), static_cast<int16_t>(kUr), 0);
  const __m128i v_coeff = _mm_setr_epi16(
      static_cast<int16_t>(kVb), static_cast<int16_t>(kVg),
      static_cast<int16_t>(kVr), 0, static_cast<int16_t>(kVb),
      static_cast<int16_t>(kVg), static_cast<int16_t>(kVr), 0);
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
