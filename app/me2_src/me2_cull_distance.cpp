// Mass Effect 2 - size-aware draw distance for small static meshes. Ported from masseffect-cinnamon (ME1 Switch fork,
// app/src/native/me_cull_distance.cpp).
//
// Small static meshes (rocks, debris, props) are drawn however far away when the designers set no CullDistance, and
// on the console the frame time grows about linearly with the number of draws (2026-10-26 log: ~40 ms + ~21 us a
// draw). Here a small primitive gets a draw distance proportional to its size: beyond me2_cull_small_factor x its
// bounds sphere radius (at least me2_cull_small_min_distance) its static mesh elements are not drawn. At the 360-line
// scene and the game's field of view a primitive at 100 x its radius is a few pixels across. Primitives with a radius
// above me2_cull_small_max_radius (buildings, cliffs, terrain, large props) keep their own distance; dynamic
// primitives (characters, particles, effects) are not on this path at all.
//
// Where: the static mesh element loop of UE3's InitViews, sub_8269E460 (tools/pch_cull_distance.py): it compares
// distance^2 x LODFactor^2 with each element's MaxDrawDistanceSquared (FStaticMesh +212); the patch makes it read the
// maximum through me2_cull_max_distance_sq(max, FPrimitiveSceneInfoCompact*, base). The compact info's bounds:
// origin +16, box extent +28, sphere radius +40 (passed to the frustum test sub_822CDFF0 by the caller sub_8269B040).
// The distance is the LOD-scaled one, as the game's own MinDrawDistance/MaxDrawDistance comparison uses.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

REXCVAR_DEFINE_INT32(me2_cull_small_factor, 100, "ME2",
                     "Small static meshes are not drawn beyond this many times their bounds radius (0 = off; 100 = a "
                     "few pixels across at 360 lines). Fewer draws for the UE3 render thread, Renut and the GPU")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_cull_small_max_radius, 256, "ME2",
                     "me2_cull_small_factor applies only to static meshes with a bounds radius up to this (Unreal "
                     "units); larger ones keep their own draw distance")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_cull_small_min_distance, 2000, "ME2",
                     "me2_cull_small_factor never culls closer than this (Unreal units)")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// 2026-10-28 (console profile, Normandy: ~520 of ~2470 draws a frame are the depth prepass, each a full draw for the
// UE3 render thread, the console's limiter): small static meshes are left out of the depth PREPASS only. The base pass
// still draws them and writes their depth (the whole prepass off left the frame identical, 2026-10-26); Umbra loses
// only small occluders. An element is small when its bounds radius x factor is below its (LOD-scaled) distance.
REXCVAR_DEFINE_INT32(me2_prepass_small_factor, 0, "ME2",
                     "Static meshes smaller on screen than 1/factor of their distance (radius x factor < distance) are "
                     "not drawn in the depth prepass (0 = off; 30 ~ 20 px across at 360 lines). The base pass still "
                     "draws them")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-11-01 (run 13 stacks: UE3's render thread is the critical path; ~450 prepass draws a frame in heavy scenes, each
// ~15-20 us of it): an A/B of a second factor in the same run. With me2_prepass_small_factor_ab_s > 0 the factor
// alternates between me2_prepass_small_factor and _b every that many seconds (the "[prepass]" lines name it). A smaller
// factor leaves more out (radius x factor < distance).
REXCVAR_DEFINE_INT32(me2_prepass_small_factor_b, 0, "ME2",
                     "me2_prepass_small_factor_ab_s: the other prepass factor (0 = off in those periods)")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_prepass_small_factor_ab_s, 0, "ME2",
                     "Alternate me2_prepass_small_factor and _b every this many seconds; 0 = only the first")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_prepass_small_max_radius, 512, "ME2",
                     "me2_prepass_small_factor: only static meshes with a bounds radius up to this (Unreal units)")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap
extern "C" bool me2_occlusion_camera(float out[3]);  // me2_occlusion.cpp

namespace {

// Per static mesh Id (FStaticMesh +224): the frame (+1) in which InitViews found it small. UE3 render thread only.
std::vector<uint32_t> g_prepass_small;
uint32_t g_prepass_last_compact = 0;
uint64_t g_prepass_last_frame = ~uint64_t(0);
uint64_t g_prepass_skipped = 0, g_prepass_drawn = 0;
std::chrono::steady_clock::time_point g_prepass_report = std::chrono::steady_clock::now();

// me2_prepass_small_factor(_b, _ab_s): this moment's factor.
int32_t PrepassFactor() {
  static const int32_t a = REXCVAR_GET(me2_prepass_small_factor);
  static const int32_t b = REXCVAR_GET(me2_prepass_small_factor_b);
  static const int32_t ab_s = REXCVAR_GET(me2_prepass_small_factor_ab_s);
  if (ab_s <= 0) return a;
  static const auto start = std::chrono::steady_clock::now();
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
  return ((secs / ab_s) & 1) ? b : a;
}

uint32_t LoadBE32(const uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}

// 2026-10-29 (console, Normandy: 0 elements left out): with Umbra the static meshes are made visible by the command
// interpreter sub_827486B8 (Umbra's object callback), not by the InitViews loop that marks them below. The prepass
// hook therefore also measures the element itself: FStaticMesh -> its FPrimitiveSceneInfo (the offset found once: the
// pointer whose StaticMeshes array, +0x14 / +0x18, holds the mesh) -> the primitive's bounds (FBoxSphereBounds: origin,
// extent, radius at +24; its offset found once from a compact info, whose origin +16 and radius +40 are copies) ->
// distance from the camera Umbra got.
int32_t g_mesh_psi_off = 0xD8;   // FStaticMesh -> FPrimitiveSceneInfo* (PC prologue 2026-10-29: found +0xD8; same binary on the Switch)
int32_t g_psi_bounds_off = 0x30;  // FPrimitiveSceneInfo -> Bounds.Origin (found +0x30; Umbra levels never run the InitViews loop that learns it)
bool GuestPtrOk(uint32_t p) { return p >= 0x10000 && p < 0xFFFF0000u; }
float LoadF32(const uint8_t* base, uint32_t va) {
  const uint32_t b = LoadBE32(base, va);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
void LearnMeshPsi(uint32_t psi, const uint8_t* base);
void LearnBounds(uint32_t compact, const uint8_t* base) {
  const uint32_t psi = LoadBE32(base, compact);
  if (!psi)
    return;
  LearnMeshPsi(psi, base);
  if (g_psi_bounds_off >= 0)
    return;
  const uint32_t o0 = LoadBE32(base, compact + 16), o1 = LoadBE32(base, compact + 20), o2 = LoadBE32(base, compact + 24);
  const uint32_t r = LoadBE32(base, compact + 40);
  if (!r)
    return;
  for (uint32_t off = 0x20; off < 0x400; off += 4)
    if (LoadBE32(base, psi + off) == o0 && LoadBE32(base, psi + off + 4) == o1 && LoadBE32(base, psi + off + 8) == o2 &&
        LoadBE32(base, psi + off + 24) == r) {
      g_psi_bounds_off = int32_t(off);
      REXLOG_INFO("[prepass] FPrimitiveSceneInfo bounds at +0x{:X}", off);
      return;
    }
}
uint32_t MeshPsi(uint32_t mesh, const uint8_t* base) {
  return g_mesh_psi_off >= 0 ? LoadBE32(base, mesh + uint32_t(g_mesh_psi_off)) : 0;
}
// From InitViews, where both are known (the primitive and its StaticMeshes): the word of the mesh that points back at
// its primitive. Only the mesh's own bytes are read.
void LearnMeshPsi(uint32_t psi, const uint8_t* base) {
  if (g_mesh_psi_off >= 0)
    return;
  const uint32_t data = LoadBE32(base, psi + 0x14);
  const uint32_t count = LoadBE32(base, psi + 0x18);
  if (!data || count == 0 || count > 256)
    return;
  const uint32_t mesh = LoadBE32(base, data);
  if (!mesh)
    return;
  for (uint32_t off = 0xC0; off < 0x140; off += 4)
    if (LoadBE32(base, mesh + off) == psi) {
      g_mesh_psi_off = int32_t(off);
      REXLOG_INFO("[prepass] FStaticMesh -> FPrimitiveSceneInfo at +0x{:X}", off);
      return;
    }
}
// Whether the prepass element's primitive is small for its distance from the camera.
bool ElementSmall(uint32_t element, const uint8_t* base, int32_t factor, float max_radius) {
  if (g_psi_bounds_off < 0 || g_mesh_psi_off < 0)
    return false;
  const uint32_t mesh = LoadBE32(base, element + 4);
  if (!GuestPtrOk(mesh))
    return false;
  const uint32_t psi = MeshPsi(mesh, base);
  if (!GuestPtrOk(psi))
    return false;
  const uint32_t b = psi + uint32_t(g_psi_bounds_off);
  const float radius = LoadF32(base, b + 24);
  if (!(radius > 0.0f) || radius > max_radius)
    return false;
  static uint64_t cam_frame = ~uint64_t(0);
  static float cam[3];
  static bool cam_ok = false;
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  if (frame != cam_frame) {
    cam_frame = frame;
    cam_ok = me2_occlusion_camera(cam);
  }
  if (!cam_ok)
    return false;
  const float dx = LoadF32(base, b) - cam[0], dy = LoadF32(base, b + 4) - cam[1], dz = LoadF32(base, b + 8) - cam[2];
  const float d = radius * float(factor);
  return dx * dx + dy * dy + dz * dz > d * d;
}

// The compact's primitive (+0 FPrimitiveSceneInfo; its StaticMeshes: data +0x14, count +0x18; each FStaticMesh's Id
// at +224): every element marked small for this frame.
void MarkPrepassSmall(uint32_t compact, const uint8_t* base, float radius, double dist_sq) {
  const int32_t factor = PrepassFactor();
  static const float max_radius = float(REXCVAR_GET(me2_prepass_small_max_radius));
  if (factor <= 0)
    return;
  LearnBounds(compact, base);
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  if (compact == g_prepass_last_compact && frame == g_prepass_last_frame)
    return;  // the primitive's next element: already decided
  g_prepass_last_compact = compact;
  g_prepass_last_frame = frame;
  if (!(radius > 0.0f) || radius > max_radius)
    return;
  const double d = double(radius) * double(factor);
  if (dist_sq <= d * d)
    return;
  const uint32_t psi = LoadBE32(base, compact);
  if (!psi)
    return;
  const uint32_t data = LoadBE32(base, psi + 0x14);
  const int32_t count = int32_t(LoadBE32(base, psi + 0x18));
  for (int32_t i = 0; data && i < count && i < 256; ++i) {
    const uint32_t mesh = LoadBE32(base, data + uint32_t(i) * 4);
    if (!mesh)
      continue;
    const uint32_t id = LoadBE32(base, mesh + 224);
    if (id >= (1u << 22))
      continue;
    if (id >= g_prepass_small.size())
      g_prepass_small.resize(size_t(id) + 4096, 0);
    g_prepass_small[id] = uint32_t(frame) + 1;
  }
}

// The prepass's per-element draw (static depth draw lists, sub_8269F3E8 -> sub_826A07E8 and sub_8269F540 ->
// sub_826A08D0; the menu path's sub_82748018 too): r4 = the element, +0 its static mesh Id. A small one is not drawn
// (the next element of its drawing policy then sets the policy's shared state, as for a first element).
bool PrepassSkip(uint32_t element, const uint8_t* base) {
  const int32_t factor = PrepassFactor();
  if (factor <= 0 || !element)
    return false;
  const uint32_t id = LoadBE32(base, element);
  const uint32_t frame = uint32_t(g_sa2_game_frames.load(std::memory_order_relaxed)) + 1;
  static const float max_radius = float(REXCVAR_GET(me2_prepass_small_max_radius));
  const bool skip = (id < g_prepass_small.size() && g_prepass_small[id] == frame) ||
                    ElementSmall(element, base, factor, max_radius);
  ++(skip ? g_prepass_skipped : g_prepass_drawn);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_prepass_report >= std::chrono::seconds(10)) {
    REXLOG_INFO("[prepass] last ~10 s: {} static depth prepass elements drawn, {} small ones left out (factor {})",
                g_prepass_drawn, g_prepass_skipped, factor);
    g_prepass_drawn = g_prepass_skipped = 0;
    g_prepass_report = now;
  }
  return skip;
}

struct Settings {
  bool on = false;
  double factor = 0.0, max_radius = 0.0, min_distance = 0.0;
};

// Read once (the configuration is loaded before the first frame); a namespace-scope flag instead of a function-local
// static, so the per-element path has no guard check.
Settings g_settings;
std::atomic<bool> g_ready{false};
std::mutex g_init_mutex;

const Settings& Get() {
  if (g_ready.load(std::memory_order_acquire)) return g_settings;
  std::lock_guard lock(g_init_mutex);
  if (!g_ready.load(std::memory_order_relaxed)) {
    g_settings.factor = double(REXCVAR_GET(me2_cull_small_factor));
    g_settings.max_radius = double(REXCVAR_GET(me2_cull_small_max_radius));
    g_settings.min_distance = double(REXCVAR_GET(me2_cull_small_min_distance));
    g_settings.on = g_settings.factor > 0.0;
    REXLOG_INFO("[cull] small static meshes: {} (beyond {} x radius, at least {} units, radius up to {})",
                g_settings.on ? "on" : "off", g_settings.factor, g_settings.min_distance, g_settings.max_radius);
    g_ready.store(true, std::memory_order_release);
  }
  return g_settings;
}

// UE3's render thread only (InitViews).
uint64_t g_calls = 0, g_clamped = 0, g_culled = 0, g_logged = 0;
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();

}  // namespace

// dist_sq: the element's distance squared x LOD factor squared (f31), only for the statistics: an element is culled
// here when its own maximum would have drawn it and the size-based one does not.
extern "C" double me2_cull_max_distance_sq(double max_sq, uint32_t compact, uint8_t* base, double dist_sq) {
  const Settings& s = Get();
  if (!compact) return max_sq;
  uint32_t bits;
  std::memcpy(&bits, base + compact + 40, sizeof(bits));
  bits = __builtin_bswap32(bits);
  float radius;
  std::memcpy(&radius, &bits, sizeof(radius));
  MarkPrepassSmall(compact, base, radius, dist_sq);
  if (!s.on) return max_sq;
  ++g_calls;
  if (g_logged < 8) {
    ++g_logged;
    REXLOG_INFO("[cull] sample primitive {:08X}: bounds radius {:.1f}, own max draw distance {:.0f}", compact, radius,
                max_sq > 0.0 && max_sq < 1e30 ? std::sqrt(max_sq) : -1.0);
  }
  if ((g_calls & 0xFFF) == 0) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last >= std::chrono::seconds(10)) {
      REXLOG_INFO("[cull] last ~10 s: {} static mesh element tests, {} with a size-based draw distance, {} culled by it "
                  "({:.1f} %)", g_calls, g_clamped, g_culled, g_calls ? 100.0 * double(g_culled) / double(g_calls) : 0.0);
      g_calls = g_clamped = g_culled = 0;
      g_last = now;
    }
  }
  if (!(radius > 0.0f) || radius > s.max_radius) return max_sq;
  const double d = std::max(s.min_distance, double(radius) * s.factor);
  const double d2 = d * d;
  if (d2 >= max_sq) return max_sq;
  ++g_clamped;
  if (dist_sq >= d2 && dist_sq < max_sq) ++g_culled;
  return d2;
}

REX_EXTERN(__imp__sub_826A07E8);
REX_HOOK_RAW(sub_826A07E8) {
  if (PrepassSkip(ctx.r4.u32, base))
    return;
  __imp__sub_826A07E8(ctx, base);
}
REX_EXTERN(__imp__sub_826A08D0);
REX_HOOK_RAW(sub_826A08D0) {
  if (PrepassSkip(ctx.r4.u32, base))
    return;
  __imp__sub_826A08D0(ctx, base);
}

// ---------------------------------------------------------------------------------------------------------------------
// me2-nx-deko (2026-11-02): the same size-based draw distance on UMBRA levels, where the InitViews loop above is
// (almost) never reached. Umbra asks the game about every instance it reaches with command 50 of sub_827486B8 (Umbra's
// object callback): the instance (Umbra command [self+4], +48 = its index into the table [0x833960E8]; entry +0 =
// FPrimitiveSceneInfo*, +8..+16 position, +20 its max draw distance) is answered "skip_instance" (sub_82DDDD90(self,
// 0x821932F4)) when hidden or when its distance x the view's LOD factor (0x8340C920, camera at 0x8340C924, both written
// by command 0/1 from the FSceneView; on only in perspective views, flag 0x83406568) exceeds its max draw distance.
// A skipped instance is drawn in no pass at all: no depth prepass (no hole), no base pass, no light passes, no
// occlusion box. Here the same command also skips a primitive that is small for its distance: bounds radius (PSI
// +0x48) x factor < distance x LOD factor, never closer than the minimum distance. Primitives with static meshes
// (PSI StaticMeshes count +0x18 > 0) use me2_cull_small_*; the others (characters, movable props, effects) their own
// me2_cull_dynamic_* settings.
REXCVAR_DEFINE_INT32(me2_cull_dynamic_factor, 100, "ME2",
                     "Umbra levels: primitives without static meshes (characters, movable props, effects) are not "
                     "drawn beyond this many times their bounds radius (0 = off)")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_cull_dynamic_max_radius, 512, "ME2",
                     "me2_cull_dynamic_factor applies only to primitives with a bounds radius up to this (Unreal units)")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_cull_dynamic_min_distance, 2000, "ME2",
                     "me2_cull_dynamic_factor never culls closer than this (Unreal units)")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_cull_umbra_ab_s, 0, "ME2",
                     "A/B: the size-based culling of Umbra instances (static and dynamic) on and off in turn every "
                     "this many seconds; 0 = always on (as the factors say)")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
constexpr uint32_t kUmbraObjectTable = 0x833960E8;  // [table]: entry pointers by the command's +48 index
constexpr uint32_t kUmbraLodOn = 0x83406568;        // the view has a LOD distance (perspective)
constexpr uint32_t kUmbraLodFactor = 0x8340C920;    // FSceneView +496, then the camera position (+384..+392)
constexpr uint32_t kSkipInstanceString = 0x821932F4;  // "skip_instance"
struct UmbraCullStats {
  uint64_t tested = 0, static_skipped = 0, dynamic_skipped = 0;
  std::chrono::steady_clock::time_point report = std::chrono::steady_clock::now();
} g_umbra_cull;  // UE3's render thread only (Umbra's traversal)

bool UmbraCullOn() {
  static const int32_t ab_s = REXCVAR_GET(me2_cull_umbra_ab_s);
  if (ab_s <= 0) return true;
  static const auto start = std::chrono::steady_clock::now();
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
  return ((secs / ab_s) & 1) == 0;
}

void UmbraCullReport() {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_umbra_cull.report < std::chrono::seconds(10)) return;
  REXLOG_INFO("[cull] Umbra last ~10 s: {} instances tested, {} small static and {} small dynamic ones skipped "
              "({:.1f} %){}",
              g_umbra_cull.tested, g_umbra_cull.static_skipped, g_umbra_cull.dynamic_skipped,
              g_umbra_cull.tested ? 100.0 * double(g_umbra_cull.static_skipped + g_umbra_cull.dynamic_skipped) /
                                        double(g_umbra_cull.tested)
                                  : 0.0,
              UmbraCullOn() ? "" : " (A/B: off now)");
  g_umbra_cull.tested = g_umbra_cull.static_skipped = g_umbra_cull.dynamic_skipped = 0;
  g_umbra_cull.report = now;
}
}  // namespace

REX_EXTERN(__imp__sub_82DDDD90);  // Umbra: the callback's answer (self, "skip_instance")

// Called by the sub_827486B8 hook (me2_occlusion.cpp) for command 50 before the original; true = answered (skipped).
extern "C" bool me2_umbra_size_cull(PPCContext& ctx, uint8_t* base) {
  static const int32_t static_factor = REXCVAR_GET(me2_cull_small_factor);
  static const int32_t dynamic_factor = REXCVAR_GET(me2_cull_dynamic_factor);
  if (static_factor <= 0 && dynamic_factor <= 0) return false;
  static const float static_max = float(REXCVAR_GET(me2_cull_small_max_radius));
  static const float static_min = float(REXCVAR_GET(me2_cull_small_min_distance));
  static const float dynamic_max = float(REXCVAR_GET(me2_cull_dynamic_max_radius));
  static const float dynamic_min = float(REXCVAR_GET(me2_cull_dynamic_min_distance));
  const uint32_t self = ctx.r3.u32;
  if (!GuestPtrOk(self) || !LoadBE32(base, kUmbraLodOn)) return false;
  const uint32_t cmd = LoadBE32(base, self + 4);
  if (!GuestPtrOk(cmd)) return false;
  const uint32_t index = LoadBE32(base, cmd + 48);
  const uint32_t table = LoadBE32(base, kUmbraObjectTable);
  if (index == 0xFFFFFFFFu || index > (1u << 20) || !GuestPtrOk(table)) return false;
  const uint32_t entry = LoadBE32(base, table + index * 4u);
  if (!GuestPtrOk(entry)) return false;
  const uint32_t psi = LoadBE32(base, entry);
  if (!GuestPtrOk(psi) || g_psi_bounds_off < 0) return false;
  // me2-nx-deko (2026-11-03): the report's clock read once per 1,024 tests, not per test (~1,300 tests a frame).
  if ((++g_umbra_cull.tested & 1023) == 0)
    UmbraCullReport();
  if (!UmbraCullOn()) return false;
  const bool is_static = int32_t(LoadBE32(base, psi + 0x18)) > 0;  // StaticMeshes.Num()
  const int32_t factor = is_static ? static_factor : dynamic_factor;
  if (factor <= 0) return false;
  const uint32_t b = psi + uint32_t(g_psi_bounds_off);
  const float radius = LoadF32(base, b + 24);
  if (!(radius > 0.0f) || radius > (is_static ? static_max : dynamic_max)) return false;
  const float lod = LoadF32(base, kUmbraLodFactor);
  if (!(lod > 0.0f)) return false;
  const float dx = LoadF32(base, b) - LoadF32(base, kUmbraLodFactor + 4);
  const float dy = LoadF32(base, b + 4) - LoadF32(base, kUmbraLodFactor + 8);
  const float dz = LoadF32(base, b + 8) - LoadF32(base, kUmbraLodFactor + 12);
  const float d = std::max(is_static ? static_min : dynamic_min, radius * float(factor));
  if ((dx * dx + dy * dy + dz * dz) * lod * lod <= d * d) return false;
  ++(is_static ? g_umbra_cull.static_skipped : g_umbra_cull.dynamic_skipped);
  // The original's own skip path: sub_82DDDD90(self, "skip_instance"), then return.
  ctx.r3.u64 = self;
  ctx.r4.u64 = kSkipInstanceString;
  __imp__sub_82DDDD90(ctx, base);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// me2-nx-deko (2026-11-01, run 14 "[draw-callers]": heavy scenes draw ~1,260 times a frame, 407 of them additive per-light
// passes; UE3's render thread is the critical path at ~20 us a draw). sub_82688C70(FSceneRenderer r3,
// FLightSceneInfo r4, DPG r5) draws one dynamic light for every view: per view (renderer +64 data / +68 count, 1,776
// bytes each) its FVisibleLightInfo (view +976, 116 bytes a light by the light's Id at +244) holds per DPG (28 bytes)
// two TArray<FPrimitiveSceneInfo*> (+4 and +16: data, count) of the dynamic primitives the light reaches; the loops call
// each visible one's proxy DrawDynamicElements (vtable +12 / +28) with the light's drawer, one additive draw per mesh
// element. me2_light_small_factor: before the function runs, primitives whose bounds radius x factor is below their
// distance from the camera (a few dozen pixels across or less) are taken out of those two lists, which are put back
// after it: those small or distant objects get no light from this dynamic light (their base pass, shadows and every
// other pass are unchanged). The user accepted a slight change on small / distant objects for steadier frame times.
// 2026-11-01 (run 16, A/B every 60 s, windows paired across each switch): 25 = +6.6 % fps (+-2.6), +9.5 % in heavy
// scenes; up to 50 % of the dynamic and 24 % of the static light draws left out. On by default.
REXCVAR_DEFINE_INT32(me2_light_small_factor, 25, "ME2",
                     "UE3's dynamic-light passes leave out primitives smaller on screen than 1/factor of their distance "
                     "(radius x factor < distance); 0 = off (every lit primitive)")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_light_small_max_radius, 512, "ME2",
                     "me2_light_small_factor: only primitives with a bounds radius up to this (Unreal units)")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_light_small_factor_ab_s, 0, "ME2",
                     "A/B: me2_light_small_factor on and off in turn every this many seconds; 0 = always on")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
void StoreBE32(uint8_t* base, uint32_t va, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + va, &v, 4);
}
int32_t LightFactor() {
  static const int32_t f = REXCVAR_GET(me2_light_small_factor);
  static const int32_t ab_s = REXCVAR_GET(me2_light_small_factor_ab_s);
  if (f <= 0 || ab_s <= 0) return f;
  static const auto start = std::chrono::steady_clock::now();
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
  return ((secs / ab_s) & 1) ? 0 : f;
}
struct LightListSave {
  uint32_t array_va = 0;  // the TArray (data at +0, count at +4)
  uint32_t count = 0;
  std::vector<uint32_t> items;
};
thread_local std::vector<LightListSave> t_light_saved;
uint64_t g_light_kept = 0, g_light_left = 0, g_light_calls = 0;
// Static meshes in the light's draw lists (see the DrawElement hooks below): drawn / left out.
uint64_t g_light_static_kept = 0, g_light_static_left = 0;
// The factor of the light pass running on this thread (0 = not in one, or off).
thread_local int32_t t_light_factor = 0;
std::chrono::steady_clock::time_point g_light_report = std::chrono::steady_clock::now();
}  // namespace

namespace {
void LightReport(int32_t factor) {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_light_report < std::chrono::seconds(10)) return;
  REXLOG_INFO("[light-cull] last ~10 s: {} light passes, {} lit dynamic primitives kept, {} small / distant ones left "
              "out ({:.0f} %); static mesh light draws: {} drawn, {} small / distant ones left out ({:.0f} %); factor "
              "now {}",
              g_light_calls, g_light_kept, g_light_left,
              g_light_kept + g_light_left ? 100.0 * double(g_light_left) / double(g_light_kept + g_light_left) : 0.0,
              g_light_static_kept, g_light_static_left,
              g_light_static_kept + g_light_static_left
                  ? 100.0 * double(g_light_static_left) / double(g_light_static_kept + g_light_static_left)
                  : 0.0,
              factor);
  g_light_kept = g_light_left = g_light_calls = 0;
  g_light_static_kept = g_light_static_left = 0;
  g_light_report = now;
}
}  // namespace

// me2-nx-deko (2026-11-02): whole dynamic lights that are small for their distance are not drawn at all (all their
// passes in every view). The light's sphere: UE3's point / spot light keeps LightToWorld = scale(Radius) x
// translation(Position) followed by Position (FVector4, W 1); the offset is learned from the first lights (the
// matrix's translation row equals the position after it, W = 1, column 3 = 0 0 0 1) and the radius is the length of
// its first row (rows of equal length: uniform scale). Directional lights (W 0) and anything not matching keep drawing.
REXCVAR_DEFINE_INT32(me2_light_whole_factor, 25, "ME2",
                     "Whole dynamic (point / spot) lights whose radius x factor is below their distance from the camera "
                     "are not drawn (0 = off)")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_light_whole_ab_s, 0, "ME2",
                     "A/B: me2_light_whole_factor on and off in turn every this many seconds; 0 = always on")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
int32_t g_light_l2w_off = -1;  // FLightSceneInfo -> LightToWorld (Position at +64), learned
int32_t g_light_l2w_candidate = -1, g_light_l2w_votes = 0;
uint32_t g_light_l2w_last = 0;
uint64_t g_light_whole_tested = 0, g_light_whole_skipped = 0, g_light_whole_unsized = 0;
constexpr uint32_t kLightRadiusOff = 0x220;  // point-light scene info: Radius, then InvRadius at +0x224
std::chrono::steady_clock::time_point g_light_whole_report = std::chrono::steady_clock::now();

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::fabs(a)); }

// Whether 'off' looks like LightToWorld followed by a point position.
bool LightLayoutAt(uint32_t light, const uint8_t* base, uint32_t off) {
  const uint32_t m = light + off;
  if (LoadF32(base, m + 12) != 0.0f || LoadF32(base, m + 28) != 0.0f || LoadF32(base, m + 44) != 0.0f ||
      LoadF32(base, m + 60) != 1.0f || LoadF32(base, m + 76) != 1.0f)
    return false;
  float len2 = 0.0f;
  for (int k = 0; k < 3; ++k) {
    const float t = LoadF32(base, m + 48 + 4 * k);
    if (!Near(t, LoadF32(base, m + 64 + 4 * k)))
      return false;
    len2 += t * t;
  }
  return len2 > 1.0f;  // not the origin (an identity-like matrix)
}

void LearnLightLayout(uint32_t light, const uint8_t* base) {
  if (g_light_l2w_off >= 0 || g_light_l2w_votes < 0 || light == g_light_l2w_last)
    return;
  g_light_l2w_last = light;
  int32_t found = -1;
  for (uint32_t off = 8; off <= 0x1C0; off += 4)
    if (LightLayoutAt(light, base, off)) {
      found = int32_t(off);
      break;
    }
  if (found < 0)
    return;  // a directional light, or not this layout
  if (found == g_light_l2w_candidate) {
    if (++g_light_l2w_votes >= 4) {
      g_light_l2w_off = found;
      REXLOG_INFO("[light-cull] FLightSceneInfo LightToWorld at +0x{:X}, Position at +0x{:X} (learned from 4 lights)",
                  found, found + 64);
    }
  } else {
    g_light_l2w_candidate = found;
    g_light_l2w_votes = 1;
  }
}

// True when the whole light is small for its distance. Radius: the length of LightToWorld's first row; rows of equal
// length required (a uniform scale), and a plausible radius.
bool LightWholeSmall(uint32_t light, const uint8_t* base, const float cam[3]) {
  static const int32_t factor = REXCVAR_GET(me2_light_whole_factor);
  static const int32_t ab_s = REXCVAR_GET(me2_light_whole_ab_s);
  if (factor <= 0)
    return false;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_light_whole_report >= std::chrono::seconds(10)) {
    REXLOG_INFO("[light-cull] whole lights last ~10 s: {} point light passes with a radius, {} small / distant ones "
                "not drawn (factor {}); {} light passes without a usable radius (other light types){}",
                g_light_whole_tested, g_light_whole_skipped, factor, g_light_whole_unsized,
                g_light_l2w_off < 0 ? ", LightToWorld layout not learned yet" : "");
    g_light_whole_tested = g_light_whole_skipped = g_light_whole_unsized = 0;
    g_light_whole_report = now;
  }
  LearnLightLayout(light, base);
  if (g_light_l2w_off < 0 || !LightLayoutAt(light, base, uint32_t(g_light_l2w_off))) {
    ++g_light_whole_unsized;
    return false;
  }
  const uint32_t m = light + uint32_t(g_light_l2w_off);
  // 2026-11-02 (emulator run: layout learned at +0x80, but no light passed the old radius = |LightToWorld row 0| check:
  // the matrix is not scaled by the radius in ME2). The point-light scene infos (ctors sub_82325A78 / sub_82345B38,
  // base sub_82687BC0) copy ULightComponent+388 Radius to +0x220 and store 1 / Radius at +0x224: used only when the
  // pair agrees (other light classes keep drawing).
  ++g_light_whole_unsized;
  const float radius = LoadF32(base, light + kLightRadiusOff), inv = LoadF32(base, light + kLightRadiusOff + 4);
  if (!(radius >= 16.0f) || radius > 1.0e6f || !(std::fabs(radius * inv - 1.0f) < 1e-3f))
    return false;
  --g_light_whole_unsized;
  ++g_light_whole_tested;
  bool on = true;
  if (ab_s > 0) {
    static const auto start = std::chrono::steady_clock::now();
    on = ((std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() / ab_s) &
          1) == 0;
  }
  const float dx = LoadF32(base, m + 64) - cam[0], dy = LoadF32(base, m + 68) - cam[1],
              dz = LoadF32(base, m + 72) - cam[2];
  const float d = radius * float(factor);
  const bool small = dx * dx + dy * dy + dz * dz > d * d;
  if (small && on)
    ++g_light_whole_skipped;
  return small && on;
}
}  // namespace

REX_EXTERN(__imp__sub_82688C70);
REX_HOOK_RAW(sub_82688C70) {
  {
    float cam[3];
    if (GuestPtrOk(ctx.r4.u32) && me2_occlusion_camera(cam) && LightWholeSmall(ctx.r4.u32, base, cam)) {
      ctx.r3.u64 = 0;  // bDirty FALSE: nothing drawn (the caller sub_82689xxx ORs it into its own)
      return;
    }
  }
  static const bool configured = REXCVAR_GET(me2_light_small_factor) > 0;
  if (!configured) {
    __imp__sub_82688C70(ctx, base);
    return;
  }
  const int32_t factor = LightFactor();
  // The static mesh hooks below see the factor while this light pass runs.
  struct FactorScope {
    explicit FactorScope(int32_t f) { t_light_factor = f; }
    ~FactorScope() { t_light_factor = 0; }
  } factor_scope(factor);
  float cam[3];
  if (factor <= 0 || !me2_occlusion_camera(cam)) {
    ++g_light_calls;
    __imp__sub_82688C70(ctx, base);
    LightReport(factor);
    return;
  }
  static const float max_radius = float(REXCVAR_GET(me2_light_small_max_radius));
  const uint32_t renderer = ctx.r3.u32, light = ctx.r4.u32, dpg = ctx.r5.u32;
  if (!GuestPtrOk(renderer) || !GuestPtrOk(light) || dpg > 4) {
    __imp__sub_82688C70(ctx, base);
    return;
  }
  const int32_t views = int32_t(LoadBE32(base, renderer + 68));
  const uint32_t view_data = LoadBE32(base, renderer + 64);
  const uint32_t id = LoadBE32(base, light + 244);
  const double f = double(factor);
  t_light_saved.clear();
  ++g_light_calls;
  for (int32_t v = 0; v < views && v < 4 && GuestPtrOk(view_data); ++v) {
    const uint32_t view = view_data + uint32_t(v) * 1776u;
    const uint32_t infos = LoadBE32(base, view + 976);
    if (!GuestPtrOk(infos) || id > 4096) continue;
    const uint32_t block = infos + id * 116u + dpg * 28u;
    for (const uint32_t list : {block + 4u, block + 16u}) {
      const uint32_t data = LoadBE32(base, list);
      const int32_t count = int32_t(LoadBE32(base, list + 4));
      if (!GuestPtrOk(data) || count <= 0 || count > 8192) continue;
      LightListSave save;
      save.array_va = list;
      save.count = uint32_t(count);
      save.items.resize(size_t(count));
      uint32_t kept = 0;
      for (int32_t i = 0; i < count; ++i) save.items[size_t(i)] = LoadBE32(base, data + uint32_t(i) * 4u);
      for (int32_t i = 0; i < count; ++i) {
        const uint32_t psi = save.items[size_t(i)];
        bool small = false;
        if (GuestPtrOk(psi)) {
          const uint32_t b = psi + uint32_t(g_psi_bounds_off);
          const float radius = LoadF32(base, b + 24);
          if (radius > 0.0f && radius <= max_radius) {
            const double dx = LoadF32(base, b) - cam[0], dy = LoadF32(base, b + 4) - cam[1],
                         dz = LoadF32(base, b + 8) - cam[2];
            const double d = double(radius) * f;
            small = dx * dx + dy * dy + dz * dz > d * d;
          }
        }
        if (small) {
          ++g_light_left;
        } else {
          StoreBE32(base, data + kept * 4u, psi);
          ++kept;
          ++g_light_kept;
        }
      }
      if (kept != uint32_t(count)) {
        StoreBE32(base, list + 4, kept);
        t_light_saved.push_back(std::move(save));
      }
    }
  }
  __imp__sub_82688C70(ctx, base);
  // The lists as they were (the light's other passes and the next frame's bookkeeping read them).
  for (const LightListSave& save : t_light_saved) {
    const uint32_t data = LoadBE32(base, save.array_va);
    for (uint32_t i = 0; i < save.count; ++i) StoreBE32(base, data + i * 4u, save.items[i]);
    StoreBE32(base, save.array_va + 4, save.count);
  }
  t_light_saved.clear();
  LightReport(factor);
}

// ---------------------------------------------------------------------------------------------------------------------
// me2-nx-deko (2026-11-01, run 15: the dynamic-primitive lists above hold only ~3 primitives a frame; the ~280-450
// additive draws a frame of the light passes are STATIC meshes): inside sub_82688C70 the light's static mesh draw lists
// are drawn by sub_82325DC8 / sub_822E4828 / sub_823462A8 (one per light-interaction type), each three lists (+4, +32,
// +60) whose DrawVisible loops test the view's static mesh visibility bit of each element and call the list's
// DrawElement(view r3, element r4 {+0 static mesh Id, +4 FStaticMesh*}, policy link r5, bool* shared state set r6):
// if the shared state is not set yet it sets the policy's shared state and the flag, then per batch element the
// material parameters, the mesh render state and the draw. The +4 and +60 lists call these six functions (the +32
// lists inline theirs: ~3 % of the draws, left alone). A small / distant element (ElementSmall: bounds radius x
// factor < distance, radius up to me2_light_small_max_radius) is not drawn by the light: the next element of its
// policy then sets the shared state, as for a first element (as the prepass skip). Only while a light pass of
// sub_82688C70 runs on this thread with me2_light_small_factor in force.
namespace {
inline bool LightStaticSkip(uint32_t element, const uint8_t* base) {
  const int32_t factor = t_light_factor;
  if (factor <= 0 || !GuestPtrOk(element))
    return false;
  static const float max_radius = float(REXCVAR_GET(me2_light_small_max_radius));
  const bool skip = ElementSmall(element, base, factor, max_radius);
  ++(skip ? g_light_static_left : g_light_static_kept);
  return skip;
}
}  // namespace

#define ME2_LIGHT_STATIC_HOOK(fn)              \
  REX_EXTERN(__imp__##fn);                     \
  REX_HOOK_RAW(fn) {                           \
    if (LightStaticSkip(ctx.r4.u32, base))     \
      return;                                  \
    __imp__##fn(ctx, base);                    \
  }
ME2_LIGHT_STATIC_HOOK(sub_82329BE0)  // sub_82325DC8 +4 list
ME2_LIGHT_STATIC_HOOK(sub_82329CE0)  // sub_82325DC8 +60 list
ME2_LIGHT_STATIC_HOOK(sub_822E6A18)  // sub_822E4828 +4
ME2_LIGHT_STATIC_HOOK(sub_822E6BF0)  // sub_822E4828 +60
ME2_LIGHT_STATIC_HOOK(sub_823477B0)  // sub_823462A8 +4
ME2_LIGHT_STATIC_HOOK(sub_823479C8)  // sub_823462A8 +60
#undef ME2_LIGHT_STATIC_HOOK

// ---------------------------------------------------------------------------------------------------------------------
// me2-nx-deko (2026-11-02, user: mesh LOD bias): the view's LOD distance factor (FSceneView +496, LODDistanceFactor)
// scaled. UE3 multiplies every LOD and draw-distance decision by it: the static mesh LOD bands (FStaticMesh
// Min/MaxDrawDistance against distance x factor, sub_8269E460's f1), the skeletal mesh LOD (screen size from the views
// of the previous frame), particle LOD, primitive draw distances, and on Umbra levels the per-instance answer of
// command 50 (the global 0x8340C920 that command 0 of sub_827486B8 copies from the view). A scale > 1 makes everything
// "farther": simpler LODs sooner and draw distances (and the size-based culls above) that much shorter. ME2 has no
// static mesh LOD bias setting (only SkeletalMeshLODBias in [SystemSettings]), so this is the static meshes' knob.
// Applied right before command 0 reads the view; each view's value is scaled once (the value written is remembered,
// so a view seen again in the same frame is left alone).
REXCVAR_DEFINE_DOUBLE(me2_lod_distance_scale, 1.0, "ME2",
                      "LOD distance factor scale: > 1 = simpler LODs sooner and shorter draw distances for everything "
                      "(1.5 = as if 1.5 times farther); 1 = the game's")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_lod_distance_ab_s, 0, "ME2",
                     "A/B: me2_lod_distance_scale on and off in turn every this many seconds; 0 = always on")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void me2_lod_distance_scale_view(PPCContext& ctx, uint8_t* base) {
  static const double scale = REXCVAR_GET(me2_lod_distance_scale);
  if (!(scale > 0.0) || scale == 1.0 || !ctx.r3.u32) return;
  static const int32_t ab_s = REXCVAR_GET(me2_lod_distance_ab_s);
  if (ab_s > 0) {
    static const auto start = std::chrono::steady_clock::now();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    if ((secs / ab_s) & 1) return;
  }
  uint32_t view;
  std::memcpy(&view, base + ctx.r3.u32 + 36, 4);
  view = __builtin_bswap32(view);
  if (view < 0x10000u || view >= 0xA0000000u) return;
  uint32_t bits;
  std::memcpy(&bits, base + view + 496, 4);
  bits = __builtin_bswap32(bits);
  // The last value written per view (UE3's render thread only): a view already scaled this frame keeps it.
  static uint32_t last_view[8] = {}, last_bits[8] = {};
  static uint32_t next = 0;
  for (int i = 0; i < 8; ++i)
    if (last_view[i] == view && last_bits[i] == bits) return;
  float factor;
  std::memcpy(&factor, &bits, 4);
  if (!(factor > 0.0f) || factor > 1e6f) return;
  factor = float(double(factor) * scale);
  std::memcpy(&bits, &factor, 4);
  const uint32_t be = __builtin_bswap32(bits);
  std::memcpy(base + view + 496, &be, 4);
  last_view[next & 7] = view;
  last_bits[next & 7] = bits;
  ++next;
  static uint32_t logged = 0;
  if (logged < 3) {
    ++logged;
    REXLOG_INFO("[lod] view {:08X}: LOD distance factor x{:.2f} -> {:.4f}", view, scale, factor);
  }
}
