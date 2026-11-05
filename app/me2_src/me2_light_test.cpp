// me2-nx-deko (2026-11-03): native early "no" of UE3's light / primitive overlap test.
//
// Console stacks of the Kasumi party (LOGS/more_more_logs, 16-20 fps, the UE3 render thread at a full core): 5.9 % of
// that thread is UE3 re-linking a moved primitive with the scene's lights. The render command (sub_8268B730 ->
// sub_8268B478 -> sub_82690460) walks EVERY light of the scene for each primitive that moved (the party crowd, their
// attachments) and calls sub_82688188(FLightSceneInfoCompact* r3, FPrimitiveSceneInfoCompact* r4, r5) for each. With
// r5 != 0 it first tests the two bounding spheres and returns 0 when they are apart, which is what almost every light
// answers; that test alone was 4.2 % of the thread (the samples sit on its dot product, compare and return).
//
// The guest test (from the generated code):
//   light  = 16 bytes at (r3 + 16) & ~15: x, y, z, radius            (lvx)
//   prim   = the floats at r4 + 16, +20, +24 (bounds origin)        (lvx + lvx + lvsl + vperm: an unaligned load)
//   pr     = the float at r4 + 40 (bounds sphere radius)            (lvlx)
//   d      = light.xyz - prim; d2 = vmsum3fp(d, d); r = light.radius + pr
//   d2 > r * r -> return 0
//
// Here that compare is made natively with a margin: only a pair clearly apart (d2 > r * r * (1 + 1/1024) + 1e-30) is
// answered 0 at once; everything else, and every NaN, runs the original, which makes its own test again. So the
// answer cannot differ from the original's whatever the rounding (fused multiply-add, flush-to-zero of tiny values).
//
// Self-checked like the hot natives: the first me2_light_test_checks early answers and then one in
// me2_light_test_check_every also run the original; a different answer logs "[light-test] DIFFERENCE" and turns the
// early answer off for the session.
//
// 2026-11-05 (console stacks, Omega standing, build 19:15): ~20k calls a frame, and 6.2 % of the UE3 render thread sat
// on this hook's two shared atomic counters (ldxr/stxr on a cache line next to globals other cores write) plus the
// function-local static guards (ldar each call) - more than the test itself. The counters are now per thread, added
// to the shared ones once every 4096 calls; the cvars are read once into plain values.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(me2_light_test_fast, true, "ME2",
                    "Answer UE3's light / primitive sphere test (sub_82688188) natively when the spheres are clearly "
                    "apart (self-checked)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_light_test_checks, 4096, "ME2",
                     "me2_light_test_fast: the first this many early answers also run the original and compare")
    .range(0, 1 << 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_light_test_check_every, 1024, "ME2",
                     "me2_light_test_fast: after the first checks, one early answer in this many is still checked; "
                     "0 = none")
    .range(0, 1 << 20)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

alignas(64) std::atomic<bool> g_off{false};  // read every call: own cache line, away from written globals
alignas(64) std::atomic<uint64_t> g_calls{0}, g_early{0}, g_checked{0};
std::atomic<int64_t> g_last_report{0};

// Per-thread counts (UE3 calls this from its render thread), added to g_calls / g_early every 4096 calls.
thread_local uint32_t t_calls = 0, t_early = 0;
thread_local uint64_t t_early_total = 0;  // this thread's early answers (picks the ones to check)

// The cvars, read once (relaxed atomics: plain loads, no static-guard barrier on every call).
alignas(64) std::atomic<int> g_cfg{0};  // 0 not read, 1 on, 2 off
uint64_t g_checks = 0, g_every = 0;
[[maybe_unused]] alignas(64) char g_pad_after_cfg = 0;  // keeps the next globals off g_cfg's cache line
inline bool Enabled() {
  int c = g_cfg.load(std::memory_order_relaxed);
  if (c == 0) [[unlikely]] {
    g_checks = uint64_t(REXCVAR_GET(me2_light_test_checks));
    g_every = uint64_t(REXCVAR_GET(me2_light_test_check_every));
    c = REXCVAR_GET(me2_light_test_fast) ? 1 : 2;
    g_cfg.store(c, std::memory_order_release);
  }
  return c == 1;
}

inline float LoadF(const uint8_t* base, uint32_t ea) {
  uint32_t bits;
  std::memcpy(&bits, base + ea, 4);
  bits = __builtin_bswap32(bits);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// true: the spheres are clearly apart, the original would return 0.
inline bool ClearlyApart(const uint8_t* base, uint32_t light, uint32_t prim) {
  const uint32_t l = (light + 16) & ~15u;
  const float dx = LoadF(base, l) - LoadF(base, prim + 16);
  const float dy = LoadF(base, l + 4) - LoadF(base, prim + 20);
  const float dz = LoadF(base, l + 8) - LoadF(base, prim + 24);
  const float r = LoadF(base, l + 12) + LoadF(base, prim + 40);
  const float d2 = dx * dx + dy * dy + dz * dz;
  const float rr = r * r;
  return d2 > rr * (1.0f + 1.0f / 1024.0f) + 1e-30f;  // false for any NaN
}

void Report() {
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = g_last_report.load(std::memory_order_relaxed);
  if (last == 0) {
    g_last_report.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10 || !g_last_report.compare_exchange_strong(last, now, std::memory_order_relaxed))
    return;
  const uint64_t calls = g_calls.exchange(0, std::memory_order_relaxed);
  const uint64_t early = g_early.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[light-test] last ~10 s: {} light / primitive tests, {} answered 'apart' natively ({:.0f} %); "
              "{} early answers checked against the original so far{}",
              calls, early, calls ? 100.0 * double(early) / double(calls) : 0.0,
              g_checked.load(std::memory_order_relaxed), g_off.load(std::memory_order_relaxed) ? " (OFF)" : "");
}

}  // namespace

REX_EXTERN(__imp__sub_82688188);
REX_HOOK_RAW(sub_82688188) {
  if (!Enabled()) {
    __imp__sub_82688188(ctx, base);
    return;
  }
  if (++t_calls == 4096) [[unlikely]] {
    g_calls.fetch_add(t_calls, std::memory_order_relaxed);
    g_early.fetch_add(t_early, std::memory_order_relaxed);
    t_calls = t_early = 0;
    Report();
  }
  if (ctx.r5.u32 != 0 && !g_off.load(std::memory_order_relaxed) &&
      ClearlyApart(base, ctx.r3.u32, ctx.r4.u32)) {
    ++t_early;
    const uint64_t n = ++t_early_total;
    if (n <= g_checks || (g_every && n % g_every == 0)) [[unlikely]] {
      const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
      __imp__sub_82688188(ctx, base);
      g_checked.fetch_add(1, std::memory_order_relaxed);
      if (ctx.r3.u32 != 0) {
        g_off.store(true, std::memory_order_relaxed);
        REXLOG_ERROR("[light-test] DIFFERENCE: light {:08X} primitive {:08X} r5 {:08X}: the original says {:08X}, the "
                     "native test 'apart'; early answers off for this session",
                     r3, r4, r5, ctx.r3.u32);
      }
      return;
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82688188(ctx, base);
}
