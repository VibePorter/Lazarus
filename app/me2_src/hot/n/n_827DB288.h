// ME2 (2026-10-27): UE3's texture + sampler setter of the material parameters (FMaterialShaderParameters::Set,
// sub_825DCDA0 -> sub_827DB288(sampler, sampler state, texture param)), with the three XDK functions it calls inlined:
// D3DDevice_SetTexture sub_82A9AF70 and the two filter setters sub_82A9D268 / sub_82A9D0C0. Console stack profile of
// 22:2x / 22:5x (Omega, 13.4-13.9 fps, UE3 render thread X827BD330 100 % on core 0): 827DB288 3.2-4.1 %, 82A9AF70
// 1.4-1.7 %, the filter setters ~0.5 % of that thread's samples, about 4 calls a draw.
//
// sub_827DB288(r3 = s (sampler, u16 from the caller), r4 = p (UE3 sampler state), r5 = t (texture param)):
//   dev = [0x833D074C]; M = (1 << 63) >> (s + 32)            (fetch constant dirty bit, srd: 0 for s + 32 >= 64)
//   SetTexture(dev, s, tex = [t + 8], M); MinFilter(dev, s, [p + 4]); MagFilter(dev, s, [p + 8])
//   fc = dev + 24 * (s + 48)  (fetch constant s: 6 words)
//   fc[3] = rlwimi(fc[3], [p + 12], 23, 7, 8);             dev[24] |= M
//   fc[0] = rlwimi(fc[0], [p + 16], 10, 19, 21);           dev[24] |= M
//   fc[0] = rlwimi(fc[0], [p + 20], 13, 16, 18);           dev[24] |= M
//   fc[0] = rlwimi(fc[0], [p + 24], 16, 13, 15);           dev[24] |= M
//   fc[5] |= 24;                                            dev[24] |= M
// SetTexture(dev, s, tex, M): old = dev[12552 + 4 s]; if tex: fc[0..5] rebuilt from tex[28..48] (bit fields, the
//   per-sampler bytes dev[11958 + s] / dev[11984 + s] clamp the mip range), dev[24] |= M; dev[12552 + 4 s] = tex;
//   then for old != 0: dev[10908] != 0 -> old[8] = dev[10908]; else (dev[10912] & old[0]) != 0 -> a packet is written
//   into the command buffer (and sub_82AA15E8 grows it when full; the packet's top bit comes from an uninitialised
//   stack word): DECLINED here (decided before any write: the original runs).
// Filter setters (dev, s, v): fc[4] / fc[3] bit fields from v, the sampler byte dev[11932 + s] through the table
//   0x822483A8 and dev[12010 + s]; dev[24] |= M. They differ only in the shifts (see Filter below).
// Every word is computed exactly as the generated code does in 32 bits (the 64-bit parts of rlwimi / add / andc never
// reach the stored low words). DECLINED: s >= 32 (never seen; the original handles it).
// Registers: the originals leave r3-r12 / ctr / cr as scratch; tools/hot/liveness_me2.py: none of the 80 direct call
// sites reads a volatile register after the call (one pointer to it in data, 0x822687E0: callers through a pointer
// cannot rely on scratch values).
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_827DB288 {

inline constexpr Cmp kCmp = {0, 0, 0};

inline constexpr uint32_t kDevicePtr = 0x833D074Cu;
inline constexpr uint32_t kFilterTable = 0x822483A8u;

// PPC rotate-and-mask on 32 bits (bit 0 = most significant).
constexpr uint32_t Mask(uint32_t mb, uint32_t me) {
  const uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
  return mb <= me ? (a & b) : (a | b);
}
constexpr uint32_t Rotl(uint32_t v, uint32_t sh) { return sh ? (v << sh) | (v >> (32 - sh)) : v; }
constexpr uint32_t Rlwinm(uint32_t v, uint32_t sh, uint32_t mb, uint32_t me) { return Rotl(v, sh) & Mask(mb, me); }
constexpr uint32_t Rlwimi(uint32_t a, uint32_t v, uint32_t sh, uint32_t mb, uint32_t me) {
  const uint32_t m = Mask(mb, me);
  return (Rotl(v, sh) & m) | (a & ~m);
}

inline uint32_t Fc(uint32_t dev, uint32_t s) { return dev + 24u * (s + 48u); }

// sub_82A9D268 (min filter: kMag false) / sub_82A9D0C0 (mag filter: kMag true).
template <bool kMag>
inline void Filter(uint8_t* base, uint32_t dev, uint32_t s, uint32_t v, uint64_t m) {
  const uint32_t fc = Fc(dev, s);
  const uint32_t r7 = v >> 2;
  uint32_t r29 = Ld32(base, fc + 16);
  uint32_t r31 = Ld32(base, fc + 12);
  const uint32_t r6 = kMag ? Rlwinm(r29, 22, 31, 31) : Rlwinm(r29, 21, 31, 31);
  const uint32_t r8 = Ld32(base, kFilterTable + 4u * uint32_t(Ld8(base, dev + s + 11932)));
  r29 = kMag ? Rlwimi(r29, r7, 11, 20, 20) : Rlwimi(r29, r7, 10, 21, 21);
  uint32_t r9 = r8 & ~((r6 | r7) - 1u);
  r9 = kMag ? Rlwinm(r9, 4, 0, 27) : Rlwinm(r9, 6, 0, 25);
  r9 = r9 | r7 | v;
  if (kMag) {
    r31 = Rlwimi(r31, r9, 21, 9, 10);
    r31 = Rlwimi(r31, r9, 21, 4, 6);
  } else {
    r31 = Rlwimi(r31, r9, 19, 11, 12);
    r31 = Rlwimi(r31, r9, 19, 4, 6);
  }
  St32(base, fc + 16, r29);
  St32(base, fc + 12, r31);
  const uint32_t b = Ld8(base, dev + s + 12010);
  const uint32_t lim = (b >> 2) - 1u;
  uint32_t r6b = Rlwimi(r31, r31, 31, 13, 31);
  r6b = Rlwimi(r6b, r31, 31, 1, 11);
  uint32_t r10 = (Rlwinm(r6b, 13, 20, 31) & lim) + (b & ~lim);
  r10 = Rlwimi(r10, r29, 0, 0, 29);
  St32(base, fc + 16, r10);
  St64(base, dev + 24, Ld64(base, dev + 24) | m);
}

// sub_82A9AF70 for a call that needs no command buffer packet (checked by the caller).
inline void SetTexture(uint8_t* base, uint32_t dev, uint32_t s, uint32_t tex, uint64_t m) {
  const uint32_t slot = dev + 4u * (s + 3138u);
  const uint32_t old = Ld32(base, slot);
  if (tex) {
    const uint32_t fc = Fc(dev, s);
    const uint32_t t48 = Ld32(base, tex + 48), t32 = Ld32(base, tex + 32), t44 = Ld32(base, tex + 44);
    const uint32_t t28 = Ld32(base, tex + 28), t36 = Ld32(base, tex + 36), t40 = Ld32(base, tex + 40);
    const uint32_t f0 = Ld32(base, fc + 0), f1 = Ld32(base, fc + 4), f3 = Ld32(base, fc + 12);
    const uint32_t f4 = Ld32(base, fc + 16), f5 = Ld32(base, fc + 20);
    const uint32_t hi48 = (Rlwinm(t48, 12, 20, 31) + 512u) & 0x1000u;
    const uint32_t hi32 = (Rlwinm(t32, 12, 20, 31) + 512u) & 0x1000u;
    const uint32_t w5 = Rlwimi(hi48 + Rlwinm(t48, 0, 3, 22), f5, 0, 23, 31);
    const uint32_t w1 = Rlwimi(hi32 + (t32 & 0x1FFFFFFFu), f1, 0, 20, 20);
    uint32_t w4 = Rlwimi(t44, f4, 0, 30, 21);
    const uint32_t w0 = Rlwimi(t28, f0, 0, 10, 21);
    const uint32_t w3 = Rlwimi(t40, f3, 0, 1, 12);
    const uint32_t lod_lo = std::max(Rlwinm(t44, 30, 28, 31), uint32_t(Ld8(base, dev + s + 11958)));
    const uint32_t lod_hi = std::min(Rlwinm(t44, 26, 28, 31), uint32_t(Ld8(base, dev + s + 11984)));
    w4 = Rlwimi(w4, lod_lo, 2, 26, 29);
    w4 = Rlwimi(w4, lod_hi, 6, 22, 25);
    St32(base, fc + 8, t36);
    St32(base, fc + 0, w0);
    St32(base, fc + 4, w1);
    St32(base, fc + 12, w3);
    St32(base, fc + 20, w5);
    St32(base, fc + 16, w4);
    St64(base, dev + 24, Ld64(base, dev + 24) | m);
  }
  St32(base, slot, tex);
  if (old) {
    const uint32_t fence = Ld32(base, dev + 10908);
    if (fence) St32(base, old + 8, fence);
  }
}

// Whether SetTexture would write a command buffer packet (the declined path).
inline bool NeedsPacket(const uint8_t* base, uint32_t dev, uint32_t s) {
  const uint32_t old = Ld32(base, dev + 4u * (s + 3138u));
  return old && Ld32(base, dev + 10908) == 0 && (Ld32(base, dev + 10912) & Ld32(base, old)) != 0;
}

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32, p = ctx.r4.u32, t = ctx.r5.u32;
  const uint32_t dev = Ld32(base, kDevicePtr);
  if (s >= 32 || !dev || NeedsPacket(base, dev, s)) return false;
  const uint64_t m = (uint64_t(1) << 63) >> (s + 32);
  SetTexture(base, dev, s, Ld32(base, t + 8), m);
  Filter<false>(base, dev, s, Ld32(base, p + 4), m);
  Filter<true>(base, dev, s, Ld32(base, p + 8), m);
  const uint32_t fc = Fc(dev, s);
  St32(base, fc + 12, Rlwimi(Ld32(base, fc + 12), Ld32(base, p + 12), 23, 7, 8));
  uint32_t w0 = Ld32(base, fc + 0);
  w0 = Rlwimi(w0, Ld32(base, p + 16), 10, 19, 21);
  w0 = Rlwimi(w0, Ld32(base, p + 20), 13, 16, 18);
  w0 = Rlwimi(w0, Ld32(base, p + 24), 16, 13, 15);
  St32(base, fc + 0, w0);
  St32(base, fc + 20, Ld32(base, fc + 20) | 24u);
  St64(base, dev + 24, Ld64(base, dev + 24) | m);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const uint32_t s = ctx.r3.u32;
  const uint32_t dev = Ld32(base, kDevicePtr);
  if (s >= 32 || !dev) return;
  w.Add(Fc(dev, s), 24);
  w.Add(dev + 24, 8);
  w.Add(dev + 4u * (s + 3138u), 4);
  const uint32_t old = Ld32(base, dev + 4u * (s + 3138u));
  if (old) w.Add(old + 8, 4);
}

}  // namespace me::hot::n_827DB288
