// Mass Effect 2 - PhysX SDK warnings without formatting them. Ported from masseffect-cinnamon (ME1 Switch fork,
// app/src/native/me_physx.cpp: ~13 % of ME1's main thread was the CRT printf core under this function, plus the
// busy poll it sits in).
//
// ME2 (2026-10-26, from the disassembly): the PhysX SDK's error-stream report is sub_82DA2018(r3 SDK object, r4 code,
// r5 file, r6 line, r7 out flag, r8 format, r9 va_list), reached through the varargs wrapper sub_82D7A890 (905 call
// sites). Same body as ME1's sub_82B01A88: lock (sub_82DB2ED0 on object +52), last error code +40, first error code
// +36, error stream at +28; code 107 (assertion) asks the stream whether to break; otherwise vsnprintf (sub_82D62150)
// into a 160-byte buffer (grown while it does not fit) and a virtual call of the stream (code 208: print, else
// reportError). The message only reaches the game's log. NpScene::fetchResults (sub_8303C1D0) reports
// "Scene::fetchResults: simulate() was not called or aborted" (code 1, NpScene.cpp line 2750); in ME1 the game polled
// fetchResults(non-blocking) in a loop until the PhysX threads finished the step, formatting that warning each time.
//
// me2_physx_no_warnings (default on): any code but 107 updates +40/+36 like the original and returns 0 without
// formatting or calling the stream, then yields the core (with core migration on the Switch) so the PhysX threads can
// run instead of the poll. A count per source line is logged every 10 s ([physx]), to see whether ME2 polls too.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(me2_physx_no_warnings, true, "ME2",
                    "Drop PhysX SDK warnings (sub_82DA2018) instead of formatting them for the game log, and yield "
                    "the core on each one (ME1 polled fetchResults with one per poll)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

// -1 = not read yet, 0 = off, 1 = on. A namespace-scope atomic instead of a function-local static: no guard check on
// every call.
std::atomic<int> g_on{-1};

// Report counts (rare path compared with the work it replaces).
std::mutex g_mutex;
uint64_t g_total = 0, g_window = 0;
uint32_t g_last_line = 0, g_last_code = 0, g_last_line_count = 0;
std::chrono::steady_clock::time_point g_window_start{};

void Count(uint32_t code, uint32_t line) {
  std::lock_guard lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (g_total == 0) {
    g_window_start = now;
    REXLOG_INFO("[physx] warnings dropped without formatting (first: code {}, NpScene/SDK line {})", code, line);
  }
  ++g_total;
  ++g_window;
  if (line == g_last_line && code == g_last_code) {
    ++g_last_line_count;
  } else {
    g_last_line = line;
    g_last_code = code;
    g_last_line_count = 1;
  }
  if (now - g_window_start >= std::chrono::seconds(10)) {
    REXLOG_INFO("[physx] {} warnings in the last ~10 s ({} in all; last: code {}, line {}, {} in a row)", g_window,
                g_total, code, line, g_last_line_count);
    g_window = 0;
    g_window_start = now;
  }
}

}  // namespace

REX_EXTERN(__imp__sub_82DA2018);
REX_HOOK_RAW(sub_82DA2018) {
  int on = g_on.load(std::memory_order_relaxed);
  if (on < 0) {
    on = REXCVAR_GET(me2_physx_no_warnings) ? 1 : 0;
    g_on.store(on, std::memory_order_relaxed);
  }
  if (on && ctx.r4.u32 != 107) {
    const uint32_t sdk = ctx.r3.u32, code = ctx.r4.u32;
    Store32(base, sdk + 40, code);                                   // stw r25,40(r29)
    if (Load32(base, sdk + 36) == 0) Store32(base, sdk + 36, code);  // first error code
    Count(code, ctx.r6.u32);
    // Give the core away: in ME1 this came from a busy poll of fetchResults while the PhysX threads ran the step.
#if REX_PLATFORM_SWITCH
    svcSleepThread(YieldType_WithCoreMigration);
#else
    std::this_thread::yield();
#endif
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82DA2018(ctx, base);
}

// me2-nx-deko (2026-11-06, me2_physx_step_ms): fewer PhysX substeps a frame. UE3's TickRBPhysScene (TU2 sub_825D9A28:
// scene r3, DeltaTime f1, MaxSubstep f2, bFixedTimeStep r6) splits each frame into NumSubSteps = ceil(DeltaTime /
// MaxSubstep) steps (at least 1, at most WorldInfo +1144 MaxPhysicsSubsteps) and hands them to NpScene::setTiming
// (sub_8302B410, vtable slot 328: step f1, count r5, NX_TIMESTEP_FIXED). ME2's step is UE3's 0.02 s, so every frame
// at 25-30 fps runs two full collision and solver passes, and three under 25 fps. Console 11-06 (combat, 15-19 fps): the
// game thread waited ~52 % of the time in fetchResults (sub_8302C630) for the PhysX thread (X83055738, 16-25 % of a
// core). A longer step (e.g. 40 ms) gives one pass a frame from 25 fps up. Variable timing only (r6 == 0).
REXCVAR_DEFINE_INT32(me2_physx_step_ms, 0, "ME2",
                     "Longest PhysX substep in ms (UE3's is 20: two substeps a frame at 30 fps). E.g. 40 = one substep a "
                     "frame from 25 fps up. 0 = the game's")
    .range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
struct StepStats {
  uint64_t calls = 0, fixed = 0;
  double dt = 0.0, game_steps = 0.0, our_steps = 0.0, game_step_min = 1e9, game_step_max = 0.0;
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};
StepStats g_step;  // game thread only
}  // namespace

REX_EXTERN(__imp__sub_825D9A28);
REX_HOOK_RAW(sub_825D9A28) {  // TickRBPhysScene(FRBPhysScene*, FLOAT DeltaTime, FLOAT MaxSubstep, ..., UBOOL bFixedTimeStep)
  static const double ours = double(REXCVAR_GET(me2_physx_step_ms)) / 1000.0;
  const double dt = ctx.f1.f64, step = ctx.f2.f64;
  StepStats& s = g_step;
  ++s.calls;
  if (ctx.r6.u32 != 0) {
    ++s.fixed;
  } else if (dt > 0.0 && step > 0.0) {
    s.dt += dt;
    s.game_step_min = std::min(s.game_step_min, step);
    s.game_step_max = std::max(s.game_step_max, step);
    s.game_steps += std::max(1.0, std::ceil(dt / step - 1e-6));
    if (ours > step) ctx.f2.f64 = ours;
    s.our_steps += std::max(1.0, std::ceil(dt / ctx.f2.f64 - 1e-6));
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - s.since >= std::chrono::seconds(60)) {
    const uint64_t n = s.calls - s.fixed;
    if (n)
      REXLOG_INFO("[physx] 60 s: {} physics ticks (frame {:.1f} ms on average), the game's substep {:.1f}-{:.1f} ms -> "
                  "{:.2f} substeps a tick; with me2_physx_step_ms {} -> {:.2f} (before the MaxPhysicsSubsteps cap); {} "
                  "fixed-step ticks left alone",
                  n, 1000.0 * s.dt / double(n), 1000.0 * s.game_step_min, 1000.0 * s.game_step_max,
                  s.game_steps / double(n), int(ours * 1000.0 + 0.5), s.our_steps / double(n), s.fixed);
    s = StepStats{};
    s.since = now;
  }
  __imp__sub_825D9A28(ctx, base);
}
