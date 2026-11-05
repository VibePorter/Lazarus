// ME2 (2026-11-06): UE3's pixel shader parameter setters (SetPixelShaderValue / SetPixelShaderValues), from the
// console stack profile of 2026-11-05 23:5x (native visibility build, Omega): sub_822EB710 2.3 % + sub_822C8C10 2.2 %
// of the UE3 render thread's own samples. Thin wrappers: the value padded to whole float4s in a global scratch array
// when it is not a multiple of 16 bytes, then XDK SetPixelShaderConstantF (sub_82A92D28, n_82A92C50.h) for one
// register. Here the same, with the constant setter inlined (shader_consts::Set).
//
//   sub_822EB710(param r3, value r4): one float, at most 4 bytes
//     n = min(u16 [param + 4], 4); n == 0: nothing
//   sub_822C8C10(param r3, value r4, index r5): element `index` of a float4 array
//     off = (index << 4) (32 bits); n = min(int32(u16 [param + 4] - off), 16); n <= 0: nothing
//   pad = n & 15; if pad: the scratch TArray at 0x8340620C {data, num, max} (initialised once: bit 0 of 0x83406218)
//     holds the value: memcpy(data, value, n); memset(data + n, 0, pad); src = data   (else src = value)
//   start = (u16 [param + 2] + off) >> 4 (32 bits; off = 0 for 822EB710); count = (n + 15) >> 4
//   g0 = start >> 2; g1 = (start + count - 1) >> 2; mask = (0x8000000000000000 >>arith min(g1 - g0, 63)) >> g0
//     (0 if g0 & 0x40)
//   SetPixelShaderConstantF([0x833D074C], start, src, count, mask)
// DECLINED (decided before any write, the original runs): the scratch not initialised yet or too small (num < n +
// pad: it grows), value and scratch overlapping, and the constant setter's own declines (shader_consts::Set).
// Registers: tools/hot/liveness_me2.py (2026-11-06): 25 + 130 direct call sites, none reads a volatile register after
// the call; r3-r7 are left as the original leaves them anyway (the constant setter's arguments).
#pragma once

#include <cstring>

#include "../me_hot_common.h"
#include "n_82A92C50.h"

namespace me::hot::shader_params {

inline constexpr uint32_t kScratch = 0x8340620Cu;  // TArray<BYTE> {data, num, max}
inline constexpr uint32_t kScratchInit = 0x83406218u;
inline constexpr uint32_t kDevicePtr = 0x833D074Cu;

struct Plan {
  bool none = false;   // nothing to do (n <= 0)
  uint32_t n = 0, pad = 0, src = 0, data = 0, start = 0, count = 0, dev = 0;
  uint64_t mask = 0;
};

// The call's plan from its inputs (reads only). false = declined.
inline bool MakePlan(const PPCContext& ctx, const uint8_t* base, bool array, Plan& p) {
  const uint32_t param = ctx.r3.u32, value = ctx.r4.u32;
  const uint32_t bytes = Ld16(base, param + 4);
  uint32_t off = 0;
  int32_t n;
  if (array) {
    off = ctx.r5.u32 << 4;
    n = int32_t(bytes - off);
    if (n >= 16) n = 16;
  } else {
    n = int32_t(bytes);
    if (n >= 4) n = 4;
  }
  if (n <= 0) {
    p.none = true;
    return true;
  }
  p.n = uint32_t(n);
  p.pad = p.n & 15;
  p.src = value;
  if (p.pad) {
    if (!(Ld32(base, kScratchInit) & 1)) return false;
    p.data = Ld32(base, kScratch);
    if (Ld32(base, kScratch + 4) < p.n + p.pad) return false;
    const uint64_t d0 = p.data, d1 = d0 + p.n + p.pad, v0 = value, v1 = v0 + p.n;
    if (d1 > 0x100000000ull || v1 > 0x100000000ull) return false;
    if (d0 < v1 && v0 < d1) return false;  // overlap
    if (PhysOff(p.data) != PhysOff(uint32_t(d1 - 1)) || PhysOff(value) != PhysOff(uint32_t(v1 - 1))) return false;
    p.src = p.data;
  }
  p.start = (uint32_t(Ld16(base, param + 2)) + off) >> 4;
  p.count = (p.n + 15) >> 4;
  const uint32_t g0 = p.start >> 2, g1 = (p.start + p.count - 1) >> 2;
  uint32_t sh = (g1 - g0) & 0x7F;
  if (sh > 63) sh = 63;
  const uint64_t m = uint64_t(int64_t(0x8000000000000000ull) >> sh);
  p.mask = (g0 & 0x40) ? 0 : (m >> (g0 & 0x7F));
  p.dev = Ld32(base, kDevicePtr);
  // the constant setter's declines (shader_consts::Set), checked here before the scratch is written
  const uint32_t dst = (p.dev + ((p.start + 376) << 4)) & ~0xFu, len = p.count * 16;
  if (p.count > 512) return false;
  if (uint64_t(dst) + len > 0x100000000ull || uint64_t(p.src) + len > 0x100000000ull) return false;
  if (uint64_t(dst) < uint64_t(p.src) + len && uint64_t(p.src) < uint64_t(dst) + len) return false;
  if (PhysOff(dst) != PhysOff(dst + len - 1) || PhysOff(p.src) != PhysOff(p.src + len - 1)) return false;
  return true;
}

inline bool Run(PPCContext& ctx, uint8_t* base, bool array) {
  Plan p;
  if (!MakePlan(ctx, base, array, p)) return false;
  if (p.none) return true;
  if (p.pad) {
    std::memcpy(Raw(base, p.data), Raw(base, ctx.r4.u32), p.n);
    std::memset(Raw(base, p.data + p.n), 0, p.pad);
    MarkCards(base, p.data, p.n + p.pad);
  }
  ctx.r3.u64 = p.dev;
  ctx.r4.u64 = p.start;
  ctx.r5.u64 = p.src;
  ctx.r6.u64 = p.count;
  ctx.r7.u64 = p.mask;
  return shader_consts::Set(ctx, base, 376, 8);  // cannot decline: its conditions were checked above
}

inline void WritesOf(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w, bool array) {
  Plan p;
  if (!MakePlan(ctx, base, array, p) || p.none) return;
  if (p.pad) w.Add(p.data, p.n + p.pad);
  w.Add((p.dev + ((p.start + 376) << 4)) & ~0xFu, p.count * 16);
  w.Add(p.dev + 8, 8);
}

}  // namespace me::hot::shader_params

namespace me::hot::n_822EB710 {
inline constexpr Cmp kCmp = {0, 0, 0};
inline bool Native(PPCContext& ctx, uint8_t* base) { return shader_params::Run(ctx, base, false); }
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  shader_params::WritesOf(ctx, base, w, false);
}
}  // namespace me::hot::n_822EB710

namespace me::hot::n_822C8C10 {
inline constexpr Cmp kCmp = {0, 0, 0};
inline bool Native(PPCContext& ctx, uint8_t* base) { return shader_params::Run(ctx, base, true); }
inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  shader_params::WritesOf(ctx, base, w, true);
}
}  // namespace me::hot::n_822C8C10
