// ME2 (2026-10-27): XDK D3D shader float constant setters, from the console stack profile of 22:2x (build 22:07, heavy
// Omega scene at 13.4 fps, the UE3 render thread X827BD330 100 % on core 0): 3.0 % + 1.0 % of that thread's own samples,
// the hottest XDK functions. The generated code emulates every lvlx/lvrx pair with two shuffle-table loads per float4.
//
//   sub_82A92C50  vertex shader constants: dst = dev + (start + 120) * 16, pending mask at dev + 0
//   sub_82A92D28  pixel shader constants:  dst = dev + (start + 376) * 16, pending mask at dev + 8
//
// r3 = device, r4 = start register, r5 = src (any alignment), r6 = float4 count (32-bit), r7 = dirty group bits (64-bit).
// The original copies count float4s (groups of 4 with all loads before the stores, then single ones; lvlx/lvrx of an
// address and of address + 16 OR'd = the 16 bytes at that address; stvx at dst & ~15), then ORs r7 into the 64-bit
// pending mask. For ranges that do not overlap the result is a plain copy, then the OR.
// DECLINED (the original runs): counts above 512, overlapping ranges, ranges that wrap around 4 GB or (PC only) cross
// 0xE0000000 with the +0x1000 physical offset.
// Registers: r3 unchanged; the original leaves r4-r11, ctr and v59-v63 (locals of the generated code) as scratch.
// tools/hot/liveness_me2.py: 13 + 11 direct call sites, one reads r7 after the call (sub_827DB3D0), which neither
// version modifies.
#pragma once

#include <cstring>

#include "../me_hot_common.h"

// me2-nx-deko (2026-11-07, Ryujinx captures 14505 / 15300: a light pass's vertex constants c237-c240 held a memory
// structure (pointers, small integers, NaN) instead of a matrix, and its pixels came out at 1e34 -> white blobs):
// me2_diag_bad_consts (me_hot_guest.cpp) logs uploads whose floats are NaN, infinite or huge, with the guest callers.
// -1 = not read yet, 0 = off, 1 = on.
extern "C" int8_t g_me2_bad_consts;
extern "C" void Me2BadConstsCheck(const PPCContext& ctx, const uint8_t* base, bool vertex, uint32_t src, uint32_t count);

namespace me::hot::shader_consts {

inline constexpr Cmp kCmp = {R(3) | R(7), 0, 0};

inline uint32_t Dst(const PPCContext& ctx, uint32_t base_reg) {
  return (ctx.r3.u32 + ((ctx.r4.u32 + base_reg) << 4)) & ~0xFu;
}

inline bool Set(PPCContext& ctx, uint8_t* base, uint32_t base_reg, uint32_t mask_off) {
  const uint32_t dev = ctx.r3.u32, src = ctx.r5.u32, count = ctx.r6.u32;
  if (count > 512) return false;
  const uint32_t dst = Dst(ctx, base_reg), n = count * 16;
  if (n) {
    if (uint64_t(dst) + n > 0x100000000ull || uint64_t(src) + n > 0x100000000ull) return false;
    if (uint64_t(dst) < uint64_t(src) + n && uint64_t(src) < uint64_t(dst) + n) return false;  // overlap
    if (PhysOff(dst) != PhysOff(dst + n - 1) || PhysOff(src) != PhysOff(src + n - 1)) return false;
#if !defined(ME_HOT_SHADOW_ACCESSORS)
    if (g_me2_bad_consts != 0) [[unlikely]]
      Me2BadConstsCheck(ctx, base, base_reg == 120, src, count);
#endif
    std::memcpy(Raw(base, dst), Raw(base, src), n);
    MarkCards(base, dst, n);  // ME2: the store cards
  }
  St64(base, dev + mask_off, Ld64(base, dev + mask_off) | ctx.r7.u64);
  return true;
}

inline void Writes(const PPCContext& ctx, me::hot::Writes& w, uint32_t base_reg, uint32_t mask_off) {
  const uint32_t count = ctx.r6.u32;
  if (count <= 512) w.Add(Dst(ctx, base_reg), count * 16);
  w.Add(ctx.r3.u32 + mask_off, 8);
}

}  // namespace me::hot::shader_consts

namespace me::hot::n_82A92C50 {
inline constexpr Cmp kCmp = shader_consts::kCmp;
inline bool Native(PPCContext& ctx, uint8_t* base) { return shader_consts::Set(ctx, base, 120, 0); }
inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) { shader_consts::Writes(ctx, w, 120, 0); }
}  // namespace me::hot::n_82A92C50

namespace me::hot::n_82A92D28 {
inline constexpr Cmp kCmp = shader_consts::kCmp;
inline bool Native(PPCContext& ctx, uint8_t* base) { return shader_consts::Set(ctx, base, 376, 8); }
inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) { shader_consts::Writes(ctx, w, 376, 8); }
}  // namespace me::hot::n_82A92D28
