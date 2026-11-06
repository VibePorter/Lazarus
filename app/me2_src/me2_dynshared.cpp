// me2-nx-deko (2026-11-06): skinned-mesh DrawShared diagnostic (me2_dynshared_diag, default off; changes nothing).
//
// WHY
//   Console stacks 2026-11-06 19:44 (party, fps < 28): FSkeletalMeshSceneProxy::DrawDynamicElements (sub_8254AD38,
//   proxy vtable 0x82157220 slot 3; constructor sub_8254A7D0 from USkeletalMeshComponent::CreateSceneProxy
//   sub_8254C330) is 9.5 % of UE3's render thread. Its base-pass draws go 8267F140 -> 82675940 -> 82679DB8 =
//   build the policy (82677B70), DrawShared sub_8267A668 (4.4 % of the thread), SetMeshRenderState sub_8267A850, the
//   draw. Unlike the static lists (DrawShared once per link), the dynamic path runs DrawShared for every section.
//   Inside it, sub_8267AD78 (pixel shader / material parameters: r3 = policy +20, r4 = policy +8 material render
//   proxy, r5 = view) is 3.3 % (texture binds: cold D3D headers on the console).
//
// WHAT
//   For both functions: calls a frame, us each, and whether a call had the same key as the previous call of the same
//   function (8267A668: the policy's first 48 bytes, r4, r5; 8267AD78: r3, r4, r5; both within one render command,
//   Me2RenderCommandEpoch), and whether the call changed the D3D device at all (0x5E00 bytes before / after, the
//   pending masks +0..+3F left out: they are cleared by every draw). Same key and no change = a skip would be exact
//   (what UE3's static lists do: DrawShared once, then the elements). Also the draws in between (Renut's count).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp
extern std::atomic<uint64_t> g_me2_draws_total;  // renut d3d hooks/draw.cpp: every draw
uint64_t Me2RenderCommandEpoch(const uint8_t* base);  // me2_material_cache.cpp

REXCVAR_DEFINE_BOOL(me2_dynshared_diag, false, "ME2/Diagnostics",
                    "Skinned-mesh DrawShared (sub_8267A668) and its material parameters (sub_8267AD78): how often a "
                    "call repeats the previous one and whether it changes the D3D device; '[dynshared]' lines every "
                    "10 s. Changes nothing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kDevicePtr = 0x833D074Cu;
constexpr uint32_t kDeviceBytes = 0x5E00;
constexpr uint32_t kMaskBytes = 0x40;  // pending masks: cleared by each draw, set again by any constant write

uint32_t L32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
bool Readable(uint32_t a, uint32_t n) { return a >= 0x10000u && uint64_t(a) + n <= 0xA0000000ull; }
inline uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v * 0x9E3779B97F4A7C15ull;
  return (h << 27 | h >> 37) * 0xFF51AFD7ED558CCDull;
}

struct Stats {
  uint64_t calls = 0, ns = 0, same = 0, same_noop = 0, same_changed = 0, diff_noop = 0, diff_changed = 0;
  uint64_t same_draws[4] = {};          // draws since the previous call (0, 1, 2, 3+), same key
  std::map<uint32_t, uint32_t> offsets;  // same key but changed: the first changed device offset (16-byte line)
  uint64_t last_key = 0, last_draws = 0;
};
struct Fn {
  const char* name;
  Stats s;
};
// me2-nx-deko (2026-11-06): [2] / [3] the translucency pass's DrawShared (sub_826B6678 inside sub_826B37B0: particles,
// ~14 % of the console's UE3 render thread at the party, ~110 draws a frame), keyed by the whole policy and by its
// material + shaders + blend only (policy +8, +16, +20, +28, +32: each emitter may have its own vertex factory, +4).
Fn g_fn[4] = {{"DrawShared 8267A668"},
              {"material parameters 8267AD78"},
              {"translucent DrawShared 826B6678 (whole policy)"},
              {"translucent DrawShared 826B6678 (material, shaders, blend; vertex factory left out)"}};
thread_local int t_in_translucent = 0;  // inside sub_826B37B0 (the translucency pass's dynamic mesh draw)
std::thread::id g_owner;
std::chrono::steady_clock::time_point g_start{};
uint64_t g_frames0 = 0;

void Report(std::chrono::steady_clock::time_point now) {
  const double f = double(std::max<uint64_t>(g_sa2_game_frames.load(std::memory_order_relaxed) - g_frames0, 1));
  const double secs = std::chrono::duration<double>(now - g_start).count();
  for (Fn& fn : g_fn) {
    Stats& s = fn.s;
    if (!s.calls) continue;
    const auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
    std::string offs;
    std::vector<std::pair<uint32_t, uint32_t>> v(s.offsets.begin(), s.offsets.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < v.size() && i < 8; ++i) offs += fmt::format(" +{:X} x{}", v[i].first, v[i].second);
    REXLOG_INFO("[dynshared] {:.0f} s, {}: {:.1f} calls a frame, {:.2f} us each ({:.3f} ms a frame); SAME KEY as the "
                "previous call {:.1f} % (of them: device unchanged {:.1f} %, changed {}{}; draws in between 0/1/2/3+: "
                "{}/{}/{}/{}); other key: device unchanged {} of {}",
                secs, fn.name, double(s.calls) / f, s.calls ? double(s.ns) / double(s.calls) / 1000.0 : 0.0,
                double(s.ns) / f / 1e6, pct(s.same, s.calls), pct(s.same_noop, s.same), s.same_changed,
                offs.empty() ? "" : " at" + offs, s.same_draws[0], s.same_draws[1], s.same_draws[2], s.same_draws[3],
                s.diff_noop, s.calls - s.same);
    const uint64_t key = s.last_key, draws = s.last_draws;
    s = Stats{};
    s.last_key = key, s.last_draws = draws;
  }
  g_start = now;
  g_frames0 = g_sa2_game_frames.load(std::memory_order_relaxed);
}

bool DiagOn() {
  static const bool on = REXCVAR_GET(me2_dynshared_diag);
  return on;
}

void ObserveRt(int I, PPCFunc* Original, PPCContext& ctx, uint8_t* base) {
  if (!DiagOn()) {
    Original(ctx, base);
    return;
  }
  // UE3's render thread only; a new one is started after level loads: a thread takes over after a quiet second.
  static std::atomic<int64_t> last_ms{0};
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
  if (g_owner != std::this_thread::get_id()) {
    if (g_owner != std::thread::id{} && now_ms - last_ms.load(std::memory_order_relaxed) < 1000) {
      Original(ctx, base);
      return;
    }
    g_owner = std::this_thread::get_id();
  }
  last_ms.store(now_ms, std::memory_order_relaxed);
  Stats& s = g_fn[I].s;
  uint64_t key = Mix(Mix(Mix(Mix(0, ctx.r3.u32), ctx.r4.u32), ctx.r5.u32), Me2RenderCommandEpoch(base));
  uint64_t key2 = 0;  // I == 2: the material key (slot 3)
  if ((I == 0 || I == 2) && Readable(ctx.r3.u32, 48)) {
    for (int i = 0; i < 6; ++i) {
      uint64_t w;
      std::memcpy(&w, base + ctx.r3.u32 + 8 * i, 8);
      key = Mix(key, w);
    }
    if (I == 2) {
      key2 = Mix(Mix(0, ctx.r4.u32), Me2RenderCommandEpoch(base));
      for (const uint32_t off : {8u, 16u, 20u, 28u, 32u}) key2 = Mix(key2, L32(base, ctx.r3.u32 + off));
    }
  }
  const uint32_t dev = L32(base, kDevicePtr);
  const bool dev_ok = Readable(dev, kDeviceBytes);
  static thread_local std::vector<uint8_t> before[4];
  if (dev_ok) before[I].assign(base + dev, base + dev + kDeviceBytes);
  const auto t0 = std::chrono::steady_clock::now();
  Original(ctx, base);
  const auto t1 = std::chrono::steady_clock::now();
  if (g_start == std::chrono::steady_clock::time_point{}) {
    g_start = t1;
    g_frames0 = g_sa2_game_frames.load(std::memory_order_relaxed);
  }
  uint32_t changed_at = kDeviceBytes;
  if (dev_ok) {
    const uint8_t* now = base + dev;
    for (uint32_t i = kMaskBytes; i < kDeviceBytes; i += 16)
      if (std::memcmp(&before[I][i], now + i, 16) != 0) {
        changed_at = i;
        break;
      }
  }
  const bool noop = dev_ok && changed_at == kDeviceBytes;
  const uint64_t draws = g_me2_draws_total.load(std::memory_order_relaxed);
  ++s.calls;
  s.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  if (key == s.last_key) {
    ++s.same;
    ++s.same_draws[std::min<uint64_t>(draws - s.last_draws, 3)];
    if (noop) {
      ++s.same_noop;
    } else {
      ++s.same_changed;
      ++s.offsets[changed_at];
    }
  } else {
    if (noop) ++s.diff_noop;
  }
  s.last_key = key;
  s.last_draws = draws;
  if (I == 2) {
    Stats& m = g_fn[3].s;
    ++m.calls;
    m.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (key2 == m.last_key) {
      ++m.same;
      ++m.same_draws[std::min<uint64_t>(draws - m.last_draws, 3)];
      if (noop) {
        ++m.same_noop;
      } else {
        ++m.same_changed;
        ++m.offsets[changed_at];
      }
    } else if (noop) {
      ++m.diff_noop;
    }
    m.last_key = key2;
    m.last_draws = draws;
  }
  if ((I == 0 || I == 2) && t1 - g_start >= std::chrono::seconds(10)) Report(t1);
}

template <int I, PPCFunc* Original>
void Observe(PPCContext& ctx, uint8_t* base) {
  ObserveRt(I, Original, ctx, base);
}

}  // namespace

#define ME2_DYNSHARED_HOOK(I, ADDR)    \
  REX_EXTERN(__imp__sub_##ADDR);       \
  REX_HOOK_RAW(sub_##ADDR) { Observe<I, __imp__sub_##ADDR>(ctx, base); }
ME2_DYNSHARED_HOOK(0, 8267A668)
ME2_DYNSHARED_HOOK(1, 8267AD78)

// The translucency pass's dynamic mesh draw: its DrawShared calls are observed (me2_shadow_diag.cpp's hook of
// sub_826B6678 asks Me2DynSharedTranslucent).
REX_EXTERN(__imp__sub_826B37B0);
REX_HOOK_RAW(sub_826B37B0) {
  if (!DiagOn()) {
    __imp__sub_826B37B0(ctx, base);
    return;
  }
  ++t_in_translucent;
  __imp__sub_826B37B0(ctx, base);
  --t_in_translucent;
}
bool Me2DynSharedTranslucent() { return t_in_translucent > 0 && DiagOn(); }
void Me2DynSharedObserveTranslucent(PPCFunc* original, PPCContext& ctx, uint8_t* base) {
  ObserveRt(2, original, ctx, base);
}
