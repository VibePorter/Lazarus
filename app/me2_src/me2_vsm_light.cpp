// me2-nx-deko (2026-11-07): white blobs / blinking light on characters, the shadowed point light's VSM matrix.
//
// WHY
//   Ryujinx captures 14505 / 15300 and the [bad-consts] diagnostic: a skinned light pass (RenderLights 82688C70 ->
//   FSkeletalMeshSceneProxy::DrawDynamicElements 8254AD38 -> 82689600 -> the policy factory's DrawDynamicMesh
//   sub_82742B10 -> VS parameter setter 82683820) uploads VS c237-c240 from light + 592: a world-to-shadow matrix
//   that the pixel shader divides by w and uses to sample the light's variance shadow map (R16G16_FLOAT). Only the
//   light's own shadow method sub_82743600 (vtable 0x82192B88 slot 21) writes it (sub_827432E0, after rendering the
//   light's projected shadow), and only on frames where it finds that shadow. The light scene info (704 bytes,
//   sub_82743A78 -> constructor sub_82741AB8) never initialises it: until the shadow is rendered once, the light pass
//   reads whatever the allocation held. Pointers and small integers as floats -> 1e25-1e30 coordinates -> pixels at
//   1e34 -> fp16 max -> bloom white blobs. Character light environments recreate these lights often, hence flicker.
//
// WHAT
//   me2_vsm_light_init (default on): the matrix is zeroed when the light is created (w = 0: the shader's clamp keeps
//   the coordinates finite). me2_vsm_light_diag: '[vsm-light]' lines every 10 s: this class's light passes a frame,
//   and whether the light's shadow matrix was computed this frame, on an earlier frame (stale: the shadow was not
//   rendered this frame, the pass uses an old matrix and whatever the shadow map holds), or never.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap

REXCVAR_DEFINE_BOOL(me2_vsm_light_init, true, "ME2",
                    "Zero the shadowed point light's world-to-shadow matrix (light + 592) when the light is created: "
                    "the game leaves it uninitialised until the light's shadow is first rendered (garbage there = "
                    "white blobs on characters)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_vsm_light_diag, false, "ME2/Diagnostics",
                    "Count the shadowed point light's light passes by the age of its shadow matrix; '[vsm-light]' "
                    "lines every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kLightVtable = 0x82192B88u;  // the light scene info class (constructor sub_82741AB8)
constexpr uint32_t kMatrix = 592;               // world-to-shadow matrix, 64 bytes
constexpr uint32_t kMatrixBytes = 64;

bool Readable(uint32_t a, uint32_t n) { return a >= 0x10000u && uint64_t(a) + n <= 0xA0000000ull; }
uint32_t L32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
bool MatrixZero(const uint8_t* base, uint32_t light) {
  for (uint32_t i = 0; i < kMatrixBytes; ++i)
    if (base[light + kMatrix + i]) return false;
  return true;
}

bool DiagOn() {
  static const bool on = REXCVAR_GET(me2_vsm_light_diag);
  return on;
}

std::atomic<uint64_t> g_created{0}, g_zeroed{0};

// Render thread only (the shadow method and the light passes).
struct Diag {
  std::unordered_map<uint32_t, uint64_t> computed;  // light -> frame its matrix was last computed
  uint64_t passes = 0, fresh = 0, stale = 0, never = 0, shadow_calls = 0, shadow_found = 0;
  std::vector<uint32_t> ages;
  std::chrono::steady_clock::time_point since{};
  uint64_t frame0 = 0;
} g_d;

void Report() {
  const auto now = std::chrono::steady_clock::now();
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  if (g_d.since == std::chrono::steady_clock::time_point{}) {
    g_d.since = now;
    g_d.frame0 = frame;
    return;
  }
  if (now - g_d.since < std::chrono::seconds(10)) return;
  const double frames = double(std::max<uint64_t>(1, frame - g_d.frame0));
  const double n = double(std::max<uint64_t>(1, g_d.passes));
  uint32_t median = 0, p90 = 0;
  if (!g_d.ages.empty()) {
    std::sort(g_d.ages.begin(), g_d.ages.end());
    median = g_d.ages[g_d.ages.size() / 2];
    p90 = g_d.ages[g_d.ages.size() * 9 / 10];
  }
  REXLOG_INFO("[vsm-light] last ~10 s: {:.1f} light passes a frame of the shadowed point light; shadow matrix "
              "computed this frame {:.1f} %, on an earlier frame {:.1f} % (age median {} / p90 {} frames), never "
              "{:.1f} %; its shadow method {:.1f} calls a frame, shadow found {:.1f}; lights created {}, matrices "
              "zeroed {}",
              double(g_d.passes) / frames, 100.0 * double(g_d.fresh) / n, 100.0 * double(g_d.stale) / n, median, p90,
              100.0 * double(g_d.never) / n, double(g_d.shadow_calls) / frames, double(g_d.shadow_found) / frames,
              g_created.load(std::memory_order_relaxed), g_zeroed.load(std::memory_order_relaxed));
  g_d.passes = g_d.fresh = g_d.stale = g_d.never = g_d.shadow_calls = g_d.shadow_found = 0;
  g_d.ages.clear();
  if (g_d.computed.size() > 4096) g_d.computed.clear();
  g_d.since = now;
  g_d.frame0 = frame;
}

}  // namespace

// The light component's CreateSceneInfo (r3 = component): allocates the 704-byte light and constructs it.
REX_EXTERN(__imp__sub_82743A78);
REX_HOOK_RAW(sub_82743A78) {
  __imp__sub_82743A78(ctx, base);
  static const bool init = REXCVAR_GET(me2_vsm_light_init);
  const uint32_t light = ctx.r3.u32;
  if (!Readable(light, 704)) return;
  g_created.fetch_add(1, std::memory_order_relaxed);
  if (!init || L32(base, light) != kLightVtable) return;
  std::memset(base + light + kMatrix, 0, kMatrixBytes);
  g_zeroed.fetch_add(1, std::memory_order_relaxed);
}

// The light's shadow method (r3 = light, r4 = the frame's shadow list): finds the light's projected shadow, renders
// it and computes the matrix (returns 1), or returns 0.
REX_EXTERN(__imp__sub_82743600);
REX_HOOK_RAW(sub_82743600) {
  if (!DiagOn()) {
    __imp__sub_82743600(ctx, base);
    return;
  }
  const uint32_t light = ctx.r3.u32;
  __imp__sub_82743600(ctx, base);
  ++g_d.shadow_calls;
  if (ctx.r3.u32 == 1) {
    ++g_d.shadow_found;
    g_d.computed[light] = g_sa2_game_frames.load(std::memory_order_relaxed);
  }
}

// The policy factory's DrawDynamicMesh (r4 = view, r5 = light, r6 = mesh): the light pass.
REX_EXTERN(__imp__sub_82742B10);
REX_HOOK_RAW(sub_82742B10) {
  if (DiagOn()) {
    const uint32_t light = ctx.r5.u32;
    if (Readable(light, 704) && L32(base, light) == kLightVtable) {
      ++g_d.passes;
      const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
      const auto it = g_d.computed.find(light);
      if (it != g_d.computed.end() && it->second == frame) {
        ++g_d.fresh;
      } else if (it != g_d.computed.end() && !MatrixZero(base, light)) {
        ++g_d.stale;
        g_d.ages.push_back(uint32_t(std::min<uint64_t>(frame - it->second, 1000000)));
      } else {
        ++g_d.never;
      }
    }
    Report();
  }
  __imp__sub_82742B10(ctx, base);
}
