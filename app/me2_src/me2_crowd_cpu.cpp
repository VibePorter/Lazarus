// Mass Effect 2 - dynamic light environment (DLE) updates every N-th frame. Ported from masseffect-cinnamon (ME1
// Switch fork, app/src/native/me_crowd_cpu.cpp, masseffect_dle_visible_every; shipped there as 2).
//
// WHAT THE ENGINE DOES (ME2, from the disassembly, 2026-10-26)
//   UDynamicLightEnvironmentComponent (StaticClass sub_82423C78, 196 bytes, vtable 0x820D65D8) and SFXGame's
//   UBioDynamicLightEnvironmentComponent (sub_82409B10, 320 bytes, vtable 0x8218EFD8, its subclass; ME2's characters
//   use it). Vtable slot 81 is Attach (creates the light environment state at +100: 744 bytes, or 912 for Bio), slot 85
//   is Tick(FLOAT DeltaTime): sub_822F3538 (if bEnabled, bit 31 of +88: the state's Tick, its vtable slot 4) and the
//   Bio override sub_82732B90 (the same, then its own per-light work over the state's light list). Both run on the
//   game thread, every frame, for every attached character's light environment.
//
// WHAT THE CVAR ADDS
//   me2_dle_visible_every = N (2..8): each component's Tick runs only every N-th time it is called, with N times the
//   delta time (so blends such as BlendTime keep their speed); the components are spread over the frames by address,
//   and a component's first Tick runs at once. Character lighting reacts up to N-1 frames late (N = 2: one frame,
//   33 ms at 30 fps). ME2 sets the force-full-update flag from script only (no native writer found), so it is not
//   honoured here; with N = 2 a forced update is at most one frame late. 0/1 = every frame.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <unordered_map>

REXCVAR_DEFINE_INT32(me2_dle_visible_every, 2, "ME2",
                     "Game thread: run each character light environment update only every N-th frame (N times the "
                     "delta time); 0/1 = every frame")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// ME2 (2026-10-27): moving characters every frame. With every 2nd update the shadow of a walking character (Shepard,
// the squad) jerked: UE3 places a character's shadow from its light environment's bounds, which lagged a frame behind
// the character. The state (component +100) keeps the owner's bounds of its last update at +8 (FBoxSphereBounds: origin
// x, y, z); its own update (state vtable slot 1, sub_822F0A10) takes them from the owner's primitive components
// (owner = component +76, Components = owner +92 / +96; a primitive whose light environment (+232) is this component
// has its bounds at +204, kept current by UE3 as the actor moves). A component whose primitive's bounds origin is more
// than this many units from the state's runs at once (a walk is ~7-13 units a frame at 30 fps; a standing character's
// idle animation moves its bounds a little).
REXCVAR_DEFINE_DOUBLE(me2_dle_moving_units, 3.0, "ME2",
                      "me2_dle_visible_every: a character whose bounds moved more than this many units since its last "
                      "light environment update is updated at once (0 = no exception)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ME2 (2026-10-26): the Switch's core layout for ME2. UE3's game thread (the heaviest guest thread: actors, animation,
// physics, script) gets core 0 alone; every other guest thread cores 1-2 (Renut's render thread is on core 1, its record
// worker on core 2). Applied by the game thread itself at its first light environment tick (component ticks run on the
// game thread), through the SDK's layout (system/xthread.cpp RexSetGuestThreadCoreLayout: the caller is the main
// thread; the others move at their next wait). Replaces sa2_renut_core_layout, which pinned ME2's Swap thread instead.
// 2026-10-26 (boss-fight profile): the game thread was 18-45 % of a core, the busiest guest thread 60-80 % was another:
// UE3's render thread, which also runs Renut's D3D hooks for every draw. So the default (1) gives core 0 to the thread
// that issues the D3D draws (me2_hooks.cpp); 2 = the game thread (here, at its first light environment tick); 0 = off.
REXCVAR_DEFINE_INT32(me2_core_layout, 1, "ME2",
                     "Switch: the guest thread alone on core 0, the others on cores 1-2: 1 = UE3's render thread (the "
                     "one issuing D3D draws), 2 = UE3's game thread, 0 = off (every guest thread on any core)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The caller variant: the XEX main thread idles in ME2; the thread that ticks the components is the game thread.
extern "C" uint32_t RexSetGuestThreadCoreLayoutCaller(int main_core, uint32_t others_mask, int others_preferred);

REX_EXTERN(__imp__sub_822F3538);  // UDynamicLightEnvironmentComponent::Tick(FLOAT DeltaTime)
REX_EXTERN(__imp__sub_82732B90);  // UBioDynamicLightEnvironmentComponent::Tick(FLOAT DeltaTime)

namespace {

std::atomic<int> g_every{-1};  // -1 = not read yet (no function-local static guard on a per-frame path)

// Game thread only.
struct Slot {
  uint32_t countdown = 0;  // calls until the next run
  uint32_t frames = 0;     // calls since the last run (the delta time is scaled by it)
};
std::unordered_map<uint32_t, Slot> g_counters;
uint64_t g_ran = 0, g_skipped = 0;
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();

int Every() {
  int every = g_every.load(std::memory_order_relaxed);
  if (every < 0) {
    every = REXCVAR_GET(me2_dle_visible_every);
    g_every.store(every, std::memory_order_relaxed);
#if REX_PLATFORM_SWITCH
    if (REXCVAR_GET(me2_core_layout) == 2) {
      const uint32_t moved = RexSetGuestThreadCoreLayoutCaller(0, 0x6, 2);  // this (game) thread on core 0
      REXLOG_INFO("[crowd] me2_core_layout: UE3's game thread alone on core 0, the other guest threads on cores 1-2 "
                  "({} threads)", moved);
    }
#endif
    REXLOG_INFO("[crowd] light environment updates: {}", every > 1 ? fmt::format("every {} frames", every)
                                                                  : std::string("every frame"));
  }
  return every;
}

uint64_t g_moving = 0;  // updates run early because the character moved (10 s count)

uint32_t Load32(const uint8_t* base, uint32_t va) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va)); }
float LoadF(const uint8_t* base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
bool GuestPointer(uint32_t va) { return va >= 0x10000u && va < 0xF0000000u; }

// The component's character moved more than me2_dle_moving_units since its light environment's last update.
bool Moved(const uint8_t* base, uint32_t component) {
  static const double units = REXCVAR_GET(me2_dle_moving_units);
  if (units <= 0.0) return false;
  const uint32_t state = Load32(base, component + 100), owner = Load32(base, component + 76);
  if (!GuestPointer(state) || !GuestPointer(owner)) return false;
  const uint32_t data = Load32(base, owner + 92), count = Load32(base, owner + 96);
  if (!GuestPointer(data) || count > 256) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t prim = Load32(base, data + 4 * i);
    if (!GuestPointer(prim) || Load32(base, prim + 232) != component) continue;
    const float dx = LoadF(base, prim + 204) - LoadF(base, state + 8);
    const float dy = LoadF(base, prim + 208) - LoadF(base, state + 12);
    const float dz = LoadF(base, prim + 212) - LoadF(base, state + 16);
    const float d2 = dx * dx + dy * dy + dz * dz;
    return d2 == d2 && double(d2) > units * units;  // NaN (not set up yet): not moved
  }
  return false;
}

// true: run the original now (with ctx.f1 scaled); false: skip this frame.
bool Throttle(PPCContext& ctx, const uint8_t* base) {
  const int every = Every();
  if (every <= 1) return true;
  const uint32_t component = ctx.r3.u32;
  if (g_counters.size() > 8192) g_counters.clear();  // components come and go with levels
  auto [it, fresh] = g_counters.try_emplace(component);
  Slot& slot = it->second;
  bool run;
  if (fresh) {
    slot.countdown = 1 + (component >> 4) % uint32_t(every);  // spread over the frames; this first one runs
    slot.frames = 0;
    run = true;
  } else {
    ++slot.frames;
    if (--slot.countdown == 0) {
      slot.countdown = uint32_t(every);
      ctx.f1.f64 = ctx.f1.f64 * double(slot.frames);  // the time since this component's last update
      slot.frames = 0;
      run = true;
    } else if (Moved(base, component)) {
      // A moving character: now, and its turn starts over from here.
      ctx.f1.f64 = ctx.f1.f64 * double(slot.frames);
      slot.frames = 0;
      slot.countdown = uint32_t(every);
      ++g_moving;
      run = true;
    } else {
      run = false;
    }
  }
  if (run) ++g_ran; else ++g_skipped;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last >= std::chrono::seconds(10)) {
    REXLOG_INFO("[crowd] 10 s: light environment ticks {} run ({} of them early: the character moved), {} skipped "
                "(every {}), {} components", g_ran, g_moving, g_skipped, every, g_counters.size());
    g_ran = g_skipped = g_moving = 0;
    g_last = now;
  }
  return run;
}

}  // namespace

REX_HOOK_RAW(sub_822F3538) {
  if (Throttle(ctx, base)) __imp__sub_822F3538(ctx, base);
}

REX_HOOK_RAW(sub_82732B90) {
  if (Throttle(ctx, base)) __imp__sub_82732B90(ctx, base);
}
