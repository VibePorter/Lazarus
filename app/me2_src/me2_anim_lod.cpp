// Mass Effect 2 - animation LOD: skeletal meshes far away or out of sight update less often.
//
// 2026-11-02 (user: animation and physics LOD, picture cost accepted). ME2's own performance targets ([Stats.Perf] in
// Coalesced.ini: "SkelComp Tick Time=8 ms, Anim Tick Time=4 ms, Skel Skin Time=2 ms") show what characters cost the
// Xbox's game thread every frame. ME2 has no setting for it (no update-rate or animation LOD keys in its ini).
//
// WHAT THE ENGINE DOES (ME2, from the image)
//   USkeletalMeshComponent (StaticClass registration sub_823672C0, constructor sub_82547408, vtable 0x82156D50):
//   vtable slot 85 (UActorComponent::Tick(FLOAT DeltaTime), the slot of the light environment's Tick too) is
//   sub_82537968: the animation tree, the pose, bone controllers, attachments, physics blending, for every skeletal
//   mesh of every actor, every frame (game thread). It finds "recently rendered" itself: LastRenderTime (+432) later
//   than the world time - 1 s (the time: float +776 of [[[[GEngine 0x833D60B0] + 84] + 60] + 0]), or the flag
//   0x40000000 of +760 (update even when not rendered).
//
// WHAT THE CVARS ADD (the scheme of me2_crowd_cpu.cpp's light environment throttle)
//   A component's Tick runs only every N-th time it is called, with the delta time of all the calls since its last run
//   (animation speed, blends and root motion keep their pace; a far character just moves in coarser steps):
//     me2_anim_hidden_every = N: not rendered for a second (and not flagged to update anyway);
//     me2_anim_far_every = N: small on screen, bounds sphere radius (+228, FBoxSphereBounds at +204) / its distance
//       from the camera (the last frame's view position, 0x8340C924, written by the render thread) below
//       me2_anim_far_ratio (0.02 = about 2 % of the distance: a character ~50 times farther than its own radius).
//   Everything else (Shepard, the squad close by, anyone near or large on screen) every frame; a component that
//   becomes near or visible gets its saved-up time at once. Components are spread over the frames by address.
//   0/1 = every frame. me2_anim_ab_s: both on / off in turn for an A/B. "[anim]" every 10 s.
#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

REXCVAR_DEFINE_INT32(me2_anim_hidden_every, 0, "ME2",
                     "Skeletal meshes not rendered for a second update every this many frames (with the saved-up "
                     "time); 0/1 = every frame")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_anim_far_every, 0, "ME2",
                     "Skeletal meshes small on screen (me2_anim_far_ratio) update every this many frames; 0/1 = every "
                     "frame")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_anim_far_ratio, 0.02, "ME2",
                      "me2_anim_far_every applies when bounds radius / camera distance is below this (0.02: farther "
                      "than ~50 times its radius)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_anim_ab_s, 0, "ME2",
                     "A/B: the animation LOD on and off in turn every this many seconds; 0 = always on")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82537968);  // USkeletalMeshComponent::Tick(FLOAT DeltaTime)

namespace {
constexpr uint32_t kGEngine = 0x833D60B0;
constexpr uint32_t kCamera = 0x8340C924;  // the view position (x, y, z), written by sub_827486B8 command 0

uint32_t Load32(const uint8_t* base, uint32_t va) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va)); }
float LoadF(const uint8_t* base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
bool GuestPointer(uint32_t va) { return va >= 0x10000u && va < 0xF0000000u; }

struct Settings {
  int hidden = 0, far = 0, ab_s = 0;
  double ratio = 0.02;
};
const Settings& Get() {
  static const Settings s = [] {
    Settings t;
    t.hidden = REXCVAR_GET(me2_anim_hidden_every);
    t.far = REXCVAR_GET(me2_anim_far_every);
    t.ratio = REXCVAR_GET(me2_anim_far_ratio);
    t.ab_s = REXCVAR_GET(me2_anim_ab_s);
    REXLOG_INFO("[anim] skeletal mesh updates: not rendered every {}, small on screen (radius / distance < {:.3f}) every "
                "{}{}", t.hidden > 1 ? t.hidden : 1, t.ratio, t.far > 1 ? t.far : 1,
                t.ab_s > 0 ? fmt::format(" (A/B every {} s)", t.ab_s) : std::string());
    return t;
  }();
  return s;
}

// Game thread only.
struct Slot {
  uint32_t countdown = 0;
  uint32_t pending = 0;  // calls skipped since the last run
};
std::unordered_map<uint32_t, Slot> g_slots;
uint64_t g_ran = 0, g_skipped = 0, g_hidden = 0, g_far = 0;
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();
const auto g_start = std::chrono::steady_clock::now();

// How often this component updates now (1 = every frame).
int Every(const uint8_t* base, uint32_t component, const Settings& s) {
  if (s.ab_s > 0) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_start).count();
    if ((secs / s.ab_s) & 1) return 1;
  }
  if (s.hidden > 1) {
    const uint32_t a = Load32(base, kGEngine), b = GuestPointer(a) ? Load32(base, a + 84) : 0;
    const uint32_t c = GuestPointer(b) ? Load32(base, b + 60) : 0, d = GuestPointer(c) ? Load32(base, c) : 0;
    if (GuestPointer(d)) {
      const float now = LoadF(base, d + 776), last = LoadF(base, component + 432);
      const bool forced = (Load32(base, component + 760) & 0x40000000u) != 0;
      if (!forced && now == now && last == last && last <= now - 1.0f) {
        ++g_hidden;
        return s.hidden;
      }
    }
  }
  if (s.far > 1) {
    const float r = LoadF(base, component + 228);
    const float dx = LoadF(base, component + 204) - LoadF(base, kCamera);
    const float dy = LoadF(base, component + 208) - LoadF(base, kCamera + 4);
    const float dz = LoadF(base, component + 212) - LoadF(base, kCamera + 8);
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (r > 0.0f && r < 1.0e5f && d2 == d2 && double(r) * double(r) < s.ratio * s.ratio * double(d2)) {
      ++g_far;
      return s.far;
    }
  }
  return 1;
}

// true: run the original now (ctx.f1 = the time since this component's last run); false: skip this frame.
bool Throttle(PPCContext& ctx, const uint8_t* base) {
  const Settings& s = Get();
  if (s.hidden <= 1 && s.far <= 1) return true;
  const uint32_t component = ctx.r3.u32;
  const int every = Every(base, component, s);
  if (g_slots.size() > 8192) g_slots.clear();  // components come and go with levels
  auto [it, fresh] = g_slots.try_emplace(component);
  Slot& slot = it->second;
  bool run;
  if (fresh) {
    slot.countdown = every > 1 ? 1 + (component >> 4) % uint32_t(every) : 1;
    slot.pending = 0;
    run = true;
  } else if (every <= 1 || slot.countdown <= 1) {
    run = true;
  } else {
    --slot.countdown;
    ++slot.pending;
    run = false;
  }
  if (run) {
    if (slot.pending) ctx.f1.f64 = ctx.f1.f64 * double(slot.pending + 1);
    slot.pending = 0;
    if (!fresh) slot.countdown = uint32_t(every > 1 ? every : 1);
    ++g_ran;
  } else {
    ++g_skipped;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last >= std::chrono::seconds(10)) {
    REXLOG_INFO("[anim] 10 s: skeletal mesh ticks {} run, {} skipped; decisions: {} not rendered, {} small on screen; "
                "{} components", g_ran, g_skipped, g_hidden, g_far, g_slots.size());
    g_ran = g_skipped = g_hidden = g_far = 0;
    g_last = now;
  }
  return run;
}
}  // namespace

REX_HOOK_RAW(sub_82537968) {
  if (Throttle(ctx, base)) __imp__sub_82537968(ctx, base);
}
