// sa2-nx (2026-10-20): whether two 64-byte lines differ, for the device chunks of the delta chain (backend/queued.cpp)
// and the pushed shared constants' lines (render/draw.cpp). GCC left the eight 64-bit words of the plain version scalar
// (about 30 instructions a line: 9 % of the game thread in the second console run); four 128-bit loads a side, XORed,
// ORed and one horizontal max are about 10 (sa2_renut_neon_compare).
#pragma once

#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace sa2::renut {

inline bool Line64DiffersPlain(const uint8_t *a, const uint8_t *b) {
  uint64_t diff = 0;
  for (uint32_t k = 0; k < 64; k += 8) {
    uint64_t x, y;
    std::memcpy(&x, a + k, 8);
    std::memcpy(&y, b + k, 8);
    diff |= x ^ y;
  }
  return diff != 0;
}

inline bool Line64Differs(const uint8_t *a, const uint8_t *b, bool neon) {
#if defined(__aarch64__)
  if (neon) {
    const uint8x16x4_t x = vld1q_u8_x4(a);
    const uint8x16x4_t y = vld1q_u8_x4(b);
    const uint8x16_t d = vorrq_u8(vorrq_u8(veorq_u8(x.val[0], y.val[0]), veorq_u8(x.val[1], y.val[1])),
                                  vorrq_u8(veorq_u8(x.val[2], y.val[2]), veorq_u8(x.val[3], y.val[3])));
    return vmaxvq_u32(vreinterpretq_u32_u8(d)) != 0;
  }
#else
  (void)neon;
#endif
  return Line64DiffersPlain(a, b);
}

#if defined(__aarch64__)
// me2-nx-deko (2026-10-27): which of up to 16 64-byte lines differ, bit k for line k (at a + offset[k] / b + offset[k]),
// n <= 16 lines. Line64Differs per line ends each one in a horizontal max, a vector -> general register move and a
// branch: the console stack profile of 22:2x had 7 % of the UE3 render thread in that loop of CaptureDelta (39 lines a
// draw, about 3 of them differing). Here every line is XORed and ORed down to one vector, the 16 vectors are folded by
// four pairwise max steps into one (byte k = line k), and a single mask leaves the vector unit. Lines past n
// reload line 0 and are masked off.
inline uint32_t Lines64DiffMask16(const uint8_t *a, const uint8_t *b, const uint16_t *offset, uint32_t n) {
  uint8x16_t d[16];
  for (uint32_t k = 0; k < 16; ++k) {
    const uint32_t at = offset[k < n ? k : 0];
    const uint8x16x4_t x = vld1q_u8_x4(a + at);
    const uint8x16x4_t y = vld1q_u8_x4(b + at);
    d[k] = vorrq_u8(vorrq_u8(veorq_u8(x.val[0], y.val[0]), veorq_u8(x.val[1], y.val[1])),
                    vorrq_u8(veorq_u8(x.val[2], y.val[2]), veorq_u8(x.val[3], y.val[3])));
  }
  // vpmaxq_u8(p, q): bytes 0-7 the pairwise maxima of p, 8-15 those of q. After the 4th step byte k covers line k.
  for (uint32_t w = 8; w; w >>= 1)
    for (uint32_t k = 0; k < w; ++k)
      d[k] = vpmaxq_u8(d[2 * k], d[2 * k + 1]);
  static constexpr uint8_t kBits[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
  const uint8x16_t bits = vandq_u8(vtstq_u8(d[0], d[0]), vld1q_u8(kBits));
  const uint32_t mask = uint32_t(vaddv_u8(vget_low_u8(bits))) | (uint32_t(vaddv_u8(vget_high_u8(bits))) << 8);
  return mask & ((1u << n) - 1u);
}
#endif

}  // namespace sa2::renut
