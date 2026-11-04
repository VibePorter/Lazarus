// ME2 (2026-10-27): CRT strncpy (8-bit), found from the PGO profile (Ryujinx sessions): 0.57 % of the guest arc counts,
// 728 M iterations of its loops (the zero padding of large destination buffers). The byte version of ME1's wcsncpy
// (cinnamon n_82AD9D80.h, same control flow).
//
//   r3 = dst, r4 = src, r5 = n (32-bit). r3 is never modified by the original (mr r11,r3); nothing else is returned.
//   n == 0: nothing. Else copy bytes (lbzu / stb) until a copied byte is 0 or n bytes were copied; after a copied zero
//   the remaining (n - c) bytes (c = bytes copied, terminator included) are zeroed (stbu loop). Net effect: exactly
//   [dst, dst + n) is written: c bytes of src, then zeros.
//
// Declined (nothing modified, the original runs): a destination that overlaps the c source bytes (the original's byte
// order would be observable), ranges that wrap around 4 GB, and (PC only) ranges that cross 0xE0000000 with the +0x1000
// physical offset. The terminator search reads at most n source bytes (memchr stops in the aligned block of the match).
// Registers: the original leaves r9-r11 and r5 as scratch; ME2 call-site liveness: none of the 24 direct call sites
// reads a volatile register after the call.
#pragma once

#include <cstring>

#include "../me_hot_common.h"

namespace me::hot::n_82D61750 {

inline constexpr Cmp kCmp = kCmpRet;

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n == 0) return true;
  if (uint64_t(dst) + n > 0x100000000ull || uint64_t(src) + n > 0x100000000ull) return false;
  if (PhysOff(dst) != PhysOff(dst + n - 1) || PhysOff(src) != PhysOff(src + n - 1)) return false;
  const uint8_t* s = Raw(base, src);
  const void* z = std::memchr(s, 0, n);
  const uint32_t c = z ? uint32_t(static_cast<const uint8_t*>(z) - s) + 1 : n;
  if (uint64_t(dst) < uint64_t(src) + c && uint64_t(src) < uint64_t(dst) + n) return false;  // overlap
  uint8_t* d = Raw(base, dst);
  MarkCards(base, dst, n);  // ME2: the store cards
  std::memcpy(d, s, c);
  if (c < n) std::memset(d + c, 0, n - c);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  w.Add(ctx.r3.u32, ctx.r5.u32);  // > 1 MB sets overflow
}

}  // namespace me::hot::n_82D61750
