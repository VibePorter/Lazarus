// ME2 (2026-10-27): Wwise fixed-point linear resampler, mono s16 -> float (reached through the pointer table at
// 0x822901B0; the stereo variant sub_82E5A180 is the next entry). Found from the PGO profile: the 3rd hottest guest
// function (3.1 % of the arc counts); its vector loop is very slow when recompiled (4 vector spills with byte swaps,
// 8 lvlx gathers through the mask table and 32 single-byte stores per 4 outputs).
//
//   r3 = source {[+0] s16 sample base, [+14] u16 frames available `avail`}, r4 = output {[+0] float base,
//   [+14] u16 <- frames written}, r5 = output capacity, r6 = state st {[+0] u16 previous sample `hist`, [+24] source
//   offset (frames), [+28] output offset, [+32] position (16.16 fixed point), [+36] step (16.16)}. Returns r3 = 43 (more
//   output room) or 45 (output full).
//   src16 = [r3] + 2 * st24 - 2 (index 0 = the sample before the frame at st24; index 0 reads as `hist` in part 1).
//   rem = r5 - st28; n1 = min((step - pos + 0xFFFF) / step, rem)                    (outputs that still use hist)
//   n2 = min(rem - n1, ((avail << 16) - pos1 + step - 1) / step)                     (pos1 = pos after part 1)
//   Part 1 (n1, scalar): v = int32((s[1] - hist) * frac + (hist << 16)); out = float(float(v) * 2^-31) (fcfid, frsp,
//     fmuls by the .rdata float at 0x8205EA00 = 2^-31); pos += step, frac = pos & 0xFFFF.
//   Part 2 (n2 / 4 blocks, VMX, FZ=1): lanes p_j = pos + j * step; per block s0 = src16[p >> 16], s1 = the next
//     sample, out_j = (s1 * 2^-15 - s0 * 2^-15) * (frac_j * 2^-16) + s0 * 2^-15 (vmulfp then vaddfp: two roundings),
//     p_j += 4 * step with unsigned saturation (vadduws); 16-byte unaligned store (stvlx/stvrx).
//   Part 3 (n2 & 3, or all of n2 when n2 < 4; scalar): the part 1 formula with s0 = src16[pos >> 16] (as u16 << 16)
//     and s1 = the next sample.
//   Epilogue: k = min((pos >> 16) & 0xFFFF, avail); if k: st0 = src16[k]; st32 = pos - (k << 16);
//     [r3+14] -= k; total = n1 + n2; [r4+14] = total + st28; st24 = (k == avail) ? 0 : st24 + k;
//     total == rem ? r3 = 45 : (st28 += total, r3 = 43).
// FPSCR: lfs of the constant clears the flush mode on every path; the vector part sets it; every part 3 sample clears
// it again. Final state reproduced: cleared unless the vector part ran and part 3 did not.
// Exactness: same integer arithmetic (mullw = 64-bit product of the 32-bit operands, extsw of the low word, u32
// position wrap in the scalar parts, saturation in the vector part); the float steps are the recompiled ones, written
// as separate statements (no contraction: -ffp-contract=off on the Switch, separate statements elsewhere).
// Not reproduced: the guest stack scratch below r1 (index spills, the fcfid round trip) and the volatile registers the
// original leaves behind (only r3 is compared: the function is only called through a pointer, so its callers treat
// every volatile register as clobbered).
// Declined (nothing modified, the original runs): step == 0 (the original traps), an odd sample base (lvlx of a
// halfword split across 16-byte blocks), more than 2^20 outputs.
#pragma once

#include <cstdint>

#include "../me_hot_common.h"

namespace me::hot::wwise_resample {

inline constexpr uint32_t kScale = 0x8205EA00u;  // float 2^-31 in .rdata

struct Plan {
  uint32_t st, pos, step, avail, outoff, rem, n1, n2, src16, out;
};

inline bool MakePlan(const PPCContext& ctx, const uint8_t* base, uint32_t frame_bytes, Plan& p) {
  p.st = ctx.r6.u32;
  p.pos = Ld32(base, p.st + 32);
  p.step = Ld32(base, p.st + 36);
  p.outoff = Ld32(base, p.st + 28);
  p.avail = Ld16(base, ctx.r3.u32 + 14);
  if (p.step == 0) return false;
  p.rem = ctx.r5.u32 - p.outoff;
  p.n1 = (p.step - p.pos + 0xFFFFu) / p.step;
  if (p.rem < p.n1) p.n1 = p.rem;
  const uint32_t pos1 = p.pos + p.n1 * p.step;
  const uint32_t lim = ((p.avail << 16) - pos1 + p.step - 1u) / p.step;
  p.n2 = p.rem - p.n1;
  if (!(p.n2 < lim)) p.n2 = lim;
  if (uint64_t(p.n1) + p.n2 > (1u << 20)) return false;
  p.src16 = Ld32(base, ctx.r3.u32) + frame_bytes * Ld32(base, p.st + 24) - frame_bytes;
  p.out = Ld32(base, ctx.r4.u32) + p.outoff * 4u;
  return true;
}

// fcfid, frsp, fmuls by the 2^-31 constant
inline float Scalar(int32_t v, float k) {
  const float a = float(double(v));
  return float(double(a) * double(k));
}

inline int32_t Interp(int32_t diff, uint32_t frac, uint32_t s0_hi) {
  const uint64_t prod = uint64_t(int64_t(diff) * int64_t(int32_t(frac)));
  return int32_t(uint32_t(prod + s0_hi));
}

inline uint32_t SatAdd(uint32_t a, uint32_t b) {
  const uint32_t s = a + b;
  return s < a ? 0xFFFFFFFFu : s;
}

}  // namespace me::hot::wwise_resample

namespace me::hot::n_82E59DF8 {

inline constexpr Cmp kCmp = {R(3), 0, 0};

inline bool Native(PPCContext& ctx, uint8_t* base) {
  using namespace wwise_resample;
  Plan p;
  if (!MakePlan(ctx, base, 2, p)) return false;
  if (p.src16 & 1) return false;
  const uint32_t s = p.src16;
  ctx.fpscr.disableFlushMode();
  const float k = LdF32(base, kScale);
  uint32_t pos = p.pos;
  uint32_t o = p.out;
  if (p.n1) {  // part 1: hist and the first sample
    const uint16_t hist = Ld16(base, p.st);
    const int32_t diff = int32_t(int16_t(Ld16(base, s + 2))) - int32_t(int16_t(hist));
    const uint32_t hi = uint32_t(hist) << 16;
    uint32_t frac = pos & 0xFFFFu;
    for (uint32_t i = 0; i < p.n1; ++i) {
      const int32_t v = Interp(diff, frac, hi);
      pos += p.step;
      frac = pos & 0xFFFFu;
      StF32(base, o, Scalar(v, k));
      o += 4;
    }
  }
  uint32_t n3 = p.n2;
  if (p.n2 >= 4) {  // part 2: four lanes per block
    ctx.fpscr.enableFlushMode();
    uint32_t lane[4] = {pos, pos + p.step, pos + 2u * p.step, pos + 3u * p.step};
    const uint32_t step4 = p.step << 2;
    const float kS16 = 1.0f / 32768.0f, kFrac = 1.0f / 65536.0f;  // 0x38000000, 0x37800000
    float f[4];
    for (int j = 0; j < 4; ++j) f[j] = float(lane[j] & 0xFFFFu) * kFrac;
    for (uint32_t b = 0, nb = p.n2 >> 2; b < nb; ++b) {
      float out[4];
      for (int j = 0; j < 4; ++j) {
        const uint32_t off = (lane[j] >> 16) << 1;
        const float s0 = float(int32_t(int16_t(Ld16(base, s + off)))) * kS16;
        const float s1 = float(int32_t(int16_t(Ld16(base, s + off + 2u)))) * kS16;
        const float d = s1 - s0;
        const float m = d * f[j];
        out[j] = m + s0;
        lane[j] = SatAdd(lane[j], step4);
        f[j] = float(lane[j] & 0xFFFFu) * kFrac;
      }
      for (int j = 0; j < 4; ++j) StF32(base, o + 4u * j, out[j]);
      o += 16;
    }
    pos = lane[0];
    n3 = p.n2 & 3u;
  }
  if (n3) {  // part 3: scalar tail
    ctx.fpscr.disableFlushMode();
    for (uint32_t i = 0; i < n3; ++i) {
      const uint32_t a = s + (((pos >> 16) & 0xFFFFu) << 1);
      const uint32_t frac = pos & 0xFFFFu;
      pos += p.step;
      const uint16_t s0 = Ld16(base, a);
      const int32_t diff = int32_t(int16_t(Ld16(base, a + 2))) - int32_t(int16_t(s0));
      StF32(base, o, Scalar(Interp(diff, frac, uint32_t(s0) << 16), k));
      o += 4;
    }
  }
  // epilogue
  uint32_t kc = (pos >> 16) & 0xFFFFu;
  if (!(kc < p.avail)) kc = p.avail;
  if (kc) St16(base, p.st, Ld16(base, s + (kc << 1)));
  St32(base, p.st + 32, pos - (kc << 16));
  St16(base, ctx.r3.u32 + 14, uint16_t(Ld16(base, ctx.r3.u32 + 14) - kc));
  const uint32_t total = p.n1 + p.n2;
  St16(base, ctx.r4.u32 + 14, uint16_t(total + Ld32(base, p.st + 28)));
  St32(base, p.st + 24, kc == p.avail ? 0u : Ld32(base, p.st + 24) + kc);
  if (total == p.rem) {
    ctx.r3.u64 = 45;
  } else {
    St32(base, p.st + 28, Ld32(base, p.st + 28) + total);
    ctx.r3.u64 = 43;
  }
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  wwise_resample::Plan p;
  if (!wwise_resample::MakePlan(ctx, base, 2, p)) return;  // declined: nothing written
  w.Add(p.out, 4u * (p.n1 + p.n2));
  w.Add(p.st, 2);
  w.Add(p.st + 24, 12);
  w.Add(ctx.r3.u32 + 14, 2);
  w.Add(ctx.r4.u32 + 14, 2);
}

}  // namespace me::hot::n_82E59DF8

// ---------------------------------------------------------------------------------------------------------------------
// sub_82E5A180 - the stereo variant (next entry of the pointer table, 0x822901B8): interleaved L/R s16 frames (4 bytes;
// src = [r3] + 4 * st24 - 4), st+0 / st+2 = previous L / R sample, two output planes: L at [r4] + 4 * st28, R at
// [r4] + 4 * (u16 [r4+12] + st28). Same parts, formulas, saturation, FPSCR sequence and epilogue as the mono version
// (the epilogue stores both previous samples). Store order kept: part 1 and part 3 write R before L, part 2 L before R.
// The vector part loads each frame with one lvlx: a frame at an address with (a & 15) == 14 would read R as 0, so a
// source base that is not 4-byte aligned is DECLINED (then no frame is ever split).
namespace me::hot::n_82E5A180 {

inline constexpr Cmp kCmp = {R(3), 0, 0};

inline bool Native(PPCContext& ctx, uint8_t* base) {
  using namespace wwise_resample;
  Plan p;
  if (!MakePlan(ctx, base, 4, p)) return false;
  if (p.src16 & 3) return false;
  const uint32_t s = p.src16;
  const uint32_t outL0 = p.out;
  const uint32_t outR0 = Ld32(base, ctx.r4.u32) + (uint32_t(Ld16(base, ctx.r4.u32 + 12)) + p.outoff) * 4u;
  ctx.fpscr.disableFlushMode();
  const float k = LdF32(base, kScale);
  uint32_t pos = p.pos;
  uint32_t oL = outL0, oR = outR0;
  if (p.n1) {  // part 1: the previous frame and the first frame
    const uint16_t hL = Ld16(base, p.st), hR = Ld16(base, p.st + 2);
    const int32_t dL = int32_t(int16_t(Ld16(base, s + 4))) - int32_t(int16_t(hL));
    const int32_t dR = int32_t(int16_t(Ld16(base, s + 6))) - int32_t(int16_t(hR));
    const uint32_t hiL = uint32_t(hL) << 16, hiR = uint32_t(hR) << 16;
    uint32_t frac = pos & 0xFFFFu;
    for (uint32_t i = 0; i < p.n1; ++i) {
      const int32_t vL = Interp(dL, frac, hiL);
      const int32_t vR = Interp(dR, frac, hiR);
      pos += p.step;
      frac = pos & 0xFFFFu;
      StF32(base, oR, Scalar(vR, k));
      oR += 4;
      StF32(base, oL, Scalar(vL, k));
      oL += 4;
    }
  }
  uint32_t n3 = p.n2;
  if (p.n2 >= 4) {
    ctx.fpscr.enableFlushMode();
    uint32_t lane[4] = {pos, pos + p.step, pos + 2u * p.step, pos + 3u * p.step};
    const uint32_t step4 = p.step << 2;
    const float kS16 = 1.0f / 32768.0f, kFrac = 1.0f / 65536.0f;
    float f[4];
    for (int j = 0; j < 4; ++j) f[j] = float(lane[j] & 0xFFFFu) * kFrac;
    for (uint32_t b = 0, nb = p.n2 >> 2; b < nb; ++b) {
      float outL[4], outR[4];
      for (int j = 0; j < 4; ++j) {
        const uint32_t a = s + ((lane[j] >> 16) << 2);
        const float l0 = float(int32_t(int16_t(Ld16(base, a)))) * kS16;
        const float r0 = float(int32_t(int16_t(Ld16(base, a + 2u)))) * kS16;
        const float l1 = float(int32_t(int16_t(Ld16(base, a + 4u)))) * kS16;
        const float r1 = float(int32_t(int16_t(Ld16(base, a + 6u)))) * kS16;
        const float dl = l1 - l0;
        const float dr = r1 - r0;
        const float ml = dl * f[j];
        const float mr = dr * f[j];
        outL[j] = ml + l0;
        outR[j] = mr + r0;
        lane[j] = SatAdd(lane[j], step4);
        f[j] = float(lane[j] & 0xFFFFu) * kFrac;
      }
      for (int j = 0; j < 4; ++j) StF32(base, oL + 4u * j, outL[j]);
      for (int j = 0; j < 4; ++j) StF32(base, oR + 4u * j, outR[j]);
      oL += 16;
      oR += 16;
    }
    pos = lane[0];
    n3 = p.n2 & 3u;
  }
  if (n3) {
    ctx.fpscr.disableFlushMode();
    for (uint32_t i = 0; i < n3; ++i) {
      const uint32_t a = s + (((pos >> 16) & 0xFFFFu) << 2);
      const uint32_t frac = pos & 0xFFFFu;
      pos += p.step;
      const uint16_t l0 = Ld16(base, a), r0 = Ld16(base, a + 2);
      const int32_t dl = int32_t(int16_t(Ld16(base, a + 4))) - int32_t(int16_t(l0));
      const int32_t dr = int32_t(int16_t(Ld16(base, a + 6))) - int32_t(int16_t(r0));
      const int32_t vL = Interp(dl, frac, uint32_t(l0) << 16);
      const int32_t vR = Interp(dr, frac, uint32_t(r0) << 16);
      StF32(base, oR, Scalar(vR, k));
      oR += 4;
      StF32(base, oL, Scalar(vL, k));
      oL += 4;
    }
  }
  uint32_t kc = (pos >> 16) & 0xFFFFu;
  if (!(kc < p.avail)) kc = p.avail;
  if (kc) {
    const uint32_t a = s + (kc << 2);
    const uint16_t l = Ld16(base, a), r = Ld16(base, a + 2);
    St16(base, p.st, l);
    St16(base, p.st + 2, r);
  }
  St32(base, p.st + 32, pos - (kc << 16));
  St16(base, ctx.r3.u32 + 14, uint16_t(Ld16(base, ctx.r3.u32 + 14) - kc));
  const uint32_t total = p.n1 + p.n2;
  St16(base, ctx.r4.u32 + 14, uint16_t(total + Ld32(base, p.st + 28)));
  St32(base, p.st + 24, kc == p.avail ? 0u : Ld32(base, p.st + 24) + kc);
  if (total == p.rem) {
    ctx.r3.u64 = 45;
  } else {
    St32(base, p.st + 28, Ld32(base, p.st + 28) + total);
    ctx.r3.u64 = 43;
  }
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  wwise_resample::Plan p;
  if (!wwise_resample::MakePlan(ctx, base, 4, p)) return;
  const uint32_t outR0 = Ld32(base, ctx.r4.u32) + (uint32_t(Ld16(base, ctx.r4.u32 + 12)) + p.outoff) * 4u;
  w.Add(p.out, 4u * (p.n1 + p.n2));
  w.Add(outR0, 4u * (p.n1 + p.n2));
  w.Add(p.st, 4);
  w.Add(p.st + 24, 12);
  w.Add(ctx.r3.u32 + 14, 2);
  w.Add(ctx.r4.u32 + 14, 2);
}

}  // namespace me::hot::n_82E5A180
