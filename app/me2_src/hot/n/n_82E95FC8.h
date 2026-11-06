// ME2 (2026-11-06): Wwise reverb core sub_82E95FC8: four delay lines with a one-pole filter each, mixed with each other
// every sample (a feedback delay network), added to three output channels. Console stacks of 2026-11-06 (Kasumi's
// party, 19:21): 25.8 % of the Wwise thread's busy samples, the hottest Wwise function there. The recompiled loop
// re-reads twelve state fields per sample (it cannot know the stores miss them), spills through the stack, and zeroes
// a store card per store (8 a sample).
//
// sub_82E95FC8(r3 = st, r4 = in, r5 = out0, r6 = out1, r7 = out2, r8 = n), floats big-endian:
//   line k (k = 0..3): len [st + 12k], buf [st + 12k + 4], idx [st + 12k + 8]; filter state y_k [st + 48 + 12k],
//   b_k [st + 52 + 12k], a_k [st + 56 + 12k]. Per sample i:
//     tap_k = buf_k[idx_k];  y_k = float(fma(b_k, tap_k, -float(a_k * y_k)))          (fmuls, fmsubs: double fma)
//     h = st3 + st2 ... the Hadamard-like mix below, in the generated code's operand order;
//     out0[i] += ..., out1[i] += ..., out2[i] += ...;  m = sum * -0.5f;
//     buf0 <- (y1 + m) + in[i], buf1 <- (y2 + m) + in[i], buf2 <- (y3 + m) + in[i], buf3 <- (y0 + m) + in[i]
//     idx_k = idx_k + 1 == len_k ? 0 : idx_k + 1
//   then idx_k stored, and y_k stored as float(float(y_k + K) - K), K = the .rdata float at 0x8206D5E0 (bits
//   0x219392EF: the denormal flush). Flush mode cleared at entry (ctx.fpscr.disableFlushMode, unconditional).
// The native keeps every pointer, length, coefficient and filter state in registers: valid while the 96-byte state block
// is not inside a range the loop writes (checked; otherwise declined). The per-sample loads and stores keep the
// original's order (taps 2, 1, 3, 0; out0..2 read; in read; out0..2 written; buf0..3 written), so any aliasing among
// the outputs, the input and the delay lines gives the original's result. Cards: the written ranges once, at the end.
// Arithmetic: exactly the generated code's (REX_FMULS / REX_FADDS / REX_FSUBS in float, fmsubs as double fma then
// float), compiled with -ffp-contract=off (app/CMakeLists.txt) so nothing is fused.
// Registers: the original leaves r4-r12, f0-f13, ctr, cr6 as scratch; tools/hot/liveness_me2.py: its one direct call
// site reads none after the call. One pointer to it in data (0x82292428): callers through it cannot rely on scratch.
// Declined (the original runs): any line with len == 0, idx >= len or a buffer range that wraps 4 GB; the state block
// overlapping a written range.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../me_hot_common.h"

namespace me::hot::n_82E95FC8 {

inline constexpr Cmp kCmp = {0, 0, 0};

inline constexpr uint32_t kMixGain = 0xBF000000u;  // REX_RO_U32(0x82009A74): -0.5f
inline constexpr uint32_t kFlush = 0x219392EFu;    // REX_RO_U32(0x8206D5E0)

inline float Bits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline uint32_t BitsOf(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
inline float LdF(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return Bits(__builtin_bswap32(v));
}
// A store without its card (the cards of the written ranges are zeroed once at the end); the shadow build redirects it.
inline void StF(uint8_t* base, uint32_t a, float f) {
  const uint32_t v = __builtin_bswap32(BitsOf(f));
#if defined(ME_HOT_SHADOW_ACCESSORS)
  std::memcpy(StorePtr(base, a, 4), &v, 4);
#else
  std::memcpy(base + a + PhysOff(a), &v, 4);
#endif
}
inline bool Overlaps(uint32_t a, uint64_t alen, uint32_t b, uint64_t blen) {
  return alen && blen && uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

struct Lines {
  uint32_t len[4], buf[4], idx[4];
};

inline bool Read(const PPCContext& ctx, const uint8_t* base, Lines& l) {
  const uint32_t s = ctx.r3.u32;
  if (uint64_t(s) + 96 > 0x100000000ull) return false;
  for (int k = 0; k < 4; ++k) {
    l.len[k] = Ld32(base, s + 12 * k);
    l.buf[k] = Ld32(base, s + 12 * k + 4);
    l.idx[k] = Ld32(base, s + 12 * k + 8);
    if (l.len[k] == 0 || l.idx[k] >= l.len[k]) return false;
    if (uint64_t(l.buf[k]) + 4ull * l.len[k] > 0x100000000ull) return false;
  }
  return true;
}

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32, n = ctx.r8.u32;
  const uint32_t in = ctx.r4.u32, o0 = ctx.r5.u32, o1 = ctx.r6.u32, o2 = ctx.r7.u32;
  Lines l;
  if (!Read(ctx, base, l)) return false;
  const uint64_t bytes = 4ull * n;
  if (n && (uint64_t(in) + bytes > 0x100000000ull || uint64_t(o0) + bytes > 0x100000000ull ||
            uint64_t(o1) + bytes > 0x100000000ull || uint64_t(o2) + bytes > 0x100000000ull))
    return false;
  // The state block must stay out of everything the loop writes (its fields are read once, here).
  if (n) {
    if (Overlaps(s, 96, o0, bytes) || Overlaps(s, 96, o1, bytes) || Overlaps(s, 96, o2, bytes)) return false;
    for (int k = 0; k < 4; ++k)
      if (Overlaps(s, 96, l.buf[k], 4ull * l.len[k])) return false;
  }
  ctx.fpscr.disableFlushMode();
  float y0 = LdF(base, s + 48), y1 = LdF(base, s + 60), y2 = LdF(base, s + 72), y3 = LdF(base, s + 84);
  if (n) {
    const float b0 = LdF(base, s + 52), a0 = LdF(base, s + 56);
    const float b1 = LdF(base, s + 64), a1 = LdF(base, s + 68);
    const float b2 = LdF(base, s + 76), a2 = LdF(base, s + 80);
    const float b3 = LdF(base, s + 88), a3 = LdF(base, s + 92);
    const float gain = Bits(kMixGain);
    uint32_t i0 = l.idx[0], i1 = l.idx[1], i2 = l.idx[2], i3 = l.idx[3];
    for (uint32_t i = 0; i < n; ++i) {
      const float p2 = a2 * y2;                    // f8 = f10 * f0
      const float p1 = a1 * y1;                    // f5 = f7 * f12
      const float p3 = a3 * y3;                    // f3 = f6 * f13
      const float tap2 = LdF(base, l.buf[2] + 4u * i2);
      const float p0 = a0 * y0;                    // f11 = f1 * f11
      const float tap1 = LdF(base, l.buf[1] + 4u * i1);
      y2 = float(std::fma(double(b2), double(tap2), -double(p2)));
      const float tap3 = LdF(base, l.buf[3] + 4u * i3);
      y1 = float(std::fma(double(b1), double(tap1), -double(p1)));
      y3 = float(std::fma(double(b3), double(tap3), -double(p3)));
      const float tap0 = LdF(base, l.buf[0] + 4u * i0);
      const float old0 = LdF(base, o0 + 4u * i), old1 = LdF(base, o1 + 4u * i), old2 = LdF(base, o2 + 4u * i);
      y0 = float(std::fma(double(b0), double(tap0), -double(p0)));
      const float x = LdF(base, in + 4u * i);
      float f5 = y3 + y2;    // f5 = f13 + f0
      float f4 = y0 - y1;    // f4 = f11 - f12
      float f3 = y1 + y0;    // f3 = f12 + f11
      float f2 = f5 + y1;    // f2 = f5 + f12
      float f1 = f4 + y2;    // f1 = f4 + f0
      f5 = f3 - y2;          // f5 = f3 - f0
      f4 = f4 - y2;          // f4 = f4 - f0
      f3 = f2 + y0;          // f3 = f2 + f11
      f2 = f1 - y3;          // f2 = f1 - f13
      f1 = f5 - y3;          // f1 = f5 - f13
      f5 = f4 + y3;          // f5 = f4 + f13
      const float m = f3 * gain;  // f4 = f3 * f9
      StF(base, o0 + 4u * i, f2 + old0);
      StF(base, o1 + 4u * i, f1 + old1);
      StF(base, o2 + 4u * i, f5 + old2);
      const float w0 = y1 + m, w1 = y2 + m, w2 = y3 + m, w3 = y0 + m;  // f8, f7, f6, f5
      StF(base, l.buf[0] + 4u * i0, w0 + x);
      StF(base, l.buf[1] + 4u * i1, w1 + x);
      StF(base, l.buf[2] + 4u * i2, w2 + x);
      StF(base, l.buf[3] + 4u * i3, w3 + x);
      i0 = i0 + 1 == l.len[0] ? 0 : i0 + 1;
      i1 = i1 + 1 == l.len[1] ? 0 : i1 + 1;
      i2 = i2 + 1 == l.len[2] ? 0 : i2 + 1;
      i3 = i3 + 1 == l.len[3] ? 0 : i3 + 1;
    }
    St32(base, s + 8, i0);
    St32(base, s + 20, i1);
    St32(base, s + 32, i2);
    St32(base, s + 44, i3);
    MarkCards(base, o0, uint32_t(bytes));
    MarkCards(base, o1, uint32_t(bytes));
    MarkCards(base, o2, uint32_t(bytes));
    for (int k = 0; k < 4; ++k) {
      // The written part of line k: n entries from its index on, wrapping (the whole line when n >= len).
      const uint32_t len = l.len[k], from = l.idx[k];
      if (n >= len) {
        MarkCards(base, l.buf[k], 4u * len);
      } else {
        const uint32_t first = std::min(n, len - from);
        MarkCards(base, l.buf[k] + 4u * from, 4u * first);
        if (n > first) MarkCards(base, l.buf[k], 4u * (n - first));
      }
    }
  } else {
    St32(base, s + 8, l.idx[0]);
    St32(base, s + 20, l.idx[1]);
    St32(base, s + 32, l.idx[2]);
    St32(base, s + 44, l.idx[3]);
  }
  const float k = Bits(kFlush);
  const float f10 = y1 + k, f9 = y2 + k, f8 = y3 + k, f7 = y0 + k;
  StF(base, s + 60, f10 - k);
  StF(base, s + 72, f9 - k);
  StF(base, s + 84, f8 - k);
  StF(base, s + 48, f7 - k);
  MarkCards(base, s + 48, 48);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  Lines l;
  if (!Read(ctx, base, l)) return;
  const uint32_t n = ctx.r8.u32;
  if (n) {
    w.Add(ctx.r5.u32, 4u * n);
    w.Add(ctx.r6.u32, 4u * n);
    w.Add(ctx.r7.u32, 4u * n);
    for (int k = 0; k < 4; ++k) w.Add(l.buf[k], 4u * l.len[k]);
  }
  w.Add(ctx.r3.u32, 96);
}

}  // namespace me::hot::n_82E95FC8
