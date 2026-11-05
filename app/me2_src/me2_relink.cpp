// me2-nx-deko (2026-11-05): UE3's primitive transform update on the render thread, measured (and optionally skipped
// when it changes nothing).
//
// Console stacks (Omega standing, build 19:15): 15 % of the UE3 render thread is the render command sub_8268B730
// (FScene::UpdatePrimitiveTransform_RenderThread, about 20k light tests a frame while nothing visibly moves):
//   sub_8268B730(cmd): sub_8268B478(p = cmd + 16); return 192 (the command's size)
//   sub_8268B478(p):  prim = [p + 0] (FPrimitiveSceneInfo); scene = [prim + 4]; proxy = [prim + 8]
//     scene->vtbl[+96](scene, prim, 0)
//     memcpy(proxy + 32, p + 16, 64)        LocalToWorld
//     proxy + 96 = float [p + 172]           LocalToWorld determinant
//     proxy->vtbl[+64](proxy)                (transform changed)
//     prim + 48 .. +76 = p + 144 .. +172     bounds (origin, box extent, sphere radius: 7 floats)
//     sub_82689710(scene->PrimitivesCompact[prim + 32], prim)   (compact copy of the bounds)
//     sub_82690880(prim)                     RemoveFromScene (light interactions, static meshes of the draw lists)
//     sub_82690460(prim)                     AddToScene (static meshes back into the draw lists; every light of the
//                                            scene tested with sub_82688188, interactions created with sub_8268FA28)
//
// me2_relink_diag: "[relink]" lines every 10 s: updates a frame, their time, how many carry exactly the transform,
// determinant and bounds the primitive already has (remove + add would rebuild the same state), by proxy class.
// me2_relink_skip_same (default on since the console A/B of 2026-11-05: Omega 213 updates a frame, 78 % no-ops, re-link
// 2.88 -> 0.43 ms a frame, 19.22 -> 19.65 fps, nothing visibly wrong): such an update is not executed at all ("relink" in me2_ab_cycle turns the
// skip off in its slots). PC relink_1105 (prologue): 80-95 % of 80-130 updates a frame are such no-ops; with the skip
// the light tests drop from ~185-290k to ~8k per 10 s.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "me2_ab_cycle.h"

REXCVAR_DEFINE_BOOL(me2_relink_diag, false, "ME2/Diagnostics",
                    "\"[relink]\" lines: UE3's primitive transform updates on the render thread (sub_8268B730), their "
                    "time and how many change nothing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_relink_skip_same, true, "ME2",
                    "Skip a primitive transform update (remove from and add to the scene, every light tested "
                    "again) whose transform, determinant and bounds equal the primitive's current ones")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap

REX_EXTERN(__imp__sub_8268B730);

namespace {

inline uint32_t L32(const uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}
inline bool GuestOk(uint32_t va) { return va >= 0x10000u && va < 0xF0000000u; }

struct Cls {
  uint32_t vtbl = 0;
  uint64_t n = 0, same = 0, ns = 0;
};
struct Window {
  uint64_t n = 0, same = 0, skipped = 0, ns = 0, same_ns = 0, frames0 = 0;
  uint64_t same_xform_only = 0;  // same transform, different bounds (animated bounds)
  std::vector<Cls> cls;
  std::chrono::steady_clock::time_point start{};
};
Window g_w;
uint32_t g_scene_fn = 0, g_proxy_fn_logged = 0;

// The update would leave the primitive exactly as it is.
bool Same(const uint8_t* base, uint32_t p, uint32_t prim, uint32_t proxy, bool* xform_same) {
  const bool x = std::memcmp(base + p + 16, base + proxy + 32, 64) == 0 &&
                 std::memcmp(base + p + 172, base + proxy + 96, 4) == 0;
  *xform_same = x;
  return x && std::memcmp(base + p + 144, base + prim + 48, 28) == 0;
}

void Report(std::chrono::steady_clock::time_point now) {
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const double f = double(std::max<uint64_t>(1, frame - g_w.frames0));
  const double n = double(std::max<uint64_t>(1, g_w.n));
  REXLOG_INFO("[relink] 10 s: {:.1f} primitive transform updates a frame, {:.1f} us each = {:.2f} ms a frame; "
              "{:.1f} % change nothing (transform + bounds the same; {:.2f} ms a frame), {:.1f} % only the bounds "
              "changed; {:.1f} a frame skipped (me2_relink_skip_same)",
              g_w.n / f, double(g_w.ns) / 1e3 / n, double(g_w.ns) / 1e6 / f, 100.0 * g_w.same / n,
              double(g_w.same_ns) / 1e6 / f, 100.0 * g_w.same_xform_only / n, g_w.skipped / f);
  std::sort(g_w.cls.begin(), g_w.cls.end(), [](const Cls& a, const Cls& b) { return a.ns > b.ns; });
  std::string by;
  for (size_t i = 0; i < g_w.cls.size() && i < 6; ++i) {
    const Cls& c = g_w.cls[i];
    by += fmt::format("{}proxy vtable {:08X}: {:.1f} a frame, {:.0f} % same, {:.2f} ms a frame", i ? "; " : "",
                      c.vtbl, c.n / f, c.n ? 100.0 * c.same / c.n : 0.0, double(c.ns) / 1e6 / f);
  }
  if (!by.empty()) REXLOG_INFO("[relink]   by class: {}", by);
  const uint64_t keep_scene = g_scene_fn;
  g_w = Window{};
  g_w.start = now;
  g_w.frames0 = frame;
  g_scene_fn = keep_scene;
}

}  // namespace

REX_HOOK_RAW(sub_8268B730) {
  static const bool diag = REXCVAR_GET(me2_relink_diag);
  static const bool skip = REXCVAR_GET(me2_relink_skip_same);
  if (!diag && !skip) {
    __imp__sub_8268B730(ctx, base);
    return;
  }
  const uint32_t p = ctx.r3.u32 + 16;
  const uint32_t prim = GuestOk(p) ? L32(base, p) : 0;
  const uint32_t proxy = GuestOk(prim) ? L32(base, prim + 8) : 0;
  if (!GuestOk(prim) || !GuestOk(proxy)) {
    __imp__sub_8268B730(ctx, base);
    return;
  }
  bool xform_same = false;
  const bool same = Same(base, p, prim, proxy, &xform_same);
  if (same && skip && !Me2AbOff(Me2Ab::kRelink)) {
    ctx.r3.u64 = 192;  // the command's size, as the original returns
    if (diag) ++g_w.skipped, ++g_w.n, ++g_w.same;
  } else {
    if (diag && !g_scene_fn) {
      const uint32_t scene = L32(base, prim + 4);
      if (GuestOk(scene)) {
        g_scene_fn = L32(base, L32(base, scene) + 96);
        REXLOG_INFO("[relink] scene virtual +96 = {:08X}, first proxy vtable {:08X} (+64 = {:08X})", g_scene_fn,
                    L32(base, proxy), L32(base, L32(base, proxy) + 64));
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    __imp__sub_8268B730(ctx, base);
    if (!diag) return;
    const uint64_t ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ++g_w.n;
    g_w.ns += ns;
    if (same) ++g_w.same, g_w.same_ns += ns;
    else if (xform_same) ++g_w.same_xform_only;
    const uint32_t vt = L32(base, proxy);
    auto it = std::find_if(g_w.cls.begin(), g_w.cls.end(), [vt](const Cls& c) { return c.vtbl == vt; });
    if (it == g_w.cls.end()) {
      g_w.cls.push_back(Cls{vt});
      it = g_w.cls.end() - 1;
    }
    ++it->n;
    it->ns += ns;
    if (same) ++it->same;
  }
  if (!diag) return;
  const auto now = std::chrono::steady_clock::now();
  if (g_w.start == std::chrono::steady_clock::time_point{}) {
    g_w.start = now;
    g_w.frames0 = g_sa2_game_frames.load(std::memory_order_relaxed);
  } else if (now - g_w.start >= std::chrono::seconds(10)) {
    Report(now);
  }
}
