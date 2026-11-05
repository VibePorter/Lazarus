// me2-nx-deko (2026-11-01): native sub_825DCDA0, UE3's per-draw material pixel shader parameter setup, with the
// evaluated values kept for the rest of the frame.
//
// Run 13's stacks (rex_stacks.log, 28-32 fps band): UE3's render thread is the critical path in heavy scenes, and
// sub_825DCDA0 is 9.3 % of its samples (~15 % of its busy time): 3.8 % itself, the rest the uniform expressions'
// Evaluate trees (sub_82319818 / 823199E8 / 82319B60 / 8231A6E0 and the material instance getters) and the setters. It
// is called by the drawing policies' SetSharedState / SetMeshRenderState for every policy and, on the Umbra base pass
// (sub_82748200 -> sub_826B6678 -> sub_8267AA00), for every element: the same material is set again and again in one
// frame.
//
// sub_825DCDA0(params r3, FMaterialRenderContext* r4 {+0 proxy, +4 time, +8 real time, +12 view}), from the
// disassembly:
//   material = proxy->vtable[0](proxy)                                  (GetMaterial)
//   vector entries (params +0 data, +4 count, 16 bytes: u8 type, +4 index, +8 FShaderParameter):
//     type 15: value = index < material +76 ? (material +72)[index]->vtable[3](expr, ctx, &v) : (0, 0, 0, 1)-ish
//              defaults; sub_822EB710(entry +8, &v)
//     type 8:  the same with material +60 / +64 and other defaults; sub_822C8C10(entry +8, &v, 0)
//   texture entries (params +12 data, +16 count, 16 bytes: u8 type, +4 index, +8 u16 sampler, +10 u16 used):
//     type 16 / 32: tex = (material +84 / +96)[index]->vtable[4](expr, ctx, &tex) (index < +88 / +100), else and when
//     null the default texture ([0x833ED75C] / [0x833ED780]); if used: tex +28 = the double at 0x833E7040 (the
//     texture's last render time), sub_827DB288(sampler, tex +24, tex +20)
//   then (material not null) the view's constants: sub_822C8AC8(params +30, view +64), (+36, view +320),
//   (+42, view +256), sub_822C8C10(params +48, view +384, 0); if material->vtable[24]() : sub_822EB710(params +84,
//   &(1 / view family +56)); and sub_826A2B60(params +54, view, 0).
//
// Within one render command the evaluated values cannot change for one (params, proxy, material, time, view):
// parameter values reach the proxies through render commands of their own. So the first call's values are kept, keyed
// by those and the render command (Me2RenderCommandEpoch, me2_material_cache.cpp: an id that changes with each command);
// the next calls of the same command call only the setters, with the same values, in the same order. 2026-11-01 (run
// 16): this was keyed by the frame (Swap) first. The texture "last render time" store and everything after the loops
// are done on every call.
//
// Self-checked: the first me2_material_params_checks hits and then one in me2_material_params_check_every evaluate
// everything again and compare; any difference logs "[material-params] DIFFERENCE". Runs 16 and 17: one main-menu
// material (6F20A820, vector entry 1, type 15) gives 20.0 and then 2.5 for the same key, within one render command too
// (so not a parameter update between commands: its value depends on something outside the key). Since 2026-11-01 such a
// material is evaluated on every call from then on and the cache stays on for the others (up to 64 materials, then the
// cache is off for the session; before, the first difference turned it off).
//
// 2026-11-05: the cache above keys on the shader's parameter list (params), so an object's base pass and its light
// passes (other shaders, same material) never share it, and the console's stacks put 20 % of the UE3 render thread in
// this function (the per-object draws of 8254AD38 -> 8267F140 -> 82675940 -> 8267A668 most). me2_material_params_expr
// (default on) keys the evaluated values by the uniform expression itself instead: (expression, material render proxy,
// time, real time, view, render command), so every pass and every object with the same material instance reuses them;
// the params-level cache is then not used. Self-checked per expression (an expression that ever differs is evaluated
// on every call from then on). me2_material_params_prefetch (default on): the textures are found first and the memory
// the binds read (the texture parameter, its sampler state, the D3D texture's header) is prefetched while the vector
// parameters are set. "material" in me2_ab_cycle turns the whole native off in its slots (fps A/B).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "hot/me_hot_common.h"
#include "me2_ab_cycle.h"

REXCVAR_DEFINE_BOOL(me2_material_params, false, "ME2",
                    "Native sub_825DCDA0 (UE3's material shader parameter setup) with the evaluated values kept for "
                    "the rest of the frame on the render thread, self-checked")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_material_params_expr, true, "ME2",
                    "me2_material_params: keep the evaluated values per uniform expression (shared by every pass and "
                    "object with the same material instance) instead of per shader parameter list")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_material_params_prefetch, true, "ME2",
                    "me2_material_params: find the textures first and prefetch what their binds read")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_material_params_ab_s, 0, "ME2",
                     "A/B: me2_material_params on and off in turn every this many seconds; 0 = always on")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_material_params_checks, 2048, "ME2",
                     "me2_material_params: the first this many hits are checked against a fresh evaluation")
    .range(0, 1 << 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_material_params_check_every, 256, "ME2",
                     "me2_material_params: after the first checks, one hit in this many is still checked; 0 = none")
    .range(0, 1 << 20)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

uint64_t Me2RenderCommandEpoch(const uint8_t* base);  // me2_material_cache.cpp: changes with each render command
extern std::atomic<uint64_t> g_sa2_game_frames;       // src/sa2_capture.cpp: counted at each Swap

REX_EXTERN(__imp__sub_825DCDA0);
REX_EXTERN(sub_822EB710);  // set a float4 pixel shader parameter (r3 parameter, r4 value)
REX_EXTERN(sub_822C8C10);  // set a float4 parameter (r3 parameter, r4 value, r5 0)
REX_EXTERN(sub_822C8AC8);  // set a matrix parameter (r3 parameter, r4 value)
REX_EXTERN(sub_827DB288);  // set texture + sampler (r3 sampler, r4 sampler state, r5 texture)
REX_EXTERN(sub_826A2B60);  // the scene texture parameters (r3 parameters, r4 view, r5 0)

namespace {
using me::hot::Ld16;
using me::hot::Ld32;
using me::hot::Ld8;
using me::hot::St32;

constexpr uint32_t kOne = 0x82066FACu;          // f29
constexpr uint32_t kDefaultA = 0x820CBFA0u;     // f30
constexpr uint32_t kDefaultB = 0x82000A2Cu;     // f31
constexpr uint32_t kTexture2D = 0x833ED75Cu;    // [ ]: the default texture of type 16 entries
constexpr uint32_t kTextureCube = 0x833ED780u;  // [ ]: type 32
constexpr uint32_t kRenderTime = 0x833E7040u;   // double: written to a used texture +28
constexpr uint32_t kFrameSize = 272;            // the original's frame (its buffers at +80 ... +160)

bool GuestOk(uint32_t p) { return p >= 0x10000 && p < 0xFFFF0000u; }

// Guest functions by address (vtable entries), resolved once.
PPCFunc* Resolve(uint32_t fn) {
  struct Slot {
    uint32_t fn = 0;
    PPCFunc* f = nullptr;
  };
  static Slot slots[1024];
  Slot& s = slots[(fn >> 2) & 1023];
  if (s.fn != fn) {
    s.f = rex::runtime::ResolveIndirectFunction(fn);
    s.fn = fn;
  }
  return s.f;
}
void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t slot_offset) {
  const uint32_t fn = Ld32(base, Ld32(base, object) + slot_offset);
  Resolve(fn)(ctx, base);
}

// One evaluated value: 4 words (vector entries) or the texture (word 0).
struct Value {
  uint32_t w[4];
};

struct Entry {
  uint64_t frame = 0;  // Me2RenderCommandEpoch (0 = empty)
  uint32_t params = 0, proxy = 0, material = 0, time = 0, real_time = 0, view = 0;
  uint32_t vectors = 0, textures = 0;
  uint32_t first = 0;  // index of the first value in g_values
};
constexpr uint32_t kEntries = 4096;
Entry g_cache[kEntries];
std::vector<Value> g_values;
uint64_t g_values_frame = 0;
std::atomic<std::thread::id> g_owner{};
std::atomic<uint64_t> g_owner_frame{0};  // the frame of the owner's last call
bool g_take_over = false;                // a new owner: the table is emptied first (owner thread only)
uint64_t g_owners = 0;                   // threads that owned the table so far
std::atomic<bool> g_disabled{false};
// 2026-11-01 (runs 16-17): one main-menu material (6F20A820, vector entry 1, type 15) gives 20.0 and then 2.5 for the
// same params / proxy / view / time within one render command: its value depends on something outside the key. A
// material that ever differs is evaluated on every call from then on (owner thread only); the cache stays on for
// the rest. Past kMaxExcluded materials the cache turns off for the session.
constexpr size_t kMaxExcluded = 64;
std::vector<uint32_t> g_excluded;
bool Excluded(uint32_t material) {
  return !g_excluded.empty() && std::find(g_excluded.begin(), g_excluded.end(), material) != g_excluded.end();
}
// After a difference (the call already used the fresh values).
void Exclude(uint32_t material) {
  if (Excluded(material)) return;
  if (g_excluded.size() >= kMaxExcluded) {
    g_disabled.store(true, std::memory_order_relaxed);
    REXLOG_ERROR("[material-params] {} materials excluded: the cache is off for this session", g_excluded.size());
    return;
  }
  g_excluded.push_back(material);
  REXLOG_WARN("[material-params] material {:08X} is evaluated on every call from now on ({} excluded); the cache "
              "stays on for the others", material, g_excluded.size());
}
int64_t g_checks_left = -1;
uint32_t g_since_check = 0;
uint64_t g_hits = 0, g_misses = 0, g_checked = 0, g_original = 0;
std::chrono::steady_clock::time_point g_report = std::chrono::steady_clock::now();

bool On() {
  static const bool on = REXCVAR_GET(me2_material_params);
  if (!on || g_disabled.load(std::memory_order_relaxed)) return false;
  if (Me2AbOff(Me2Ab::kMaterial)) return false;
  static const int32_t ab_s = REXCVAR_GET(me2_material_params_ab_s);
  if (ab_s > 0) {
    static const auto start = std::chrono::steady_clock::now();
    const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    if ((secs / ab_s) & 1) return false;  // the "off" half of the A/B
  }
  // 2026-11-01 (run 19: both caches reported for 10 s, then never again in 6 minutes): UE3 renders on another thread
  // after a load (the rendering thread is stopped and started again around loads), and the first owner kept the table
  // for good. The owner may change once it has not called for kOwnerIdleFrames frames; the new one starts from an
  // empty table (g_take_over). Never two threads at once: a thread calling while the owner is active goes to the
  // original.
  const std::thread::id me = std::this_thread::get_id();
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  std::thread::id owner = g_owner.load(std::memory_order_relaxed);
  if (owner == me) {
    g_owner_frame.store(frame, std::memory_order_relaxed);
    return true;
  }
  constexpr uint64_t kOwnerIdleFrames = 30;
  if (owner != std::thread::id{} && frame - g_owner_frame.load(std::memory_order_relaxed) < kOwnerIdleFrames)
    return false;
  if (!g_owner.compare_exchange_strong(owner, me)) return false;
  g_owner_frame.store(frame, std::memory_order_relaxed);
  g_take_over = true;
  ++g_owners;
  return true;
}

// The expression-level cache (me2_material_params_expr): one value per (expression, proxy, time, real time, view,
// render command, kind). Owner thread only, like the rest.
struct XEntry {
  uint64_t epoch = 0;  // Me2RenderCommandEpoch (0 = empty)
  uint32_t expr = 0, proxy = 0, time = 0, real_time = 0, view = 0, kind = 0;
  uint32_t w[4] = {};
};
constexpr uint32_t kXEntries = 16384;
XEntry g_xcache[kXEntries];
uint64_t g_xepoch = 0;  // the render command of the current setup
struct XStats {
  uint64_t lookups = 0, hits = 0, setups = 0, checked = 0, frames0 = 0;
} g_x;
std::vector<uint32_t> g_x_excluded;
bool g_x_off = false;
int64_t g_x_checks_left = -1;
uint32_t g_x_since_check = 0;
bool XOn() {
  static const bool on = REXCVAR_GET(me2_material_params_expr);
  return on && !g_x_off;
}
XEntry& XSlot(uint32_t expr, uint32_t proxy, uint32_t time, uint32_t view, uint32_t kind) {
  uint64_t h = uint64_t(expr) * 0x9E3779B97F4A7C15ull ^ uint64_t(proxy) * 0xC2B2AE3D27D4EB4Full ^
               uint64_t(view) * 0x165667B19E3779F9ull ^ (uint64_t(time) << 29) ^ kind;
  h ^= h >> 29;
  return g_xcache[h & (kXEntries - 1)];
}
// 0: not cached (evaluate, then XStore), 1: a hit (value in w), 2: a hit to check (evaluate, then XCheck).
int XFind(uint32_t expr, uint32_t context, const uint8_t* base, uint32_t kind, uint32_t w[4], XEntry** slot) {
  const uint32_t proxy = Ld32(base, context), time = Ld32(base, context + 4), real_time = Ld32(base, context + 8),
                 view = Ld32(base, context + 12);
  XEntry& e = XSlot(expr, proxy, time, view, kind);
  *slot = &e;
  ++g_x.lookups;
  if (e.epoch != g_xepoch || e.expr != expr || e.proxy != proxy || e.time != time || e.real_time != real_time ||
      e.view != view || e.kind != kind)
    return 0;
  if (!g_x_excluded.empty() && std::find(g_x_excluded.begin(), g_x_excluded.end(), expr) != g_x_excluded.end())
    return 0;
  ++g_x.hits;
  std::memcpy(w, e.w, 16);
  if (g_x_checks_left < 0) g_x_checks_left = REXCVAR_GET(me2_material_params_checks);
  static const int32_t every = REXCVAR_GET(me2_material_params_check_every);
  if (g_x_checks_left > 0) {
    --g_x_checks_left;
    ++g_x.checked;
    return 2;
  }
  if (every > 0 && ++g_x_since_check >= uint32_t(every)) {
    g_x_since_check = 0;
    ++g_x.checked;
    return 2;
  }
  return 1;
}
void XStore(XEntry* e, uint32_t expr, uint32_t context, const uint8_t* base, uint32_t kind, const uint32_t w[4]) {
  e->epoch = g_xepoch;
  e->expr = expr;
  e->proxy = Ld32(base, context);
  e->time = Ld32(base, context + 4);
  e->real_time = Ld32(base, context + 8);
  e->view = Ld32(base, context + 12);
  e->kind = kind;
  std::memcpy(e->w, w, 16);
}
// A checked hit: the fresh value against the kept one.
void XCheck(XEntry* e, uint32_t expr, uint32_t kind, const uint32_t fresh[4], const uint32_t kept[4]) {
  if (std::memcmp(fresh, kept, kind == 2 ? 4 : 16) == 0) return;
  REXLOG_ERROR("[material-params] DIFFERENCE: expression {:08X} (kind {}) proxy {:08X}: {:08X} {:08X} {:08X} {:08X}, "
               "kept {:08X} {:08X} {:08X} {:08X}; evaluated on every call from now on",
               expr, kind, e->proxy, fresh[0], fresh[1], fresh[2], fresh[3], kept[0], kept[1], kept[2], kept[3]);
  e->epoch = 0;
  if (g_x_excluded.size() >= 256) {
    g_x_off = true;
    REXLOG_ERROR("[material-params] 256 expressions excluded: the expression cache is off for this session");
    return;
  }
  g_x_excluded.push_back(expr);
}

void Report() {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_report < std::chrono::seconds(10)) return;
  REXLOG_INFO("[material-params] last ~10 s: {} material setups from the frame's values, {} evaluated ({:.0f} % hits), "
              "{} by the original (no material); {} hits checked so far; {} owner thread(s) so far{}",
              g_hits, g_misses, g_hits + g_misses ? 100.0 * double(g_hits) / double(g_hits + g_misses) : 0.0,
              g_original, g_checked, g_owners, g_disabled.load(std::memory_order_relaxed) ? " (OFF after a difference)" : "");
  if (g_x.lookups)
    REXLOG_INFO("[material-params] expressions last ~10 s: {} setups ({:.0f} a frame), {} expression values: {} from "
                "the command's values ({:.0f} %), {} evaluated; {} hits checked so far, {} expression(s) excluded{}",
                g_x.setups,
                double(g_x.setups) / std::max<double>(1.0, double(g_sa2_game_frames.load() - g_x.frames0)),
                g_x.lookups, g_x.hits, 100.0 * double(g_x.hits) / double(g_x.lookups), g_x.lookups - g_x.hits,
                g_x.checked, g_x_excluded.size(), g_x_off ? " (expression cache OFF)" : "");
  g_x.lookups = g_x.hits = g_x.setups = 0;
  g_x.frames0 = g_sa2_game_frames.load();
  g_hits = g_misses = g_original = 0;
  g_report = now;
}

void Store4(uint8_t* base, uint32_t a, const uint32_t w[4]) {
  for (int i = 0; i < 4; ++i) St32(base, a + 4u * uint32_t(i), w[i]);
}
void Load4(const uint8_t* base, uint32_t a, uint32_t w[4]) {
  for (int i = 0; i < 4; ++i) w[i] = Ld32(base, a + 4u * uint32_t(i));
}

// The vector entry's value as the original computes it, left in its buffer (frame +128 for type 15, +144 for type 8).
void EvalVector(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t entry, uint32_t type, uint32_t material,
                uint32_t context, uint32_t w[4]) {
  const uint32_t index = Ld32(base, entry + 4);
  const uint32_t one = Ld32(base, kOne), a = Ld32(base, kDefaultA), b = Ld32(base, kDefaultB);
  const uint32_t buf = type == 15 ? frame + 128 : frame + 144;
  const uint32_t data_off = type == 15 ? 72 : 60;
  if (material && int32_t(index) < int32_t(Ld32(base, material + data_off + 4))) {
    const uint32_t expr = Ld32(base, Ld32(base, material + data_off) + (index << 2));
    XEntry* slot = nullptr;
    uint32_t kept[4];
    const int found = XOn() ? XFind(expr, context, base, 1, kept, &slot) : 0;
    if (found == 1) {
      std::memcpy(w, kept, 16);
      return;  // SetVector stores it into buf before the setter, as the evaluation would have left it
    }
    ctx.r3.u64 = expr;
    ctx.r4.u64 = context;
    ctx.r5.u64 = buf;
    CallVirtual(ctx, base, expr, 12);
    Load4(base, buf, w);
    if (found == 2) XCheck(slot, expr, 1, w, kept);
    else if (slot) XStore(slot, expr, context, base, 1, w);
    return;
  }
  if (type == 15) {
    w[0] = b, w[1] = b, w[2] = b, w[3] = one;
  } else {
    w[0] = a, w[1] = b, w[2] = a, w[3] = one;
  }
}

// The texture entry's texture as the original finds it (type 16 / 32).
uint32_t EvalTexture(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t entry, uint32_t type, uint32_t material,
                     uint32_t context) {
  const uint32_t index = Ld32(base, entry + 4);
  const uint32_t data_off = type == 32 ? 96 : 84;
  const uint32_t fallback = type == 32 ? kTextureCube : kTexture2D;
  St32(base, frame + 80, 0);
  if (!material || int32_t(index) >= int32_t(Ld32(base, material + data_off + 4))) {
    const uint32_t tex = Ld32(base, fallback);
    St32(base, frame + 80, tex);
    if (tex) return tex;
    // (the original then evaluates anyway: same below)
  }
  const uint32_t expr = Ld32(base, Ld32(base, material + data_off) + (index << 2));
  XEntry* slot = nullptr;
  uint32_t kept[4] = {};
  const int found = XOn() ? XFind(expr, context, base, 2, kept, &slot) : 0;
  uint32_t tex;
  if (found == 1) {
    tex = kept[0];
    St32(base, frame + 80, tex);
  } else {
    ctx.r3.u64 = expr;
    ctx.r4.u64 = context;
    ctx.r5.u64 = frame + 80;
    CallVirtual(ctx, base, expr, 16);
    tex = Ld32(base, frame + 80);
    const uint32_t fresh[4] = {tex, 0, 0, 0};
    if (found == 2) XCheck(slot, expr, 2, fresh, kept);
    else if (slot) XStore(slot, expr, context, base, 2, fresh);
  }
  if (!tex) {
    tex = Ld32(base, fallback);
    St32(base, frame + 80, tex);
  }
  return tex;
}

void SetVector(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t entry, uint32_t type, const uint32_t w[4]) {
  const uint32_t buf = type == 15 ? frame + 128 : frame + 144;
  Store4(base, buf, w);
  ctx.r3.u64 = entry + 8;
  ctx.r4.u64 = buf;
  if (type == 15) {
    sub_822EB710(ctx, base);
  } else {
    ctx.r5.u64 = 0;
    sub_822C8C10(ctx, base);
  }
}

void SetTexture(PPCContext& ctx, uint8_t* base, uint32_t entry, uint32_t tex) {
  if (Ld16(base, entry + 10) == 0) return;
  St32(base, tex + 28, Ld32(base, kRenderTime));
  St32(base, tex + 32, Ld32(base, kRenderTime + 4));
  ctx.r3.u64 = Ld16(base, entry + 8);
  ctx.r4.u64 = Ld32(base, tex + 24);
  ctx.r5.u64 = Ld32(base, tex + 20);
  sub_827DB288(ctx, base);
}

// After the loops: the view's constants and the scene textures, as the original.
void Tail(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t params, uint32_t material, uint32_t context) {
  ctx.r3.u64 = params + 30;
  ctx.r4.u64 = Ld32(base, context + 12) + 64;
  sub_822C8AC8(ctx, base);
  ctx.r3.u64 = params + 36;
  ctx.r4.u64 = Ld32(base, context + 12) + 320;
  sub_822C8AC8(ctx, base);
  ctx.r3.u64 = params + 42;
  ctx.r4.u64 = Ld32(base, context + 12) + 256;
  sub_822C8AC8(ctx, base);
  ctx.r3.u64 = params + 48;
  ctx.r4.u64 = Ld32(base, context + 12) + 384;
  ctx.r5.u64 = 0;
  sub_822C8C10(ctx, base);
  ctx.r3.u64 = material;
  CallVirtual(ctx, base, material, 96);
  if (ctx.r3.u32 != 0) {
    const uint32_t view = Ld32(base, context + 12);
    const float one = me::hot::LdF32(base, kOne);
    const float d = me::hot::LdF32(base, Ld32(base, view) + 56);
    const float v = float(double(one) / double(d));  // fdivs, as the generated code
    me::hot::StF32(base, frame + 80, v);
    ctx.r3.u64 = params + 84;
    ctx.r4.u64 = frame + 80;
    sub_822EB710(ctx, base);
  }
  ctx.r3.u64 = params + 54;
  ctx.r4.u64 = Ld32(base, context + 12);
  ctx.r5.u64 = 0;
  sub_826A2B60(ctx, base);
}

void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t params = ctx.r3.u32, context = ctx.r4.u32;
  const uint32_t sp = ctx.r1.u32, frame = sp - kFrameSize;
  St32(base, frame, sp);  // the back chain, as stwu
  ctx.r1.u32 = frame;

  const uint32_t proxy = Ld32(base, context);
  ctx.r3.u64 = proxy;
  ctx.r4.u64 = proxy;
  CallVirtual(ctx, base, proxy, 0);
  const uint32_t material = ctx.r3.u32;
  if (!material) {
    // Not the cached path: the original from the start (GetMaterial again: it only reads).
    ctx.r1.u32 = sp;
    ctx.r3.u64 = params;
    ctx.r4.u64 = context;
    ++g_original;
    __imp__sub_825DCDA0(ctx, base);
    return;
  }
  const uint32_t vec_data = Ld32(base, params), vec_count = Ld32(base, params + 4);
  const uint32_t tex_data = Ld32(base, params + 12), tex_count = Ld32(base, params + 16);
  const uint32_t time = Ld32(base, context + 4), real_time = Ld32(base, context + 8);
  const uint32_t view = Ld32(base, context + 12);
  const uint64_t now_frame = Me2RenderCommandEpoch(base);
  if (g_take_over) {
    g_take_over = false;
    for (Entry& entry : g_cache) entry.frame = 0;
    g_values_frame = 0;
    for (XEntry& x : g_xcache) x.epoch = 0;
  }
  g_xepoch = now_frame;
  ++g_x.setups;
  const bool xmode = XOn();
  if (g_values_frame != now_frame) {
    g_values.clear();
    g_values_frame = now_frame;
  }
  uint64_t h = uint64_t(params) * 0x9E3779B97F4A7C15ull ^ uint64_t(proxy) * 0xC2B2AE3D27D4EB4Full ^
               uint64_t(view) * 0x165667B19E3779F9ull ^ uint64_t(material) ^ (uint64_t(time) << 32);
  h ^= h >> 31;
  Entry& e = g_cache[h & (kEntries - 1)];
  const bool excluded = xmode || Excluded(material);  // the expression cache replaces the params-level one
  const bool hit = !excluded && e.frame == now_frame && e.params == params && e.proxy == proxy && e.material == material &&
                   e.time == time && e.real_time == real_time && e.view == view && e.vectors == vec_count &&
                   e.textures == tex_count && e.first + vec_count + tex_count <= g_values.size();

  bool check = false;
  if (hit) {
    if (g_checks_left < 0) g_checks_left = REXCVAR_GET(me2_material_params_checks);
    static const int32_t every = REXCVAR_GET(me2_material_params_check_every);
    const bool sampled = every > 0 && ++g_since_check >= uint32_t(every);
    if (sampled) g_since_check = 0;
    if (g_checks_left > 0) {
      --g_checks_left;
      check = true;
    } else if (sampled) {
      check = true;
    }
    if (check) ++g_checked;
    ++g_hits;
  } else {
    ++g_misses;
  }

  const bool record = !hit && !excluded && vec_count < 4096 && tex_count < 4096;
  bool differs = false;
  uint32_t first = uint32_t(g_values.size());
  if (record) g_values.resize(g_values.size() + vec_count + tex_count);

  // me2_material_params_prefetch: the textures first (their evaluation only reads), the texture parameter's line
  // prefetched; after the vectors the sampler state and the D3D texture; the D3D texture's header one bind ahead.
  static const bool prefetch = REXCVAR_GET(me2_material_params_prefetch);
  constexpr uint32_t kMaxPre = 32;
  uint32_t pre_tex[kMaxPre];
  const bool pre = prefetch && xmode && !hit && tex_count <= kMaxPre;
  if (pre) {
    for (uint32_t j = 0; j < tex_count; ++j) {
      const uint32_t entry = tex_data + 16u * j;
      const uint32_t type = Ld8(base, entry);
      pre_tex[j] = 0;
      if (type != 16 && type != 32) continue;
      pre_tex[j] = EvalTexture(ctx, base, frame, entry, type, material, context);
      if (GuestOk(pre_tex[j]) && Ld16(base, entry + 10)) __builtin_prefetch(base + pre_tex[j] + 20);
    }
  }
  for (uint32_t i = 0; i < vec_count; ++i) {
    const uint32_t entry = vec_data + 16u * i;
    const uint32_t type = Ld8(base, entry);
    if (type != 15 && type != 8) continue;
    uint32_t w[4];
    if (hit && !check) {
      std::memcpy(w, g_values[e.first + i].w, 16);
    } else {
      EvalVector(ctx, base, frame, entry, type, material, context, w);
      if (record) std::memcpy(g_values[first + i].w, w, 16);
      if (check && std::memcmp(w, g_values[e.first + i].w, 16) != 0 && !differs) {
        differs = true;
        REXLOG_ERROR("[material-params] DIFFERENCE: params {:08X} proxy {:08X} material {:08X} vector entry {} "
                     "(type {}): {:08X} {:08X} {:08X} {:08X}, kept {:08X} {:08X} {:08X} {:08X}",
                     params, proxy, material, i, type, w[0], w[1], w[2], w[3], g_values[e.first + i].w[0],
                     g_values[e.first + i].w[1], g_values[e.first + i].w[2], g_values[e.first + i].w[3]);
      }
    }
    SetVector(ctx, base, frame, entry, type, w);
  }
  if (pre) {
    for (uint32_t j = 0; j < tex_count; ++j) {
      const uint32_t tex = pre_tex[j];
      if (!GuestOk(tex) || !Ld16(base, tex_data + 16u * j + 10)) continue;
      const uint32_t t = Ld32(base, tex + 20), p = Ld32(base, tex + 24);
      if (GuestOk(t)) __builtin_prefetch(base + t + 8);
      if (GuestOk(p)) __builtin_prefetch(base + p);
    }
  }
  for (uint32_t j = 0; j < tex_count; ++j) {
    const uint32_t entry = tex_data + 16u * j;
    const uint32_t type = Ld8(base, entry);
    if (type != 16 && type != 32) continue;
    if (pre) {
      // the next bound texture's D3D header (the line of its +8 pointer was prefetched above)
      for (uint32_t k = j + 1; k < tex_count; ++k) {
        const uint32_t nt = pre_tex[k];
        if (!GuestOk(nt) || !Ld16(base, tex_data + 16u * k + 10)) continue;
        const uint32_t t = Ld32(base, nt + 20);
        const uint32_t dt = GuestOk(t) ? Ld32(base, t + 8) : 0;
        if (GuestOk(dt)) __builtin_prefetch(base + dt + 28);
        break;
      }
      St32(base, frame + 80, pre_tex[j]);
      SetTexture(ctx, base, entry, pre_tex[j]);
      continue;
    }
    uint32_t tex;
    if (hit && !check) {
      tex = g_values[e.first + vec_count + j].w[0];
      St32(base, frame + 80, tex);
    } else {
      tex = EvalTexture(ctx, base, frame, entry, type, material, context);
      if (record) g_values[first + vec_count + j].w[0] = tex;
      if (check && tex != g_values[e.first + vec_count + j].w[0] && !differs) {
        differs = true;
        REXLOG_ERROR("[material-params] DIFFERENCE: params {:08X} proxy {:08X} material {:08X} texture entry {} "
                     "(type {}): {:08X}, kept {:08X}",
                     params, proxy, material, j, type, tex, g_values[e.first + vec_count + j].w[0]);
      }
    }
    SetTexture(ctx, base, entry, tex);
  }
  if (differs) {
    Exclude(material);
    e.frame = 0;  // the kept values are not to be used again
  }
  if (record) {
    e.frame = now_frame;
    e.params = params;
    e.proxy = proxy;
    e.material = material;
    e.time = time;
    e.real_time = real_time;
    e.view = view;
    e.vectors = vec_count;
    e.textures = tex_count;
    e.first = first;
  }
  Tail(ctx, base, frame, params, material, context);
  ctx.r1.u32 = sp;
}

}  // namespace

extern "C" uint64_t Me2ElemTimeBegin();     // me2_shadow_diag.cpp (me2_element_timing)
extern "C" void Me2ElemTimeEnd(uint64_t t0);
REX_HOOK_RAW(sub_825DCDA0) {
  const uint64_t timed = Me2ElemTimeBegin();
  if (!On() || !GuestOk(ctx.r3.u32) || !GuestOk(ctx.r4.u32)) {
    __imp__sub_825DCDA0(ctx, base);
    Me2ElemTimeEnd(timed);
    return;
  }
  Native(ctx, base);
  Me2ElemTimeEnd(timed);
  Report();
}
