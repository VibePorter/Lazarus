// me2-nx-deko (2026-11-05): what the dynamic shadow depth passes cost, and how many of them could be kept from the
// frame before (the question before a shadow-map cache).
//
// FProjectedShadowInfo::RenderDepth (TU2 sub_826A59F8; r3 = the FProjectedShadowInfo, r4 = the FSceneRenderer, its
// views at +64 / count +68; the shadow's viewport X/Y/size at +324/+328/+332/+336, its light scene info at +4) draws one
// shadow's depth: the subject's and the casters' depth draws into the shadow map, then the blur passes.
//
// Always (cheap): every 10 s "[shadow] 10 s: N passes (n a frame), D draws (d a frame, X % of all draws), the UE3 render
//   thread T ms a frame inside RenderDepth (Renut's draw hooks included)".
// me2_shadow_diag (diagnostic): every draw of a pass is hashed with what it draws (primitive, range, shaders,
//   declaration, index / vertex buffers and strides, the first 16 texture slots, all 256 vertex and pixel shader
//   float constants: skinned characters' bones are vertex constants); a pass whose hash equals the last one of the
//   same shadow (key: light scene info, viewport size, the light's pass ordinal in the frame) drew exactly the same
//   thing again = what a cache could have kept. Upper bound: vertex buffers are hashed by address, not contents
//   (CPU-written vertices would look unchanged).
#include "me2_replay.h"  // me2-nx-deko (2026-11-06): draw-setup replay observers (ME2_TIMED_LEAF)
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(me2_shadow_diag, false, "ME2/Diagnostics",
                    "Hash every shadow depth pass's draws (shaders, buffers, all shader constants) and count the passes "
                    "that drew exactly what the same shadow drew the frame before (\"[shadow]\" lines)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(me2_element_diag, false, "ME2/Diagnostics",
                    "Base-pass static mesh elements (the 5 per-element draws of the base-pass lists, Umbra's op 164 "
                    "and DrawVisible): hash what each element itself sets (shader constants it changes, streams, "
                    "indices, declaration, textures, draw) and count the draws equal to that element's previous one "
                    "(\"[element]\" lines; the native draw-list replay's question)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(me2_texbind_diag, false, "ME2/Diagnostics",
                    "Time the texture + sampler setter sub_827DB288 and count the binds that change nothing (the same "
                    "texture and fetch constant already in the slot): \"[texbind]\" lines")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(me2_element_timing, false, "ME2/Diagnostics",
                    "Time the base-pass element draws and, inside them, the calls a native replay would still make "
                    "(vertex factory, rasterizer state, index buffer, the draw with Renut's hook): \"[element-time]\" "
                    "lines (the replay's possible saving = the rest)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_sa2_game_frames;   // src/sa2_capture.cpp: counted at each Swap
extern std::atomic<uint64_t> g_me2_draws_total;   // renut d3d hooks/draw.cpp: every draw
extern void (*g_me2_draw_observer)(uint32_t device, uint32_t primitive, uint32_t start, uint32_t count,
                                   uint32_t indexed);
extern void (*g_me2_draw_post)();

namespace {

constexpr uint32_t kDevVsConstants = 120 * 16;  // float constant c0 of the vertex shader (n_82A82B10.h)
constexpr uint32_t kDevPsConstants = 376 * 16;  // ... of the pixel shader
constexpr uint32_t kDevDecl = 11824, kDevIndices = 12444, kDevStreams = 12468, kDevStrides = 12536;
constexpr uint32_t kDevTextures = 12552, kDevPs = 12700, kDevVs = 12704;

const uint8_t* g_base = nullptr;
thread_local uint64_t* t_hash = nullptr;  // the pass being measured (UE3 render thread)
uint32_t g_device = 0;                    // the D3D device the draws name (one device)
// me2_element_diag: the element being measured: what it changed relative to its entry (the constant registers and
// their values; a hash of everything else its draws used).
struct ElemReg {
  uint16_t id;  // bank << 8 | register (bank 0 = vertex, 1 = pixel shader float constants)
  uint8_t v[16];
};
constexpr int kRestFields = 55;
struct ElemAcc {
  uint64_t other = 0;
  uint32_t draws = 0;
  std::vector<ElemReg> regs;
  uint32_t rest[kRestFields] = {};
};
thread_local ElemAcc* t_elem = nullptr;
thread_local uint8_t t_snap[8192];
thread_local uint32_t t_rest_entry[52];
constexpr uint32_t kDevBytes = 24064;           // the XDK 8955 device (0x5E00)
constexpr uint32_t kDevBlocks = kDevBytes / 32;
thread_local uint8_t t_dev_mark[kDevBytes];     // the device at the last mark (entry, then each draw)
thread_local int t_dev_phase = -1;              // -1 off, 0 before the first draw, 1 after a draw
struct DevHist {
  uint32_t before[kDevBlocks] = {}, after[kDevBlocks] = {};
};
DevHist* g_dev_hist[2] = {};
thread_local int t_dev_class = 0;
void DevDiff(const uint8_t* d, uint32_t* hist) {
  for (uint32_t b = 0; b < kDevBlocks; ++b)
    if (std::memcmp(d + 32 * b, t_dev_mark + 32 * b, 32) != 0) ++hist[b];
}
constexpr uint32_t kInherited = 0xFFFFFFFFu;

void ReadRest(const uint8_t* d, uint32_t* out) {
  const uint32_t offs[4] = {kDevVs, kDevPs, kDevDecl, kDevIndices};
  for (int i = 0; i < 4; ++i) {
    uint32_t v;
    std::memcpy(&v, d + offs[i], 4);
    out[i] = __builtin_bswap32(v);
  }
  for (int i = 0; i < 16; ++i) {
    uint32_t v;
    std::memcpy(&v, d + kDevStreams + 4 * i, 4);
    out[4 + i] = __builtin_bswap32(v);
    out[20 + i] = d[kDevStrides + i];
    std::memcpy(&v, d + kDevTextures + 4 * i, 4);
    out[36 + i] = __builtin_bswap32(v);
  }
}

inline uint64_t Mix(uint64_t h, uint64_t v) {
  h = (h ^ v) * 0x9E3779B97F4A7C15ull;
  return h ^ (h >> 29);
}
uint64_t MixBytes(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = Mix(h, w);
  }
  return h;
}

void ObserveElement(const uint8_t* d, uint32_t primitive, uint32_t start, uint32_t count, uint32_t indexed) {
  ElemAcc& a = *t_elem;
  uint64_t h = a.other;
  h = Mix(h, (uint64_t(primitive) << 32) | indexed);
  h = Mix(h, (uint64_t(start) << 32) | count);
  // The constants this element changed since its entry (VS c0-c255, PS c0-c255), latest value of each.
  const uint8_t* cur[2] = {d + kDevVsConstants, d + kDevPsConstants};
  for (int bank = 0; bank < 2; ++bank)
    for (uint32_t r = 0; r < 256; ++r) {
      const uint8_t* now = cur[bank] + 16 * r;
      if (std::memcmp(now, t_snap + 4096 * bank + 16 * r, 16) == 0) continue;
      const uint16_t id = uint16_t(bank << 8 | r);
      auto it = std::find_if(a.regs.begin(), a.regs.end(), [id](const ElemReg& x) { return x.id == id; });
      if (it == a.regs.end()) {
        a.regs.push_back({id, {}});
        it = a.regs.end() - 1;
      }
      std::memcpy(it->v, now, 16);
    }
  uint32_t w[4];
  std::memcpy(&w[0], d + kDevVs, 4);
  std::memcpy(&w[1], d + kDevPs, 4);
  std::memcpy(&w[2], d + kDevDecl, 4);
  std::memcpy(&w[3], d + kDevIndices, 4);
  h = MixBytes(h, reinterpret_cast<const uint8_t*>(w), sizeof(w));
  h = MixBytes(h, d + kDevStreams, 16 * 4);
  h = MixBytes(h, d + kDevStrides, 16);
  h = MixBytes(h, d + kDevTextures, 16 * 4);
  a.other = h;
  uint32_t now[52];
  ReadRest(d, now);
  for (int i = 0; i < 52; ++i)
    if (now[i] != t_rest_entry[i]) a.rest[i] = now[i];  // set by this element (else stays kInherited)
  a.rest[52] = (primitive << 1) | (indexed & 1);
  a.rest[53] = start;
  a.rest[54] = count;
  // The comparison's "other" = the fields this element set (its own shaders, buffers, textures, draw).
  uint64_t o = 0x243F6A8885A308D3ull;
  for (int i = 0; i < kRestFields; ++i) o = Mix(o, a.rest[i]);
  a.other = o;
  ++a.draws;
}

// me2_element_timing: inside a timed element call (depth), inside one of its timed calls (no double counting), inside
// its link's DrawShared, and what the calls took, [outside / inside DrawShared][slot]: 0 vertex factory, 1 rasterizer
// state, 2 index buffer, 3 the draws, 4 vertex shader parameters, 5 pixel shader / material parameters, 6 static
// render states, 7 bound shader state.
constexpr int kLeafSlots = 13;  // + 8 light map / texture setup, 9 state getters, 10 reference release, 11 parameter
                                // setters inside the material setup 825DCDA0, 12 setters outside it
thread_local int t_time_depth = 0;
thread_local bool t_in_leaf = false;
thread_local int t_in_shared = 0;
thread_local uint64_t t_shared_ns = 0;
thread_local bool t_draw_open = false;
thread_local std::chrono::steady_clock::time_point t_draw_t0;
thread_local uint64_t t_leaf_ns[2][kLeafSlots];
// The pixel shader parameter functions (8267AA00 / AB38 / AC58) are containers: their own time to slot 5, and inside
// them the material setup sub_825DCDA0 (me2_material_params.cpp opens it) and the parameter setters are timed too.
thread_local int t_in_ps = 0, t_in_mat = 0;
thread_local uint64_t t_mat_ns = 0;
thread_local uint64_t t_tex_ns = 0;  // the texture setter sub_827DB288 inside the material setup (me2_texbind_diag)

void DrawPost() {
  if (!t_draw_open) return;
  t_draw_open = false;
  t_leaf_ns[t_in_shared ? 1 : 0][3] += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_draw_t0).count());
}

void Observe(uint32_t device, uint32_t primitive, uint32_t start, uint32_t count, uint32_t indexed) {
  if (t_time_depth > 0 && !t_in_leaf) {
    t_draw_open = true;
    t_draw_t0 = std::chrono::steady_clock::now();
  }
  if (!g_base || !device) return;
  g_device = device;
  if (t_elem && t_dev_phase >= 0) {
    const uint8_t* d = g_base + device;
    DevHist& h = *g_dev_hist[t_dev_class];
    DevDiff(d, t_dev_phase == 0 ? h.before : h.after);  // between draws counts as "after"
    std::memcpy(t_dev_mark, d, kDevBytes);
    t_dev_phase = 1;
  }
  if (t_elem) ObserveElement(g_base + device, primitive, start, count, indexed);
  if (!t_hash) return;
  const uint8_t* d = g_base + device;
  uint64_t h = *t_hash;
  h = Mix(h, (uint64_t(primitive) << 32) | indexed);
  h = Mix(h, (uint64_t(start) << 32) | count);
  uint32_t w[4];
  std::memcpy(&w[0], d + kDevVs, 4);
  std::memcpy(&w[1], d + kDevPs, 4);
  std::memcpy(&w[2], d + kDevDecl, 4);
  std::memcpy(&w[3], d + kDevIndices, 4);
  h = MixBytes(h, reinterpret_cast<const uint8_t*>(w), sizeof(w));
  h = MixBytes(h, d + kDevStreams, 16 * 4);
  h = MixBytes(h, d + kDevStrides, 16);
  h = MixBytes(h, d + kDevTextures, 16 * 4);
  h = MixBytes(h, d + kDevVsConstants, 256 * 16);
  h = MixBytes(h, d + kDevPsConstants, 256 * 16);
  *t_hash = h;
}

uint32_t L32(const uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}

struct Window {
  uint64_t passes = 0, draws = 0, ns = 0, frames0 = 0, all_draws0 = 0;
  uint64_t hashed = 0, same = 0, same_draws = 0, first_seen = 0;
  std::chrono::steady_clock::time_point start{};
};
Window g_w;  // UE3 render thread only

struct Last {
  uint64_t hash = 0, frame = 0;
};
std::unordered_map<uint64_t, Last> g_last;  // diag: per shadow key
uint64_t g_ordinal_frame = ~uint64_t(0);
std::unordered_map<uint64_t, uint32_t> g_ordinal;  // light -> passes drawn for it this frame

void Report(std::chrono::steady_clock::time_point now, uint64_t frame) {
  const double secs = std::chrono::duration<double>(now - g_w.start).count();
  const uint64_t frames = frame - g_w.frames0;
  const uint64_t all = g_me2_draws_total.load(std::memory_order_relaxed) - g_w.all_draws0;
  const double f = frames ? double(frames) : 1.0;
  REXLOG_INFO("[shadow] {:.0f} s: {} passes ({:.1f} a frame), {} draws ({:.0f} a frame, {:.1f} % of all draws), the UE3 "
              "render thread {:.2f} ms a frame inside RenderDepth (Renut's draw hooks included)",
              secs, g_w.passes, double(g_w.passes) / f, g_w.draws, double(g_w.draws) / f,
              all ? 100.0 * double(g_w.draws) / double(all) : 0.0, double(g_w.ns) / 1e6 / f);
  if (g_w.hashed)
    REXLOG_INFO("[shadow] me2_shadow_diag: {} passes hashed, {} drew exactly what the same shadow drew last time "
                "({:.1f} %, {} draws = {:.1f} % of the shadow draws), {} first seen",
                g_w.hashed, g_w.same, 100.0 * double(g_w.same) / double(g_w.hashed), g_w.same_draws,
                g_w.draws ? 100.0 * double(g_w.same_draws) / double(g_w.draws) : 0.0, g_w.first_seen);
  g_w = Window{};
  g_w.start = now;
  g_w.frames0 = frame;
  g_w.all_draws0 = g_me2_draws_total.load(std::memory_order_relaxed);
  if (g_last.size() > 4096) g_last.clear();
}

}  // namespace

REX_EXTERN(__imp__sub_826A59F8);  // FProjectedShadowInfo::RenderDepth
REX_HOOK_RAW(sub_826A59F8) {
  static const bool diag = REXCVAR_GET(me2_shadow_diag);
  const uint32_t info = ctx.r3.u32;
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const auto t0 = std::chrono::steady_clock::now();
  if (g_w.start == std::chrono::steady_clock::time_point{}) {
    g_w.start = t0;
    g_w.frames0 = frame;
    g_w.all_draws0 = g_me2_draws_total.load(std::memory_order_relaxed);
  }
  const uint64_t draws0 = g_me2_draws_total.load(std::memory_order_relaxed);
  uint64_t hash = 0x6A09E667F3BCC908ull;
  if (diag) {
    g_base = base;
    if (!g_me2_draw_observer) g_me2_draw_observer = Observe;
    t_hash = &hash;
  }
  __imp__sub_826A59F8(ctx, base);
  t_hash = nullptr;
  const auto t1 = std::chrono::steady_clock::now();
  const uint64_t d = g_me2_draws_total.load(std::memory_order_relaxed) - draws0;
  ++g_w.passes;
  g_w.draws += d;
  g_w.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  if (diag && info) {
    const uint32_t light = L32(base, info + 4);
    if (frame != g_ordinal_frame) {
      g_ordinal_frame = frame;
      g_ordinal.clear();
    }
    const uint32_t ordinal = g_ordinal[light]++;
    const uint64_t key = Mix(Mix(uint64_t(light) << 8 | ordinal, L32(base, info + 332)), L32(base, info + 336));
    ++g_w.hashed;
    auto it = g_last.find(key);
    if (it == g_last.end()) {
      ++g_w.first_seen;
      g_last[key] = {hash, frame};
    } else {
      if (it->second.hash == hash) {
        ++g_w.same;
        g_w.same_draws += d;
      }
      it->second = {hash, frame};
    }
  }
  if (t1 - g_w.start >= std::chrono::seconds(10)) Report(t1, frame);
}

// ---- me2_element_diag ----------------------------------------------------------------------------------------------
// Two classes of element call: 0 = its link's shared state already drawn (what the element itself sets), 1 = the
// link's first element this pass (DrawShared runs inside: shaders, material and view constants too). Per class: how
// many calls set exactly what the same element (same class) set last time, and for those that did not, which
// constant registers differ (with one example old -> new value) and how often the rest (shaders, streams, indices,
// declaration, textures, draw) differs.
namespace {
constexpr int kElemFns = 5;
constexpr uint32_t kElemAddr[kElemFns] = {0x826A0990, 0x826A0A58, 0x826A0B28, 0x826A0BF8, 0x826A0CC8};
const char* RestName(int f, char* buf) {
  if (f == 0) return "vertex shader";
  if (f == 1) return "pixel shader";
  if (f == 2) return "declaration";
  if (f == 3) return "index buffer";
  if (f < 20) { std::snprintf(buf, 32, "stream %d", f - 4); return buf; }
  if (f < 36) { std::snprintf(buf, 32, "stride %d", f - 20); return buf; }
  if (f < 52) { std::snprintf(buf, 32, "texture %d", f - 36); return buf; }
  return f == 52 ? "primitive" : f == 53 ? "start" : "count";
}
struct ElemClass {
  uint32_t rest_hist[kRestFields] = {};
  uint32_t rest_ex[kRestFields][2] = {};
  uint64_t calls = 0, compared = 0, same = 0, other_diff = 0, regs = 0, first = 0;
  uint32_t hist[512] = {};
  bool example_set[512] = {};
  uint8_t ex_old[512][16], ex_new[512][16];
};
struct ElemWindow {
  ElemClass cls[2];
  uint64_t calls[kElemFns] = {}, no_device = 0;
  std::chrono::steady_clock::time_point start{};
};
ElemWindow* g_ew = nullptr;
struct ElemRec {
  uint64_t other;
  std::vector<ElemReg> regs;
  uint32_t rest[kRestFields];
};
std::unordered_map<uint64_t, ElemRec> g_elem_last;  // (class, function, element) -> what it set last time

std::string Floats(const uint8_t* v) {
  float f[4];
  for (int i = 0; i < 4; ++i) {
    uint32_t w;
    std::memcpy(&w, v + 4 * i, 4);
    w = __builtin_bswap32(w);
    std::memcpy(&f[i], &w, 4);
  }
  return fmt::format("({:.4g} {:.4g} {:.4g} {:.4g})", f[0], f[1], f[2], f[3]);
}

void ElemReport(std::chrono::steady_clock::time_point now) {
  ElemWindow& w = *g_ew;
  std::string fns;
  for (int i = 0; i < kElemFns; ++i)
    if (w.calls[i]) fns += fmt::format(" {:08X} {}", kElemAddr[i], w.calls[i]);
  for (int c = 0; c < 2; ++c) {
    const ElemClass& k = w.cls[c];
    if (!k.calls) continue;
    std::array<int, 512> order{};
    for (int i = 0; i < 512; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int x, int y) { return k.hist[x] > k.hist[y]; });
    std::string top;
    for (int n = 0; n < 12 && k.hist[order[n]]; ++n) {
      const int id = order[n];
      top += fmt::format("; {} c{} {}", id >> 8 ? "PS" : "VS", id & 255, k.hist[id]);
      if (n < 5 && k.example_set[id])
        top += fmt::format(" e.g. {} -> {}", Floats(k.ex_old[id]), Floats(k.ex_new[id]));
    }
    const uint64_t diff = k.compared - k.same;
    std::array<int, kRestFields> ro{};
    for (int i = 0; i < kRestFields; ++i) ro[i] = i;
    std::sort(ro.begin(), ro.end(), [&](int x, int y) { return k.rest_hist[x] > k.rest_hist[y]; });
    std::string rest;
    char nb[32];
    for (int n = 0; n < 8 && k.rest_hist[ro[n]]; ++n)
      rest += fmt::format("; {} {} e.g. {:08X} -> {:08X}", RestName(ro[n], nb), k.rest_hist[ro[n]],
                          k.rest_ex[ro[n]][0], k.rest_ex[ro[n]][1]);
    REXLOG_INFO("[element] {:.0f} s, {}: {} calls, {} compared with the same element's last call ({} first seen), {} "
                "set exactly the same ({:.1f} %), {:.1f} changed constant registers per call; of the {} that differ: "
                "what it set of shaders/streams/indices/declaration/textures (inherited bindings left out) or its draw differs "
                "in {}{}; registers differing most{}",
                std::chrono::duration<double>(now - w.start).count(),
                c ? "first element of its link (DrawShared inside)" : "shared state already drawn", k.calls, k.compared,
                k.first, k.same, k.compared ? 100.0 * double(k.same) / double(k.compared) : 0.0,
                k.calls ? double(k.regs) / double(k.calls) : 0.0, diff, k.other_diff, rest, top);
  }
  REXLOG_INFO("[element] calls by function:{}; {} before a device was seen", fns, w.no_device);
  // Device blocks the elements' guest code changed (offset: calls), before the first draw and after it.
  for (int c = 0; c < 2 && g_dev_hist[0]; ++c)
    for (int phase = 0; phase < 2; ++phase) {
      const uint32_t* h = phase ? g_dev_hist[c]->after : g_dev_hist[c]->before;
      std::string list;
      int n = 0;
      for (uint32_t b = 0; b < kDevBlocks; ++b)
        if (h[b]) {
          if (n++ < 60) list += fmt::format(" +{:X}:{}", 32 * b, h[b]);
        }
      REXLOG_INFO("[element-dev] {}, {} its draw: {} device blocks of 32 bytes changed (offset:calls){}",
                  c ? "first element of its link" : "shared state already drawn",
                  phase ? "after" : "before", n, list);
      std::memset(phase ? g_dev_hist[c]->after : g_dev_hist[c]->before, 0, sizeof(uint32_t) * kDevBlocks);
    }
  *g_ew = ElemWindow{};
  g_ew->start = now;
  if (g_elem_last.size() > 200000) g_elem_last.clear();
}

void ElemCompare(ElemClass& k, uint64_t key, ElemAcc& acc) {
  std::sort(acc.regs.begin(), acc.regs.end(), [](const ElemReg& x, const ElemReg& y) { return x.id < y.id; });
  k.regs += acc.regs.size();
  auto it = g_elem_last.find(key);
  if (it == g_elem_last.end()) {
    ++k.first;
    ElemRec r{acc.other, std::move(acc.regs), {}};
    std::memcpy(r.rest, acc.rest, sizeof(r.rest));
    g_elem_last.emplace(key, std::move(r));
    return;
  }
  ++k.compared;
  ElemRec& old = it->second;
  bool same = old.other == acc.other;
  if (!same) {
    ++k.other_diff;
    for (int f = 0; f < kRestFields; ++f)
      if (old.rest[f] != acc.rest[f]) {
        if (!k.rest_hist[f]) {
          k.rest_ex[f][0] = old.rest[f];
          k.rest_ex[f][1] = acc.rest[f];
        }
        ++k.rest_hist[f];
      }
  }
  // Registers whose value differs, or that only one of the two calls set.
  size_t i = 0, j = 0;
  while (i < old.regs.size() || j < acc.regs.size()) {
    const ElemReg* o = i < old.regs.size() ? &old.regs[i] : nullptr;
    const ElemReg* n = j < acc.regs.size() ? &acc.regs[j] : nullptr;
    int id;
    const uint8_t *ov = nullptr, *nv = nullptr;
    if (o && (!n || o->id < n->id)) {
      id = o->id, ov = o->v, ++i;
    } else if (n && (!o || n->id < o->id)) {
      id = n->id, nv = n->v, ++j;
    } else {
      id = o->id, ov = o->v, nv = n->v, ++i, ++j;
      if (std::memcmp(ov, nv, 16) == 0) continue;
    }
    same = false;
    ++k.hist[id & 511];
    if (!k.example_set[id & 511] && ov && nv) {
      k.example_set[id & 511] = true;
      std::memcpy(k.ex_old[id & 511], ov, 16);
      std::memcpy(k.ex_new[id & 511], nv, 16);
    }
  }
  if (same) ++k.same;
  old.other = acc.other;
  old.regs = std::move(acc.regs);
  std::memcpy(old.rest, acc.rest, sizeof(old.rest));
}

#if defined(ME2_WRITE_CENSUS)
extern "C" uint64_t Me2CensusEnter(int phase);
extern "C" void Me2CensusLeave(uint64_t token);
#endif
struct TimeClass {
  uint64_t calls = 0, ns = 0, shared_ns = 0, mat_ns = 0, tex_ns = 0, leaf[2][kLeafSlots] = {};
};
struct TimeWindow {
  TimeClass cls[2];
  uint64_t frames0 = 0;
  std::chrono::steady_clock::time_point start{};
};
TimeWindow g_tw;

void TimeReport(std::chrono::steady_clock::time_point now) {
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const double f = double(std::max<uint64_t>(1, frame - g_tw.frames0));
  for (int c = 0; c < 2; ++c) {
    const TimeClass& k = g_tw.cls[c];
    if (!k.calls) continue;
    const double n = double(k.calls);
    uint64_t outside = 0, inside = 0;
    for (int i = 0; i < 11; ++i) outside += k.leaf[0][i], inside += k.leaf[1][i];
    const uint64_t element_part = k.ns - std::min(k.ns, k.shared_ns);
    auto us = [&](uint64_t v) { return double(v) / 1e3 / n; };
    REXLOG_INFO("[element-time] {}: {:.0f} calls a frame, {:.2f} us a call = {:.2f} ms a frame; element part {:.2f} us "
                "(vertex factory {:.2f}, rasterizer state {:.2f}, index buffer {:.2f}, draw with Renut's hook {:.2f}, "
                "the rest {:.2f} = what a replay could skip, incl. the native constant setters)",
                c ? "first element of its link (DrawShared inside)" : "shared state already drawn", n / f, us(k.ns),
                double(k.ns) / 1e6 / f, us(element_part), us(k.leaf[0][0]), us(k.leaf[0][1]), us(k.leaf[0][2]),
                us(k.leaf[0][3]), us(element_part - std::min(element_part, outside)));
    if (k.shared_ns)
      REXLOG_INFO("[element-time]   DrawShared {:.2f} us a call = {:.2f} ms a frame: vertex shader parameters {:.2f}, "
                  "pixel shader / material parameters {:.2f}, static render states {:.2f}, bound shader state {:.2f}, "
                  "vertex factory {:.2f}, light map / textures (8266FED0) {:.2f}, state getters {:.2f}, reference "
                  "release {:.2f}, draws {:.2f}, the rest (its first virtual call, native samplers) {:.2f}",
                  us(k.shared_ns), double(k.shared_ns) / 1e6 / f, us(k.leaf[1][4]), us(k.leaf[1][5]), us(k.leaf[1][6]),
                  us(k.leaf[1][7]), us(k.leaf[1][0]), us(k.leaf[1][8]), us(k.leaf[1][9]), us(k.leaf[1][10]),
                  us(k.leaf[1][3]), us(k.shared_ns - std::min(k.shared_ns, inside)));
    if (k.leaf[1][5])
      REXLOG_INFO("[element-time]   pixel shader / material parameters {:.2f} us a call: material setup sub_825DCDA0 "
                  "{:.2f} (of it the parameter setters {:.2f}, the rest = evaluating the parameters, texture setters "
                  "{:.2f}); outside it: setters {:.2f}, the three virtual calls and the rest {:.2f}",
                  us(k.leaf[1][5]), us(k.mat_ns), us(k.leaf[1][11]), us(k.mat_ns - std::min(k.mat_ns, k.leaf[1][11])),
                  us(k.leaf[1][12]),
                  us(k.leaf[1][5] - std::min(k.leaf[1][5], k.mat_ns + k.leaf[1][12])));
    if (k.tex_ns)
      REXLOG_INFO("[element-time]   of the material setup: texture + sampler setter sub_827DB288 {:.2f} us a call "
                  "(DrawShared), everything else {:.2f}", us(k.tex_ns),
                  us(k.mat_ns - std::min(k.mat_ns, k.tex_ns + k.leaf[1][11])));
  }
  g_tw = TimeWindow{};
  g_tw.start = now;
  g_tw.frames0 = frame;
}

template <int K, PPCFunc* Original>
void TimedLeaf(PPCContext& ctx, uint8_t* base) {
  if (K == 11 && !t_in_ps) {  // a parameter setter outside the pixel shader parameters: not timed
    Original(ctx, base);
    return;
  }
  if (K == 11 && !t_in_mat) {
    TimedLeaf<12, Original>(ctx, base);
    return;
  }
  if (t_time_depth <= 0 || t_in_leaf) {
    Original(ctx, base);
    return;
  }
  t_in_leaf = true;
  const auto t0 = std::chrono::steady_clock::now();
  Original(ctx, base);
  t_leaf_ns[t_in_shared ? 1 : 0][K] +=
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  t_in_leaf = false;
}

// A pixel shader parameter function: a container (its own time to slot 5, the calls inside it timed on their own).
template <PPCFunc* Original>
void TimedPs(PPCContext& ctx, uint8_t* base) {
  if (t_time_depth <= 0 || t_in_leaf || t_in_ps) {
    Original(ctx, base);
    return;
  }
  ++t_in_ps;
  const auto t0 = std::chrono::steady_clock::now();
  Original(ctx, base);
  t_leaf_ns[t_in_shared ? 1 : 0][5] +=
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  --t_in_ps;
}

// The link's DrawShared (one per base-pass list type), timed as a whole inside a timed element call; its own calls
// count as "inside DrawShared".
template <PPCFunc* Original>
void TimedShared(PPCContext& ctx, uint8_t* base) {
  if (t_time_depth <= 0 || t_in_leaf || t_in_shared) {
    Original(ctx, base);
    return;
  }
  ++t_in_shared;
  const auto t0 = std::chrono::steady_clock::now();
  Original(ctx, base);
  t_shared_ns +=
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  --t_in_shared;
}

template <int I, PPCFunc* Original>
void ElementDraw(PPCContext& ctx, uint8_t* base) {
  static const bool on = REXCVAR_GET(me2_element_diag);
  static const bool timing = REXCVAR_GET(me2_element_timing);
  if (timing && !on && t_time_depth == 0) {
    g_base = base;
    if (!g_me2_draw_observer) g_me2_draw_observer = Observe;
    if (!g_me2_draw_post) g_me2_draw_post = DrawPost;
    const int cls = (!ctx.r6.u32 || L32(base, ctx.r6.u32) == 0) ? 1 : 0;
    for (auto& row : t_leaf_ns)
      for (auto& v : row) v = 0;
    t_shared_ns = 0;
    t_mat_ns = 0;
    t_tex_ns = 0;
    const auto t0 = std::chrono::steady_clock::now();
    if (g_tw.start == std::chrono::steady_clock::time_point{}) {
      g_tw.start = t0;
      g_tw.frames0 = g_sa2_game_frames.load(std::memory_order_relaxed);
    }
    ++t_time_depth;
    Original(ctx, base);
    --t_time_depth;
    const auto t1 = std::chrono::steady_clock::now();
    TimeClass& k = g_tw.cls[cls];
    ++k.calls;
    k.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < kLeafSlots; ++i) k.leaf[j][i] += t_leaf_ns[j][i];
    k.shared_ns += t_shared_ns;
    k.mat_ns += t_mat_ns;
    k.tex_ns += t_tex_ns;
    if (t1 - g_tw.start >= std::chrono::seconds(10)) TimeReport(t1);
    return;
  }
  if (!on) {
#if defined(ME2_WRITE_CENSUS)
    // Write census phase 4 (me2_census_phases bit 3): the element's own part, its link's shared state already drawn.
    if (ctx.r6.u32 && L32(base, ctx.r6.u32) != 0) {
      const uint64_t token = Me2CensusEnter(4);
      Original(ctx, base);
      Me2CensusLeave(token);
      return;
    }
#endif
    Original(ctx, base);
    return;
  }
  if (!g_ew) g_ew = new ElemWindow();
  // (view r3, element r4, link r5, shared-state flag r6)
  const uint32_t element = ctx.r4.u32, shared = ctx.r6.u32;
  const auto now = std::chrono::steady_clock::now();
  if (g_ew->start == std::chrono::steady_clock::time_point{}) g_ew->start = now;
  ++g_ew->calls[I];
  g_base = base;
  if (!g_me2_draw_observer) g_me2_draw_observer = Observe;
  if (!g_device) {
    ++g_ew->no_device;
    Original(ctx, base);
  } else {
    const int cls = (!shared || L32(base, shared) == 0) ? 1 : 0;
    const uint8_t* d = base + g_device;
    std::memcpy(t_snap, d + kDevVsConstants, 4096);
    std::memcpy(t_snap + 4096, d + kDevPsConstants, 4096);
    ReadRest(d, t_rest_entry);
    if (!g_dev_hist[0]) {
      g_dev_hist[0] = new DevHist();
      g_dev_hist[1] = new DevHist();
    }
    std::memcpy(t_dev_mark, d, kDevBytes);
    t_dev_phase = 0;
    t_dev_class = cls;
    ElemAcc acc;
    acc.other = 0x243F6A8885A308D3ull;
    for (int i = 0; i < 52; ++i) acc.rest[i] = kInherited;
    ElemAcc* saved = t_elem;
    t_elem = &acc;
    Original(ctx, base);
    if (t_dev_phase == 1) DevDiff(d, g_dev_hist[cls]->after);  // after its last draw, to its exit
    t_dev_phase = -1;
    t_elem = saved;
    ElemClass& k = g_ew->cls[cls];
    ++k.calls;
    ElemCompare(k, (uint64_t(cls) << 40) | (uint64_t(I) << 32) | element, acc);
  }
  if (now - g_ew->start >= std::chrono::seconds(10)) ElemReport(now);
}
}  // namespace

#define ME2_ELEMENT_DRAW(I, ADDR)                   \
  REX_EXTERN(__imp__sub_##ADDR);                    \
  REX_HOOK_RAW(sub_##ADDR) { ElementDraw<I, __imp__sub_##ADDR>(ctx, base); }
ME2_ELEMENT_DRAW(0, 826A0990)
ME2_ELEMENT_DRAW(1, 826A0A58)
ME2_ELEMENT_DRAW(2, 826A0B28)
ME2_ELEMENT_DRAW(3, 826A0BF8)
ME2_ELEMENT_DRAW(4, 826A0CC8)

// The calls a replay would still make (me2_element_timing), timed only inside a timed element call.
#define ME2_TIMED_LEAF(K, ADDR)                                                                    \
  REX_EXTERN(__imp__sub_##ADDR);                                                                   \
  REX_HOOK_RAW(sub_##ADDR) {                                                                       \
    const auto me2_replay_scope = me2::replay::EnterAt<0x##ADDR##u>(ctx, base); /* me2_replay.h */ \
    TimedLeaf<K, __imp__sub_##ADDR>(ctx, base);                                                    \
  }
ME2_TIMED_LEAF(0, 8266F820)  // vertex factory set (streams, declaration; calls sub_825A6118)
ME2_TIMED_LEAF(0, 825A6118)  // vertex factory streams (SetStreamSource each)
ME2_TIMED_LEAF(1, 827E16A8)  // RHISetRasterizerState (cull mode, depth bias)
ME2_TIMED_LEAF(2, 82A9DD80)  // D3DDevice_SetIndices
ME2_TIMED_LEAF(4, 826824B0)  // vertex shader parameters (DrawShared)
#define ME2_TIMED_PS(ADDR)                          \
  REX_EXTERN(__imp__sub_##ADDR);                    \
  REX_HOOK_RAW(sub_##ADDR) { TimedPs<__imp__sub_##ADDR>(ctx, base); }
ME2_TIMED_PS(8267AA00)  // pixel shader / material parameters (DrawShared of 826A0990)
ME2_TIMED_PS(8267AB38)  // the same, other base-pass policies
ME2_TIMED_PS(8267AC58)  // the same (DrawShared 826B4C68 / 826B5938)
// 2026-11-06: 822C8C10 / 822EB710 are hot natives now (hot/n/n_822EB710.h): no longer timed here (one hook a
// function).
ME2_TIMED_LEAF(11, 822C8AC8)  // matrix setter (view constants)
ME2_TIMED_LEAF(8, 8266FED0)  // light map / texture setup (DrawShared 826B4C68)
ME2_TIMED_LEAF(9, 8267AE98)  // state getters
ME2_TIMED_LEAF(9, 8267AF60)
ME2_TIMED_LEAF(9, 8267B028)
ME2_TIMED_LEAF(9, 8267B0F0)
ME2_TIMED_LEAF(9, 8267B1B8)
ME2_TIMED_LEAF(9, 822E91F0)
ME2_TIMED_LEAF(10, 822C92F8)  // reference release
// ME2 (2026-11-06): 827DB818 (static render state set, slot 6) is a hot native now (hot/n/n_827DB818.h): one hook per
// function, so it is no longer timed here (slot 6 reads 0).
ME2_TIMED_LEAF(7, 827DB1F0)  // bound shader state
#define ME2_TIMED_SHARED(ADDR)                      \
  REX_EXTERN(__imp__sub_##ADDR);                    \
  REX_HOOK_RAW(sub_##ADDR) { TimedShared<__imp__sub_##ADDR>(ctx, base); }
// me2-nx-deko (2026-11-06): 826B6678 is also the translucency pass's DrawShared (particles): me2_dynshared_diag
// observes those calls (me2_dynshared.cpp).
bool Me2DynSharedTranslucent();
void Me2DynSharedObserveTranslucent(PPCFunc* original, PPCContext& ctx, uint8_t* base);
REX_EXTERN(__imp__sub_826B6678);
REX_HOOK_RAW(sub_826B6678) {  // DrawShared of 826A0990
  if (Me2DynSharedTranslucent())
    Me2DynSharedObserveTranslucent(&TimedShared<__imp__sub_826B6678>, ctx, base);
  else
    TimedShared<__imp__sub_826B6678>(ctx, base);
}
ME2_TIMED_SHARED(826B4400)  // of 826A0A58 / 826A0B28
ME2_TIMED_SHARED(826B4C68)  // of 826A0BF8
ME2_TIMED_SHARED(826B5938)  // of 826A0CC8

// me2_material_params.cpp's hook of sub_825DCDA0 brackets the material setup with these (any path: native or original).
extern "C" uint64_t Me2ElemTimeBegin() {
  if (t_time_depth <= 0 || !t_in_ps || t_in_leaf || t_in_mat) return 0;
  ++t_in_mat;
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) | 1;
}
extern "C" void Me2ElemTimeEnd(uint64_t t0) {
  if (!t0) return;
  --t_in_mat;
  const uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  t_mat_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::duration(int64_t(now - (t0 & ~uint64_t(1))))).count());
}

// me2_texbind_diag: me_hot_guest.cpp's hook of sub_827DB288 brackets every call (native or original) with these.
// Snapshot of the slot's texture pointer and its fetch constant (6 words) before, compared after: unchanged = the bind
// changed nothing but the dirty bit (and the old texture's fence word).
namespace {
constexpr uint32_t kTexDevicePtr = 0x833D074Cu;
struct TexBindSnap {
  uint32_t dev = 0, s = 0, slot = 0;
  uint8_t fc[24];
  bool dirty = false;
  std::chrono::steady_clock::time_point t0;
};
thread_local TexBindSnap t_tb;
struct TexBindWindow {
  uint64_t calls = 0, ns = 0, same_tex = 0, noop = 0, noop_dirty = 0, in_mat = 0, null_tex = 0, frames0 = 0;
  std::chrono::steady_clock::time_point start{};
};
TexBindWindow g_tb;
bool TexDirty(const uint8_t* base, uint32_t dev, uint32_t s) {
  uint64_t v;
  std::memcpy(&v, base + dev + 24, 8);
  v = __builtin_bswap64(v);
  return s + 32 < 64 && (v & ((uint64_t(1) << 63) >> (s + 32))) != 0;
}
}  // namespace

extern "C" bool Me2TexBindBegin(const uint8_t* base, uint32_t s) {
  static const bool on = REXCVAR_GET(me2_texbind_diag);
  if (!on) return false;
  const uint32_t dev = L32(base, kTexDevicePtr);
  if (!dev || s >= 32) return false;
  t_tb.dev = dev;
  t_tb.s = s;
  t_tb.slot = L32(base, dev + 4u * (s + 3138u));
  std::memcpy(t_tb.fc, base + dev + 24u * (s + 48u), 24);
  t_tb.dirty = TexDirty(base, dev, s);
  t_tb.t0 = std::chrono::steady_clock::now();
  return true;
}

extern "C" void Me2TexBindEnd(const uint8_t* base) {
  const auto t1 = std::chrono::steady_clock::now();
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t_tb.t0).count());
  const uint32_t dev = t_tb.dev, s = t_tb.s;
  const uint32_t slot = L32(base, dev + 4u * (s + 3138u));
  const bool same_tex = slot == t_tb.slot;
  const bool noop = same_tex && std::memcmp(t_tb.fc, base + dev + 24u * (s + 48u), 24) == 0;
  if (t_in_mat) t_tex_ns += ns;
  if (g_tb.start == std::chrono::steady_clock::time_point{}) {
    g_tb.start = t1;
    g_tb.frames0 = g_sa2_game_frames.load(std::memory_order_relaxed);
  }
  ++g_tb.calls;
  g_tb.ns += ns;
  g_tb.same_tex += same_tex;
  g_tb.noop += noop;
  g_tb.noop_dirty += noop && t_tb.dirty;
  g_tb.in_mat += t_in_mat ? 1 : 0;
  g_tb.null_tex += slot == 0;
  if (t1 - g_tb.start < std::chrono::seconds(10)) return;
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const double f = double(std::max<uint64_t>(1, frame - g_tb.frames0)), n = double(g_tb.calls);
  REXLOG_INFO("[texbind] {:.0f} binds a frame, {:.0f} ns a bind = {:.2f} ms a frame; same texture already in the slot "
              "{:.1f} %, whole fetch constant unchanged (bind changes nothing) {:.1f} % (of those dirty bit already set "
              "{:.1f} %); null texture {:.1f} %; inside a timed material setup {:.1f} %",
              n / f, double(g_tb.ns) / n, double(g_tb.ns) / 1e6 / f, 100.0 * g_tb.same_tex / n, 100.0 * g_tb.noop / n,
              g_tb.noop ? 100.0 * g_tb.noop_dirty / g_tb.noop : 0.0, 100.0 * g_tb.null_tex / n,
              100.0 * g_tb.in_mat / n);
  g_tb = TexBindWindow{};
  g_tb.start = t1;
  g_tb.frames0 = frame;
}
