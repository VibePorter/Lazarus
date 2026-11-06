// ME2 (2026-11-06): UE3's static depth-stencil / render-state setter sub_827DB818(state r3) with the nine XDK render-state
// setters it calls inlined. Console stacks of 2026-11-06 (Kasumi party heavy spot, UE3 render thread at 95 % of core 0):
// 827DB818 1.7-1.9 % self (LTO had already inlined the setters), flat over its ~2400 instructions: nine calls, each
// re-reading the device's stencil shadow and writing the same four words and the dirty mask again.
//
// sub_827DB818(s): dev = [0x833D074C], then in this order (each setter reads what the previous one wrote):
//   82A9B888(dev, [s+4])  82A9CAA0(dev, [s+4])  82A9B918(dev, [s+8])   82A9B9A8(dev, [s+12])  82A9BA38(dev, [s+16])
//   82A9BC18(dev, [s+20]) 82A9BAC8(dev, [s+24]) 82A9BB38(dev, [s+28])  82A9BBA8(dev, [s+32])
//   dev+10556 = rlwimi(dev+10556, [s+36], 3, 28, 28); dirty |= 0x40200
//   dev+10556 = rlwimi([s+40], dev+10556, 0, 0, 28);  dirty |= 0x200
//   dev+10500 = float(double(u32 [s+44])) * [0x8338798C] (fcfid, frsp, fmuls); dirty |= 0x08000000
// The setters (dev words: W = +11856 stencil/depth shadow, F = +11860 flags, the "commit" = the same value v to +10552,
// +10584, +10588, +10592 and dirty (dev+16, 64-bit) |= 0x407):
//   B888: F = rlwimi(F, x, 31, 0, 0); v = F bit 1 ? W : Pack(W); x == 0 -> v = 0x10001; commit v
//   CAA0: +12036 = x; [+12448] == 0 -> done; r8 = +10372, k = (r8 >> 16) & 15; k not 2/3/10/12 -> done;
//         ((r8 >> 19) & 1) == x -> done; +10372 = see below; dirty |= 1 << 56
//   B918 / B9A8 / BA38: W = rlwimi(W, x, sh, mb, me) (5,24,26 / 0,27,31 / 8,19,23); F bit 0 clear -> done;
//         v = F bit 1 ? W : Pack(W); commit v
//   BC18: F = rlwimi(F, x, 30, 1, 1); v = x != 0 ? W : Pack(W); F bit 1 clear -> v = 0x10001; commit v
//   BAC8 / BB38 / BBA8: W = rlwimi(W, x, sh, mb, me) (21,8,10 / 16,11,15 / 24,3,7); F bits 0 and 1 both set -> commit W
// The native keeps W / F / the commit value in registers and stores each word once at the end: the same final memory
// (single thread; dirty is the OR of every setter's bits). The original's guest stack frame (back chain, LR slot, the
// fcfid temporary at r1+80) is scratch below the caller's stack pointer and not written.
// Registers: the originals leave r3-r12 / f0-f13 / cr as scratch. tools/hot/liveness_me2.py: 118 direct call sites; the two
// it reports (r4 in sub_82743010, r10 in sub_8267D090) are `subfe rX,rY,rY`, which reads only the carry (a local of the
// caller here). One pointer to it in data (0x82269578): callers through a pointer cannot rely on scratch values.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_827DB818 {

inline constexpr Cmp kCmp = {0, 0, 0};

inline constexpr uint32_t kDevicePtr = 0x833D074Cu;
inline constexpr uint32_t kScale = 0x8338798Cu;  // lis r4,-31944; lfs f0,31116(r4)

// PPC rotate-and-mask on 32 bits (bit 0 = most significant); only low words reach the stores.
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

// The setters' repacking of the shadow word when flag bit 1 is clear (andi. 4112 / rlwinm 4,12,27 / clrlwi 16 / or /
// rlwinm 12,0,15 / rlwinm 0,12,10 / rlwinm 0,4,2 / or).
constexpr uint32_t Pack(uint32_t w) {
  uint32_t r10 = (w & 4112u) | Rlwinm(w, 4, 12, 27);
  r10 = Rlwinm(r10, 12, 0, 15);
  r10 = Rlwinm(r10, 0, 12, 10);
  r10 = Rlwinm(r10, 0, 4, 2);
  return r10 | (w & 0xFFFFu);
}

struct State {
  uint32_t w, f;           // +11856, +11860
  bool commit = false;     // the four words get `v`
  uint32_t v = 0;
  uint64_t dirty = 0;
  void Commit(uint32_t value) {
    commit = true;
    v = value;
    dirty |= 0x407u;
  }
};

// B918 / B9A8 / BA38: shadow insert; commit (repacked unless flag bit 1) when flag bit 0 is set.
template <uint32_t kSh, uint32_t kMb, uint32_t kMe>
inline void ShadowSet(State& st, uint32_t x) {
  st.w = Rlwimi(st.w, x, kSh, kMb, kMe);
  if (!(st.f & 0x80000000u)) return;
  st.Commit((st.f & 0x40000000u) ? st.w : Pack(st.w));
}

// BAC8 / BB38 / BBA8: shadow insert; commit the word as it is when flag bits 0 and 1 are both set.
template <uint32_t kSh, uint32_t kMb, uint32_t kMe>
inline void ShadowSetRaw(State& st, uint32_t x) {
  st.w = Rlwimi(st.w, x, kSh, kMb, kMe);
  if (!(st.f & 0x80000000u) || !(st.f & 0x40000000u)) return;
  st.Commit(st.w);
}

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32;
  const uint32_t dev = Ld32(base, kDevicePtr);
  if (!dev) return false;
  // me2-nx-deko (2026-11-06): 82A9CAA0's rewrite of +10372 (its "dirty |= 1 << 56" branch) is left to the original. The
  // guard reported the same difference twice with identical inputs (console 11-06 01:07 and 03:40: dev+16 bit 56 set by
  // the original, not by the native), so the branch below does not reproduce it in some case not yet understood. Its
  // condition reads only words no earlier step writes (+12448, +10372, [s + 4]): decided before any store.
  {
    const uint32_t x = Ld32(base, s + 4);
    if (Ld32(base, dev + 12448) != 0) {
      const uint32_t r8 = Ld32(base, dev + 10372);
      const uint32_t k = Rlwinm(r8, 16, 28, 31);
      if ((k == 2 || k == 3 || k == 10 || k == 12) && (Rlwinm(r8, 13, 31, 31) ^ x) != 0) return false;
    }
  }
  State st{Ld32(base, dev + 11856), Ld32(base, dev + 11860)};
  // 82A9B888
  {
    const uint32_t x = Ld32(base, s + 4);
    st.f = Rlwimi(st.f, x, 31, 0, 0);
    uint32_t v = (st.f & 0x40000000u) ? st.w : Pack(st.w);
    if (int32_t(x) == 0) v = 0x10001u;
    st.Commit(v);
  }
  // 82A9CAA0 (the same x, [s+4])
  {
    const uint32_t x = Ld32(base, s + 4);
    St32(base, dev + 12036, x);
    if (Ld32(base, dev + 12448) != 0) {
      const uint32_t r8 = Ld32(base, dev + 10372);
      const uint32_t k = Rlwinm(r8, 16, 28, 31);
      if ((k == 2 || k == 3 || k == 10 || k == 12) && (Rlwinm(r8, 13, 31, 31) ^ x) != 0) {
        const uint32_t r9 = Rlwinm(k, 31, 1, 31) - 3u;  // 64-bit in the original: only its low word is used below
        const uint32_t xm1 = x - 1u;
        const uint32_t r7 = Rlwinm(~xm1, 16, 0, 15);
        const uint32_t r11 = Rlwinm(k + 3u, 17, 0, 14);
        const uint32_t r10 = Rlwinm(r9 & xm1, 16, 0, 15);
        const uint32_t w = Rlwimi((r7 & r11) | r10, r8, 0, 16, 11);
        St32(base, dev + 10372, w);
        st.dirty |= uint64_t(1) << 56;
      }
    }
  }
  ShadowSet<5, 24, 26>(st, Ld32(base, s + 8));    // 82A9B918
  ShadowSet<0, 27, 31>(st, Ld32(base, s + 12));   // 82A9B9A8
  ShadowSet<8, 19, 23>(st, Ld32(base, s + 16));   // 82A9BA38
  // 82A9BC18
  {
    const uint32_t x = Ld32(base, s + 20);
    st.f = Rlwimi(st.f, x, 30, 1, 1);
    uint32_t v = int32_t(x) != 0 ? st.w : Pack(st.w);
    if (!(st.f & 0x40000000u)) v = 0x10001u;
    st.Commit(v);
  }
  ShadowSetRaw<21, 8, 10>(st, Ld32(base, s + 24));  // 82A9BAC8
  ShadowSetRaw<16, 11, 15>(st, Ld32(base, s + 28)); // 82A9BB38
  ShadowSetRaw<24, 3, 7>(st, Ld32(base, s + 32));   // 82A9BBA8
  St32(base, dev + 11856, st.w);
  St32(base, dev + 11860, st.f);
  if (st.commit) {
    St32(base, dev + 10552, st.v);
    St32(base, dev + 10584, st.v);
    St32(base, dev + 10588, st.v);
    St32(base, dev + 10592, st.v);
  }
  // The tail of sub_827DB818.
  uint32_t r10 = Rlwimi(Ld32(base, dev + 10556), Ld32(base, s + 36), 3, 28, 28);
  r10 = Rlwimi(Ld32(base, s + 40), r10, 0, 0, 28);
  St32(base, dev + 10556, r10);
  const float scale = LdF32(base, kScale);
  const float v = float(double(Ld32(base, s + 44))) * scale;  // fcfid of the zero-extended word, frsp, fmuls
  StF32(base, dev + 10500, v);
  st.dirty |= 0x40200u | 0x08000000u;
  St64(base, dev + 16, Ld64(base, dev + 16) | st.dirty);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  (void)ctx;
  const uint32_t dev = Ld32(base, kDevicePtr);
  if (!dev) return;
  w.Add(dev + 16, 8);
  w.Add(dev + 10372, 4);
  w.Add(dev + 10500, 4);
  w.Add(dev + 10552, 8);      // +10552, +10556
  w.Add(dev + 10584, 12);     // +10584, +10588, +10592
  w.Add(dev + 11856, 8);      // +11856, +11860
  w.Add(dev + 12036, 4);
}

}  // namespace me::hot::n_827DB818
