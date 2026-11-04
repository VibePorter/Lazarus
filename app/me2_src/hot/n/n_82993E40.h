// ME2 (2026-10-27): UE3 TMap<FName, V>::Find, two instances with the same code and another pair size, from the second
// PGO profile (2 Ryujinx sessions on the Normandy, gameplay): the lookups of FMaterialInstance parameters by name
// (sub_82311C70 GetVectorValue -> sub_82993E40, sub_82311D40 GetScalarValue -> sub_825DB270), on the UE3 render thread.
//   sub_82993E40  pairs of 28 bytes (mulli r11,r11,28), 0.37 % of the arc counts, 135 M loop iterations
//   sub_825DB270  pairs of 16 bytes (rlwinm r11,r11,4,0,27), 0.19 %, 67 M
//
// r3 = TMap (Pairs.Data +0, Pairs.Num +4, Hash +12, HashSize +16), r4 = FName* (Index +0, Number +4).
//   if (!Hash || Num <= 0) return 0;
//   i = Hash[(HashSize - 1) & key.Index];            (the key's first word goes through a std/lwz pair at r1-16:
//   while (i != -1) {                                 callee scratch below r1, not reproduced)
//     p = Data + i * stride;                          (64-bit: lwz zero-extends i; mulli / rlwinm + add as below)
//     if (p->Key.Index == key.Index && p->Key.Number == key.Number) return p + 12;   (p->HashNext at +0)
//     i = p->HashNext;
//   }
//   return 0;
// Loads use the low 32 bits of the address (the generated code's u32 effective addresses); r3 keeps the 64-bit sum the
// original computes. No memory is written. The original leaves r5-r11 / cr6 as scratch: every one of the 8 + 36 direct
// call sites reads only r3 (tools/hot/liveness_me2.py), no tail calls, no pointers to them in the image.
#pragma once
#include "../me_hot_common.h"

namespace me::hot::tmap_find {
inline constexpr Cmp kCmp = {R(3), 0, 0};

template <uint32_t kStride>
inline void Find(PPCContext& ctx, const uint8_t* base) {
  const uint32_t map = ctx.r3.u32, key = ctx.r4.u32;
  const uint32_t hash = Ld32(base, map + 12);
  if (hash == 0 || int32_t(Ld32(base, map + 4)) <= 0) {
    ctx.r3.u64 = 0;
    return;
  }
  const uint32_t k0 = Ld32(base, key);
  const uint32_t mask = Ld32(base, map + 16) - 1;
  uint32_t i = Ld32(base, hash + ((mask & k0) << 2));
  if (i == 0xFFFFFFFFu) {
    ctx.r3.u64 = 0;
    return;
  }
  const uint32_t data = Ld32(base, map);
  const uint32_t k1 = Ld32(base, key + 4);
  for (;;) {
    uint64_t p;
    if constexpr (kStride == 16)
      p = uint64_t(uint32_t(i << 4)) + data;  // rlwinm r11,r11,4,0,27 (32-bit result), add
    else
      p = uint64_t(i) * kStride + data;  // mulli on the zero-extended index, add
    const uint32_t e = uint32_t(p);
    if (Ld32(base, e + 4) == k0 && Ld32(base, e + 8) == k1) {
      ctx.r3.u64 = p + 12;
      return;
    }
    i = Ld32(base, e);
    if (i == 0xFFFFFFFFu) {
      ctx.r3.u64 = 0;
      return;
    }
  }
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}  // reads only
}  // namespace me::hot::tmap_find

namespace me::hot::n_82993E40 {
inline constexpr Cmp kCmp = tmap_find::kCmp;
inline void Native(PPCContext& ctx, uint8_t* base) { tmap_find::Find<28>(ctx, base); }
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) { tmap_find::Writes(ctx, base, w); }
}  // namespace me::hot::n_82993E40

namespace me::hot::n_825DB270 {
inline constexpr Cmp kCmp = tmap_find::kCmp;
inline void Native(PPCContext& ctx, uint8_t* base) { tmap_find::Find<16>(ctx, base); }
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) { tmap_find::Writes(ctx, base, w); }
}  // namespace me::hot::n_825DB270
