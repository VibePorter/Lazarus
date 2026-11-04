/**
 * @file    gpu/d3d/hooks/draw.cpp
 * @brief   Draw, clear and resolve entry points, and the shader setters.
 *
 * The draws replace N&B's D3D entirely: the originals would flush the device's
 * dirty state into GPU packets, including binding shaders by reading their
 * microcode, which renderer-owned shader objects do not carry. Clear, Resolve
 * and tiling queue their host work and do the device work of N&B's D3D
 * natively (d3d_native.h): dirty masks, the tiling state, fences on the
 * destination.
 *
 * The host work runs on the render thread (queued.h); a device recording a
 * command buffer records its draws, clears and resolves (command_buffers.h)
 * instead. Neither takes State().mutex here.
 */
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/backend/video.h"

using namespace renut::gpu;

// me2-nx-deko (2026-11-01, run 13: UE3's render thread costs ~20 us a draw and heavy scenes have 1,100-1,300 draws a
// frame; per-light additive passes looked like the largest group): each draw counted by its UE3 caller (host return
// addresses as image offsets: the generated functions are sub_XXXXXXXX in the ELF) and by what it does: depth only
// (no colour channel), opaque (src ONE, dest ZERO), additive (dest ONE: UE3's per-light passes), other blending; plus
// whether it writes depth. "[draw-callers]" every 10 s: the top 24 (caller chain, kind) with draws a frame.
REXCVAR_DEFINE_BOOL(me2_diag_draw_callers, false, "ME2/Diagnostics",
                    "Count draws by UE3 caller and kind (depth / opaque / additive / blend); '[draw-callers]' every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#if defined(__SWITCH__)
extern "C" char _start;  // the image's base (rex_profile.log "image+0x...")
#endif
extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap

// A float argument also takes the GPR (or stack) slot of its position, which
// the marshaling does not skip: the u32 after each double in the hooks'
// parameters stands in for it.

namespace {

constexpr u32 kE_OUTOFMEMORY = 0x8007000Eu;
// D3DVIEWPORT9 {0, 0, 0xFFFF, 0xFFFF, 0.0, 1.0} in N&B's data (unk_820018D4):
// the whole surface.
constexpr u32 kFullViewport = 0x82248230;  // me2-nx-deko: ME2 BeginTiling `lis r11,-32220; addi r30,r11,30240`
// D3DRS_HIZWRITEENABLE's index in the render state table.
constexpr u32 kRenderStateHiZWriteEnable = 97;

// Guest memory Begin*Vertices hands out; the game writes one draw's vertices
// into it before its next D3D call. One per device, as devices record on
// different threads.
constexpr u32 kVerticesScratchSize = 4u << 20;
std::mutex g_vertices_scratch_mutex;  // devices call from their own threads
std::unordered_map<u32, u32> g_vertices_scratch;

u32 VerticesScratch(u32 device) {
  std::lock_guard lock(g_vertices_scratch_mutex);
  u32 &scratch = g_vertices_scratch[device];
  if (!scratch)
    scratch = records::AllocPhysicalBlock(kVerticesScratchSize);
  return scratch;
}

bool Active() {
  auto &s = State();
  return s.host_ready && !s.shutting_down.load(std::memory_order_acquire);
}

void StoreU32(u32 va, u32 value) {
  if (auto *p = GuestPtr<be_u32>(va))
    *p = value;
}

u32 LoadU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

bool ReadRect(u32 va, ClearRect &rect) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  rect = {i32(u32(p[0])), i32(u32(p[1])), i32(u32(p[2])), i32(u32(p[3]))};
  return true;
}

bool ReadVector4(u32 va, float out[4]) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  for (int i = 0; i < 4; ++i) {
    const u32 bits = p[i];
    std::memcpy(&out[i], &bits, 4);
  }
  return true;
}

// me2_diag_draw_callers (UE3's render thread draws; the counters are that thread's only).
struct DrawCallerKey {
  uint64_t frames[5];
  uint32_t kind;
  bool operator==(const DrawCallerKey&) const = default;
};
struct DrawCallerKeyHash {
  size_t operator()(const DrawCallerKey& k) const {
    uint64_t h = k.kind * 0x9E3779B97F4A7C15ull;
    for (uint64_t f : k.frames) h = (h ^ f) * 0x100000001B3ull;
    return size_t(h);
  }
};
thread_local std::unordered_map<DrawCallerKey, uint64_t, DrawCallerKeyHash> t_draw_callers;
thread_local uint64_t t_draw_kinds[8] = {};
thread_local std::chrono::steady_clock::time_point t_draw_callers_since{};
thread_local uint64_t t_draw_callers_frame0 = 0;

__attribute__((noinline)) void NoteDrawCaller(u32 device) {
  const u32 bc = LoadU32(device + native::off::BlendControl0);
  const u32 dc = LoadU32(device + native::off::DepthControl);
  const u32 mask = LoadU32(device + native::off::ColorMask);
  const u32 src = bc & 31, dest = (bc >> 8) & 31;
  uint32_t kind = (mask & 0xF) == 0 ? 0 : (src == 1 && dest == 0) ? 1 : dest == 1 ? 2 : 3;
  if (dc & 4) kind |= 4;  // writes depth
  ++t_draw_kinds[kind];
  DrawCallerKey key{};
  key.kind = kind;
#if defined(__SWITCH__) && defined(__aarch64__)
  const uintptr_t base = reinterpret_cast<uintptr_t>(&_start);
  uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  // [0] the hook, [1] the host-to-guest wrapper, then the guest D3D call's callers (sub_826913A0, UE3's caller...)
  uint64_t all[8] = {};
  for (int i = 0; i < 8 && fp; ++i) {
    const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(fp);
    const uintptr_t next = frame[0], lr = frame[1];
    if (!lr || lr < base) break;
    all[i] = lr - base;
    if (next <= fp || next - fp > 0x10000 || (next & 7)) break;
    fp = next;
  }
  for (int i = 0; i < 5; ++i) key.frames[i] = all[i + 2];
#endif
  ++t_draw_callers[key];
  const auto now = std::chrono::steady_clock::now();
  if (t_draw_callers_since == std::chrono::steady_clock::time_point{}) {
    t_draw_callers_since = now;
    t_draw_callers_frame0 = g_sa2_game_frames.load(std::memory_order_relaxed);
  }
  if (now - t_draw_callers_since < std::chrono::seconds(10)) return;
  const uint64_t frames = std::max<uint64_t>(1, g_sa2_game_frames.load(std::memory_order_relaxed) - t_draw_callers_frame0);
  std::vector<std::pair<uint64_t, const DrawCallerKey*>> top;
  for (const auto& [k, v] : t_draw_callers) top.push_back({v, &k});
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  static const char* const kKind[4] = {"depth", "opaque", "additive", "blend"};
  uint64_t total = 0;
  for (uint64_t n : t_draw_kinds) total += n;
  std::string text = fmt::format("[draw-callers] {} frames, {:.0f} draws a frame: depth {:.0f} (+z {:.0f}), opaque {:.0f} "
                                 "(+z {:.0f}), additive {:.0f} (+z {:.0f}), blend {:.0f} (+z {:.0f}); top callers "
                                 "(image offsets of the D3D call's callers, innermost first):",
                                 frames, double(total) / double(frames), double(t_draw_kinds[0]) / double(frames),
                                 double(t_draw_kinds[4]) / double(frames), double(t_draw_kinds[1]) / double(frames),
                                 double(t_draw_kinds[5]) / double(frames), double(t_draw_kinds[2]) / double(frames),
                                 double(t_draw_kinds[6]) / double(frames), double(t_draw_kinds[3]) / double(frames),
                                 double(t_draw_kinds[7]) / double(frames));
  for (size_t i = 0; i < top.size() && i < 24; ++i) {
    const DrawCallerKey& k = *top[i].second;
    text += fmt::format("\n    {:7.1f} a frame  {:<8}{}", double(top[i].first) / double(frames), kKind[k.kind & 3],
                        (k.kind & 4) ? "+z" : "  ");
    for (uint64_t f : k.frames)
      if (f) text += fmt::format(" +{:x}", f);
  }
  RGPU_INFO("{}", text);
  t_draw_callers.clear();
  for (uint64_t& n : t_draw_kinds) n = 0;
  t_draw_callers_since = now;
  t_draw_callers_frame0 = g_sa2_game_frames.load(std::memory_order_relaxed);
}

bool DrawCallersOn() {
  static const bool on = REXCVAR_GET(me2_diag_draw_callers);
  return on;
}

}  // namespace (re-opened below)
// me2-nx-deko (2026-11-01): every draw, for the frame diagnostics' "[stream-wait]" (app/me2_src/me2_frame_diag.cpp:
// frames with almost no draws while the game waits for level streaming).
std::atomic<uint64_t> g_me2_draws_total{0};
namespace {

// void D3DDevice_DrawVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartVertex,
//                             UINT VertexCount) [0x8222CE00]
void D3DDevice_DrawVertices_hook(u32 device, u32 primitive, u32 start_vertex, u32 vertex_count) {
  if (!Active())
    return;
  g_me2_draws_total.fetch_add(1, std::memory_order_relaxed);
  if (DrawCallersOn())
    NoteDrawCaller(device);
  DrawCall call;
  call.primitive = primitive;
  call.start_vertex = start_vertex;
  call.count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex,
//                                    UINT StartIndex, UINT IndexCount) [0x82656230]
void D3DDevice_DrawIndexedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                        u32 start_index, u32 index_count) {
  if (!Active())
    return;
  g_me2_draws_total.fetch_add(1, std::memory_order_relaxed);
  if (DrawCallersOn())
    NoteDrawCaller(device);
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedTessellatedVertices(D3DDevice*, D3DTESSPRIMITIVETYPE,
//     INT BaseVertexIndex, UINT StartIndex, UINT IndexCount) [0x82230B20]
// Replaced like the other draws: the original's state flush binds the shader
// from its microcode, and bugchecks (0xF5) on a tessellation shader without
// the tessellation state the original SetVertexShader keeps. The draw is
// tessellated on the host (DrawCall::tessellation).
void D3DDevice_DrawIndexedTessellatedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                                   u32 start_index, u32 index_count) {
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.tessellation = true;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               const void *pVertexStreamZeroData, UINT Stride) [0x8222E808]
void D3DDevice_DrawVerticesUP_hook(u32 device, u32 primitive, u32 vertex_count, u32 data,
                                   u32 stride) {
  if (!Active() || !data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(data);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_count * stride, 0);
  else
    QueueDraw(device, call, vertex_count * stride, 0);
}

// void D3DDevice_DrawIndexedVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT MinVertexIndex,
//     UINT NumVertices, UINT IndexCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
//     const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) [0x82656190]
void D3DDevice_DrawIndexedVerticesUP_hook(u32 device, u32 primitive, u32 min_vertex,
                                          u32 num_vertices, u32 index_count, u32 index_data,
                                          u32 index_format, u32 vertex_data, u32 stride) {
  if (!Active() || !index_data || !vertex_data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.count = index_count;
  call.up_indices = GuestPtr<const u8>(index_data);
  call.up_indices_32 = (index_format & 4) != 0;  // D3DFMT_INDEX32
  call.up_vertices = GuestPtr<const u8>(vertex_data);
  call.up_stride = stride;
  call.up_vertex_count = min_vertex + num_vertices;
  const u32 vertex_bytes = call.up_vertex_count * stride;
  const u32 index_bytes = index_count * (call.up_indices_32 ? 4 : 2);
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_bytes, index_bytes);
  else
    QueueDraw(device, call, vertex_bytes, index_bytes);
}

// void *D3DDevice_BeginVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               UINT VertexStreamZeroStride) [0x8222D2B0]
// Callers write the vertices, then end inline by restoring m_pRing from the
// device, so the draw's state is captured now and its vertices copied at the
// device's next hook (FlushDeferredVertices).
u32 D3DDevice_BeginVertices_hook(u32 device, u32 primitive, u32 vertex_count, u32 stride) {
  const u64 bytes = u64(vertex_count) * stride;
  if (!bytes || bytes > kVerticesScratchSize)
    return 0;
  // The inline end: m_pRing = [device + kDeviceOffsetVerticesRingEnd].
  StoreU32(device + d3d::kDeviceOffsetVerticesRingEnd,
           LoadU32(device + u32(offsetof(d3d::Device, m_pRing))));

  const u32 scratch = VerticesScratch(device);
  if (!scratch)
    return 0;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(scratch);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  // Either way the device's previous Begin*Vertices data leaves the scratch
  // before the game writes this draw's over it.
  if (IsRecordingCommandBuffer(device))
    RecordDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else if (Active())
    QueueDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else
    FlushDeferredVertices(device);
  return scratch;
}

// A clear: the renderer's, recorded or queued, then N&B's device work.
// whole_target: BeginTiling's, of one tile's worth of EDRAM, which on the
// host is the whole tiled target.
void Clear(u32 device, u32 flags, const ClearRect *rect, const float color[4], float z,
           u32 stencil, bool whole_target) {
  const ClearRect *host_rect = whole_target ? nullptr : rect;
  if (IsRecordingCommandBuffer(device))
    RecordClear(device, flags, host_rect, color, z, stencil);
  else if (Active())
    QueueClear(device, flags, host_rect, color, z, stencil);
  const native::Rect native_rect = rect ? native::Rect{rect->x1, rect->y1, rect->x2, rect->y2}
                                        : native::Rect{};
  native::Clear(native::DeviceAt(device), flags, rect ? &native_rect : nullptr);
}

// A resolve: the renderer's copy (and clear), then N&B's device work.
void Resolve(u32 device, u32 flags, const ClearRect *rect, u32 dest_texture, i32 dest_x,
             i32 dest_y, u32 dest_level, u32 dest_slice, u32 color_va, float clear_z,
             u32 clear_stencil, u32 parameters) {
  float color[4] = {0, 0, 0, 0};
  const bool has_color = color_va && ReadVector4(color_va, color);
  if (IsRecordingCommandBuffer(device)) {
    RecordResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                  has_color ? color : nullptr, clear_z, clear_stencil);
  } else if (Active()) {
    QueueResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                 has_color ? color : nullptr, clear_z, clear_stencil);
  }
  native::Resolve(native::DeviceAt(device), flags, dest_texture, clear_z, clear_stencil,
                  parameters);
}

// void D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT *pRect,
//                       const D3DVECTOR4 *pColor, float Z, DWORD Stencil) [0x822811A0]
// Z takes r7's slot, so Stencil arrives in r8.
void D3DDevice_ClearF_hook(u32 device, u32 flags, u32 rect_va, u32 color_va, double z,
                           u32 /*z_slot*/, u32 stencil) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  float color[4] = {0, 0, 0, 0};  // unk_82001160 without one
  if (color_va)
    ReadVector4(color_va, color);
  Clear(device, flags, has_rect ? &rect : nullptr, color, float(z), stencil, false);
}

// void D3DDevice_Clear(D3DDevice*, DWORD Count, const D3DRECT *pRects, DWORD Flags,
//     D3DCOLOR Color, float Z, DWORD Stencil) [0x822812C8]: ClearF with the
// color as a vector (XMLoadColor: each byte as 1 + b / 2^23, scaled by
// 2^23 / 255 and offset), per rect. Z takes r8's slot.
void D3DDevice_Clear_hook(u32 device, u32 count, u32 rects_va, u32 flags, u32 color_argb,
                          double z, u32 /*z_slot*/, u32 stencil) {
  const float scale = std::bit_cast<float>(0x47008081u);  // aG, unk_82001150
  auto channel = [scale](u32 byte) {
    return std::fmaf(std::bit_cast<float>(0x3F800000u | (byte & 0xFF)), scale, -scale);
  };
  const float color[4] = {channel(color_argb >> 16), channel(color_argb >> 8),
                          channel(color_argb), channel(color_argb >> 24)};
  if (!rects_va) {
    Clear(device, flags, nullptr, color, float(z), stencil, false);
    return;
  }
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (ReadRect(rects_va + i * 16, rect))
      Clear(device, flags, &rect, color, float(z), stencil, false);
  }
}

// void D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT *pSourceRect,
//     D3DBaseTexture *pDestTexture, const D3DPOINT *pDestPoint, UINT DestLevel,
//     UINT DestSliceOrFace, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil, const D3DRESOLVE_PARAMETERS *pParameters) [0x8222D6C8]
// ClearZ is in f1 and takes the first stack slot; ClearStencil and pParameters
// are in the two after it.
void D3DDevice_Resolve_hook(u32 device, u32 flags, u32 rect_va, u32 dest_texture, u32 point_va,
                            u32 dest_level, u32 dest_slice, u32 color_va, double clear_z,
                            u32 /*z_slot*/, u32 clear_stencil, u32 parameters) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  i32 dest_x = 0, dest_y = 0;
  if (const auto *p = GuestPtr<const be_u32>(point_va)) {
    dest_x = i32(u32(p[0]));
    dest_y = i32(u32(p[1]));
  }
  Resolve(device, flags, has_rect ? &rect : nullptr, dest_texture, dest_x, dest_y, dest_level,
          dest_slice, color_va, float(clear_z), clear_stencil, parameters);
}

// void D3DDevice_BeginTiling(D3DDevice*, DWORD Flags, DWORD Count,
//     const D3DRECT *pTileRects, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil) [0x822788A0]
// The surfaces bound from here to EndTiling are backed by targets covering
// every tile (SetTilingExtentLocked). N&B's BeginTiling clears with Z 0.0 and
// stencil 0 (its ClearZ and ClearStencil go unused), unless the device records
// a one-pass Z pass (Flags3 0x20); its ring segment for the tiles is left out.
void D3DDevice_BeginTiling_hook(u32 device, u32 /*flags*/, u32 count, u32 rects_va, u32 color_va,
                                double /*clear_z*/, u32 /*z_slot*/, u32 /*clear_stencil*/) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return;
  std::vector<ClearRect> tiles;
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (!ReadRect(rects_va + i * 16, rect))
      break;
    tiles.push_back(rect);
  }
  count = u32(tiles.size());
  if (!IsRecordingCommandBuffer(device)) {
    FlushDeferredVertices(device);
    u32 width = 0, height = 0;
    for (const ClearRect &rect : tiles) {
      width = std::max(width, u32(rect.x2));
      height = std::max(height, u32(rect.y2));
    }
    render_thread::Queue([tiles, width, height] {
      SetTilingExtentLocked(width, height);
      SetTilesLocked(tiles.data(), u32(tiles.size()));
    });
  }

  d.SetW(native::off::TileCount, count);
  u32 extent_x = 0, extent_y = 0;
  for (u32 i = 0; i < count; ++i) {
    const ClearRect &rect = tiles[i];
    extent_x = std::max(extent_x, u32(rect.x2));
    extent_y = std::max(extent_y, u32(rect.y2));
    d.SetW(native::off::TileRects + i * 16, u32(rect.x1));
    d.SetW(native::off::TileRects + i * 16 + 4, u32(rect.y1));
    d.SetW(native::off::TileRects + i * 16 + 8, u32(rect.x2));
    d.SetW(native::off::TileRects + i * 16 + 12, u32(rect.y2));
    d.SetW(native::off::TileOrigins + i * 8, u32(rect.x1) & ~31u);
    d.SetW(native::off::TileOrigins + i * 8 + 4, u32(rect.y1) & ~31u);
  }
  d.SetW(native::off::TilingExtent, extent_x);
  d.SetW(native::off::TilingExtent + 4, extent_y);
  d.SetW(0x338C/*NB:0x3384*/, 0);

  if (!(d.B(native::off::Flags3) & 0x20)) {
    // The largest tile, from its origin: its clear covers a tile of EDRAM.
    u32 width = 0, height = 0;
    for (u32 i = 0; i < count; ++i) {
      width = std::max(width, u32(tiles[i].x2) - d.W(native::off::TileOrigins + i * 8));
      height = std::max(height, u32(tiles[i].y2) - d.W(native::off::TileOrigins + i * 8 + 4));
    }
    // The viewport (x, y, width, height, min z, max z) and scissor rect, for
    // after the clear.
    float viewport[6];
    for (u32 i = 0; i < 6; ++i)
      viewport[i] = d.F(0x3170/*NB:0x3168*/ + i * 4);
    i32 scissor[4];
    for (u32 i = 0; i < 4; ++i)
      scissor[i] = i32(d.W(native::off::ScissorRect + i * 4));
    native::SetViewport(d, kFullViewport);
    native::SetScissorRect(d, 0, 0, 0xFFFF, 0xFFFF);  // dword_82001190
    const ClearRect rect{0, 0, i32(width), i32(height)};
    float color[4] = {0, 0, 0, 0};
    if (color_va)
      ReadVector4(color_va, color);
    Clear(device, 0x3F, &rect, color, 0.0f, 0, true);
    native::SetViewport(d, viewport[0], viewport[1], viewport[2], viewport[3], viewport[4],
                        viewport[5]);
    native::SetScissorRect(d, scissor[0], scissor[1], scissor[2], scissor[3]);
  }
  if (const auto *color = color_va ? GuestPtr<const u8>(color_va) : nullptr)
    std::memcpy(d.bytes + 0x3390, color, 16);
  else
    std::memset(d.bytes + 0x3390, 0, 16);
  d.SetF(0x33A0, 0.0f);  // flt_82000D18
  d.SetW(0x33A4, 0);

  const u32 target = d.W(native::off::RenderTargets);
  d.SetW(0x31BC/*NB:0x31B4*/, target ? target : d.W(native::off::DepthStencil));
  for (u32 i = 0; i < 5; ++i)  // the 4 targets, then depth
    d.SetW(native::off::TilingSurfaces + i * 4, d.W(native::off::RenderTargets + i * 4));
  d.SetW(0x31B8/*NB:0x31B0*/, 2);
  d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x20);
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // The tiles' commands go into a ring segment of their own from a KickOff.
  native::KickOff(device);
  native::SetViewport(d, kFullViewport);
  native::ResetAllState(d);
}

// HRESULT D3DDevice_EndTiling(D3DDevice*, DWORD ResolveFlags,
//     const D3DRECT *pResolveRects, D3DBaseTexture *pDestTexture,
//     const D3DVECTOR4 *pClearColor, float ClearZ, DWORD ClearStencil,
//     const D3DRESOLVE_PARAMETERS *pParameters) [0x82212328]
// N&B's EndTiling resolves each tile's rect (ResolveFlags 0x300: color and
// depth cleared) with BeginTiling's clear values, the last one with
// pClearColor, Z 1.0 and stencil 0; ResolveFlags, pResolveRects, ClearZ,
// ClearStencil and pParameters go unused. The host resolve reads each rect
// from the full-size target. Left out: the tiles' ring segment, D3D's
// WorkerDpc callback that replays it per tile, and its fence's packets.
u32 D3DDevice_EndTiling_hook(u32 device, u32 /*resolve_flags*/, u32 /*rects_va*/,
                             u32 dest_texture, u32 color_va, double /*clear_z*/, u32 /*z_slot*/,
                             u32 /*clear_stencil*/, u32 /*parameters*/) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return 0;
  const u32 count = d.W(native::off::TileCount);
  if (dest_texture) {
    for (u32 i = 0; i < count; ++i) {
      const u32 tile = native::off::TileRects + i * 16;
      ClearRect rect{i32(d.W(tile)), i32(d.W(tile + 4)), i32(d.W(tile + 8)), i32(d.W(tile + 12))};
      // A tile at the surface's edge resolves to a multiple of 8.
      const u32 surface = d.W(0x31BC/*NB:0x31B4*/);
      const auto *s = surface ? GuestPtr<const be_u32>(surface) : nullptr;
      const u32 size = s ? u32(s[9]) : 0;
      if (u32(rect.x2) == (size >> 18) + 1)
        rect.x2 = i32((u32(rect.x2) + 7) & ~7u);
      if (u32(rect.y2) == native::Extract(size, 3, 15) + 1)
        rect.y2 = i32((u32(rect.y2) + 7) & ~7u);
      native::SetPredication(d, 3u << (i * 2));
      native::SetRenderState(d, kRenderStateHiZWriteEnable,
                             (d.B(native::off::Flags3) & 0x20) ? 0 : 2);
      const i32 dest_x = i32(d.W(tile)), dest_y = i32(d.W(tile + 4));
      if (i + 1 < count) {
        Resolve(device, 0x300, &rect, dest_texture, dest_x, dest_y, 0, 0, device + 0x3390,
                d.F(0x33A0), d.W(0x33A4), 0);
      } else {
        Resolve(device, 0x300, &rect, dest_texture, dest_x, dest_y, 0, 0, color_va,
                1.0f /*flt_82000D0C*/, 0, 0);
      }
    }
  }
  native::SetPredication(d, 0);
  native::SetRenderState(d, kRenderStateHiZWriteEnable, 2);
  if (count != 1) {
    // Around the WorkerDpc callback.
    native::SetPredication(d, 0x80000000u);
    native::SetPredication(d, 0);
  }
  native::KickOff(device);

  d.SetB(native::off::Flags3, d.B(native::off::Flags3) & 0xCF);
  d.SetB(native::off::Flags, d.B(native::off::Flags) & 0xDF);
  d.SetW(0x31BC/*NB:0x31B4*/, 0);
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // sub_82210138: the segment's fence, unless the device's command memory ran
  // out (EndCommandBuffer marks a device so until the next Begin).
  const bool out_of_memory = (d.B(native::off::Flags1) & 0x20) != 0;
  if (out_of_memory) {
    d.SetB(native::off::Flags1, d.B(native::off::Flags1) & 0xDF);
    native::RewindRing(d);
  } else {
    native::SetFence(device);
  }
  native::SetViewport(d, kFullViewport);

  if (!IsRecordingCommandBuffer(device)) {
    render_thread::Queue([] {
      SetTilingExtentLocked(0, 0);
      SetTilesLocked(nullptr, 0);
    });
  }
  if (out_of_memory) {
    native::ResetAllState(d);
    return kE_OUTOFMEMORY;
  }
  return 0;
}

// void D3DDevice_BeginExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x82269618]. The renderer finds a draw's memexport streams in its shaders.
void D3DDevice_BeginExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::BeginExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_EndExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x822693B0]: the resource is busy until the current fence, which completes
// once the render thread has written the stream's results.
void D3DDevice_EndExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::EndExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_Present(D3DDevice*) [0x826541E0]: the automatic back buffer
// resolved into the automatic front buffer, then Swap. N&B creates its device
// without an automatic front buffer, so its callers (XUI, the threaded movie
// player) have nothing to resolve to.
void D3DDevice_Present_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return;
  FlushDeferredVertices(device);
  float viewport[6];
  for (u32 i = 0; i < 6; ++i)
    viewport[i] = d.F(0x3170/*NB:0x3168*/ + i * 4);
  i32 scissor[4];
  for (u32 i = 0; i < 4; ++i)
    scissor[i] = i32(d.W(native::off::ScissorRect + i * 4));
  native::SetRenderTarget(d, 0, d.W(native::off::AutoBackBuffer));
  // SynchronizeToPresentationInterval (0x8228A748): its swap callback and wait
  // go into the ring; the swap is pending.
  native::SetPredication(d, 0);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | d3d::kSwapFlagPending);
  const u32 front_buffer = d.W(native::off::AutoFrontBuffer);
  if (front_buffer)
    Resolve(device, 0, nullptr, front_buffer, 0, 0, 0, 0, 0, 1.0f, 0, 0);
  native::KickOff(device);
  native::SetViewport(d, viewport[0], viewport[1], viewport[2], viewport[3], viewport[4],
                      viewport[5]);
  native::SetScissorRect(d, scissor[0], scissor[1], scissor[2], scissor[3]);
  native::Swap(device, front_buffer, device + 0x3548);
}

// void D3DDevice_SetVertexShader(D3DDevice*, D3DVertexShader*) [0x8222A858]
void D3DDevice_SetVertexShader_hook(u32 device, u32 shader) {
  native::SetVertexShader(native::DeviceAt(device), shader);
}

// void D3DDevice_SetPixelShader(D3DDevice*, D3DPixelShader*) [0x82226E18]
void D3DDevice_SetPixelShader_hook(u32 device, u32 shader) {
  native::SetPixelShader(native::DeviceAt(device), shader);
}

} // namespace

// ---------------------------------------------------------------------------
// sa2-nx (2026-10-20): SA2's hooks (N&B's addresses in brackets)
// ---------------------------------------------------------------------------
// The draws are Renut's at SA2's addresses (same signatures; sa2-nx's own hooks there, src/me/sa2_me_hooks.cpp and
// src/me/sa2_d3d_api.cpp, hand over to these when Renut's renderer is chosen). SA2 does not tile, export, record
// command buffers or draw tessellated, and its Present is left to its own code (its Resolve and Swap are hooked).
//
// Clear and Resolve: the host work is queued from the device as it is at the call (the same snapshot N&B's hooks
// take), then SA2's own function does the device work N&B's hooks did natively (d3d_native.h is N&B's layout): its
// packets go into the ring sink, its inline data into the scratch (device.cpp), its fences through the hooked
// BlockOnFence and KickOff. SA2's Clear (sub_82563248) calls ClearF once per rect, and the game calls ClearF directly
// too, so ClearF is the hook.

namespace {

void QueueHostClear(u32 device, u32 flags, const ClearRect *rect, const float color[4], float z, u32 stencil) {
  if (IsRecordingCommandBuffer(device))
    RecordClear(device, flags, rect, color, z, stencil);
  else if (Active())
    QueueClear(device, flags, rect, color, z, stencil);
}

void QueueHostResolve(u32 device, u32 flags, const ClearRect *rect, u32 dest_texture, i32 dest_x, i32 dest_y,
                      u32 dest_level, u32 dest_slice, u32 color_va, float clear_z, u32 clear_stencil) {
  float color[4] = {0, 0, 0, 0};
  const bool has_color = color_va && ReadVector4(color_va, color);
  if (IsRecordingCommandBuffer(device)) {
    RecordResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                  has_color ? color : nullptr, clear_z, clear_stencil);
  } else if (Active()) {
    QueueResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                 has_color ? color : nullptr, clear_z, clear_stencil);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// me2-nx-deko (2026-10-25): Mass Effect 2's hooks (XDK 2.0.8955; N&B's addresses in brackets, MGR's in braces)
// ---------------------------------------------------------------------------
// As MGR's: the draws are Renut's; Clear, Resolve and the window scissor queue the host work, then ME2's own function
// does the device work. ME2 tiles (UE3's predicated tiling, 2x MSAA in two tiles; sub_827DCE98 begins, sub_827DD040
// resolves each tile under SetPredication sub_82AA4228 and then ends): BeginTiling and EndTiling are ME2's own too,
// around the host's tiled targets (SetTilingExtentLocked / SetTilesLocked, as N&B's native hooks above do).

REX_EXTERN(__imp__sub_82A9B2A8);
REX_EXTERN(__imp__sub_82AA2B30);
REX_EXTERN(__imp__sub_82AA31F8);
REX_EXTERN(__imp__sub_82AA4398);
REX_EXTERN(__imp__sub_82AA46F0);

namespace {

// Set while ME2's BeginTiling runs: its ClearF clears one tile's worth of EDRAM, the whole tiled target on the host.
thread_local bool t_me2_in_begin_tiling = false;

// HRESULT D3DDevice_BeginIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT NumVertices,
//     UINT IndexCount, D3DFORMAT IndexDataFormat, UINT VertexStreamZeroStride, void **ppIndexData,
//     void **ppVertexData) [sub_82A9EAB8]: as BeginVertices, with the indices in a scratch of their own; the inline end
// restores m_pRing from +13444 as BeginVertices' (ME2's `stw r11,13444(r31)`).
constexpr u32 kIndicesScratchSize = 1u << 20;
std::mutex g_indices_scratch_mutex;
std::unordered_map<u32, u32> g_indices_scratch;

u32 IndicesScratch(u32 device) {
  std::lock_guard lock(g_indices_scratch_mutex);
  u32 &scratch = g_indices_scratch[device];
  if (!scratch)
    scratch = records::AllocPhysicalBlock(kIndicesScratchSize);
  return scratch;
}

u32 Me2BeginIndexedVertices(u32 device, u32 primitive, u32 base_vertex, u32 num_vertices, u32 index_count,
                            u32 index_format, u32 stride, u32 out_indices, u32 out_vertices) {
  const bool is32 = (index_format & 4) != 0;  // D3DFMT_INDEX32 (ME2 tests bit 2)
  const u64 vertex_bytes = u64(num_vertices) * stride;
  const u64 index_bytes = u64(index_count) * (is32 ? 4 : 2);
  StoreU32(out_indices, 0);
  StoreU32(out_vertices, 0);
  if (!vertex_bytes || vertex_bytes > kVerticesScratchSize || !index_bytes || index_bytes > kIndicesScratchSize)
    return kE_OUTOFMEMORY;
  StoreU32(device + d3d::kDeviceOffsetVerticesRingEnd, LoadU32(device + u32(offsetof(d3d::Device, m_pRing))));
  const u32 vertices = VerticesScratch(device);
  const u32 indices = IndicesScratch(device);
  if (!vertices || !indices)
    return kE_OUTOFMEMORY;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.count = index_count;
  call.up_vertices = GuestPtr<const u8>(vertices);
  call.up_stride = stride;
  call.up_vertex_count = num_vertices;
  call.up_indices = GuestPtr<const u8>(indices);
  call.up_indices_32 = is32;
  if (Active() && !IsRecordingCommandBuffer(device))
    QueueDeferredDraw(device, call, call.up_vertices, u32(vertex_bytes), call.up_indices, u32(index_bytes));
  else
    FlushDeferredVertices(device);  // recorded command buffers: not in ME2 so far (no BeginCommandBuffer hook)
  StoreU32(out_indices, indices);
  StoreU32(out_vertices, vertices);
  return 0;
}

}  // namespace

BANJO_RENUT_HOOK_SHARED(sub_82A9EFD0, D3DDevice_DrawVertices_hook);  // D3DDevice_DrawVertices [0x8222CE00] {826BC3B8}
// void D3DDevice_DrawIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartIndex, UINT IndexCount) [sub_82A9F3C0]:
// XDK 8955's has no BaseVertexIndex (N&B's and MGR's do): UE3's RHI sub_827DBE38 passes r4-r6 only, and SetIndices
// sub_82A9DD80 takes the index buffer alone.
namespace {
void Me2DrawIndexedVertices_hook(u32 device, u32 primitive, u32 start_index, u32 index_count) {
  D3DDevice_DrawIndexedVertices_hook(device, primitive, 0, start_index, index_count);
}
}  // namespace
BANJO_RENUT_HOOK_SHARED(sub_82A9F3C0, Me2DrawIndexedVertices_hook);  // [0x82656230] {826BC7D0}
BANJO_RENUT_HOOK(sub_82A9E5D8, D3DDevice_BeginVertices_hook);  // D3DDevice_BeginVertices [0x8222D2B0] {826BBEF8}

// BeginIndexedVertices: nine arguments, the ninth (ppVertexData) in the caller's argument words at +84.
BANJO_RENUT_HOOK_RAW(sub_82A9EAB8) {
  ctx.r3.u64 = Me2BeginIndexedVertices(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                                       ctx.r9.u32, ctx.r10.u32, LoadU32(ctx.r1.u32 + 84));
}

// The window offset and scissor writer (CDevice*, x1, y1, x2, y2) [sub_82A9B2A8] {826B53A0}: ME2's untiled path, like
// MGR's, writes the registers into the ring without the WindowPacket shadow Renut reads. Mirror those values.
BANJO_RENUT_HOOK_RAW(sub_82A9B2A8) {
  const u32 device = ctx.r3.u32;
  const u32 words[] = {0u,
      ((ctx.r5.u32 << 16) & 0x3FFF0000u) | (ctx.r4.u32 & 0x3FFFu),
      ((ctx.r7.u32 << 16) & 0x3FFF0000u) | (ctx.r6.u32 & 0x3FFFu)};
  if (auto *packet = GuestPtr<be_u32>(device + native::off::WindowOffset))
    for (u32 i = 0; i < 3; ++i) packet[i] = words[i];
  __imp__sub_82A9B2A8(ctx, base);
}

// void D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT *pRect, const D3DVECTOR4 *pColor, float Z,
//     DWORD Stencil) [sub_82AA2B30; N&B 0x822811A0] {826BBCF0}: Z in f1 (r7's slot). ME2's ClearF reads no stencil
// argument: its core sub_82AA2490 takes r3-r6 and f1 only and draws the clear through sub_82AA1F30 without one, and the
// callers leave r8 unset. The host clear uses stencil 0 (ASSUMPTION, check against the PC Xenos build's clears).
BANJO_RENUT_HOOK_RAW(sub_82AA2B30) {
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, rect_va = ctx.r5.u32, color_va = ctx.r6.u32;
  ClearRect rect;
  const bool has_rect = !t_me2_in_begin_tiling && rect_va && ReadRect(rect_va, rect);
  float color[4] = {0, 0, 0, 0};
  if (color_va)
    ReadVector4(color_va, color);
  QueueHostClear(device, flags, has_rect ? &rect : nullptr, color, float(ctx.f1.f64), 0);
  __imp__sub_82AA2B30(ctx, base);
}

// void D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT *pSourceRect, D3DBaseTexture *pDestTexture,
//     const D3DPOINT *pDestPoint, UINT DestLevel, const D3DVECTOR4 *pClearColor, float ClearZ, DWORD ClearStencil)
//     [sub_82AA31F8; N&B 0x8222D6C8] {826BAD38}: XDK 8955's has no DestSliceOrFace and no pParameters (N&B's and MGR's
// do). pClearColor in r9 (NULL: D3D's default colour, `cmplwi cr6,r19,0`), ClearZ in f1 (r10's slot, which callers leave
// unset), ClearStencil in the caller's argument word at +84 (the ninth slot; the depth clear sub_82AA2CF8 takes it).
BANJO_RENUT_HOOK_RAW(sub_82AA31F8) {
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, rect_va = ctx.r5.u32, dest_texture = ctx.r6.u32;
  const u32 point_va = ctx.r7.u32, dest_level = ctx.r8.u32, dest_slice = 0, color_va = ctx.r9.u32;
  const u32 clear_stencil = LoadU32(ctx.r1.u32 + 84);
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  i32 dest_x = 0, dest_y = 0;
  if (const auto *p = point_va ? GuestPtr<const be_u32>(point_va) : nullptr) {
    dest_x = i32(u32(p[0]));
    dest_y = i32(u32(p[1]));
  }
  QueueHostResolve(device, flags, has_rect ? &rect : nullptr, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                   color_va, float(ctx.f1.f64), clear_stencil);
  __imp__sub_82AA31F8(ctx, base);
}

// void D3DDevice_BeginTiling(D3DDevice*, DWORD Flags, DWORD Count, const D3DRECT *pTileRects,
//     const D3DVECTOR4 *pClearColor, float ClearZ, DWORD ClearStencil) [sub_82AA4398; N&B 0x822788A0]
BANJO_RENUT_HOOK_RAW(sub_82AA4398) {
  const u32 device = ctx.r3.u32, count = ctx.r5.u32, rects_va = ctx.r6.u32;
  std::vector<ClearRect> tiles;
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (!ReadRect(rects_va + i * 16, rect))
      break;
    tiles.push_back(rect);
  }
  if (!IsRecordingCommandBuffer(device)) {
    FlushDeferredVertices(device);
    u32 width = 0, height = 0;
    for (const ClearRect &rect : tiles) {
      width = std::max(width, u32(rect.x2));
      height = std::max(height, u32(rect.y2));
    }
    render_thread::Queue([tiles, width, height] {
      SetTilingExtentLocked(width, height);
      SetTilesLocked(tiles.data(), u32(tiles.size()));
    });
  }
  t_me2_in_begin_tiling = true;
  __imp__sub_82AA4398(ctx, base);
  t_me2_in_begin_tiling = false;
}

// D3D's end of tiling (CDevice*) [sub_82AA46F0]: after the game's per-tile resolves (sub_827DD040); its WorkerDpc
// callback replays the tiles' ring segment, which is the sink here.
BANJO_RENUT_HOOK_RAW(sub_82AA46F0) {
  const u32 device = ctx.r3.u32;
  FlushDeferredVertices(device);
  __imp__sub_82AA46F0(ctx, base);
  if (!IsRecordingCommandBuffer(device)) {
    render_thread::Queue([] {
      SetTilingExtentLocked(0, 0);
      SetTilesLocked(nullptr, 0);
    });
  }
}