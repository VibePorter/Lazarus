// me2-nx-deko (2026-11-05): how much of the UE3 render thread the static mesh draw lists take (stage 0 of a native
// static draw-list renderer: record what a list element's draw does to the device once, replay it natively after).
//
// UE3's TStaticMeshDrawList<Policy>::DrawVisible(View, StaticMeshVisibilityMap) walks the list's drawing-policy links
// and draws each element whose bit is set: per link once DrawShared, per element SetMeshRenderState + the virtual
// DrawMesh (one or two passes, two-sided materials). TU2 has 17 instantiations (generated code scan 2026-11-05: the
// functions that iterate the sparse element array with sub_822E7380 and call a per-element draw with a shared-state
// flag at r1+80); each is hooked here only to time it.
//
// Every 10 s: "[drawlists] 10 s: T ms a frame in the static draw lists (D draws a frame, X % of all draws); by list:
//   <address> calls/frame, draws/frame, ms/frame ..." (UE3 render thread; inclusive of Renut's draw hooks).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(me2_drawlist_diag, true, "ME2/Diagnostics",
                    "Time the 17 static mesh draw lists' DrawVisible on the UE3 render thread (\"[drawlists]\" lines "
                    "every 10 s)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap
extern std::atomic<uint64_t> g_me2_draws_total;  // renut d3d hooks/draw.cpp: every draw

namespace {

constexpr int kLists = 17;
constexpr uint32_t kAddr[kLists] = {0x822E5150, 0x822E5828, 0x82328C48, 0x823294F8, 0x82340C40, 0x82341600,
                                    0x82346858, 0x82347228, 0x82688668, 0x8269F3E8, 0x8269F540, 0x8269F698,
                                    0x8269F7F0, 0x8269F950, 0x8269FAB0, 0x8269FC10, 0x826A8EF8};

struct Counters {
  uint64_t calls = 0, draws = 0, ns = 0;
};
struct Window {
  std::array<Counters, kLists> list{};
  uint64_t outer_ns = 0, outer_draws = 0, frames0 = 0, all0 = 0;
  std::chrono::steady_clock::time_point start{};
};
Window g_w;           // UE3 render thread only
int g_depth = 0;      // nesting (a list drawn from inside another list's draw counts once in the total)

void Report(std::chrono::steady_clock::time_point now) {
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const uint64_t all = g_me2_draws_total.load(std::memory_order_relaxed) - g_w.all0;
  const double f = std::max<uint64_t>(1, frame - g_w.frames0);
  std::array<int, kLists> order{};
  for (int i = 0; i < kLists; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [](int a, int b) { return g_w.list[a].ns > g_w.list[b].ns; });
  std::string by;
  for (int i : order) {
    const Counters& c = g_w.list[i];
    if (!c.calls) continue;
    by += fmt::format("; {:08X} {:.1f} calls {:.0f} draws {:.2f} ms", kAddr[i], double(c.calls) / f,
                      double(c.draws) / f, double(c.ns) / 1e6 / f);
  }
  REXLOG_INFO("[drawlists] {:.0f} s: {:.2f} ms a frame in the static draw lists ({:.0f} draws a frame, {:.1f} % of all "
              "draws); per frame by list{}",
              std::chrono::duration<double>(now - g_w.start).count(), double(g_w.outer_ns) / 1e6 / f,
              double(g_w.outer_draws) / f, all ? 100.0 * double(g_w.outer_draws) / double(all) : 0.0, by);
  g_w = Window{};
  g_w.start = now;
  g_w.frames0 = frame;
  g_w.all0 = g_me2_draws_total.load(std::memory_order_relaxed);
}

template <int I, PPCFunc* Original>
void Timed(PPCContext& ctx, uint8_t* base) {
  static const bool on = REXCVAR_GET(me2_drawlist_diag);
  if (!on) {
    Original(ctx, base);
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  if (g_w.start == std::chrono::steady_clock::time_point{}) {
    g_w.start = t0;
    g_w.frames0 = g_sa2_game_frames.load(std::memory_order_relaxed);
    g_w.all0 = g_me2_draws_total.load(std::memory_order_relaxed);
  }
  const uint64_t d0 = g_me2_draws_total.load(std::memory_order_relaxed);
  ++g_depth;
  Original(ctx, base);
  --g_depth;
  const auto t1 = std::chrono::steady_clock::now();
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  const uint64_t d = g_me2_draws_total.load(std::memory_order_relaxed) - d0;
  Counters& c = g_w.list[I];
  ++c.calls;
  c.draws += d;
  c.ns += ns;
  if (g_depth == 0) {
    g_w.outer_ns += ns;
    g_w.outer_draws += d;
    if (t1 - g_w.start >= std::chrono::seconds(10)) Report(t1);
  }
}

}  // namespace

#define ME2_DRAWLIST(I, ADDR)                       \
  REX_EXTERN(__imp__sub_##ADDR);                    \
  REX_HOOK_RAW(sub_##ADDR) { Timed<I, __imp__sub_##ADDR>(ctx, base); }

ME2_DRAWLIST(0, 822E5150)
ME2_DRAWLIST(1, 822E5828)
ME2_DRAWLIST(2, 82328C48)
ME2_DRAWLIST(3, 823294F8)
ME2_DRAWLIST(4, 82340C40)
ME2_DRAWLIST(5, 82341600)
ME2_DRAWLIST(6, 82346858)
ME2_DRAWLIST(7, 82347228)
ME2_DRAWLIST(8, 82688668)
ME2_DRAWLIST(9, 8269F3E8)
ME2_DRAWLIST(10, 8269F540)
ME2_DRAWLIST(11, 8269F698)
ME2_DRAWLIST(12, 8269F7F0)
ME2_DRAWLIST(13, 8269F950)
ME2_DRAWLIST(14, 8269FAB0)
ME2_DRAWLIST(15, 8269FC10)
ME2_DRAWLIST(16, 826A8EF8)
