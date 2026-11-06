// Mass Effect 2 - a software occlusion rasterizer for Umbra's occlusion tests (me2_occlusion_cpu_source 1).
//
// WHY (2026-11-05)
//   Umbra asks for an occlusion test of a node box (command 131) and reads its answer later in the SAME frame (130).
//   The Xbox drew the box against the prepass depth and had the count in time; Renut submits at Swap, so ME2 answers
//   from earlier frames' GPU counts (me2_occlusion_umbra_latent). Turning the camera, many reads have no usable count
//   (cells partly off screen at their count, never counted) and are answered visible: console Kasumi wall 30 fps
//   standing, 15.5-19 fps turning, 1,800-2,800 draws. The reprojected-GPU-depth CPU mode (Oct 28, source 0) had holes
//   wherever the earlier frame had not looked. This one draws the occluders ITSELF with the current camera, before
//   Umbra's traversal, so every test of the frame has an answer, including the leading edge of a turn.
//
// WHAT THE ENGINE HAS (TU2, from the disassembly, 2026-11-05)
//   FScene (sub_8274AA38's r6, = [FSceneRenderer +0]) + DPG * 308 + {48, 76, 104}: the depth draw lists of the
//   depth-priority group (TStaticMeshDrawList; 48 position-only, 76 depth, 104 masked - from the drawing policies
//   their DrawShared calls: 8267FE48 sets the factory's position streams, 8267FBC0 its full streams). A list: +0 data
//   / +4 count of the drawing-policy links (pointers). A link (DrawVisible sub_8269F3E8): +8 the policy (its +12, link
//   +20, the material flags: 0x80000000 two-sided), +28 the elements' sparse array data (16 bytes each: +0 static mesh
//   Id, +4 FStaticMesh*), +52 / +56 its allocation bits (data, NumBits; bit b of word i = element 32 i + b).
//   FStaticMesh (FMeshElement first; draw wrapper sub_826913A0, SetMesh sub_825DD188, SetMeshRenderState sub_826912B8):
//   +0 FIndexBuffer* (+20 its RHI, +8 the D3DIndexBuffer: +0 Common (bit 31 32-bit indices, 29-30 endian), +24 CPU
//   address, +28 bytes), +4 FVertexFactory*, +32 LocalToWorld (FMatrix, row vectors), +176 FirstIndex, +180
//   NumPrimitives, +184 / +188 Min / MaxVertexIndex, +204 flags (bit 31 dynamic data, (>> 25) & 7 primitive type: 0
//   list, 1 strip; 0x40000000 reverse culling), +208 / +212 Min / MaxDrawDistanceSquared (InitViews, LOD choice),
//   +0xD8 FPrimitiveSceneInfo* (bounds origin +0x30), +224 Id. Indices are absolute into the stream (ME2's
//   DrawIndexedVertices has no BaseVertexIndex). The vertex factory's position streams (SetPositionStream inlined in
//   8267FE48): 12 bytes each from +216 (FVertexBuffer*, stride, ?), count +408; FVertexBuffer +20 its RHI, +8 the
//   D3DVertexBuffer: +24 CPU address (low 2 bits: fetch type), +28 fetch dword 1 (endian bits 0-1, size in dwords
//   bits 2-25). The position is assumed a float3 at offset 0 of stream 0 (FPositionVertexBuffer, FModelVertex).
//
// HOW
//   The lists are walked every frame on the UE3 render thread (sub_8274AA38, before Umbra's traversal); a mesh seen
//   for the first time (or with another index buffer / matrix / index range under the same pointer) is copied once into
//   world-space vertices and triangles. Each frame: the cached occluders whose LOD the game draws at this distance and
//   whose box is on screen and large enough are drawn front... largest first, up to a triangle budget, into a small
//   depth buffer of 1/w (nearest kept), sampled at pixel CORNERS; a pixel holds the farthest of its four corners (it
//   is covered only where all four are). A test box is occluded when every pixel under its screen rectangle holds an
//   occluder nearer than the box's nearest corner; any corner at the near plane: undecided (the box is drawn).
//   Compare mode (me2_occlusion_umbra_cpu 1) tallies against the GPU counts of the same tests; mode 2 answers Umbra.
//
// STATUS: compare mode first (memory note me2-occlusion-rasterizer: go on only with >= ~85 % agreement and ~0 false
// occluded). PC first; the per-frame walk and the scalar rasterizer are not tuned for the console yet.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "me2_ab_cycle.h"

#if REX_PLATFORM_SWITCH
#include "threading_switch.h"
#endif

REXCVAR_DEFINE_INT32(me2_occlusion_raster_w, 256, "ME2",
                     "me2_occlusion_cpu_source 1: the occlusion rasterizer's depth buffer width (pixels)")
    .range(64, 1024)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_h, 144, "ME2",
                     "me2_occlusion_cpu_source 1: the occlusion rasterizer's depth buffer height (pixels)")
    .range(36, 576)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_lists, 3, "ME2",
                     "me2_occlusion_cpu_source 1: which depth draw lists give occluders (bits: 1 position-only, 2 "
                     "depth, 4 masked - alpha-tested materials have holes)")
    .range(1, 7)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_tris, 60000, "ME2",
                     "me2_occlusion_cpu_source 1: triangles drawn a frame at most (the largest occluders on screen "
                     "first)")
    .range(1000, 4000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-11-05 (step 2, PC compare: 150k triangles a frame cost 6-7 ms and still left 1,000-2,000 occluders over budget):
// meshes covering few pixels for their triangles (detailed props) give way to large simple ones (walls, floors).
REXCVAR_DEFINE_DOUBLE(me2_occlusion_raster_min_px_per_tri, 1.0, "ME2",
                      "me2_occlusion_cpu_source 1: occluders whose screen rectangle has fewer pixels (of the "
                      "rasterizer's buffer) per triangle are not drawn (0 = no limit)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_mesh_tris, 32768, "ME2",
                     "me2_occlusion_cpu_source 1: meshes with more triangles are not used as occluders")
    .range(16, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_mem_mb, 96, "ME2",
                     "me2_occlusion_cpu_source 1: memory for the cached occluder geometry (MB); new meshes beyond it "
                     "are not used")
    .range(4, 1024)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-06): stricter occluder size limits, tried as the rotation kind "raster_lean" (or always, with
// me2_occlusion_raster_lean). Console 11-06 (Kasumi's party): 212 occluders, 31,400 triangles, 13 ms a frame on the
// worker; dense props and small objects hide little for their triangles.
REXCVAR_DEFINE_BOOL(me2_occlusion_raster_lean, false, "ME2",
                    "me2_occlusion_cpu_source 1: the lean limits below instead of min_px / min_px_per_tri")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_raster_lean_px, 48.0, "ME2",
                      "me2_occlusion_raster_lean: occluders covering fewer buffer pixels are not drawn")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_raster_lean_px_per_tri, 3.0, "ME2",
                      "me2_occlusion_raster_lean: occluders with fewer buffer pixels per triangle are not drawn")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_raster_min_px, 24.0, "ME2",
                      "me2_occlusion_cpu_source 1: occluders whose screen rectangle covers fewer pixels (of the "
                      "rasterizer's buffer) are not drawn")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// D3D's 'clockwise' is on the screen with y down. UE3 draws CM_CW (cull mode 1) unless reversed; which screen winding
// that culls on the Xbox (viewport flips) is measured with me2_occlusion_raster_dual.
REXCVAR_DEFINE_INT32(me2_occlusion_raster_winding, 1, "ME2",
                     "me2_occlusion_cpu_source 1: back faces: 0 = draw both sides, 1 = cull triangles clockwise on "
                     "the screen (y down), 2 = cull counter-clockwise")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_raster_dual, false, "ME2",
                    "Diagnostic (me2_occlusion_cpu_source 1): draw a second buffer with the other winding and tally "
                    "its answers against the GPU counts too (the '[occlusion] raster winding' line)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_raster_lod, true, "ME2",
                    "me2_occlusion_cpu_source 1: of a mesh's LODs only the one the game draws at this distance "
                    "(Min/MaxDrawDistanceSquared) is an occluder")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_native_vis_precise, true, "ME2",
                    "Native visibility: a box the rectangle test calls visible also gets the precise test of its "
                    "camera-facing faces (as Umbra's own per-instance tests)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_native_vis_worker, true, "ME2",
                    "Native visibility: the rasterizer's worker also tests Umbra's instance boxes against the buffer it "
                    "drew (the render thread re-tests only the boxes that changed since)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_raster_bias, 0.0, "ME2",
                      "me2_occlusion_cpu_source 1: a box is occluded only if the occluder's 1/w exceeds the box's "
                      "nearest 1/w by this fraction")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_dump_s, 0, "ME2",
                     "Diagnostic (me2_occlusion_cpu_source 1): every this many seconds write the depth buffer to "
                     "occlusion_raster.pgm (working directory) and log the occluder statistics; 0 = off")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(me2_occlusion_raster_trace, false, "ME2",
                    "Diagnostic (me2_occlusion_cpu_source 1): for every test the CPU calls occluded and the GPU count "
                    "calls visible, log the occluders under the box ('[raster-false]' lines, up to 40 per 10 s) and "
                    "sum them per occluder mesh in the report")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(me2_occlusion_raster_async, true, "ME2",
                    "me2_occlusion_cpu_source 1: draw the occluders on a worker thread as soon as the game thread has "
                    "built the frame's renderer (its camera), instead of on the UE3 render thread before Umbra's "
                    "traversal (off with me2_occlusion_raster_dual / _trace)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_raster_fast_tests, true, "ME2",
                    "me2_occlusion_cpu_source 1: faster box tests with the same answers (a tile wholly inside a box "
                    "triangle decides 'visible' without a pixel loop; the rectangle pre-test only checks tiles)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_raster_front_only, true, "ME2",
                    "me2_occlusion_raster_fast_tests: a closed box (8 vertices, 12 triangles) wholly in front of the "
                    "camera is tested with its front faces only (its back faces lie behind them)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_raster_test_verify, false, "ME2",
                    "Diagnostic: every box test also runs the previous (slow) test; disagreements are counted in the "
                    "[occlusion] raster line (fast occluded / slow visible must stay ~0)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_occlusion_raster_recent_s, 0.25, "ME2",
                      "Only occluders the game rendered within this many seconds of the newest render time among the "
                      "on-screen candidates (their primitive's LastRenderTime): a mesh the game keeps registered but "
                      "does not show (a closed door swapped out while it opens) stops occluding. 0 = every occluder")
    .range(0.0, 60.0)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_reuse_max, 8, "ME2",
                     "me2_occlusion_raster_reuse: at most this many jobs in a row take the previous buffer (the "
                     "occluders' render times are not part of the reuse check)")
    .range(1, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_occlusion_raster_reuse, true, "ME2",
                    "me2_occlusion_raster_async: a job with the same view-projection, camera and LOD factor as the last "
                    "one drawn, while no occluder was added, re-extracted or dropped since, takes that depth buffer "
                    "instead of drawing it again (a still camera: the same pixels)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_walk_full, 30, "ME2",
                     "me2_occlusion_cpu_source 1: while the depth draw lists read the same as at the last full walk, "
                     "the per-mesh checks run only every this many frames (0 = every frame)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-06): the worker's cores, not only its preferred one. Console 11-06 (combat, 23 fps): the worker
// (28 % of a core) on all four cores at UE3's priority took turns on core 0 with UE3's render thread and on core 2 with
// the game thread, which was ready but not running ~17 % of the time.
REXCVAR_DEFINE_INT32(me2_occlusion_raster_mask, 0, "ME2",
                     "me2_occlusion_raster_async on the Switch: the worker's core mask (e.g. 0xA = cores 1 and 3; its "
                     "preferred core me2_occlusion_raster_core when inside it). 0 = every core, as before")
    .range(0, 15)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-06, second core-plan run): the worker below UE3's game and render threads (0x3B) on every core.
// With it on cores 1 and 3 (me2_occlusion_raster_mask 0xA) behind Wwise (0x3A), Renut (0x2C) and XMA (0x2B) the render
// thread waited 6-16 ms a frame for its jobs in combat (0.25 ms before) and the game fell to 16-18 fps. Below them, it
// runs whenever either of them waits (each ~50 % of the time) and never holds their core when they wake.
REXCVAR_DEFINE_INT32(me2_occlusion_raster_prio, -1, "ME2",
                     "me2_occlusion_raster_async on the Switch: the worker's thread priority (0x3C = just below UE3's "
                     "threads at 0x3B; -1 = unchanged)")
    .range(-1, 0x3F)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_core, -1, "ME2",
                     "me2_occlusion_raster_async on the Switch: the worker's preferred core (-1 = the default)")
    .range(-1, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_occlusion_raster_wait_us, 30000, "ME2",
                     "me2_occlusion_raster_async: how long the render thread waits for an unfinished worker job "
                     "before drawing in place")
    .range(0, 200000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap

namespace {

uint32_t L32(const uint8_t* base, uint32_t va) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va)); }
uint16_t L16(const uint8_t* base, uint32_t va) { return __builtin_bswap16(*reinterpret_cast<const uint16_t*>(base + va)); }
uint32_t L32le(const uint8_t* base, uint32_t va) { return *reinterpret_cast<const uint32_t*>(base + va); }
uint16_t L16le(const uint8_t* base, uint32_t va) { return *reinterpret_cast<const uint16_t*>(base + va); }
float LF(const uint8_t* base, uint32_t va) {
  const uint32_t b = L32(base, va);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
float LFle(const uint8_t* base, uint32_t va) {
  const uint32_t b = L32le(base, va);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

// Committed guest bytes [va, va + len) (the heaps' own page table). Heap objects and physical (0xA0000000+) buffers.
bool Readable(uint32_t va, uint32_t len) {
  if (va < 0x00010000u || len == 0 || uint64_t(va) + len > 0xFFFF0000ull) return false;
  rex::memory::BaseHeap* heap = REX_KERNEL_MEMORY()->LookupHeap(va);
  return heap && (int(heap->QueryRangeAccess(va, va + len - 1)) & int(rex::memory::PageAccess::kReadOnly)) != 0;
}

// ---- the cached occluders ----------------------------------------------------------------------------------------
enum Bad : uint8_t {
  kOk = 0,
  kBadDynamic,   // dynamic vertex / index data (flags bit 31)
  kBadType,      // not a triangle list / strip
  kBadPointers,  // index buffer / vertex factory / D3D objects unreadable
  kBadStream,    // no position stream, stride out of range, odd endian
  kBadRange,     // indices or vertices outside their buffers
  kBadTooBig,    // more triangles than me2_occlusion_raster_mesh_tris
  kBadMemory,    // the geometry cache is full
  kBadValues,    // non-finite or absurd positions
  kBadCount
};
const char* kBadName[kBadCount] = {"ok", "dynamic data", "not a triangle list/strip", "unreadable pointers",
                                   "position stream", "index/vertex range", "too many triangles", "cache full",
                                   "bad positions"};

struct Occluder {
  uint32_t mesh = 0, ib = 0, vf = 0, first = 0, prims = 0;
  uint32_t l2w_bits[3] = {};  // LocalToWorld translation (bits), the change check
  uint64_t seen = 0;          // the last frame its mesh was in a list
  float lo[3] = {}, hi[3] = {};
  float origin[3] = {};  // its primitive's bounds origin (the LOD distance)
  float min_d2 = 0.0f, max_d2 = 0.0f;
  std::vector<float> v;         // world-space x, y, z
  std::vector<uint32_t> tri;    // 3 local vertex indices a triangle
  uint64_t first_seen = 0;  // the frame it was first extracted (me2_occlusion_raster_trace)
  uint32_t mesh_id = 0, flags = 0;
  uint32_t psi = 0;         // its FPrimitiveSceneInfo (+0xAC LastRenderTime: the game rendered it; trace)
  float lrt = 0.0f;         // that time when this module last drew it (trace)
  float d2 = 0.0f;          // its LOD distance squared in the last frame drawn (trace)
  uint8_t list = 0;
  uint8_t cull = 1;  // 0 both sides, 1 the normal side, 2 the reversed side
  uint8_t bad = kOk;
};
std::vector<Occluder> g_occ;
std::vector<uint32_t> g_free;
std::unordered_map<uint32_t, uint32_t> g_by_mesh;
uint64_t g_bytes = 0;  // geometry held
// Under g_cache_mutex: bumped whenever an occluder is added, re-extracted or dropped (me2_occlusion_raster_reuse: the
// same camera over the same occluders draws the same buffer).
uint64_t g_cache_gen = 1;

struct Stats {
  uint64_t frames = 0, walk_ns = 0, raster_ns = 0, elements = 0, new_meshes = 0, changed = 0, dropped = 0;
  uint64_t drawn_occ = 0, drawn_tris = 0, budget_cut = 0, small = 0, off = 0, lod_out = 0, culled_tris = 0, dense = 0;
  uint64_t covered_px = 0, bad[kBadCount] = {}, lists_bad = 0, near_clipped = 0;
  uint64_t tests = 0, test_ns = 0;
  uint64_t precise = 0, precise_occ = 0, precise_near = 0, precise_none = 0;  // the per-pixel box test
  uint64_t trace_ns = 0, vis_checked = 0, vis_set = 0, vis_bad = 0;  // trace: cost; rasterized occluders drawn by the game
  uint64_t hidden_occ = 0;  // occluders not drawn: already hidden behind those drawn before them
  uint64_t take_ns = 0, inline_ns = 0, inline_frames = 0;  // async: render thread's wait / its own drawing
  uint64_t walk_skips = 0;  // walks whose lists read as at the last full walk (no per-mesh checks)
  uint64_t not_recent = 0;  // candidates the game did not render recently (me2_occlusion_raster_recent_s)
  uint64_t off_fast = 0, lean_frames = 0;  // me2-nx-deko (2026-11-06): the centre / half-size rejections; lean frames
  uint64_t tile_accepts = 0, front_only = 0;                // fast tests: tile-level 'visible', boxes without back faces
  uint64_t verify = 0, verify_unsafe = 0, verify_more = 0;  // me2_occlusion_raster_test_verify
} g_st;
thread_local Stats* t_st = &g_st;  // where the drawing functions count (the worker: its job's)

struct Buffer {
  int w = 0, h = 0;
  std::vector<float> corner;  // (w + 1) x (h + 1) samples at pixel corners: nearest occluder 1/w (0 = none)
  std::vector<float> pix;     // w x h: the farthest of the pixel's four corners
  std::vector<float> tile;    // (w / 8) x (h / 8): the farthest pixel of the tile
  int tw = 0, th = 0;
  std::vector<uint32_t> corner_id, pix_id;  // me2_occlusion_raster_trace: the occluder slot of each sample / pixel
  float vp[16] = {}, cam[3] = {};  // the view-projection and camera it was drawn with
};
Buffer g_buf[2];          // [0] the winding in use, [1] the other one (me2_occlusion_raster_dual)
uint64_t g_ready_frame = ~uint64_t(0);
const Buffer* g_cur = nullptr;  // the buffer this frame's tests use (g_buf[0] or a worker's)
float g_cam[3] = {};  // the camera of the frame drawn (me2_occlusion_raster_trace)
const uint8_t* g_base = nullptr;  // guest memory (trace reads after the test)
uint32_t g_trace_next = 0;         // me2_occlusion_raster_trace: the next record of the ring
uint32_t g_trace_frame_first = 0;  // g_trace_next at the frame's start (Me2RasterAfterView marks from here)
std::vector<uint32_t> g_drawn_slots;  // the occluders rasterized this frame (trace: share the game drew)
uint32_t g_scene = 0, g_prepass_scene = 0;
uint64_t g_scene_mismatch = 0;

void InitBuffer(Buffer& b, int w, int h) {
  b.w = w;
  b.h = h;
  b.corner.assign(size_t(w + 1) * size_t(h + 1), 0.0f);
  b.pix.assign(size_t(w) * size_t(h), 0.0f);
  b.tw = (w + 7) / 8;
  b.th = (h + 7) / 8;
  b.tile.assign(size_t(b.tw) * size_t(b.th), 0.0f);
  if (REXCVAR_GET(me2_occlusion_raster_trace)) {
    b.corner_id.assign(b.corner.size(), ~0u);
    b.pix_id.assign(b.pix.size(), ~0u);
  }
}

void Release(Occluder& o) {
  g_bytes -= o.v.capacity() * sizeof(float) + o.tri.capacity() * sizeof(uint32_t);
  std::vector<float>().swap(o.v);
  std::vector<uint32_t>().swap(o.tri);
}

// The mesh's geometry in world space. Returns the Bad reason (kOk: o.v / o.tri filled).
uint8_t Extract(const uint8_t* base, Occluder& o, uint32_t mesh) {
  static const uint32_t max_tris = uint32_t(REXCVAR_GET(me2_occlusion_raster_mesh_tris));
  static const uint64_t max_bytes = uint64_t(REXCVAR_GET(me2_occlusion_raster_mem_mb)) << 20;
  const uint32_t flags = L32(base, mesh + 204);
  if (flags & 0x80000000u) return kBadDynamic;
  const uint32_t type = (flags >> 25) & 7;
  if (type > 1) return kBadType;
  const uint32_t ibo = L32(base, mesh), vf = L32(base, mesh + 4);
  const uint32_t first = L32(base, mesh + 176), prims = L32(base, mesh + 180);
  const uint32_t minv = L32(base, mesh + 184), maxv = L32(base, mesh + 188);
  if (!prims) return kBadType;
  if (prims > max_tris) return kBadTooBig;
  if (!Readable(ibo, 24) || !Readable(vf, 412)) return kBadPointers;
  const uint32_t ib_rhi = L32(base, ibo + 20);
  if (!Readable(ib_rhi, 12)) return kBadPointers;
  const uint32_t d3dib = L32(base, ib_rhi + 8);
  if (!Readable(d3dib, 32)) return kBadPointers;
  const uint32_t common = L32(base, d3dib), iaddr = L32(base, d3dib + 24), isize = L32(base, d3dib + 28);
  const bool i32 = (common & 0x80000000u) != 0;
  const uint32_t iend = (common >> 29) & 3;
  const uint32_t npos = L32(base, vf + 408);
  if (npos == 0 || npos > 16) return kBadStream;
  const uint32_t vbo = L32(base, vf + 216), stride = L32(base, vf + 220);
  if (stride < 12 || stride > 512 || (stride & 3)) return kBadStream;
  if (!Readable(vbo, 24)) return kBadPointers;
  const uint32_t vb_rhi = L32(base, vbo + 20);
  if (!Readable(vb_rhi, 12)) return kBadPointers;
  const uint32_t d3dvb = L32(base, vb_rhi + 8);
  if (!Readable(d3dvb, 32)) return kBadPointers;
  const uint32_t vaddr = L32(base, d3dvb + 24) & ~3u, f1 = L32(base, d3dvb + 28);
  const uint32_t vsize = f1 & 0x03FFFFFCu, vend = f1 & 3;
  if (vend != 0 && vend != 2) return kBadStream;
  if (i32 ? (iend != 0 && iend != 2) : (iend != 0 && iend != 1)) return kBadStream;
  const uint32_t isz = i32 ? 4 : 2;
  const uint64_t nidx = type == 0 ? uint64_t(prims) * 3 : uint64_t(prims) + 2;
  if ((uint64_t(first) + nidx) * isz > isize || maxv < minv || maxv - minv > 0x100000u ||
      (uint64_t(maxv) * stride + 12) > vsize)
    return kBadRange;
  if (!Readable(iaddr + first * isz, uint32_t(nidx * isz)) ||
      !Readable(vaddr + minv * stride, (maxv - minv) * stride + 12))
    return kBadPointers;
  const uint64_t need = (uint64_t(maxv - minv + 1) * 3 + uint64_t(prims) * 3) * 4;
  if (g_bytes + need > max_bytes) return kBadMemory;

  float m[16];
  for (int i = 0; i < 16; ++i) m[i] = LF(base, mesh + 32 + 4 * i);
  // Indices -> local vertices (only those referenced).
  static thread_local std::vector<int32_t> remap;
  remap.assign(size_t(maxv - minv + 1), -1);
  std::vector<float> verts;
  std::vector<uint32_t> tris;
  tris.reserve(size_t(prims) * 3);
  auto index_at = [&](uint64_t k) -> uint32_t {
    const uint32_t a = iaddr + uint32_t((first + k) * isz);
    if (i32) return iend ? L32(base, a) : L32le(base, a);
    return iend ? L16(base, a) : L16le(base, a);
  };
  auto local = [&](uint32_t idx, uint32_t& out) -> bool {
    if (idx < minv || idx > maxv) return false;
    int32_t& r = remap[idx - minv];
    if (r < 0) {
      const uint32_t a = vaddr + idx * stride;
      const float x = vend ? LF(base, a) : LFle(base, a);
      const float y = vend ? LF(base, a + 4) : LFle(base, a + 4);
      const float z = vend ? LF(base, a + 8) : LFle(base, a + 8);
      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
      r = int32_t(verts.size() / 3);
      verts.push_back(x * m[0] + y * m[4] + z * m[8] + m[12]);
      verts.push_back(x * m[1] + y * m[5] + z * m[9] + m[13]);
      verts.push_back(x * m[2] + y * m[6] + z * m[10] + m[14]);
    }
    out = uint32_t(r);
    return true;
  };
  const uint32_t restart = i32 ? 0xFFFFFFFFu : 0xFFFFu;
  if (type == 0) {
    for (uint32_t t = 0; t < prims; ++t) {
      uint32_t a, b, c;
      if (!local(index_at(uint64_t(t) * 3), a) || !local(index_at(uint64_t(t) * 3 + 1), b) ||
          !local(index_at(uint64_t(t) * 3 + 2), c))
        return kBadRange;
      if (a == b || b == c || a == c) continue;
      tris.push_back(a);
      tris.push_back(b);
      tris.push_back(c);
    }
  } else {
    uint32_t i0 = 0, i1 = 0;
    int have = 0, parity = 0;
    for (uint64_t k = 0; k < nidx; ++k) {
      const uint32_t raw = index_at(k);
      if (raw == restart) {
        have = 0;
        parity = 0;
        continue;
      }
      uint32_t li;
      if (!local(raw, li)) return kBadRange;
      if (have < 2) {
        (have == 0 ? i0 : i1) = li;
        ++have;
        continue;
      }
      if (i0 != i1 && i1 != li && i0 != li) {
        tris.push_back(parity ? i1 : i0);
        tris.push_back(parity ? i0 : i1);
        tris.push_back(li);
      }
      parity ^= 1;
      i0 = i1;
      i1 = li;
    }
  }
  if (tris.empty()) return kBadType;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t i = 0; i < verts.size(); i += 3)
    for (int c = 0; c < 3; ++c) {
      lo[c] = std::min(lo[c], verts[i + c]);
      hi[c] = std::max(hi[c], verts[i + c]);
    }
  for (int c = 0; c < 3; ++c)
    if (!(hi[c] - lo[c] < 2.0e6f) || std::fabs(lo[c]) > 1.0e7f) return kBadValues;
  verts.shrink_to_fit();
  tris.shrink_to_fit();
  o.v = std::move(verts);
  o.tri = std::move(tris);
  g_bytes += o.v.capacity() * sizeof(float) + o.tri.capacity() * sizeof(uint32_t);
  std::memcpy(o.lo, lo, sizeof(lo));
  std::memcpy(o.hi, hi, sizeof(hi));
  o.cull = (flags & 0x40000000u) ? 2 : 1;  // ReverseCulling (SetMeshRenderState XORs it in)
  o.flags = flags;
  o.mesh_id = L32(base, mesh + 224);
  o.min_d2 = LF(base, mesh + 208);
  o.max_d2 = LF(base, mesh + 212);
  const uint32_t psi = L32(base, mesh + 0xD8);
  o.psi = Readable(psi, 0xB0) ? psi : 0;
  if (Readable(psi, 0x4C)) {
    for (int c = 0; c < 3; ++c) o.origin[c] = LF(base, psi + 0x30 + 4 * c);
  } else {
    for (int c = 0; c < 3; ++c) o.origin[c] = 0.5f * (lo[c] + hi[c]);
  }
  return kOk;
}

void Visit(const uint8_t* base, uint32_t mesh, uint32_t policy_flags, uint8_t list, uint64_t frame) {
  const uint32_t ib = L32(base, mesh), vf = L32(base, mesh + 4);
  const uint32_t first = L32(base, mesh + 176), prims = L32(base, mesh + 180);
  uint32_t t[3];
  for (int c = 0; c < 3; ++c) t[c] = L32(base, mesh + 80 + 4 * c);
  auto it = g_by_mesh.find(mesh);
  uint32_t slot;
  if (it != g_by_mesh.end()) {
    slot = it->second;
    Occluder& o = g_occ[slot];
    if (o.ib == ib && o.vf == vf && o.first == first && o.prims == prims && std::memcmp(o.l2w_bits, t, 12) == 0) {
      o.seen = frame;
      return;
    }
    ++g_st.changed;
    ++g_cache_gen;
    Release(o);
  } else {
    if (!Readable(mesh, 0xE4)) return;
    if (!g_free.empty()) {
      slot = g_free.back();
      g_free.pop_back();
    } else {
      slot = uint32_t(g_occ.size());
      g_occ.emplace_back();
    }
    g_by_mesh[mesh] = slot;
    ++g_st.new_meshes;
    ++g_cache_gen;
  }
  Occluder& o = g_occ[slot];
  o = Occluder{};
  o.mesh = mesh;
  o.ib = ib;
  o.vf = vf;
  o.first = first;
  o.prims = prims;
  std::memcpy(o.l2w_bits, t, 12);
  o.seen = frame;
  o.first_seen = frame;
  o.list = list;
  o.bad = Extract(base, o, mesh);
  if (o.bad == kOk && (policy_flags & 0x80000000u)) o.cull = 0;  // two-sided material
  ++g_st.bad[o.bad];
}

// 2026-11-05 (console: the walk took 3.2-4.2 ms a frame on the UE3 render thread for ~5,300 elements that did not
// change for minutes, PC 40-80 us: each Visit is a hash lookup + 3 cache lines of its FStaticMesh). The lists are
// read first (links, allocation bits, element mesh pointers: mostly sequential) into a signature; while it is the
// last full walk's and that walk is younger than me2_occlusion_raster_walk_full frames, the Visits and the drop pass
// are skipped (a mesh changed in place, which the full walk's checks catch, shows at the next full walk).
struct WalkEntry {
  uint32_t mesh, policy_flags;
  uint8_t list;
};
std::vector<WalkEntry> g_walk_entries;
uint64_t g_walk_sig = 0, g_walk_full_frame = 0;

void Walk(const uint8_t* base, uint32_t scene, uint64_t frame) {
  static const int mask = REXCVAR_GET(me2_occlusion_raster_lists);
  static const uint64_t full_every = uint64_t(REXCVAR_GET(me2_occlusion_raster_walk_full));
  static const uint32_t kList[3] = {48, 76, 104};
  uint64_t sig = 0xCBF29CE484222325ull;
  auto mix = [&sig](uint32_t v) { sig = (sig ^ v) * 0x100000001B3ull; };
  g_walk_entries.clear();
  for (uint8_t li = 0; li < 3; ++li) {
    if (!(mask & (1 << li))) continue;
    const uint32_t list = scene + 1u * 308u + kList[li];  // DPG 1 = SDPG_World
    if (!Readable(list, 8)) {
      ++g_st.lists_bad;
      continue;
    }
    const uint32_t data = L32(base, list), num = L32(base, list + 4);
    mix(data), mix(num);
    if (!num) continue;
    if (num > 200000 || !Readable(data, 4 * num)) {
      ++g_st.lists_bad;
      continue;
    }
    for (uint32_t j = 0; j < num; ++j) {
      const uint32_t link = L32(base, data + 4 * j);
      if (!Readable(link, 60)) continue;
      const uint32_t policy_flags = L32(base, link + 20);
      const uint32_t elems = L32(base, link + 28), bits = L32(base, link + 52), nbits = L32(base, link + 56);
      mix(link), mix(policy_flags), mix(elems), mix(bits), mix(nbits);
      if (!nbits || nbits > (1u << 20)) continue;
      const uint32_t words = (nbits + 31) / 32;
      if (!Readable(bits, 4 * words) || !Readable(elems, 16 * nbits)) continue;
      for (uint32_t w = 0; w < words; ++w) {
        uint32_t word = L32(base, bits + 4 * w);
        if (w == words - 1 && (nbits & 31)) word &= (1u << (nbits & 31)) - 1;
        mix(word);
        while (word) {
          const uint32_t b = uint32_t(__builtin_ctz(word));
          word &= word - 1;
          const uint32_t mesh = L32(base, elems + 16 * (32 * w + b) + 4);
          mix(mesh);
          if (mesh) g_walk_entries.push_back({mesh, policy_flags, li});
        }
      }
    }
  }
  g_st.elements += g_walk_entries.size();
  if (full_every && sig == g_walk_sig && frame - g_walk_full_frame < full_every) {
    ++g_st.walk_skips;
    return;
  }
  g_walk_sig = sig;
  g_walk_full_frame = frame;
  for (const WalkEntry& e : g_walk_entries) Visit(base, e.mesh, e.policy_flags, e.list, frame);
  // Meshes no longer in any list: their geometry goes (their FStaticMesh may be gone; never read again).
  for (uint32_t s = 0; s < g_occ.size(); ++s) {
    Occluder& o = g_occ[s];
    if (o.mesh && o.seen != frame) {
      g_by_mesh.erase(o.mesh);
      Release(o);
      o = Occluder{};
      g_free.push_back(s);
      ++g_st.dropped;
      ++g_cache_gen;
    }
  }
}

// ---- rasterizer -------------------------------------------------------------------------------------------------
constexpr float kNear = 1.0f;  // clip w below which a point counts as at the near plane (ME2's projection: near 10)

struct V4 {
  float x, y, z, w;
};
typedef float V4f __attribute__((vector_size(16)));     // 4 floats (SSE / NEON)
typedef int32_t V4i __attribute__((vector_size(16)));   // 4 lane masks (comparisons of V4f)
V4 Xform(const float* m, float x, float y, float z) {
  return {x * m[0] + y * m[4] + z * m[8] + m[12], x * m[1] + y * m[5] + z * m[9] + m[13],
          x * m[2] + y * m[6] + z * m[10] + m[14], x * m[3] + y * m[7] + z * m[11] + m[15]};
}

// One screen triangle (sx, sy in pixels, y down; iz = 1/w) into the corner samples; cull: 0 none, 1 cull
// clockwise (area > 0 with y down), 2 cull counter-clockwise.
void DrawTri(Buffer& b, const float* p0, const float* p1, const float* p2, int cull, uint32_t id) {
  const float area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
  if (std::fabs(area) < 1e-6f) return;
  if ((cull == 1 && area > 0.0f) || (cull == 2 && area < 0.0f)) {
    ++t_st->culled_tris;
    return;
  }
  const int x0 = std::max(0, int(std::ceil(std::min({p0[0], p1[0], p2[0]}))));
  const int x1 = std::min(b.w, int(std::floor(std::max({p0[0], p1[0], p2[0]}))));
  const int y0 = std::max(0, int(std::ceil(std::min({p0[1], p1[1], p2[1]}))));
  const int y1 = std::min(b.h, int(std::floor(std::max({p0[1], p1[1], p2[1]}))));
  if (x1 < x0 || y1 < y0) return;
  // E(u, v, p) = (v.x - u.x)(p.y - u.y) - (v.y - u.y)(p.x - u.x) = A p.x + B p.y + C; E(p0, p1, p2) = area. Inside: the
  // three edge functions have area's sign; the weight of a vertex is the function of the opposite edge / area.
  auto edge = [](const float* u, const float* v, float& A, float& B, float& C) {
    A = u[1] - v[1];
    B = v[0] - u[0];
    C = u[0] * v[1] - u[1] * v[0];
  };
  float A0, B0, C0, A1, B1, C1, A2, B2, C2;
  edge(p1, p2, A0, B0, C0);  // weight of p0
  edge(p2, p0, A1, B1, C1);  // weight of p1
  edge(p0, p1, A2, B2, C2);  // weight of p2
  const float s = area > 0.0f ? 1.0f : -1.0f;
  A0 *= s, B0 *= s, C0 *= s, A1 *= s, B1 *= s, C1 *= s, A2 *= s, B2 *= s, C2 *= s;
  const float inv = 1.0f / std::fabs(area);
  const float z0 = p0[2] * inv, z1 = p1[2] * inv, z2 = p2[2] * inv;
  // 1/w as a plane over the screen: zA x + zB y + zC.
  const float zA = A0 * z0 + A1 * z1 + A2 * z2, zB = B0 * z0 + B1 * z1 + B2 * z2, zC = C0 * z0 + C1 * z1 + C2 * z2;
  const float A[3] = {A0, A1, A2}, B[3] = {B0, B1, B2}, C[3] = {C0, C1, C2};
  const int stride = b.w + 1;
  // me2-nx-deko (2026-11-06): the per-row span from per-edge lines x = slope * y + icpt, classified once per triangle
  // (console 11-06: the per-row setup with its three sign branches cost twice the fill). An edge with A > 0 bounds the
  // span from the left (A x + r >= 0 is x >= -r / A), A < 0 from the right; A == 0 bounds the rows instead (B y + C >= 0).
  // Unused bound slots hold the bounding box's side, so the row loop has no branches. The bounds are the same lines as
  // before (-(B y + C) / A, now as y * (-B / A) - C / A: ulp-level differences on a sample lying exactly on an edge).
  float ls[2] = {0.0f, 0.0f}, li[2] = {float(x0), float(x0)};  // left bounds
  float rs[2] = {0.0f, 0.0f}, ri[2] = {float(x1), float(x1)};  // right bounds
  int nl = 0, nr = 0;
  int ry0 = y0, ry1 = y1;
  for (int k = 0; k < 3; ++k) {
    if (A[k] > 0.0f || A[k] < 0.0f) {
      const float inv = 1.0f / A[k];
      const float slope = -B[k] * inv, icpt = -C[k] * inv;
      if (A[k] > 0.0f) {
        if (nl < 2) ls[nl] = slope, li[nl] = icpt, ++nl;
      } else {
        if (nr < 2) rs[nr] = slope, ri[nr] = icpt, ++nr;
      }
    } else {
      // B y + C >= 0 over the rows (B == 0: C decides all of them)
      // (clamped as floats first: -C / B can be far outside the int range)
      if (B[k] > 0.0f) {
        const float t = std::ceil(-C[k] / B[k]);
        if (!(t <= float(y1))) return;
        if (t > float(ry0)) ry0 = int(t);
      } else if (B[k] < 0.0f) {
        const float t = std::floor(-C[k] / B[k]);
        if (!(t >= float(y0))) return;
        if (t < float(ry1)) ry1 = int(t);
      } else if (C[k] < 0.0f) {
        return;
      }
    }
  }
  const float bx0 = float(x0), bx1 = float(x1);
  // 2026-11-05 (step 2): per row the span where all three edge functions are >= 0, computed from the edges (A x + r >= 0
  // is x >= -r / A for A > 0, x <= -r / A for A < 0), then a branch-free max over the span (vectorized by the compiler)
  // instead of three edge tests per pixel of the bounding box.
  for (int y = ry0; y <= ry1; ++y) {
    const float fy = float(y);
    const float lo = std::max(std::max(bx0, ls[0] * fy + li[0]), ls[1] * fy + li[1]);
    const float hi = std::min(std::min(bx1, rs[0] * fy + ri[0]), rs[1] * fy + ri[1]);
    if (!(lo <= hi)) continue;
    const int xs = int(std::ceil(lo)), xe = int(std::floor(hi));
    if (xs > xe) continue;
    const float zr = zB * fy + zC;
    float* row = &b.corner[size_t(y) * stride];
    if (b.corner_id.empty()) {
      // 4 samples at a time (GCC/Clang vector types: SSE on the PC, NEON on the Switch), then the rest one by one.
      int x = xs;
      if (xe - xs >= 3) {
        const V4f step = {0.0f, 1.0f, 2.0f, 3.0f};
        const V4f za4 = {zA, zA, zA, zA};
        for (; x + 3 <= xe; x += 4) {
          V4f r;
          std::memcpy(&r, row + x, sizeof(r));
          const float z0x = zr + zA * float(x);
          const V4f v = V4f{z0x, z0x, z0x, z0x} + za4 * step;
          r = r > v ? r : v;
          std::memcpy(row + x, &r, sizeof(r));
        }
      }
      for (; x <= xe; ++x) {
        const float v = zr + zA * float(x);
        row[x] = row[x] > v ? row[x] : v;
      }
    } else {
      uint32_t* ids = &b.corner_id[size_t(y) * stride];
      for (int x = xs; x <= xe; ++x) {
        const float v = zr + zA * float(x);
        if (v > row[x]) {
          row[x] = v;
          ids[x] = id;
        }
      }
    }
  }
}

// A clip-space polygon (after the near-plane clip) drawn as a fan.
void DrawClipTri(Buffer& b, const V4& v0, const V4& v1, const V4& v2, int cull, uint32_t id) {
  const V4 in[3] = {v0, v1, v2};
  V4 poly[4];
  int n = 0;
  const bool all_in = v0.w > kNear && v1.w > kNear && v2.w > kNear;
  if (all_in) {
    poly[0] = v0;
    poly[1] = v1;
    poly[2] = v2;
    n = 3;
  } else {
    ++t_st->near_clipped;
    for (int i = 0; i < 3; ++i) {
      const V4& p = in[i];
      const V4& q = in[(i + 1) % 3];
      const bool pin = p.w > kNear, qin = q.w > kNear;
      if (pin) poly[n++] = p;
      if (pin != qin) {
        const float t = (kNear - p.w) / (q.w - p.w);
        poly[n++] = {p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, kNear};
      }
    }
    if (n < 3) return;
  }
  float s[4][3];
  for (int i = 0; i < n; ++i) {
    const float iw = 1.0f / poly[i].w;
    s[i][0] = (poly[i].x * iw * 0.5f + 0.5f) * float(b.w);
    s[i][1] = (0.5f - poly[i].y * iw * 0.5f) * float(b.h);
    s[i][2] = iw;
  }
  DrawTri(b, s[0], s[1], s[2], cull, id);
  if (n == 4) DrawTri(b, s[0], s[2], s[3], cull, id);
}

void Resolve(Buffer& b, bool primary) {
  const int stride = b.w + 1;
  uint64_t covered = 0;
  for (int y = 0; y < b.h; ++y)
    for (int x = 0; x < b.w; ++x) {
      const float* r0 = &b.corner[size_t(y) * stride + x];
      const float* r1 = r0 + stride;
      const float v = std::min(std::min(r0[0], r0[1]), std::min(r1[0], r1[1]));
      b.pix[size_t(y) * b.w + x] = v;
      if (!b.pix_id.empty()) {  // the occluder of the farthest corner (the one that decides the pixel)
        const size_t c0 = size_t(y) * stride + x;
        const size_t c = v == r0[0] ? c0 : v == r0[1] ? c0 + 1 : v == r1[0] ? c0 + stride : c0 + stride + 1;
        b.pix_id[size_t(y) * b.w + x] = b.corner_id[c];
      }
      covered += v > 0.0f;
    }
  for (int ty = 0; ty < b.th; ++ty)
    for (int tx = 0; tx < b.tw; ++tx) {
      float m = 1e30f;
      for (int y = ty * 8; y < std::min(b.h, ty * 8 + 8); ++y)
        for (int x = tx * 8; x < std::min(b.w, tx * 8 + 8); ++x) m = std::min(m, b.pix[size_t(y) * b.w + x]);
      b.tile[size_t(ty) * b.tw + tx] = m;
    }
  if (primary) t_st->covered_px += covered;
}

struct Candidate {
  float area;
  uint32_t slot;
  float x0, x1, y0, y1, max_iz;  // the box's screen rectangle and nearest 1/w (max_iz 0: reaches the near plane)
};
struct ScreenVert {
  float x, y, iz;  // contiguous: DrawTri takes &x as float[3]
  uint32_t out;
};
static_assert(sizeof(ScreenVert) == 16 && sizeof(V4) == 16, "the 4-wide vertex transform stores them as float4s");

// 2026-11-05 (step 2): whether everything already drawn covers the box's whole screen rectangle nearer than the box's
// nearest point (then none of its triangles can change the buffer). Corner samples, early exit at the first one that
// is not nearer; large rectangles are not checked (drawn first anyway, and costly to check).
bool HiddenAlready(const Buffer& b, const Candidate& c) {
  if (!(c.max_iz > 0.0f)) return false;
  const int sx0 = std::max(0, int(std::floor(c.x0))), sx1 = std::min(b.w, int(std::ceil(c.x1)));
  const int sy0 = std::max(0, int(std::floor(c.y0))), sy1 = std::min(b.h, int(std::ceil(c.y1)));
  if (sx1 < sx0 || sy1 < sy0) return false;
  if ((sx1 - sx0 + 1) * (sy1 - sy0 + 1) > 4096) return false;
  const int stride = b.w + 1;
  for (int y = sy0; y <= sy1; ++y) {
    const float* row = &b.corner[size_t(y) * stride];
    for (int x = sx0; x <= sx1; ++x)
      if (!(row[x] > c.max_iz)) return false;
  }
  return true;
}

// Draws the cached occluders with view-projection vp into bufs[0] (and bufs[1] with the other winding when nb == 2).
// Reads g_occ: the caller holds g_cache_mutex. Statistics go to *t_st.
void Rasterize(const float* vp, const float cam[3], float lod_factor, Buffer* bufs, int nb) {
  static const uint64_t budget = uint64_t(REXCVAR_GET(me2_occlusion_raster_tris));
  static const float min_px_std = float(REXCVAR_GET(me2_occlusion_raster_min_px));
  static const float min_ppt_std = float(REXCVAR_GET(me2_occlusion_raster_min_px_per_tri));
  static const float min_px_lean = float(REXCVAR_GET(me2_occlusion_raster_lean_px));
  static const float min_ppt_lean = float(REXCVAR_GET(me2_occlusion_raster_lean_px_per_tri));
  static const bool lean_on = REXCVAR_GET(me2_occlusion_raster_lean);
  // me2-nx-deko (2026-11-06): the lean limits always (me2_occlusion_raster_lean) or in the rotation's "on" slots.
  const bool lean = (lean_on || Me2AbInRotation(Me2Ab::kRasterLean)) && !Me2AbOff(Me2Ab::kRasterLean);
  const float min_px = lean ? min_px_lean : min_px_std, min_ppt = lean ? min_ppt_lean : min_ppt_std;
  if (lean) ++t_st->lean_frames;
  static const int winding = REXCVAR_GET(me2_occlusion_raster_winding);
  static const bool lod = REXCVAR_GET(me2_occlusion_raster_lod);
  Stats& st = *t_st;
  for (int k = 0; k < nb; ++k) {
    std::fill(bufs[k].corner.begin(), bufs[k].corner.end(), 0.0f);
    std::fill(bufs[k].corner_id.begin(), bufs[k].corner_id.end(), ~0u);
    std::memcpy(bufs[k].vp, vp, sizeof(bufs[k].vp));
    std::memcpy(bufs[k].cam, cam, sizeof(bufs[k].cam));
  }
  const bool trace = !bufs[0].pix_id.empty();
  static std::vector<Candidate> cands;
  cands.clear();
  if (trace) g_drawn_slots.clear();
  const float W = float(bufs[0].w), H = float(bufs[0].h);
  const float f2 = lod_factor * lod_factor;
  for (uint32_t s = 0; s < g_occ.size(); ++s) {
    Occluder& o = g_occ[s];
    if (!o.mesh || o.bad != kOk) continue;
    if (lod) {
      const float dx = o.origin[0] - cam[0], dy = o.origin[1] - cam[1], dz = o.origin[2] - cam[2];
      const float d2 = (dx * dx + dy * dy + dz * dz) * f2;
      if (trace) o.d2 = d2;
      if (d2 < o.min_d2 || !(d2 < o.max_d2)) {
        ++st.lod_out;
        continue;
      }
    }
    // me2-nx-deko (2026-11-06): off-screen rejection from the box's centre and half-size first (console 11-06: 4,187 of
    // 5,292 occluders off screen, each costing 8 corner transforms and divisions). For a linear function of the
    // position, the largest value over the box is f(centre) + sum |coefficient| * half-size, reached at a corner: "all
    // 8 corners outside one plane" is exactly "that maximum below 0". A small margin leaves close calls to the corners.
    {
      const float cx = 0.5f * (o.lo[0] + o.hi[0]), cy = 0.5f * (o.lo[1] + o.hi[1]), cz = 0.5f * (o.lo[2] + o.hi[2]);
      const float ex = 0.5f * (o.hi[0] - o.lo[0]), ey = 0.5f * (o.hi[1] - o.lo[1]), ez = 0.5f * (o.hi[2] - o.lo[2]);
      // the max over the box of a x + b y + c z + d, minus a margin of its spread
      const auto top = [&](float a, float b, float c, float d) {
        const float mid = a * cx + b * cy + c * cz + d;
        const float spread = std::fabs(a) * ex + std::fabs(b) * ey + std::fabs(c) * ez;
        return mid + spread + 1e-4f * (std::fabs(mid) + spread + 1.0f);
      };
      const float* m = vp;
      // x + w < 0 (left), w - x < 0 (right), y + w < 0 (below), w - y < 0 (above), w - kNear <= 0 (near)
      if (top(m[0] + m[3], m[4] + m[7], m[8] + m[11], m[12] + m[15]) < 0.0f ||
          top(m[3] - m[0], m[7] - m[4], m[11] - m[8], m[15] - m[12]) < 0.0f ||
          top(m[1] + m[3], m[5] + m[7], m[9] + m[11], m[13] + m[15]) < 0.0f ||
          top(m[3] - m[1], m[7] - m[5], m[11] - m[9], m[15] - m[13]) < 0.0f ||
          top(m[3], m[7], m[11], m[15] - kNear) < 0.0f) {
        ++st.off;
        ++st.off_fast;
        continue;
      }
    }
    // The box's corners: off-screen rejection (all outside one plane) and its screen rectangle.
    int out_l = 0, out_r = 0, out_b = 0, out_t = 0, out_n = 0;
    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f, max_iz = 0.0f;
    for (int c = 0; c < 8; ++c) {
      const V4 p = Xform(vp, (c & 1) ? o.hi[0] : o.lo[0], (c & 2) ? o.hi[1] : o.lo[1], (c & 4) ? o.hi[2] : o.lo[2]);
      out_l += p.x < -p.w;
      out_r += p.x > p.w;
      out_b += p.y < -p.w;
      out_t += p.y > p.w;
      if (p.w <= kNear) {
        ++out_n;
        continue;
      }
      const float iw = 1.0f / p.w;
      const float sx = (p.x * iw * 0.5f + 0.5f) * W, sy = (0.5f - p.y * iw * 0.5f) * H;
      mnx = std::min(mnx, sx);
      mxx = std::max(mxx, sx);
      mny = std::min(mny, sy);
      mxy = std::max(mxy, sy);
      max_iz = std::max(max_iz, iw);
    }
    if (out_l == 8 || out_r == 8 || out_b == 8 || out_t == 8 || out_n == 8) {
      ++st.off;
      continue;
    }
    float area;
    if (out_n) {
      area = W * H;  // reaches the near plane: as large as the screen
      max_iz = 0.0f;
    } else {
      area = (std::min(mxx, W) - std::max(mnx, 0.0f)) * (std::min(mxy, H) - std::max(mny, 0.0f));
      if (!(area >= min_px)) {
        ++st.small;
        continue;
      }
      if (min_ppt > 0.0f && area < min_ppt * float(o.tri.size() / 3)) {
        ++st.dense;
        continue;
      }
    }
    cands.push_back({area, s, mnx, mxx, mny, mxy, max_iz});
  }
  // 2026-11-05 (console, Normandy: an opening door showed space behind it until the camera reached the doorway): the
  // static lists keep meshes the game does not show (a door's closed mesh while it opens); they occluded what Umbra
  // then culled. Only candidates rendered recently by the game draw: LastRenderTime (primitive scene info +0xAC, the
  // world time it was last drawn) within recent_s of the newest among the candidates. A mesh just come on screen joins
  // one frame late (more draws, never fewer).
  static const float recent_s = float(REXCVAR_GET(me2_occlusion_raster_recent_s));
  if (recent_s > 0.0f && g_base && !cands.empty()) {
    static std::vector<float> lrt;
    lrt.assign(cands.size(), -1.0f);
    float newest = -1.0f;
    for (size_t i = 0; i < cands.size(); ++i) {
      const uint32_t psi = g_occ[cands[i].slot].psi;
      if (!psi || !Readable(psi, 0xB0)) continue;
      const float t = LF(g_base, psi + 0xAC);
      if (std::isfinite(t)) {
        lrt[i] = t;
        newest = std::max(newest, t);
      }
    }
    if (newest > 0.0f) {
      size_t keep = 0;
      for (size_t i = 0; i < cands.size(); ++i) {
        if (lrt[i] >= 0.0f && newest - lrt[i] > recent_s) {
          ++st.not_recent;
          continue;
        }
        cands[keep++] = cands[i];
      }
      cands.resize(keep);
    }
  }
  std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.area > b.area; });
  uint64_t tris = 0;
  static std::vector<V4> clip;
  static std::vector<ScreenVert> sv;
  for (const Candidate& c : cands) {
    const Occluder& o = g_occ[c.slot];
    const uint64_t n = o.tri.size() / 3;
    if (tris + n > budget) {
      ++st.budget_cut;
      continue;
    }
    if (nb == 1 && HiddenAlready(bufs[0], c)) {
      ++st.hidden_occ;
      continue;
    }
    tris += n;
    ++st.drawn_occ;
    if (trace) {
      g_drawn_slots.push_back(c.slot);
      Occluder& od = g_occ[c.slot];
      if (od.psi) od.lrt = LF(g_base, od.psi + 0xAC);
    }
    // Each vertex once: clip position, outcode (1 left, 2 right, 4 below, 8 above, 16 at/behind the near plane) and,
    // in front of the near plane, its screen position and 1/w (the triangles share them).
    const size_t nv = o.v.size() / 3;
    clip.resize(nv);
    sv.resize(nv);
    size_t i0 = 0;
#if defined(__aarch64__)
    // me2-nx-deko (2026-11-06): four vertices at a time, the same operations in the same order as the scalar loop below
    // (separate multiplies and adds, -ffp-contract=off; IEEE division): bit-identical results. Vertices behind the near
    // plane get screen values too, which nothing reads (as their unset fields before).
    {
      const float32x4_t m0 = vdupq_n_f32(vp[0]), m1 = vdupq_n_f32(vp[1]), m2 = vdupq_n_f32(vp[2]), m3 = vdupq_n_f32(vp[3]);
      const float32x4_t m4 = vdupq_n_f32(vp[4]), m5 = vdupq_n_f32(vp[5]), m6 = vdupq_n_f32(vp[6]), m7 = vdupq_n_f32(vp[7]);
      const float32x4_t m8 = vdupq_n_f32(vp[8]), m9 = vdupq_n_f32(vp[9]), m10 = vdupq_n_f32(vp[10]), m11 = vdupq_n_f32(vp[11]);
      const float32x4_t m12 = vdupq_n_f32(vp[12]), m13 = vdupq_n_f32(vp[13]), m14 = vdupq_n_f32(vp[14]), m15 = vdupq_n_f32(vp[15]);
      const float32x4_t half = vdupq_n_f32(0.5f), one = vdupq_n_f32(1.0f), vw = vdupq_n_f32(W), vh = vdupq_n_f32(H);
      const float32x4_t nearv = vdupq_n_f32(kNear);
      const float* src = o.v.data();
      for (; i0 + 4 <= nv; i0 += 4) {
        const float32x4x3_t p = vld3q_f32(src + 3 * i0);
        const auto row = [&](float32x4_t a, float32x4_t b, float32x4_t c, float32x4_t d) {
          return vaddq_f32(vaddq_f32(vaddq_f32(vmulq_f32(p.val[0], a), vmulq_f32(p.val[1], b)), vmulq_f32(p.val[2], c)), d);
        };
        const float32x4_t cx = row(m0, m4, m8, m12), cy = row(m1, m5, m9, m13);
        const float32x4_t cz = row(m2, m6, m10, m14), cw = row(m3, m7, m11, m15);
        const float32x4x4_t cl = {{cx, cy, cz, cw}};
        vst4q_f32(reinterpret_cast<float*>(&clip[i0]), cl);
        const float32x4_t negw = vnegq_f32(cw);
        const uint32x4_t out = vorrq_u32(
            vorrq_u32(vandq_u32(vcltq_f32(cx, negw), vdupq_n_u32(1)), vandq_u32(vcgtq_f32(cx, cw), vdupq_n_u32(2))),
            vorrq_u32(vorrq_u32(vandq_u32(vcltq_f32(cy, negw), vdupq_n_u32(4)), vandq_u32(vcgtq_f32(cy, cw), vdupq_n_u32(8))),
                      vandq_u32(vcleq_f32(cw, nearv), vdupq_n_u32(16))));
        const float32x4_t iw = vdivq_f32(one, cw);
        const float32x4_t sx = vmulq_f32(vaddq_f32(vmulq_f32(vmulq_f32(cx, iw), half), half), vw);
        const float32x4_t sy = vmulq_f32(vsubq_f32(half, vmulq_f32(vmulq_f32(cy, iw), half)), vh);
        const float32x4x4_t s4 = {{sx, sy, iw, vreinterpretq_f32_u32(out)}};
        vst4q_f32(reinterpret_cast<float*>(&sv[i0]), s4);
      }
    }
#endif
    for (size_t i = i0; i < nv; ++i) {
      const V4 p = Xform(vp, o.v[3 * i], o.v[3 * i + 1], o.v[3 * i + 2]);
      clip[i] = p;
      ScreenVert& q = sv[i];
      q.out = uint32_t(p.x < -p.w) | uint32_t(p.x > p.w) << 1 | uint32_t(p.y < -p.w) << 2 |
              uint32_t(p.y > p.w) << 3 | uint32_t(p.w <= kNear) << 4;
      if (!(q.out & 16)) {
        const float iw = 1.0f / p.w;
        q.x = (p.x * iw * 0.5f + 0.5f) * W;
        q.y = (0.5f - p.y * iw * 0.5f) * H;
        q.iz = iw;
      }
    }
    for (int k = 0; k < nb; ++k) {
      int cull = 0;
      if (o.cull && winding) {
        const int base_cull = k == 0 ? winding : 3 - winding;
        cull = o.cull == 1 ? base_cull : 3 - base_cull;
      }
      for (size_t t = 0; t < o.tri.size(); t += 3) {
        const uint32_t ia = o.tri[t], ib = o.tri[t + 1], ic = o.tri[t + 2];
        const uint32_t oa = sv[ia].out, ob = sv[ib].out, oc = sv[ic].out;
        if (oa & ob & oc) continue;  // wholly outside one frustum side, or behind the near plane
        if ((oa | ob | oc) & 16)
          DrawClipTri(bufs[k], clip[ia], clip[ib], clip[ic], cull, c.slot);
        else
          DrawTri(bufs[k], &sv[ia].x, &sv[ib].x, &sv[ic].x, cull, c.slot);
      }
    }
  }
  st.drawn_tris += tris;
  for (int k = 0; k < nb; ++k) Resolve(bufs[k], k == 0);
}

void Dump(uint64_t frame) {
  if (!g_cur) return;
  const Buffer& b = *g_cur;
  if (FILE* f = std::fopen("occlusion_raster.pgm", "wb")) {
    std::fprintf(f, "P5\n%d %d\n255\n", b.w, b.h);
    std::vector<uint8_t> row(size_t(b.w));
    for (int y = 0; y < b.h; ++y) {
      for (int x = 0; x < b.w; ++x) {
        const float iz = b.pix[size_t(y) * b.w + x];
        // distance = 1 / iz world units (clip w); 0 = nothing drawn (black); near bright, ~20000 units dark.
        int g = 0;
        if (iz > 0.0f) {
          const float d = 1.0f / iz;
          g = std::clamp(int(255.0f - 50.0f * std::log10(std::max(d, 10.0f))), 40, 255);
        }
        row[size_t(x)] = uint8_t(g);
      }
      std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
  }
  uint64_t tris = 0, verts = 0, ok = 0, by_list[3] = {}, culls[3] = {};
  for (const Occluder& o : g_occ) {
    if (!o.mesh || o.bad != kOk) continue;
    ++ok;
    tris += o.tri.size() / 3;
    verts += o.v.size() / 3;
    ++by_list[o.list];
    ++culls[o.cull];
  }
  REXLOG_INFO("[occlusion-raster] dump frame {}: occlusion_raster.pgm {}x{}; {} usable occluders (lists 48/76/104: "
              "{}/{}/{}; two-sided {}, normal {}, reversed {}), {} triangles, {} vertices, {:.1f} MB",
              frame, b.w, b.h, ok, by_list[0], by_list[1], by_list[2], culls[0], culls[1], culls[2], tris, verts,
              double(g_bytes) / 1048576.0);
}

}  // namespace

// ---- me2_occlusion_raster_async: the occluders drawn on a worker thread, a frame ahead --------------------------------
// 2026-11-05 (step 2): the game thread builds each frame's FSceneRenderer (constructor sub_8269A888, called by
// BeginRenderingViewFamily sub_8269EC38 before it enqueues the draw-scene command {vtable 0x8217EA68, renderer}) with its
// views' matrices; the UE3 render thread reaches Umbra (sub_8274AA38, r7 = that renderer) up to a frame later. The
// constructor hook posts a job (view 0's view-projection +256, camera, Umbra's LOD factor); a worker draws it into one
// of kAsyncBufs buffers; the render thread takes the job of its renderer when its view-projection is the same, waiting
// at most me2_occlusion_raster_wait_us, else draws in place as before. The cache (g_occ) is read by the worker and
// written by the render thread's walk only under g_cache_mutex; a job thus sees the cache of the walk before it.
namespace {
// ---- native visibility: an axis-aligned world box against a buffer (me2_occlusion.cpp me2_native_vis) -------------
// Codes: 1 visible, 2 occluded, 3 crosses the near plane (visible), 5 outside the frustum. Conservative like
// Me2RasterTest's rectangle path: occluded only when every pixel under the box's screen rectangle holds an occluder
// nearer than the box's nearest corner. The frustum planes (row vectors: clip_j = p . column j of vp) reject a box wholly
// behind one plane before its 8 corners are projected.
struct BoxPlanes {
  float p[5][4];
};
thread_local bool t_box_worker = false;  // the worker's box pass: the render thread's statistics (g_st) are not touched
void MakeBoxPlanes(const float* m, BoxPlanes& out) {
  auto col = [&](int j, float* c) { c[0] = m[j]; c[1] = m[4 + j]; c[2] = m[8 + j]; c[3] = m[12 + j]; };
  float c0[4], c1[4], c3[4];
  col(0, c0), col(1, c1), col(3, c3);
  for (int k = 0; k < 4; ++k) {
    out.p[0][k] = c3[k] + c0[k];  // left   x >= -w
    out.p[1][k] = c3[k] - c0[k];  // right  x <= w
    out.p[2][k] = c3[k] + c1[k];  // bottom y >= -w
    out.p[3][k] = c3[k] - c1[k];  // top    y <= w
    out.p[4][k] = c3[k];          // w >= 0
  }
}
uint8_t PreciseBox(const Buffer& b, const float* mn, const float* mx, float bias);  // below (needs PreciseTri)
uint8_t BoxCodeRect(const Buffer& b, const BoxPlanes& pl, const float* mn, const float* mx, float bias) {
  for (int k = 0; k < 5; ++k) {
    const float px = pl.p[k][0] >= 0 ? mx[0] : mn[0], py = pl.p[k][1] >= 0 ? mx[1] : mn[1],
                pz = pl.p[k][2] >= 0 ? mx[2] : mn[2];
    if (pl.p[k][0] * px + pl.p[k][1] * py + pl.p[k][2] * pz + pl.p[k][3] < 0.0f) return 5;
  }
  int out_l = 0, out_r = 0, out_b = 0, out_t = 0, behind = 0;
  float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f, max_iz = 0.0f;
  bool near_cut = false;
  for (int i = 0; i < 8; ++i) {
    const V4 p = Xform(b.vp, (i & 1) ? mx[0] : mn[0], (i & 2) ? mx[1] : mn[1], (i & 4) ? mx[2] : mn[2]);
    out_l += p.x < -p.w;
    out_r += p.x > p.w;
    out_b += p.y < -p.w;
    out_t += p.y > p.w;
    behind += p.w <= 0.0f;
    if (p.w <= kNear) {
      near_cut = true;
      continue;
    }
    const float iw = 1.0f / p.w;
    const float sx = (p.x * iw * 0.5f + 0.5f) * float(b.w), sy = (0.5f - p.y * iw * 0.5f) * float(b.h);
    mnx = std::min(mnx, sx);
    mxx = std::max(mxx, sx);
    mny = std::min(mny, sy);
    mxy = std::max(mxy, sy);
    max_iz = std::max(max_iz, iw);
  }
  if (out_l == 8 || out_r == 8 || out_b == 8 || out_t == 8 || behind == 8) return 5;
  if (near_cut) return 3;
  const int x0 = std::max(0, int(std::floor(mnx))), x1 = std::min(b.w - 1, int(std::ceil(mxx)) - 1);
  const int y0 = std::max(0, int(std::floor(mny))), y1 = std::min(b.h - 1, int(std::ceil(mxy)) - 1);
  if (x1 < x0 || y1 < y0) return 5;
  const float limit = max_iz * (1.0f + bias);
  bool all = true;
  for (int ty = y0 / 8; all && ty <= y1 / 8; ++ty)
    for (int tx = x0 / 8; tx <= x1 / 8; ++tx)
      if (!(b.tile[size_t(ty) * b.tw + tx] > limit)) {
        all = false;
        break;
      }
  if (all) return 2;
  for (int y = y0; y <= y1; ++y) {
    const float* row = &b.pix[size_t(y) * b.w];
    for (int x = x0; x <= x1; ++x)
      if (!(row[x] > limit)) return 1;
  }
  return 2;
}

// 2026-11-05 (console: native visibility drew ~155 more objects a frame than Umbra; ~70 a query were objects Umbra's own
// per-instance test rejected: it tests the box's triangles, the rectangle test only the screen rectangle at the
// nearest depth): a box the rectangle test calls visible gets the precise test of its camera-facing faces too.
uint8_t BoxCode(const Buffer& b, const BoxPlanes& pl, const float* mn, const float* mx, float bias) {
  const uint8_t code = BoxCodeRect(b, pl, mn, mx, bias);
  static const bool precise = REXCVAR_GET(me2_native_vis_precise);
  if (code != 1 || !precise) return code;
  const uint8_t p = PreciseBox(b, mn, mx, bias);
  return p == 2 ? 2 : 1;
}
}  // namespace

namespace {
constexpr int kAsyncBufs = 4;
struct Job {
  uint32_t renderer = 0;
  uint64_t seq = 0;
  float vp[16] = {}, cam[3] = {};
  float lod_factor = 1.0f;
  int buf = -1;          // index into g_abuf
  int state = 0;         // 0 free, 1 posted, 2 running, 3 done
  Stats stats;
  uint64_t raster_ns = 0;
};
std::mutex g_cache_mutex;   // g_occ / g_by_mesh / g_free: the walk (render thread) vs the worker's drawing
std::mutex g_job_mutex;     // the jobs and buffer ownership
std::condition_variable g_job_cv, g_done_cv;
Job g_jobs[kAsyncBufs];
Buffer g_abuf[kAsyncBufs];
int g_abuf_owner[kAsyncBufs];  // -1 free, j = job j, 100 = the render thread's current buffer
uint64_t g_job_seq = 0;
bool g_async_on = false;       // set by the render thread's first frame (cvars read there)
std::atomic<bool> g_async_ready{false};
int g_cur_abuf = -1;           // the render thread's current async buffer (owner 100)
struct AsyncStats {
  uint64_t posted = 0, dropped_busy = 0, taken = 0, waited = 0, wait_ns = 0, not_found = 0, vp_differs = 0,
           stale_dropped = 0, walk_skipped = 0, worker_ns = 0, reused = 0;
} g_ast;  // under g_job_mutex
// me2_native_vis_worker (2026-11-05): after a job's buffer is done, the worker also tests every Umbra instance's box
// against it (a copy of Umbra's wrapper and box tables taken first, so the render thread can tell which boxes changed
// since). Published per async buffer with 'ready' (release); the render thread uses it only when ready, else does the
// pass itself. Written only while the buffer is the worker's (a new job on the buffer clears 'ready' first).
struct BoxPass {
  std::atomic<bool> ready{false};
  uint32_t count = 0;
  std::vector<uint8_t> boxes;   // count x 24 raw (big-endian) bytes of Umbra's box table
  std::vector<uint32_t> wraps;  // count raw (big-endian) wrapper pointers
  std::vector<uint8_t> codes;   // count codes (BoxCode; 6 no usable box, 7 no wrapper)
  uint64_t ns = 0;
};
BoxPass g_boxpass[kAsyncBufs];
constexpr uint32_t kNvInstanceTable = 0x833960E4u, kNvBoxTable = 0x833960FCu;  // (me2_occlusion.cpp)
void WorkerBoxPass(int buf) {
  static const bool on = REXCVAR_GET(me2_native_vis_worker);
  BoxPass& bp = g_boxpass[buf];
  if (!on || !g_base) return;
  const auto t0 = std::chrono::steady_clock::now();
  const uint8_t* base = g_base;
  const uint32_t count = L32(base, kNvInstanceTable), data = L32(base, kNvInstanceTable + 4);
  const uint32_t nboxes = L32(base, kNvBoxTable), boxes = L32(base, kNvBoxTable + 4);
  if (count == 0 || count > 65536 || nboxes < count || !Readable(data, count * 4) || !Readable(boxes, count * 24))
    return;
  bp.count = count;
  bp.wraps.resize(count);
  bp.boxes.resize(size_t(count) * 24);
  bp.codes.resize(count);
  std::memcpy(bp.wraps.data(), base + data, size_t(count) * 4);
  std::memcpy(bp.boxes.data(), base + boxes, size_t(count) * 24);
  static const float bias = float(REXCVAR_GET(me2_occlusion_raster_bias));
  const Buffer& b = g_abuf[buf];
  BoxPlanes pl;
  MakeBoxPlanes(b.vp, pl);
  t_box_worker = true;
  for (uint32_t i = 0; i < count; ++i) {
    if (bp.wraps[i] == 0) {
      bp.codes[i] = 7;
      continue;
    }
    float mn[3], mx[3];
    bool ok = true;
    for (int k = 0; k < 3; ++k) {
      uint32_t a, c;
      std::memcpy(&a, &bp.boxes[size_t(i) * 24 + 4 * k], 4);
      std::memcpy(&c, &bp.boxes[size_t(i) * 24 + 12 + 4 * k], 4);
      a = __builtin_bswap32(a), c = __builtin_bswap32(c);
      std::memcpy(&mn[k], &a, 4);
      std::memcpy(&mx[k], &c, 4);
      ok = ok && std::isfinite(mn[k]) && std::isfinite(mx[k]) && mn[k] <= mx[k];
    }
    bp.codes[i] = ok ? BoxCode(b, pl, mn, mx, bias) : 6;
  }
  bp.ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  t_box_worker = false;
  bp.ready.store(true, std::memory_order_release);
}

#if REX_PLATFORM_SWITCH
extern "C" bool RexSwitchSetCurrentThreadMask(int preferred, unsigned long long mask);  // sdk core/threading_switch.cpp
extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority);                       // sdk core/threading_switch.cpp
#endif
void Worker() {
#if REX_PLATFORM_SWITCH
  if (const int mask = REXCVAR_GET(me2_occlusion_raster_mask); mask > 0) {
    int core = REXCVAR_GET(me2_occlusion_raster_core);
    if (core < 0 || !(mask & (1 << core))) core = __builtin_ctz(unsigned(mask));
    const bool ok = RexSwitchSetCurrentThreadMask(core, unsigned(mask));
    REXLOG_INFO("[occlusion-raster] worker on cores {:#x}, preferred {} ({})", mask, core, ok ? "set" : "FAILED");
  } else if (const int core = REXCVAR_GET(me2_occlusion_raster_core); core >= 0) {
    RexSwitchSetCurrentThreadCore(core);
  }
  if (const int prio = REXCVAR_GET(me2_occlusion_raster_prio); prio >= 0) {
    const bool ok = RexSwitchSetCurrentThreadPriorityOk(prio);
    REXLOG_INFO("[occlusion-raster] worker at priority {:#x} ({})", prio, ok ? "set" : "FAILED");
  }
#endif
  for (;;) {
    int j = -1;
    {
      std::unique_lock<std::mutex> lk(g_job_mutex);
      g_job_cv.wait(lk, [&] {
        uint64_t best = ~uint64_t(0);
        for (int k = 0; k < kAsyncBufs; ++k)
          if (g_jobs[k].state == 1 && g_jobs[k].seq < best) best = g_jobs[k].seq, j = k;
        return j >= 0;
      });
      g_jobs[j].state = 2;
    }
    Job& job = g_jobs[j];
    g_boxpass[job.buf].ready.store(false, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    bool reused = false;
    {
      std::lock_guard<std::mutex> cache(g_cache_mutex);
      // me2_occlusion_raster_reuse (2026-11-05, console: the render thread waited 1-4 ms a frame for this worker at
      // ~30 fps): the same camera over the same occluders draws the same pixels, so the last buffer drawn is taken.
      // Only this thread writes the async buffers, so the last one still holds them (or is this job's own buffer).
      static const bool reuse = REXCVAR_GET(me2_occlusion_raster_reuse);
      static const int reuse_max = REXCVAR_GET(me2_occlusion_raster_reuse_max);
      static struct {
        int reused = 0;
        bool valid = false;
        int buf = -1;
        float vp[16], cam[3], lod;
        uint64_t gen;
        Stats stats;
      } last;
      if (reuse && last.valid && last.reused < reuse_max && last.gen == g_cache_gen && last.lod == job.lod_factor &&
          std::memcmp(last.vp, job.vp, sizeof(last.vp)) == 0 && std::memcmp(last.cam, job.cam, sizeof(last.cam)) == 0) {
        Buffer& dst = g_abuf[job.buf];
        if (job.buf != last.buf) {
          const Buffer& src = g_abuf[last.buf];
          std::copy(src.pix.begin(), src.pix.end(), dst.pix.begin());
          std::copy(src.tile.begin(), src.tile.end(), dst.tile.begin());
          std::copy(src.corner.begin(), src.corner.end(), dst.corner.begin());
          std::memcpy(dst.vp, src.vp, sizeof(dst.vp));
          std::memcpy(dst.cam, src.cam, sizeof(dst.cam));
        }
        job.stats = last.stats;  // the same drawing: its statistics again (the report's per-frame averages)
        last.buf = job.buf;
        ++last.reused;
        reused = true;
      } else {
        job.stats = Stats{};
        t_st = &job.stats;
        Rasterize(job.vp, job.cam, job.lod_factor, &g_abuf[job.buf], 1);
        last.valid = true;
        last.reused = 0;
        last.buf = job.buf;
        std::memcpy(last.vp, job.vp, sizeof(last.vp));
        std::memcpy(last.cam, job.cam, sizeof(last.cam));
        last.lod = job.lod_factor;
        last.gen = g_cache_gen;
        last.stats = job.stats;
      }
    }
    job.raster_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    {
      std::lock_guard<std::mutex> lk(g_job_mutex);
      job.state = 3;
      g_ast.worker_ns += job.raster_ns;
      g_ast.reused += reused;
    }
    g_done_cv.notify_all();
    WorkerBoxPass(job.buf);
  }
}

// Game thread: a renderer was just built.
void PostJob(const uint8_t* base, uint32_t renderer) {
  if (!g_async_ready.load(std::memory_order_acquire) || !Readable(renderer, 72)) return;
  // The draw-count rotation's GPU slot (me2_ab_cycle "raster"): no CPU occlusion work at all (me2_occlusion.cpp).
  if (Me2AbOff(Me2Ab::kRaster)) return;
  const uint32_t views = L32(base, renderer + 64), count = L32(base, renderer + 68);
  if (count < 1 || count > 4 || !Readable(views, 1776)) return;
  Job job;
  job.renderer = renderer;
  for (int i = 0; i < 16; ++i) job.vp[i] = LF(base, views + 256 + 4 * i);
  float m[16];
  for (int i = 0; i < 16; ++i) m[i] = LF(base, views + 64 + 4 * i);
  // Camera: -(T * R^T) of the view matrix (rows 0-2 rotation, row 3 translation), as me2_occlusion.cpp's CameraPos.
  for (int c = 0; c < 3; ++c) job.cam[c] = -(m[12] * m[c * 4 + 0] + m[13] * m[c * 4 + 1] + m[14] * m[c * 4 + 2]);
  job.lod_factor = LF(base, 0x8340C920u);
  if (!(job.lod_factor > 0.01f && job.lod_factor < 100.0f)) job.lod_factor = 1.0f;
  {
    std::lock_guard<std::mutex> lk(g_job_mutex);
    int slot = -1, buf = -1;
    for (int k = 0; k < kAsyncBufs && slot < 0; ++k)
      if (g_jobs[k].state == 0) slot = k;
    for (int k = 0; k < kAsyncBufs && buf < 0; ++k)
      if (g_abuf_owner[k] == -1) buf = k;
    if (slot < 0 || buf < 0) {
      // All busy (renderers never rendered through Umbra pile up): the oldest posted/done job goes.
      uint64_t oldest = ~uint64_t(0);
      int victim = -1;
      for (int k = 0; k < kAsyncBufs; ++k)
        if ((g_jobs[k].state == 1 || g_jobs[k].state == 3) && g_jobs[k].seq < oldest) oldest = g_jobs[k].seq, victim = k;
      if (victim < 0) {
        ++g_ast.dropped_busy;
        return;
      }
      g_abuf_owner[g_jobs[victim].buf] = -1;
      g_jobs[victim].state = 0;
      ++g_ast.stale_dropped;
      slot = victim;
      buf = -1;
      for (int k = 0; k < kAsyncBufs && buf < 0; ++k)
        if (g_abuf_owner[k] == -1) buf = k;
      if (buf < 0) {
        ++g_ast.dropped_busy;
        return;
      }
    }
    job.seq = ++g_job_seq;
    job.buf = buf;
    job.state = 1;
    g_abuf_owner[buf] = slot;
    g_jobs[slot] = job;
    ++g_ast.posted;
  }
  g_job_cv.notify_one();
}

// Render thread: the job of this renderer, waited for if still drawing; nullptr if none (or its camera differs).
// On success its buffer becomes the current one (owner 100), the previous current buffer is freed.
Buffer* TakeJob(uint32_t renderer, const float* vp, Stats& merged) {
  static const auto max_wait = std::chrono::microseconds(int64_t(REXCVAR_GET(me2_occlusion_raster_wait_us)));
  std::unique_lock<std::mutex> lk(g_job_mutex);
  int j = -1;
  uint64_t best = ~uint64_t(0);
  for (int k = 0; k < kAsyncBufs; ++k)
    if (g_jobs[k].state != 0 && g_jobs[k].renderer == renderer && g_jobs[k].seq < best) best = g_jobs[k].seq, j = k;
  if (j < 0) {
    ++g_ast.not_found;
    return nullptr;
  }
  if (g_jobs[j].state != 3) {
    ++g_ast.waited;
    const auto t0 = std::chrono::steady_clock::now();
    g_done_cv.wait_for(lk, max_wait, [&] { return g_jobs[j].state == 3; });
    g_ast.wait_ns += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    if (g_jobs[j].state != 3) return nullptr;  // still drawing: left to finish, dropped by a later post
  }
  Job& job = g_jobs[j];
  // Older jobs were for renderers that never reached Umbra (or were skipped): free them.
  for (int k = 0; k < kAsyncBufs; ++k)
    if (k != j && (g_jobs[k].state == 1 || g_jobs[k].state == 3) && g_jobs[k].seq < job.seq) {
      g_abuf_owner[g_jobs[k].buf] = -1;
      g_jobs[k].state = 0;
      ++g_ast.stale_dropped;
    }
  const bool same = std::memcmp(job.vp, vp, sizeof(job.vp)) == 0;
  if (!same) {
    ++g_ast.vp_differs;
    g_abuf_owner[job.buf] = -1;
    job.state = 0;
    return nullptr;
  }
  if (g_cur_abuf >= 0) g_abuf_owner[g_cur_abuf] = -1;
  g_cur_abuf = job.buf;
  g_abuf_owner[job.buf] = 100;
  job.state = 0;
  ++g_ast.taken;
  // The job's drawing statistics join the render thread's (raster time = the worker's).
  Stats& s = job.stats;
  merged.drawn_occ += s.drawn_occ, merged.drawn_tris += s.drawn_tris, merged.budget_cut += s.budget_cut;
  merged.small += s.small, merged.off += s.off, merged.lod_out += s.lod_out, merged.culled_tris += s.culled_tris;
  merged.dense += s.dense, merged.covered_px += s.covered_px, merged.near_clipped += s.near_clipped;
  merged.hidden_occ += s.hidden_occ;
  merged.not_recent += s.not_recent;
  merged.off_fast += s.off_fast, merged.lean_frames += s.lean_frames;
  merged.raster_ns += job.raster_ns;
  return &g_abuf[g_cur_abuf];
}
}  // namespace

// sub_8269A888 (FSceneRenderer constructor), game thread: after it, the renderer's views hold this frame's matrices.
REX_EXTERN(__imp__sub_8269A888);
REX_HOOK_RAW(sub_8269A888) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8269A888(ctx, base);
  PostJob(base, self);
}

// sub_8274AA38 (me2_occlusion.cpp), UE3 render thread, before Umbra's traversal of this view: the occluders drawn
// with this view's view-projection (row vectors, view +256) - taken from the worker when it drew the same, else here;
// then the depth draw lists walked (the cache for the next frames).
extern "C" void Me2RasterFrame(const uint8_t* base, uint32_t scene, const float* vp, const float* cam,
                               uint64_t frame, uint32_t renderer) {
  static bool init = false;
  if (!init) {
    init = true;
    const int w = REXCVAR_GET(me2_occlusion_raster_w), h = REXCVAR_GET(me2_occlusion_raster_h);
    InitBuffer(g_buf[0], w, h);
    if (REXCVAR_GET(me2_occlusion_raster_dual)) InitBuffer(g_buf[1], w, h);
    g_async_on = REXCVAR_GET(me2_occlusion_raster_async) && !REXCVAR_GET(me2_occlusion_raster_dual) &&
                 !REXCVAR_GET(me2_occlusion_raster_trace);
    if (g_async_on) {
      for (int k = 0; k < kAsyncBufs; ++k) {
        InitBuffer(g_abuf[k], w, h);
        g_abuf_owner[k] = -1;
      }
      new std::thread(Worker);  // leaked on purpose: never detach() on the Switch (it throws there)
      g_async_ready.store(true, std::memory_order_release);
    }
    REXLOG_INFO("[occlusion-raster] on: {}x{}, lists mask {}, budget {} triangles a frame, winding {}{}{}", w, h,
                REXCVAR_GET(me2_occlusion_raster_lists), REXCVAR_GET(me2_occlusion_raster_tris),
                REXCVAR_GET(me2_occlusion_raster_winding),
                REXCVAR_GET(me2_occlusion_raster_dual) ? " (+ the other winding, dual)" : "",
                g_async_on ? ", drawn on a worker thread from the game thread's renderer (async)" : "");
  }
  g_ready_frame = ~uint64_t(0);
  if (!Readable(scene, 4 * 308 + 132)) return;
  if (scene != g_scene) {
    REXLOG_INFO("[occlusion-raster] scene {:08X} (was {:08X})", scene, g_scene);
    g_scene = scene;
  }
  t_st = &g_st;
  // me2_occlusion_raster_trace: of the occluders drawn here last frame, how many the game rendered too (their
  // primitive's LastRenderTime moved on since; checked before the walk can drop them).
  for (uint32_t slot : g_drawn_slots) {
    if (slot >= g_occ.size() || !g_occ[slot].psi || !Readable(g_occ[slot].psi, 0xB0)) continue;
    ++g_st.vis_checked;
    g_st.vis_set += LF(base, g_occ[slot].psi + 0xAC) > g_occ[slot].lrt;
  }
  std::memcpy(g_cam, cam, sizeof(g_cam));
  g_trace_frame_first = g_trace_next;
  g_base = base;
  const auto t0 = std::chrono::steady_clock::now();
  Buffer* got = g_async_on ? TakeJob(renderer, vp, g_st) : nullptr;
  const auto t1 = std::chrono::steady_clock::now();
  if (got) {
    g_cur = got;
    g_st.take_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    // The walk for the next frames (skipped when the worker is drawing the next job right now).
    std::unique_lock<std::mutex> cache(g_cache_mutex, std::try_to_lock);
    if (cache.owns_lock()) {
      Walk(base, scene, frame);
      g_st.walk_ns += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t1).count());
    } else {
      std::lock_guard<std::mutex> lk(g_job_mutex);
      ++g_ast.walk_skipped;
    }
  } else {
    // In place (no job, its camera differs, or async off): walk, then draw into g_buf.
    std::lock_guard<std::mutex> cache(g_cache_mutex);
    if (g_async_on && g_cur_abuf >= 0) {  // the async buffer the tests used last frame is no longer current
      std::lock_guard<std::mutex> lk(g_job_mutex);
      g_abuf_owner[g_cur_abuf] = -1;
      g_cur_abuf = -1;
    }
    Walk(base, scene, frame);
    const auto t2 = std::chrono::steady_clock::now();
    float lod_factor = LF(base, 0x8340C920u);
    if (!(lod_factor > 0.01f && lod_factor < 100.0f)) lod_factor = 1.0f;
    Rasterize(vp, cam, lod_factor, g_buf, REXCVAR_GET(me2_occlusion_raster_dual) ? 2 : 1);
    const auto t3 = std::chrono::steady_clock::now();
    g_cur = &g_buf[0];
    g_st.walk_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
    g_st.raster_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count());
    g_st.inline_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count());
    ++g_st.inline_frames;
  }
  ++g_st.frames;
  g_ready_frame = frame;
  static const int dump_s = REXCVAR_GET(me2_occlusion_raster_dump_s);
  static auto last_dump = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (dump_s > 0 && now - last_dump >= std::chrono::seconds(dump_s)) {
    last_dump = now;
    std::lock_guard<std::mutex> cache(g_cache_mutex);
    Dump(frame);
  }
}

// me2_prepass.cpp's gameplay prepass hook: [renderer +0] there, to check sub_8274AA38's r6 is the same FScene.
extern "C" void Me2RasterNotePrepassScene(uint32_t scene) {
  if (scene != g_prepass_scene) g_prepass_scene = scene;
  if (g_scene && scene != g_scene) ++g_scene_mismatch;
}

namespace {
// ---- me2_occlusion_raster_trace: what made a box occluded that the GPU then counted visible -----------------------
struct TraceOcc {
  uint32_t slot = 0, mesh = 0, mesh_id = 0, flags = 0, tris = 0, px = 0;
  uint8_t list = 0, cull = 0;
  float lo[3] = {}, hi[3] = {}, d2 = 0.0f, min_d2 = 0.0f, max_d2 = 0.0f, iz_far = 0.0f;
  uint64_t first_seen = 0;
  uint8_t drawn = 2;  // the game rendered it since the test (LastRenderTime newer): 0 no, 1 yes, 2 unknown
  uint32_t psi = 0;
  float lrt = 0.0f;  // its LastRenderTime at the test
};
struct TraceRec {
  uint64_t frame = 0;
  float lo[3] = {}, hi[3] = {}, cam[3] = {}, box_iz = 0.0f;
  int x0 = 0, x1 = -1, y0 = 0, y1 = -1, nocc = 0;
  uint32_t px = 0;
  bool near_plane = false;
  TraceOcc occ[3];
};
constexpr uint32_t kTraceRing = 8192;
std::vector<TraceRec> g_trace;
thread_local uint32_t t_trace_token = 0;
struct FalseSum {
  uint64_t n = 0;
  TraceOcc occ;
  uint64_t frame = 0;
};
std::unordered_map<uint32_t, FalseSum> g_false_by_mesh;
uint64_t g_false_total = 0, g_false_logged = 0, g_false_window = 0;
uint64_t g_false_top_drawn[3] = {};  // false-occluded tests by whether their main occluder was drawn (no/yes/unknown)

// Records an occluded test: the box, and the (up to 3) occluders deciding the most pixels under its rectangle.
uint32_t TraceOccluded(const uint8_t* base, uint32_t verts_va, uint32_t count, float box_iz, int x0, int x1, int y0,
                       int y1, bool near_plane) {
  const auto t0 = std::chrono::steady_clock::now();
  struct Timer {
    std::chrono::steady_clock::time_point t;
    ~Timer() {
      g_st.trace_ns += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count());
    }
  } timer{t0};
  if (g_trace.empty()) g_trace.resize(kTraceRing);
  const uint32_t i = g_trace_next++ % kTraceRing;
  TraceRec& r = g_trace[i];
  r = TraceRec{};
  r.frame = g_ready_frame;
  r.box_iz = box_iz;
  r.near_plane = near_plane;
  std::memcpy(r.cam, g_cam, sizeof(r.cam));
  for (int c = 0; c < 3; ++c) {
    r.lo[c] = 1e30f;
    r.hi[c] = -1e30f;
  }
  for (uint32_t v = 0; v < std::min<uint32_t>(count, 64); ++v)
    for (int c = 0; c < 3; ++c) {
      const float f = LF(base, verts_va + 12 * v + 4 * c);
      r.lo[c] = std::min(r.lo[c], f);
      r.hi[c] = std::max(r.hi[c], f);
    }
  const Buffer& b = *g_cur;
  if (x1 < x0 || y1 < y0) {  // the box reached the near plane: the whole screen
    x0 = 0, x1 = b.w - 1, y0 = 0, y1 = b.h - 1;
  }
  r.x0 = x0, r.x1 = x1, r.y0 = y0, r.y1 = y1;
  // Per occluder: pixels decided and the farthest of them (a short list; occluders past 32 are not counted).
  std::pair<uint32_t, std::pair<uint32_t, float>> cnt[32];
  int ncnt = 0;
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const uint32_t id = b.pix_id[size_t(y) * b.w + x];
      const float iz = b.pix[size_t(y) * b.w + x];
      ++r.px;
      int k = 0;
      while (k < ncnt && cnt[k].first != id) ++k;
      if (k == ncnt) {
        if (ncnt == 32) continue;
        cnt[ncnt++] = {id, {0, iz}};
      }
      ++cnt[k].second.first;
      cnt[k].second.second = std::min(cnt[k].second.second, iz);
    }
  std::vector<std::pair<uint32_t, std::pair<uint32_t, float>>> v(cnt, cnt + ncnt);
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
  for (size_t k = 0; k < v.size() && r.nocc < 3; ++k) {
    TraceOcc& t = r.occ[r.nocc++];
    t.slot = v[k].first;
    t.px = v[k].second.first;
    t.iz_far = v[k].second.second;
    if (t.slot < g_occ.size()) {
      const Occluder& o = g_occ[t.slot];
      t.mesh = o.mesh, t.mesh_id = o.mesh_id, t.flags = o.flags, t.tris = uint32_t(o.tri.size() / 3);
      t.list = o.list, t.cull = o.cull;
      std::memcpy(t.lo, o.lo, sizeof(t.lo));
      std::memcpy(t.hi, o.hi, sizeof(t.hi));
      t.d2 = o.d2, t.min_d2 = o.min_d2, t.max_d2 = o.max_d2, t.first_seen = o.first_seen;
      t.psi = o.psi;
      if (o.psi) t.lrt = LF(base, o.psi + 0xAC);
    }
  }
  return i + 1;
}

// 2026-11-05 (first PC compare: misses 'a pixel behind the box' 5-7k and 'near plane' 4k per 10 s): the box's own
// triangles (Umbra's test geometry), each clipped at the near plane and drawn CONSERVATIVELY against the buffer: every
// pixel the triangle touches at all, at the triangle's nearest depth within that pixel (its plane's largest 1/w over the
// pixel's corners, at most its nearest vertex). Visible as soon as one such pixel is not behind an occluder. Both faces
// of the box count (the camera may be inside it). Returns 1 visible, 2 occluded, 0 no usable geometry.
struct ScreenV {
  float x, y, iz;
};
bool PreciseTri(const Buffer& b, const ScreenV& p0, const ScreenV& p1, const ScreenV& p2, float bias, bool fast) {
  const float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
  if (std::fabs(area) < 1e-8f) return false;  // edge-on: its neighbours cover it
  const float s = area > 0.0f ? 1.0f : -1.0f;
  // Edge functions oriented positive inside (as DrawTri).
  float A[3], B[3], C[3];
  const ScreenV* v[3] = {&p0, &p1, &p2};
  for (int k = 0; k < 3; ++k) {
    const ScreenV& u = *v[(k + 1) % 3];
    const ScreenV& w = *v[(k + 2) % 3];
    A[k] = (u.y - w.y) * s;
    B[k] = (w.x - u.x) * s;
    C[k] = (u.x * w.y - u.y * w.x) * s;
  }
  // 1/w plane: iz(x, y) = p0.iz + dzx (x - p0.x) + dzy (y - p0.y).
  const float inv = 1.0f / area;
  const float dzx = ((p1.iz - p0.iz) * (p2.y - p0.y) - (p2.iz - p0.iz) * (p1.y - p0.y)) * inv;
  const float dzy = ((p2.iz - p0.iz) * (p1.x - p0.x) - (p1.iz - p0.iz) * (p2.x - p0.x)) * inv;
  const float zmax = std::max({p0.iz, p1.iz, p2.iz});
  const float limit_max = zmax * (1.0f + bias);
  const int x0 = std::max(0, int(std::floor(std::min({p0.x, p1.x, p2.x}))));
  const int x1 = std::min(b.w - 1, int(std::floor(std::max({p0.x, p1.x, p2.x}))));
  const int y0 = std::max(0, int(std::floor(std::min({p0.y, p1.y, p2.y}))));
  const int y1 = std::min(b.h - 1, int(std::floor(std::max({p0.y, p1.y, p2.y}))));
  if (x1 < x0 || y1 < y0) return false;
  // Over a unit pixel square from its top-left corner, an edge function's largest value is at +max(0,A) +max(0,B).
  float ox[3];
  for (int k = 0; k < 3; ++k) ox[k] = std::max(0.0f, A[k]) + std::max(0.0f, B[k]);
  // Fast: the rows' span ends as products with 1/A (three divisions a row before; the 1e-3 widening covers the
  // reciprocal's rounding, ~1e-5 px at 256 px).
  float rA[3];
  for (int k = 0; k < 3; ++k) rA[k] = A[k] != 0.0f ? 1.0f / A[k] : 0.0f;
  const float scale = 1.0f + bias;
  const float zo = std::max(0.0f, dzx) + std::max(0.0f, dzy);
  for (int ty = y0 / 8; ty <= y1 / 8; ++ty)
    for (int tx = x0 / 8; tx <= x1 / 8; ++tx) {
      const float tmin = b.tile[size_t(ty) * b.tw + tx];
      if (tmin > limit_max) continue;  // the whole tile is nearer than this triangle anywhere
      const int ya = std::max(y0, ty * 8), yb = std::min(y1, ty * 8 + 7);
      const int xa = std::max(x0, tx * 8), xb = std::min(x1, tx * 8 + 7);
      // The plane's largest 1/w over the tile's part of the box rectangle: if every occluder pixel of the tile is
      // nearer still, nothing of this triangle shows there.
      const float zt = std::min(zmax, p0.iz + dzx * (float(dzx > 0.0f ? xb + 1 : xa) - p0.x) +
                                          dzy * (float(dzy > 0.0f ? yb + 1 : ya) - p0.y));
      if (tmin > zt * (1.0f + bias)) continue;
      if (fast) {
        // 2026-11-05 (console: ~30 us a test, 95 % of them in this per-pixel test): the tile wholly inside the
        // triangle (its four corners pass the three edge functions) and its farthest occluder pixel not nearer than
        // the triangle's farthest point over the tile: that pixel shows the box under the rule of the loop below
        // (whose depth at any covered pixel is at least the plane's minimum over the tile). No pixel loop.
        const float X0 = float(tx * 8), X1 = X0 + 8.0f, Y0 = float(ty * 8), Y1 = Y0 + 8.0f;
        bool inside = true;
        for (int k = 0; k < 3 && inside; ++k) {
          const float e = std::min(std::min(A[k] * X0 + B[k] * Y0, A[k] * X1 + B[k] * Y0),
                                   std::min(A[k] * X0 + B[k] * Y1, A[k] * X1 + B[k] * Y1)) + C[k];
          inside = e >= 0.0f;
        }
        if (inside) {
          const float zmin = p0.iz + dzx * ((dzx > 0.0f ? X0 : X1) - p0.x) + dzy * ((dzy > 0.0f ? Y0 : Y1) - p0.y);
          if (!(tmin > zmin * (1.0f + bias))) {
            if (!t_box_worker) ++g_st.tile_accepts;
            return true;
          }
        }
      }
      for (int y = ya; y <= yb; ++y) {
        const float fy = float(y);
        // 2026-11-05 (step 2): the row's conservative span, A x + (B y + C + ox) >= 0 for the three edges (x = the
        // pixel's left edge), instead of the three edge tests at every pixel; widened a little against rounding.
        float lo = float(xa), hi = float(xb);
        bool empty = false;
        for (int k = 0; k < 3; ++k) {
          const float r = B[k] * fy + C[k] + ox[k];
          if (A[k] > 0.0f)
            lo = std::max(lo, (fast ? -r * rA[k] : -r / A[k]) - 1e-3f);
          else if (A[k] < 0.0f)
            hi = std::min(hi, (fast ? -r * rA[k] : -r / A[k]) + 1e-3f);
          else if (r < 0.0f)
            empty = true;
        }
        if (empty || !(lo <= hi)) continue;
        const int xs = int(std::ceil(lo)), xe = int(std::floor(hi));
        const float* row = &b.pix[size_t(y) * b.w];
        const float zr = p0.iz + dzy * (fy - p0.y) + zo - dzx * p0.x;
        int x = xs;
        if (fast) {
          // 2026-11-05 (console ~23 us a test, mostly this loop over the pixels of occluded boxes): 4 pixels at a time
          // (NEON / SSE), the same rule per lane: visible where !(occluder > box depth * (1 + bias)) (NaN = visible).
          const V4f zmax4 = {zmax, zmax, zmax, zmax}, scale4 = {scale, scale, scale, scale};
          const V4f dzx4 = {dzx, dzx, dzx, dzx}, zr4 = {zr, zr, zr, zr};
          V4f xf = {float(x), float(x + 1), float(x + 2), float(x + 3)};
          const V4f step4 = {4.0f, 4.0f, 4.0f, 4.0f};
          for (; x + 3 <= xe; x += 4, xf += step4) {
            V4f occ;
            std::memcpy(&occ, row + x, sizeof(occ));
            const V4f zp = zr4 + dzx4 * xf;
            const V4f z = (zp < zmax4 ? zp : zmax4) * scale4;
            const V4i hidden = occ > z;  // -1 where the occluder is nearer
            uint64_t h[2];
            std::memcpy(h, &hidden, sizeof(h));
            if ((h[0] & h[1]) != ~uint64_t(0)) return true;
          }
        }
        for (; x <= xe; ++x) {
          const float z = std::min(zmax, zr + dzx * float(x));
          if (!(row[x] > z * (1.0f + bias))) return true;
        }
      }
    }
  return false;
}

// The camera position of a view-projection (row vectors): the point with clip x = y = w = 0.
bool CameraOfVp(const float* m, float* c) {
  const float a[3][3] = {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[3], m[7], m[11]}};
  const float r[3] = {-m[12], -m[13], -m[15]};
  auto det3 = [](const float (&t)[3][3]) {
    return t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
           t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
  };
  const float det = det3(a);
  if (!(std::fabs(det) > 1e-20f)) return false;
  for (int k = 0; k < 3; ++k) {
    float t[3][3];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) t[i][j] = j == k ? r[i] : a[i][j];
    c[k] = det3(t) / det;
  }
  return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]);
}

uint8_t PreciseTest(const uint8_t* base, const Buffer& b, uint32_t verts_va, uint32_t count, uint32_t idx_va,
                    uint32_t ntris, float bias, bool fast) {
  if (!idx_va || ntris == 0 || ntris > 4096 || count == 0 || count > 4096) return 0;
  static thread_local std::vector<V4> clip;
  clip.resize(count);
  bool in_front = true;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t a = verts_va + 12 * i;
    clip[i] = Xform(b.vp, LF(base, a), LF(base, a + 4), LF(base, a + 8));
    in_front = in_front && clip[i].w > kNear;
  }
  // 2026-11-05: a closed box (Umbra's 8 corners, 12 triangles) wholly in front of the camera, so the camera is outside
  // it: its back faces lie behind its front faces over the same silhouette; only the front faces are tested. Front =
  // the camera on the outer side of the face's plane (outward = away from the box's centre).
  static const bool front_only = REXCVAR_GET(me2_occlusion_raster_front_only);
  uint32_t front_mask = ~0u;
  if (fast && front_only && in_front && count == 8 && ntris == 12) {
    float cam[3], ctr[3] = {}, w[8][3];
    if (CameraOfVp(b.vp, cam)) {
      for (uint32_t i = 0; i < 8; ++i)
        for (int c = 0; c < 3; ++c) {
          w[i][c] = LF(base, verts_va + 12 * i + 4 * c);
          ctr[c] += 0.125f * w[i][c];
        }
      uint32_t mask = 0;
      bool ok = true;
      for (uint32_t t = 0; t < 12 && ok; ++t) {
        uint32_t id[3];
        for (int k = 0; k < 3; ++k) id[k] = L32(base, idx_va + 12 * t + 4 * k);
        if (id[0] >= 8 || id[1] >= 8 || id[2] >= 8) {
          ok = false;
          break;
        }
        const float* p = w[id[0]];
        const float e1[3] = {w[id[1]][0] - p[0], w[id[1]][1] - p[1], w[id[1]][2] - p[2]};
        const float e2[3] = {w[id[2]][0] - p[0], w[id[2]][1] - p[1], w[id[2]][2] - p[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float out = n[0] * (p[0] - ctr[0]) + n[1] * (p[1] - ctr[1]) + n[2] * (p[2] - ctr[2]);
        const float see = n[0] * (cam[0] - p[0]) + n[1] * (cam[1] - p[1]) + n[2] * (cam[2] - p[2]);
        // Degenerate or through the centre (not a box face): kept. Else front if the camera is on its outer side.
        if (out == 0.0f || !std::isfinite(out) || !std::isfinite(see) || (out > 0.0f) == (see > 0.0f)) mask |= 1u << t;
      }
      if (ok && mask) {
        front_mask = mask;
        ++g_st.front_only;
      }
    }
  }
  auto screen = [&](const V4& p) -> ScreenV {
    const float iw = 1.0f / p.w;
    return {(p.x * iw * 0.5f + 0.5f) * float(b.w), (0.5f - p.y * iw * 0.5f) * float(b.h), iw};
  };
  bool any = false;
  for (uint32_t t = 0; t < ntris; ++t) {
    if (t < 32 && !(front_mask & (1u << t))) continue;
    uint32_t id[3];
    for (int k = 0; k < 3; ++k) id[k] = L32(base, idx_va + 12 * t + 4 * k);
    if (id[0] >= count || id[1] >= count || id[2] >= count) return 0;
    const V4 in[3] = {clip[id[0]], clip[id[1]], clip[id[2]]};
    if ((in[0].x < -in[0].w && in[1].x < -in[1].w && in[2].x < -in[2].w) ||
        (in[0].x > in[0].w && in[1].x > in[1].w && in[2].x > in[2].w) ||
        (in[0].y < -in[0].w && in[1].y < -in[1].w && in[2].y < -in[2].w) ||
        (in[0].y > in[0].w && in[1].y > in[1].w && in[2].y > in[2].w))
      continue;
    V4 poly[4];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
      const V4& p = in[i];
      const V4& q = in[(i + 1) % 3];
      const bool pin = p.w > kNear, qin = q.w > kNear;
      if (pin) poly[n++] = p;
      if (pin != qin) {
        const float u = (kNear - p.w) / (q.w - p.w);
        poly[n++] = {p.x + (q.x - p.x) * u, p.y + (q.y - p.y) * u, p.z + (q.z - p.z) * u, kNear};
      }
    }
    if (n < 3) continue;
    any = true;
    const ScreenV s0 = screen(poly[0]), s1 = screen(poly[1]), s2 = screen(poly[2]);
    if (PreciseTri(b, s0, s1, s2, bias, fast)) return 1;
    if (n == 4 && PreciseTri(b, s0, s2, screen(poly[3]), bias, fast)) return 1;
  }
  return any ? 2 : 4;  // no triangle reached the screen: off the screen
}

// An axis-aligned world box's faces against the buffer (native visibility): the faces facing the camera when it is
// outside the box (each face 2 triangles), every face when inside; clipped at the near plane like PreciseTest.
// 1 visible, 2 occluded, 0 unusable.
uint8_t PreciseBox(const Buffer& b, const float* mn, const float* mx, float bias) {
  static const bool fast = REXCVAR_GET(me2_occlusion_raster_fast_tests);
  float cam[3];
  const bool have_cam = CameraOfVp(b.vp, cam);
  V4 clip[8];
  for (int i = 0; i < 8; ++i) clip[i] = Xform(b.vp, (i & 1) ? mx[0] : mn[0], (i & 2) ? mx[1] : mn[1], (i & 4) ? mx[2] : mn[2]);
  // faces: -x +x -y +y -z +z as corner quads (bit 0 = x, 1 = y, 2 = z)
  static const int kFace[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  auto screen = [&](const V4& p) -> ScreenV {
    const float iw = 1.0f / p.w;
    return {(p.x * iw * 0.5f + 0.5f) * float(b.w), (0.5f - p.y * iw * 0.5f) * float(b.h), iw};
  };
  bool any = false;
  for (int f = 0; f < 6; ++f) {
    if (have_cam) {
      const int axis = f / 2;
      const bool pos = (f & 1) != 0;
      const bool inside = cam[0] >= mn[0] && cam[0] <= mx[0] && cam[1] >= mn[1] && cam[1] <= mx[1] &&
                          cam[2] >= mn[2] && cam[2] <= mx[2];
      if (!inside && (pos ? !(cam[axis] > mx[axis]) : !(cam[axis] < mn[axis]))) continue;  // back face
    }
    for (int t = 0; t < 2; ++t) {
      const V4 in[3] = {clip[kFace[f][0]], clip[kFace[f][1 + t]], clip[kFace[f][2 + t]]};
      if ((in[0].x < -in[0].w && in[1].x < -in[1].w && in[2].x < -in[2].w) ||
          (in[0].x > in[0].w && in[1].x > in[1].w && in[2].x > in[2].w) ||
          (in[0].y < -in[0].w && in[1].y < -in[1].w && in[2].y < -in[2].w) ||
          (in[0].y > in[0].w && in[1].y > in[1].w && in[2].y > in[2].w))
        continue;
      V4 poly[4];
      int n = 0;
      for (int i = 0; i < 3; ++i) {
        const V4& p = in[i];
        const V4& q = in[(i + 1) % 3];
        const bool pin = p.w > kNear, qin = q.w > kNear;
        if (pin) poly[n++] = p;
        if (pin != qin) {
          const float u = (kNear - p.w) / (q.w - p.w);
          poly[n++] = {p.x + (q.x - p.x) * u, p.y + (q.y - p.y) * u, p.z + (q.z - p.z) * u, kNear};
        }
      }
      if (n < 3) continue;
      any = true;
      const ScreenV s0 = screen(poly[0]), s1 = screen(poly[1]), s2 = screen(poly[2]);
      if (PreciseTri(b, s0, s1, s2, bias, fast)) return 1;
      if (n == 4 && PreciseTri(b, s0, s2, screen(poly[3]), bias, fast)) return 1;
    }
  }
  return any ? 2 : 0;
}
}  // namespace

// One Umbra test (command 131's vertices, float3 big-endian at verts_va; its triangles, 3 x u32 at idx_va). Codes as
// CpuTest: 0 no buffer this frame, 1 visible, 2 occluded, 3 near plane, 4 off the screen. alt: the same against the
// other winding's buffer (dual), else 0.
extern "C" uint8_t Me2RasterTest(const uint8_t* base, uint32_t verts_va, uint32_t count, uint32_t idx_va,
                                 uint32_t ntris, uint64_t frame, uint8_t* alt) {
  *alt = 0;
  if (g_ready_frame != frame || !g_cur) return 0;
  const auto t0 = std::chrono::steady_clock::now();
  ++g_st.tests;
  static const float bias = float(REXCVAR_GET(me2_occlusion_raster_bias));
  static const bool dual = REXCVAR_GET(me2_occlusion_raster_dual);
  const Buffer& b0 = *g_cur;
  float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f, max_iz = 0.0f;
  uint8_t code = 0;
  if (count == 0 || count > 4096) code = 1;
  for (uint32_t i = 0; !code && i < count; ++i) {
    const uint32_t a = verts_va + 12 * i;
    const V4 p = Xform(b0.vp, LF(base, a), LF(base, a + 4), LF(base, a + 8));
    if (p.w <= kNear) {
      code = 3;
      break;
    }
    const float iw = 1.0f / p.w;
    const float sx = (p.x * iw * 0.5f + 0.5f) * float(b0.w), sy = (0.5f - p.y * iw * 0.5f) * float(b0.h);
    mnx = std::min(mnx, sx);
    mxx = std::max(mxx, sx);
    mny = std::min(mny, sy);
    mxy = std::max(mxy, sy);
    max_iz = std::max(max_iz, iw);
  }
  int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
  if (!code) {
    x0 = std::max(0, int(std::floor(mnx)));
    x1 = std::min(b0.w - 1, int(std::ceil(mxx)) - 1);
    y0 = std::max(0, int(std::floor(mny)));
    y1 = std::min(b0.h - 1, int(std::ceil(mxy)) - 1);
    if (x1 < x0 || y1 < y0) code = 4;
  }
  static const bool fast_tests = REXCVAR_GET(me2_occlusion_raster_fast_tests);
  static const bool verify = REXCVAR_GET(me2_occlusion_raster_test_verify);
  auto test = [&](const Buffer& b, bool fast) -> uint8_t {
    const float limit = max_iz * (1.0f + bias);
    bool all = true;
    for (int ty = y0 / 8; all && ty <= y1 / 8; ++ty)
      for (int tx = x0 / 8; tx <= x1 / 8; ++tx)
        if (!(b.tile[size_t(ty) * b.tw + tx] > limit)) {
          all = false;
          break;
        }
    if (all) return 2;
    // Fast: the per-pixel test decides the rest (this scan found 'occluded' for only 2-5 % of the boxes it read).
    if (fast) return 1;
    for (int y = y0; y <= y1; ++y) {
      const float* row = &b.pix[size_t(y) * b.w];
      for (int x = x0; x <= x1; ++x)
        if (!(row[x] > limit)) return 1;
    }
    return 2;
  };
  // The quick rectangle test decides most boxes (occluded at the box's nearest corner everywhere); a 'visible' or a box
  // at the near plane gets the per-pixel test with the box's own triangles.
  auto decide = [&](const Buffer& b, uint8_t quick, bool count_stats, bool fast) -> uint8_t {
    uint8_t r = quick;
    if (!r) r = test(b, fast);
    if (r == 1 || r == 3) {
      const uint8_t p = PreciseTest(base, b, verts_va, count, idx_va, ntris, bias, fast);
      if (count_stats) {
        ++g_st.precise;
        if (!p) ++g_st.precise_none;
        if (p == 2) ++g_st.precise_occ;
        if (p == 2 && r == 3) ++g_st.precise_near;
      }
      if (p) r = p;
    }
    return r;
  };
  const uint8_t quick = code;  // 0 = on screen, in front of the near plane: the rectangle test runs
  code = decide(b0, quick, true, fast_tests);
  if (dual) *alt = decide(g_buf[1], quick, false, fast_tests);
  if (verify && fast_tests) {
    // The previous test on the same box (its time counts too: a diagnostic). 2 = occluded.
    const uint64_t tiles_before = g_st.tile_accepts;
    const uint8_t slow = decide(b0, quick, false, false);
    g_st.tile_accepts = tiles_before;
    ++g_st.verify;
    if ((code == 2) != (slow == 2)) ++(code == 2 ? g_st.verify_unsafe : g_st.verify_more);
  }
  t_trace_token = 0;
  if (code == 2 && !b0.pix_id.empty())
    t_trace_token = quick == 0 ? TraceOccluded(base, verts_va, count, max_iz, x0, x1, y0, y1, false)
                               : TraceOccluded(base, verts_va, count, 0.0f, 0, -1, 0, -1, true);
  g_st.test_ns += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  return code;
}

// Native visibility (me2_native_vis, 2026-11-05): an axis-aligned world box [mn, mx] against this frame's buffer.
// Codes: 0 no buffer this frame, 1 visible, 2 occluded, 3 crosses the near plane (visible), 5 outside the frustum.
// Conservative like Me2RasterTest's rectangle path: occluded only when every pixel under the box's screen rectangle
// holds an occluder nearer than the box's nearest corner.
extern "C" uint8_t Me2RasterTestBox(const float* mn, const float* mx, uint64_t frame) {
  if (g_ready_frame != frame || !g_cur) return 0;
  static const float bias = float(REXCVAR_GET(me2_occlusion_raster_bias));
  const Buffer& b = *g_cur;
  // frustum planes of this buffer, cached per buffer and frame
  thread_local const Buffer* pl_buf = nullptr;
  thread_local uint64_t pl_frame = ~uint64_t(0);
  thread_local BoxPlanes pl;
  if (pl_buf != &b || pl_frame != frame) {
    MakeBoxPlanes(b.vp, pl);
    pl_buf = &b;
    pl_frame = frame;
  }
  return BoxCode(b, pl, mn, mx, bias);
}

// The worker's box pass for the buffer this frame's tests use, when it is ready (me2_native_vis_worker).
extern "C" bool Me2RasterBoxPass(uint64_t frame, const uint8_t** boxes, const uint32_t** wraps, const uint8_t** codes,
                                 uint32_t* count, uint64_t* worker_ns) {
  if (g_ready_frame != frame || !g_cur || g_cur_abuf < 0 || g_cur != &g_abuf[g_cur_abuf]) return false;
  BoxPass& bp = g_boxpass[g_cur_abuf];
  if (!bp.ready.load(std::memory_order_acquire)) return false;
  *boxes = bp.boxes.data();
  *wraps = bp.wraps.data();
  *codes = bp.codes.data();
  *count = bp.count;
  *worker_ns = bp.ns;
  return true;
}

// The trace token of the last Me2RasterTest on this thread (0 = none / not occluded / trace off).
extern "C" uint32_t Me2RasterTakeToken() {
  const uint32_t t = t_trace_token;
  t_trace_token = 0;
  return t;
}

// me2_occlusion.cpp LatentCollect: the GPU counted 'count' samples for a test this module called occluded.
extern "C" void Me2RasterTraceFalse(uint32_t token, uint32_t count, uint64_t now) {
  if (!token || token > g_trace.size()) return;
  TraceRec& r = g_trace[token - 1];
  for (int k = 0; k < r.nocc; ++k) {
    TraceOcc& t = r.occ[k];
    auto it = g_by_mesh.find(t.mesh);
    if (t.psi && it != g_by_mesh.end() && g_occ[it->second].psi == t.psi && Readable(t.psi, 0xB0))
      t.drawn = LF(g_base, t.psi + 0xAC) > t.lrt ? 1 : 0;
  }
  ++g_false_total;
  ++g_false_top_drawn[r.nocc ? std::min<uint8_t>(r.occ[0].drawn, 2) : 2];
  for (int k = 0; k < r.nocc; ++k) {
    FalseSum& f = g_false_by_mesh[r.occ[k].mesh];
    if (!f.n) {
      f.occ = r.occ[k];
      f.frame = r.frame;
    }
    ++f.n;
  }
  const uint64_t window = now / 300;
  if (window != g_false_window) {
    g_false_window = window;
    g_false_logged = 0;
  }
  if (g_false_logged++ >= 40) return;
  std::string occ;
  for (int k = 0; k < r.nocc; ++k) {
    const TraceOcc& t = r.occ[k];
    occ += fmt::format(
        " | occ#{} mesh {:08X} id {} list {} cull {} {} tris, {} px (farthest {:.0f} units), box [{:.0f},{:.0f},{:.0f}]-"
        "[{:.0f},{:.0f},{:.0f}], LOD d2 {:.3g} in [{:.3g},{:.3g}), flags {:08X}, cached {} frames before, {}",
        k, t.mesh, t.mesh_id, t.list, t.cull, t.tris, t.px, t.iz_far > 0.0f ? 1.0f / t.iz_far : 0.0f, t.lo[0],
        t.lo[1], t.lo[2], t.hi[0], t.hi[1], t.hi[2], t.d2, t.min_d2, t.max_d2, t.flags, r.frame - t.first_seen,
        t.drawn == 1 ? "DRAWN by the game" : t.drawn == 0 ? "NOT DRAWN by the game" : "drawn ?");
  }
  REXLOG_INFO("[raster-false] frame {} (collected {}): GPU {} samples; box [{:.0f},{:.0f},{:.0f}]-[{:.0f},{:.0f},"
              "{:.0f}] nearest {:.0f} units, camera ({:.0f},{:.0f},{:.0f}), pixels x {}..{} y {}..{} ({}){}{}",
              r.frame, now, count, r.lo[0], r.lo[1], r.lo[2], r.hi[0], r.hi[1], r.hi[2],
              r.box_iz > 0.0f ? 1.0f / r.box_iz : 0.0f, r.cam[0], r.cam[1], r.cam[2], r.x0, r.x1, r.y0, r.y1, r.px,
              r.near_plane ? ", at the near plane (per-pixel test)" : "", occ);
}

// Every ~10 s with the other occlusion reports (me2_occlusion.cpp CpuReport).
extern "C" void Me2RasterReport() {
  if (g_false_total) {
    std::vector<std::pair<uint32_t, FalseSum>> v(g_false_by_mesh.begin(), g_false_by_mesh.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
    std::string top;
    for (size_t k = 0; k < v.size() && k < 8; ++k) {
      const TraceOcc& t = v[k].second.occ;
      top += fmt::format("; mesh {:08X} x{} (list {}, {} tris, box [{:.0f},{:.0f},{:.0f}]-[{:.0f},{:.0f},{:.0f}], "
                         "cached {} frames before its first)", t.mesh, v[k].second.n, t.list, t.tris, t.lo[0],
                         t.lo[1], t.lo[2], t.hi[0], t.hi[1], t.hi[2], v[k].second.frame - t.first_seen);
    }
    REXLOG_INFO("[occlusion] raster false occluded (CPU occluded, GPU visible): {} tests (main occluder drawn by the "
                "game: {}, NOT drawn: {}, unknown: {}), {} occluder meshes involved{}", g_false_total,
                g_false_top_drawn[1], g_false_top_drawn[0], g_false_top_drawn[2], g_false_by_mesh.size(), top);
    g_false_total = 0;
    g_false_top_drawn[0] = g_false_top_drawn[1] = g_false_top_drawn[2] = 0;
    g_false_by_mesh.clear();
  }
  Stats s = g_st;
  g_st = Stats{};
  const double f = s.frames ? double(s.frames) : 1.0;
  uint64_t usable = 0, tris = 0;
  std::lock_guard<std::mutex> cache(g_cache_mutex);
  for (const Occluder& o : g_occ)
    if (o.mesh && o.bad == kOk) {
      ++usable;
      tris += o.tri.size() / 3;
    }
  std::string bad;
  for (int i = 1; i < kBadCount; ++i)
    if (s.bad[i]) bad += fmt::format(", {} {}", kBadName[i], s.bad[i]);
  REXLOG_INFO("[occlusion] raster {} frames: walk {:.0f} us ({} of the walks without per-mesh checks), raster {:.0f} us, tests {:.0f} us a frame ({} tests); "
              "{:.0f} list elements a frame, {} occluders cached ({} usable, {} triangles, {:.1f} MB), {} new, {} "
              "changed, {} dropped; new meshes not usable:{}; a frame: {:.0f} occluders drawn, {:.0f} triangles "
              "({:.0f} back faces culled, {:.0f} near-clipped), left out: {:.0f} LOD, {:.0f} off screen, {:.0f} small, "
              "{:.0f} dense, {:.0f} over budget, {:.0f} not rendered recently by the game ({:.0f} off screen by the centre test; lean limits in {} frames); {:.1f} % of the pixels covered; per-pixel box tests {} ({} occluded, {} of them at "
              "the near plane; {} without usable triangles); fast tests: {} decided 'visible' by a tile, {} boxes by their "
              "front faces; verify {} (fast occluded / slow visible {}, fast visible / slow occluded {}); unreadable "
              "lists {}",
              s.frames, double(s.walk_ns) / 1000.0 / f, s.walk_skips, double(s.raster_ns) / 1000.0 / f,
              double(s.test_ns) / 1000.0 / f, s.tests, double(s.elements) / f, g_by_mesh.size(), usable, tris,
              double(g_bytes) / 1048576.0, s.new_meshes, s.changed, s.dropped, bad.empty() ? " none" : bad.c_str(),
              double(s.drawn_occ) / f, double(s.drawn_tris) / f, double(s.culled_tris) / f,
              double(s.near_clipped) / f, double(s.lod_out) / f, double(s.off) / f, double(s.small) / f,
              double(s.dense) / f, double(s.budget_cut) / f, double(s.not_recent) / f, double(s.off_fast) / f,
              s.lean_frames, g_buf[0].w ? 100.0 * double(s.covered_px) / f / double(g_buf[0].w * g_buf[0].h) : 0.0, s.precise,
              s.precise_occ, s.precise_near, s.precise_none, s.tile_accepts, s.front_only, s.verify, s.verify_unsafe,
              s.verify_more, s.lists_bad);
  if (g_async_on) {
    AsyncStats a;
    {
      std::lock_guard<std::mutex> lk(g_job_mutex);
      a = g_ast;
      g_ast = AsyncStats{};
    }
    REXLOG_INFO("[occlusion] raster async: {} jobs posted by the game thread, {} taken by the render thread ({} after "
                "waiting, {:.0f} us a frame waited), {} frames drawn in place ({:.0f} us a frame of them): no job {}, "
                "its camera differed {}; {} stale jobs dropped, {} posts dropped (all busy), {} walks skipped (worker "
                "drawing); worker {:.0f} us a frame; {:.0f} occluders a frame left out as already hidden; {} jobs took the "
                "previous buffer (camera and occluders unchanged, me2_occlusion_raster_reuse)",
                a.posted, a.taken, a.waited, double(a.wait_ns) / 1000.0 / f, s.inline_frames,
                s.inline_frames ? double(s.inline_ns) / 1000.0 / double(s.inline_frames) : 0.0, a.not_found,
                a.vp_differs, a.stale_dropped, a.dropped_busy, a.walk_skipped, double(a.worker_ns) / 1000.0 / f,
                double(s.hidden_occ) / f, a.reused);
  } else if (s.hidden_occ) {
    REXLOG_INFO("[occlusion] raster: {:.0f} occluders a frame left out as already hidden", double(s.hidden_occ) / f);
  }
  if (s.vis_checked || s.trace_ns)
    REXLOG_INFO("[occlusion] raster trace: {:.0f} us a frame of the tests' time is the trace itself; occluders drawn "
                "here that the game rendered too (LastRenderTime): {:.1f} % of {} ({} unused)",
                double(s.trace_ns) / 1000.0 / f, s.vis_checked ? 100.0 * double(s.vis_set) / double(s.vis_checked) : 0.0,
                s.vis_checked, s.vis_bad);
}
