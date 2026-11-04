// ME2 (2026-10-27): XDK XMemCpy family, found from the PGO profile (Ryujinx sessions, gcda arc counters mapped back to
// guest functions): the 1 KB vector copy loops sub_82C4D090 / sub_82C4CBC4 under them were the 4th and 6th hottest
// guest functions (47 M and 44 M 1 KB iterations over the six sessions). Not in ME1's (cinnamon's) set.
//
//   sub_82C474A8  XMemCpy: r3 = dst, r4 = src, r5 = n (32-bit), returns r3 = dst (the original 64-bit r3, mr r3,r26).
//                 n < 16: 8-byte / 4-byte head copies and a byte loop that subtracts a global word (lwz r8,-26396 of
//                 0x8233F8D0) per byte: DECLINED here (the original runs). n >= 16: an unaligned 16-byte head (lvx/lvx/
//                 lvsl/vperm, stvlx up to 16-byte dst alignment), 16-byte vperm steps up to 128-byte dst alignment,
//                 then 128-byte blocks (aligned src: lvx/stvx with dcbzl of the line one block ahead while more than
//                 256 bytes remain; unaligned src: sub_82C4D940), 16-byte vperm steps and a stvrx tail.
//   sub_82C47348 / sub_82C473F8  XMemCpy for large copies (the second one with dcbf, write-combined targets):
//                 n < 256: CRT memcpy (sub_82D65BD0). Else memcpy of the head up to 128-byte dst alignment, the middle
//                 (n rounded down to 128 bytes) by a 1 KB-chunk vector loop (src 16-byte aligned: sub_82C4C8A0 /
//                 sub_82C4D090, else sub_82C4CBC4 / sub_82C4D420; every chunk loads all its bytes before its first
//                 store, dcbzl only on 128-byte lines inside the chunk), memcpy of the tail. Returns r3 = dst (mr r3,r27).
//
// Every path writes exactly [dst, dst + n) (the dcbzl lines all lie inside the range: they are zeroed and then fully
// stored) and reads [src, src + n) (the vector loads touch the 16-byte blocks around it; reads have no effect), so for
// ranges that do not overlap the result is a plain copy. Overlapping ranges, ranges that wrap around 4 GB and (PC only)
// ranges that cross 0xE0000000 with the +0x1000 physical offset are DECLINED (nothing modified, the original runs).
// Registers: r3 unchanged; the originals leave r4-r12 / v0-v13 as scratch. call-site liveness (cinnamon's liveness.py on
// ME2's generated code): no direct call site of the three reads a volatile register other than r3 after the call (two
// read r3; sub_82C474A8 is also tail-called from three functions).
#pragma once

#include <cstring>

#include "../me_hot_common.h"

namespace me::hot {
inline constexpr Cmp kCmpR3 = {R(3), 0, 0};
}  // namespace me::hot

namespace me::hot::xmemcpy {

// Only r3 is returned: the originals leave v0-v13 (and f1, never touched) as scratch, so kCmpRet's v1 compare would
// report their leftover vector data (PC run 2026-10-27: the first guarded call of each hook differed only in v1).

inline bool Copy(PPCContext& ctx, uint8_t* base, uint32_t min_n) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n < min_n) return false;
  if (n == 0) return true;
  if (uint64_t(dst) + n > 0x100000000ull || uint64_t(src) + n > 0x100000000ull) return false;
  if (uint64_t(dst) < uint64_t(src) + n && uint64_t(src) < uint64_t(dst) + n) return false;  // overlap
  if (PhysOff(dst) != PhysOff(dst + n - 1) || PhysOff(src) != PhysOff(src + n - 1)) return false;
  std::memcpy(Raw(base, dst), Raw(base, src), n);
  MarkCards(base, dst, n);  // ME2: the store cards
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  w.Add(ctx.r3.u32, ctx.r5.u32);  // > 1 MB sets overflow (the guard then runs the original unchecked)
}

}  // namespace me::hot::xmemcpy

namespace me::hot::n_82C474A8 {
inline constexpr Cmp kCmp = kCmpR3;
inline bool Native(PPCContext& ctx, uint8_t* base) { return xmemcpy::Copy(ctx, base, 16); }
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) { xmemcpy::Writes(ctx, base, w); }
}  // namespace me::hot::n_82C474A8

namespace me::hot::n_82C47348 {
inline constexpr Cmp kCmp = kCmpR3;
inline bool Native(PPCContext& ctx, uint8_t* base) { return xmemcpy::Copy(ctx, base, 0); }
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) { xmemcpy::Writes(ctx, base, w); }
}  // namespace me::hot::n_82C47348

namespace me::hot::n_82C473F8 {
inline constexpr Cmp kCmp = kCmpR3;
inline bool Native(PPCContext& ctx, uint8_t* base) { return xmemcpy::Copy(ctx, base, 0); }
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) { xmemcpy::Writes(ctx, base, w); }
}  // namespace me::hot::n_82C473F8
