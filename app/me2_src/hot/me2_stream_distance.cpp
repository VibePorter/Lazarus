// ME2 (2026-10-28): native sub_823A3748, the game thread's hottest function in the console profile (Omega, prepass +
// latent Umbra: 7.2 % of the game thread's busy samples).
//
// What it does (from the disassembly): for every level of the world ([0x833D60B0]: +72 levels, +76 count) it walks the
// level's array at +172 (TArray of 20-byte entries: +4 primitive, +8 / +12 its bounding spheres {data, count}, 20 bytes
// each: x y z radius factor) and, for every sphere and every view point of 'this' ([this] data, [this+4] count, 20
// bytes each: x y z), computes
//     d2 = |sphere - view|^2 (x y z masked by the vector at 0x833EBDB0), q = (d2 - radius^2) * K1,
//     near = min(near, max(q, 0)), far = max(far, min(factor * (q * (1 / sqrt(q))), K2)),
// starting from near = K0 (0x8224A710), far = 0, then primitive+316 = min(near, old), primitive+312 = max(far, old).
// It runs only when the virtual at [[0x833A6E3C]]+16 says so or bit 0 of the u64 at 0x833A6E70 is set.
// (By its shape: the texture streaming distances of the primitives.)
//
// Why it is slow recompiled: the inner loop is VMX on whole vectors (two loads + vperm per unaligned point, the
// vmsum3fp dot product through simde's dp_ps, vrsqrtefp as a vector sqrt AND divide, min / max with SSE NaN semantics as
// compare + select), but only element 0 of the result is ever stored (stvewx of a splat).
//
// The native does element 0 alone, in scalar code that rounds exactly as the recompiled vector code (bit-exact):
//   - the host vector is the guest one byte-reversed (host lane i = guest element 3 - i); simde's dp_ps 0xEF on NEON
//     multiplies, zeroes lane 0 (guest w), then vaddvq: (lane0 + lane1) + (lane2 + lane3) = (0 + z*z) + (y*y + x*x);
//   - vrsqrtefp = 1.0f / sqrtf(q) (simde: div(set1(1), sqrt)); vmaxfp / vminfp = a > b ? a : b / a < b ? a : b with
//     a = the first operand (simde without SIMDE_FAST_NANS);
//   - all of it with flush-to-zero on (the original's vector ops enable it; at its end the flush mode is on when one
//     primitive was processed, as here), -ffp-contract=off (no fused multiply-add).
// The virtual is called as the original calls it (guest frame of 240 bytes below r1). The calls to sub_828B5BE8 after
// each level are skipped: it returns at once when the iterator's removed count (+12 of the iterator at r1+96, set to 0
// and never changed here) is 0.
//
// Self-check (me2_hot_stream_distance_checks): the first N calls and then 1 in 1024 run the native without writing
// (results kept aside, repeated primitives updated in order), then the original, and compare every word the native
// would have written with what the original wrote. A difference logs and switches the native off for good.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>

#include "me_hot_common.h"

REXCVAR_DEFINE_BOOL(me2_hot_stream_distance, true, "ME2",
                    "Native sub_823A3748 (per-primitive view distances over the level arrays, game thread), "
                    "self-checked")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_hot_stream_distance_checks, 64, "ME2",
                     "me2_hot_stream_distance: the first this many calls (then 1 in 1024) are checked against the "
                     "original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
using me::hot::Ld32;
using me::hot::St32;

constexpr uint32_t kGateObject = 0x833A6E3Cu;  // [ ]: object; its vtable +16 = the gate
constexpr uint32_t kGateFlag = 0x833A6E70u;    // u64, bit 0: run without asking the gate
constexpr uint32_t kWorld = 0x833D60B0u;       // [ ]: +72 levels, +76 level count
constexpr uint32_t kMask = 0x833EBDB0u;        // vector: the bits cleared in points (vsel)
constexpr uint32_t kNearInit = 0x8224A710u;    // vector constants (element 0 used)
constexpr uint32_t kScale = 0x8224A720u;
constexpr uint32_t kFarCap = 0x8224A730u;
constexpr uint32_t kGateReturn = 0x823A3770u;  // after the original's bctrl (+0x24)

inline float F(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
inline uint32_t Bits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

std::atomic<bool> g_off{false};
std::atomic<uint64_t> g_calls{0};

// The original's gate: the virtual (as its bctrl calls it), else the flag.
bool Gate(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = Ld32(base, kGateObject);
  const uint32_t fn = Ld32(base, Ld32(base, obj) + 16);
  PPCFunc* f = rex::runtime::ResolveIndirectFunction(fn);
  const uint32_t sp = ctx.r1.u32, frame = sp - 240;
  const uint32_t self = ctx.r3.u32;
  const uint64_t lr = ctx.lr;
  St32(base, frame, sp);  // the back chain, as stwu
  ctx.r1.u32 = frame;
  ctx.r3.u64 = obj;
  ctx.lr = kGateReturn;
  f(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx.r1.u32 = sp;
  ctx.r3.u64 = self;
  ctx.lr = lr;
  if (r != 0)
    return true;
  return (Ld32(base, kGateFlag + 4) & 1u) != 0;  // u64 big-endian: bit 0 is in the second word
}

// kDry: nothing written; the words that would be are kept in 'out' (later reads of the same word see them).
template <bool kDry>
void Run(PPCContext& ctx, uint8_t* base, std::unordered_map<uint32_t, uint32_t>* out) {
  const uint32_t self = ctx.r3.u32;
  if (!Gate(ctx, base))
    return;
  const int32_t views = int32_t(Ld32(base, self + 4));
  if (views <= 0)
    return;
  const int32_t levels = int32_t(Ld32(base, Ld32(base, kWorld) + 76));
  if (levels <= 0)
    return;
  auto load = [&](uint32_t a) -> uint32_t {
    if constexpr (kDry) {
      auto it = out->find(a);
      if (it != out->end())
        return it->second;
    }
    return Ld32(base, a);
  };
  auto store = [&](uint32_t a, uint32_t v) {
    if constexpr (kDry)
      (*out)[a] = v;
    else
      St32(base, a, v);
  };
  bool flush = false;
  float near_init = 0.0f, scale = 0.0f, far_cap = 0.0f;
  for (int32_t level_index = 0; level_index < levels; ++level_index) {
    const uint32_t world = Ld32(base, kWorld);
    const uint32_t level = Ld32(base, Ld32(base, world + 72) + 4u * uint32_t(level_index));
    const uint32_t mask[3] = {Ld32(base, kMask), Ld32(base, kMask + 4), Ld32(base, kMask + 8)};
    const uint32_t array = level + 172;
    int32_t index = 0, offset = 0;
    for (;;) {
      if (offset < 0 || index >= int32_t(Ld32(base, array + 4)))
        break;
      const uint32_t entry = Ld32(base, array) + uint32_t(offset);
      ++index;
      offset += 20;
      const uint32_t prim = Ld32(base, entry + 4);
      if (!prim)
        continue;
      if (!flush) {
        // The original's first vector float op enables flush-to-zero. The barrier keeps every load (and the float
        // math on it) after the FPCR write: the asm has no memory clobber.
        ctx.fpscr.enableFlushMode();
        __asm__ __volatile__("" ::: "memory");
        flush = true;
        near_init = F(Ld32(base, kNearInit));
        scale = F(Ld32(base, kScale));
        far_cap = F(Ld32(base, kFarCap));
      }
      float near = near_init, far = 0.0f;
      const uint32_t spheres = Ld32(base, entry + 8);
      const uint32_t count = Ld32(base, entry + 12);
      const uint32_t end = spheres + count * 20u;
      for (uint32_t s = spheres; s != end; s += 20) {
        const float sx = F(Ld32(base, s) & ~mask[0]);
        const float sy = F(Ld32(base, s + 4) & ~mask[1]);
        const float sz = F(Ld32(base, s + 8) & ~mask[2]);
        const float radius = F(Ld32(base, s + 12));
        const float factor = F(Ld32(base, s + 16));
        const float r2 = radius * radius;
        const uint32_t view_base = Ld32(base, self);
        for (int32_t v = 0; v < views; ++v) {
          const uint32_t p = view_base + 20u * uint32_t(v);
          const float dx = sx - F(Ld32(base, p) & ~mask[0]);
          const float dy = sy - F(Ld32(base, p + 4) & ~mask[1]);
          const float dz = sz - F(Ld32(base, p + 8) & ~mask[2]);
          const float x2 = dx * dx;
          const float y2 = dy * dy;
          const float z2 = dz * dz;
          const float low = 0.0f + z2;  // host lanes 0 (w, masked to +0) and 1 (z)
          const float high = y2 + x2;   // host lanes 2 (y) and 3 (x)
          const float d2 = low + high;
          const float diff = d2 - r2;
          const float q = diff * scale;
          const float rs = 1.0f / __builtin_sqrtf(q);
          const float clamped = q > 0.0f ? q : 0.0f;
          const float dist = q * rs;
          near = near < clamped ? near : clamped;
          const float scaled = factor * dist;
          const float capped = scaled < far_cap ? scaled : far_cap;
          far = far > capped ? far : capped;
        }
      }
      const float old_near = F(load(prim + 316));
      store(prim + 316, Bits(near < old_near ? near : old_near));
      const float old_far = F(load(prim + 312));
      store(prim + 312, Bits(far > old_far ? far : old_far));
    }
  }
}

}  // namespace

REX_EXTERN(__imp__sub_823A3748);
REX_HOOK_RAW(sub_823A3748) {
  static const bool on = REXCVAR_GET(me2_hot_stream_distance);
  static const uint64_t checks = uint64_t(std::max(0, REXCVAR_GET(me2_hot_stream_distance_checks)));
  if (!on || g_off.load(std::memory_order_relaxed)) {
    __imp__sub_823A3748(ctx, base);
    return;
  }
  const uint64_t call = g_calls.fetch_add(1, std::memory_order_relaxed);
  if (call < checks || (call % 1024) == 0) {
    std::unordered_map<uint32_t, uint32_t> expected;
    Run<true>(ctx, base, &expected);
    __imp__sub_823A3748(ctx, base);
    uint32_t bad = 0, first = 0, want = 0, got = 0;
    for (const auto& [addr, value] : expected) {
      const uint32_t real = Ld32(base, addr);
      if (real != value) {
        if (!bad++) {
          first = addr;
          want = value;
          got = real;
        }
      }
    }
    if (bad) {
      g_off.store(true, std::memory_order_relaxed);
      REXLOG_WARN("[hot] sub_823A3748: DIFFERENCE in call {}: {} of {} words, first {:08X} native {:08X} original "
                  "{:08X}; native switched off",
                  call, bad, expected.size(), first, want, got);
    } else if (call + 1 == checks) {
      REXLOG_INFO("[hot] sub_823A3748: native ON, {} checked calls equal (last: {} words)", checks, expected.size());
    }
    return;
  }
  Run<false>(ctx, base, nullptr);
}
