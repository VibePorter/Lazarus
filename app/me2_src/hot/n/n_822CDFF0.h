// ME2 sphere/frustum test. Adapted from Cinnamon's native plane-group testing
// (n_8262CB50.h), with ME2's sphere radius and exact instruction order.
// Six direct callers include InitViews and its primitive visibility loop.
// r3 -> {planes at +12, signed count at +16}, r4 -> BE float3 center, f1 = radius.
// Plane groups contain SoA float4 X/Y/Z/D, aligned down exactly as lvx does.
// Return 0 if any lane of (cx*X + cy*Y + cz*Z - D) > float(radius), otherwise 1.
// Keep separate mul/add rounding (no FMA). NaN comparisons remain ordered.
// stfs first disables flush mode, even for an empty frustum; the first VMX op
// enables it when count > 0. The original's parameter-save scratch at r1+36
// and dead vector/GPR scratch are not outputs. Call-site liveness: r3 is the
// only written register consumed; r0/r5/r6 are untouched in both versions.
#pragma once

#include "../me_hot_common.h"
#include <simde/arm/neon.h>

namespace me::hot::n_822CDFF0 {

inline constexpr Cmp kCmp = {R(3) | R(0) | R(5) | R(6), 0, 0};

inline simde_float32x4_t LoadPlanes(const uint8_t* p) {
  return simde_vreinterpretq_f32_u8(simde_vrev32q_u8(simde_vld1q_u8(p)));
}
inline simde_float32x4_t Splat(uint32_t bits) {
  return simde_vreinterpretq_f32_u32(simde_vdupq_n_u32(bits));
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t object = ctx.r3.u32;
  const int32_t count = int32_t(Ld32(base, object + 16));
  const uint32_t planes = Ld32(base, object + 12) & ~15u;
  ctx.fpscr.disableFlushMode();
  const float radius = float(ctx.f1.f64);
  ctx.r3.s64 = 1;
  if (count <= 0) return;
  const auto cx = Splat(Ld32(base, ctx.r4.u32));
  const auto cy = Splat(Ld32(base, ctx.r4.u32 + 4));
  const auto cz = Splat(Ld32(base, ctx.r4.u32 + 8));
  const auto limit = simde_vdupq_n_f32(radius);
  ctx.fpscr.enableFlushMode();
  uint32_t address = planes;
  for (int64_t i = 0; i < count; i += 4, address += 64) {
    const auto x = LoadPlanes(Raw(base, address));
    const auto y = LoadPlanes(Raw(base, uint32_t(address + 16)));
    const auto z = LoadPlanes(Raw(base, uint32_t(address + 32)));
    const auto d = LoadPlanes(Raw(base, uint32_t(address + 48)));
    const auto xy = simde_vaddq_f32(simde_vmulq_f32(cy, y), simde_vmulq_f32(cx, x));
    const auto xyz = simde_vaddq_f32(simde_vmulq_f32(cz, z), xy);
    const auto dist = simde_vsubq_f32(xyz, d);
    if (simde_vmaxvq_u32(simde_vcgtq_f32(dist, limit))) {
      ctx.r3.s64 = 0;
      return;
    }
  }
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_822CDFF0
