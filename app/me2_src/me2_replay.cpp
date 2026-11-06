// me2-nx-deko (2026-11-06): draw-setup replay. Stage 1 = COMPARE MODE (me2_replay_compare, nothing is skipped);
// stage 2 = REPLAY (me2_replay): a mesh's recorded setter calls are made directly instead of running its setup.
//
// WHY
//   At Kasumi's party (console 2026-11-06, ~1,480 draws a frame) UE3's render thread spends ~2.5 ms a frame on the
//   base-pass draws whose link state is already set (492 a frame, 5.1 us each) and ~3.8 ms on the first draw of each link
//   (174 a frame). In both, the element's own part is SetMeshRenderState + DrawMesh; of the 5.1 us, ~2.5 us are UE3's
//   per-mesh setup beyond the vertex factory, rasterizer state, index buffer and the draw itself
//   (me2_element_timing). For a static mesh that setup produces the same calls every frame: a replay can make them
//   directly. PC at the party (11-06 20:53, replay7_party): ~900 setups a frame, 88.5 % with the same inputs as the
//   mesh's previous call, and of those 100.00 % made exactly the same calls (2.4 M compared, 0 different).
//
// WHAT
//   The base-pass element functions (826A0990 / 0A58 / 0B28 / 0BF8 / 0CC8) call, per pass (two for two-sided
//   materials), SetMeshRenderState(policy r3, view r4, [mesh + 216] r5, FStaticMesh r6, pass r7[, element + 16 r8])
//   = 82679ED0 / 8267A028 / 8267A1A8 / 8267A328 / 8267A4C8, then the policy's virtual DrawMesh. The write census of
//   2026-11-05 (phase 4) found only device writes in the element's own part (plus resource fences of the stream / index
//   setters). Recorded here, in call order, at the outermost of these setters (their reads, from the generated code):
//     82A92C50 / 82A92D28  XDK Set{Vertex,Pixel}ShaderConstantF(dev, start, src, count, mask): count * 16 bytes at src
//     822EB710             float parameter (param, value): min(size, 4) bytes at value
//     822C8C10             float4 parameter of an array (param, value, index): min(size - 16 index, 16) bytes
//     822C8AC8             matrix parameter (param, matrix): 64 bytes
//     827E16A8             RHISetRasterizerState(initializer): a 16-byte initializer built on the caller's stack
//     8266F820             the vertex factory set (r3, r4 vertex factory, r5 mesh element): pointers only
//   A record is keyed by (variant, policy, mesh, pass, view); its inputs signature is the mesh's LocalToWorld
//   (+32, 64 bytes), r5, (variants 2-5) r8 and the policy's first 64 bytes (policies are built on the stack per draw).
//
// REPLAY (me2_replay)
//   Once a key has made the same calls for the same inputs kStable times in a row, a call with those inputs makes the
//   recorded calls (through the hooked entry points, natives included) with the recorded data copied below the guest
//   stack pointer, where the original would have put it, instead of running SetMeshRenderState. A record that passes a
//   pointer into the stack as is (not copied), or could not read its data, is never replayed.
//   Self-check: a key's first kChecked replays, and every me2_replay_check_period-th after that, replay, then run the
//   original on top with recording and compare (a) its calls with the record and (b) the D3D device (0x5E00 bytes)
//   after the replay with the device after the original. The setters are idempotent, so an exact replay leaves nothing
//   for the original to change; a write the replay does not make (a setter not observed here, an inlined device write)
//   shows as a device difference. Different calls, or a device difference in two checks in a row (the device also
//   changes under other threads now and then), mark the key never-replay; the original's state stands.
//
// REPORT ("[replay]" every 10 s, either cvar)
//   Replayed calls and us each, checks (failed: calls / device, with the first differing device offsets), original
//   calls and us each; for the originals: same inputs as last time: identical effects / DIFFERENT effects; changed
//   inputs; keys first seen.

#include "me2_replay.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap

REXCVAR_DEFINE_BOOL(me2_replay_compare, false, "ME2/Diagnostics",
                    "Draw-setup replay stage 1: record what UE3's per-mesh base-pass setup (SetMeshRenderState) sets and "
                    "compare it with the same mesh's previous call; '[replay]' lines every 10 s. Changes nothing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_replay, false, "ME2",
                    "Draw-setup replay stage 2: a base-pass mesh whose setup inputs did not change makes its recorded "
                    "setter calls directly instead of running UE3's SetMeshRenderState (self-checked, see me2_replay.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_replay_check_period, 64, "ME2",
                     "me2_replay: every Nth replay of a mesh is checked against the original (power of two; the first "
                     "two of each mesh always are)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(me2_replay_ab_s, 0, "ME2/Diagnostics",
                     "me2_replay: N > 0 turns the replay on and off every N seconds and times the whole per-mesh setup "
                     "in both ('[replay] A/B' lines): the replay's net saving")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The setters a replay calls: the hooked entry points (natives and observers included).
REX_EXTERN(sub_82A92C50);
REX_EXTERN(sub_82A92D28);
REX_EXTERN(sub_822EB710);
REX_EXTERN(sub_822C8C10);
REX_EXTERN(sub_822C8AC8);
REX_EXTERN(sub_827E16A8);
REX_EXTERN(sub_8266F820);

namespace me2::replay {

thread_local void* t_rec = nullptr;
thread_local int t_depth = 0;

namespace {

constexpr int kVariants = 5;
constexpr uint32_t kVariantAddr[kVariants] = {0x82679ED0u, 0x8267A028u, 0x8267A1A8u, 0x8267A328u, 0x8267A4C8u};
constexpr uint32_t kDevicePtr = 0x833D074Cu;  // UE3's GDirect3DDevice
constexpr uint32_t kDeviceBytes = 0x5E00;     // the 2008 XDK's device
constexpr uint32_t kStable = 2;               // same inputs, same calls this many times in a row before replaying
constexpr uint32_t kChecked = 2;              // a key's first replays that are always checked

// Entry kinds of a record.
enum Kind : uint8_t { kVsConst = 1, kPsConst, kParam1, kParam4, kMatrix, kRaster, kVertexFactory, kKinds };
constexpr const char* kKindName[kKinds] = {"?",
                                           "VS constants",
                                           "PS constants",
                                           "parameter (float)",
                                           "parameter (float4 of an array)",
                                           "matrix parameter",
                                           "rasterizer state",
                                           "vertex factory"};

uint32_t L32(const uint8_t* p, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, p + a, 4);
  return __builtin_bswap32(v);
}
uint16_t L16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, base + a, 2);
  return __builtin_bswap16(v);
}
bool Readable(uint32_t a, uint32_t n) { return a >= 0x10000u && uint64_t(a) + n <= 0xA0000000ull; }

// Within a guest stack (near the stack pointer of the call that passed it): its contents are gone by the next frame.
bool OnStack(uint32_t p, uint32_t sp) { return p + 0x40000u - sp < 0x50000u; }  // [sp - 256 KB, sp + 64 KB)

struct Rec {
  std::vector<uint8_t> bytes;
  uint32_t entries = 0;
  bool unreplayable = false;  // a pointer into the stack passed as is, or data that could not be read
  void Put(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    bytes.insert(bytes.end(), b, b + n);
  }
  template <typename T>
  void Put(T v) {
    Put(&v, sizeof(v));
  }
  void Kind(uint8_t k) {
    Put(k);
    ++entries;
  }
  // n bytes at a (raw guest bytes); unreadable: zeros, and the record is not replayed.
  void Data(const uint8_t* base, uint32_t a, uint32_t n) {
    if (!n) return;
    if (!Readable(a, n)) {
      bytes.resize(bytes.size() + n, 0);
      unreplayable = true;
      return;
    }
    Put(base + a, n);
  }
  void Pointer(uint32_t p, uint32_t sp) {
    Put(p);
    if (OnStack(p, sp)) unreplayable = true;
  }
};

inline uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v * 0x9E3779B97F4A7C15ull;
  h = (h << 27 | h >> 37) * 0xFF51AFD7ED558CCDull;
  return h;
}
uint64_t Mix64Bytes(uint64_t h, const uint8_t* p) {
  for (int i = 0; i < 8; ++i) {
    uint64_t v;
    std::memcpy(&v, p + 8 * i, 8);
    h = Mix(h, v);
  }
  return h;
}

struct Last {
  uint64_t sig = 0;
  std::vector<uint8_t> bytes;
  uint32_t stable = 0;   // calls in a row with the same inputs and the same calls
  uint32_t replays = 0;  // since it became replayable
  uint8_t device_fails = 0;  // checks in a row whose device differed (see the check)
  bool unreplayable = false;
  bool bad = false;  // same inputs made different calls, or a check failed: never replayed
};
struct Window {
  uint64_t calls = 0, ns = 0, first = 0, same_in = 0, same_in_same_out = 0, same_in_diff_out = 0, diff_in = 0,
           diff_in_same_out = 0, entries = 0, bytes = 0;
  uint64_t replayed = 0, replay_ns = 0, checks = 0, check_ns = 0, check_fail_calls = 0, check_fail_device = 0,
           newly_bad = 0, unreplayable = 0;
  uint64_t calls_by[kVariants] = {};
  uint64_t diff_kind[kKinds] = {};            // the first differing entry's kind (same inputs, different effects)
  std::map<uint32_t, uint32_t> fail_offsets;  // failed checks: the first differing device offset
  std::string example;
  uint64_t kind_ns[kKinds] = {}, kind_n[kKinds] = {};
  uint64_t tick = 0;
  uint64_t ab_ns[2] = {}, ab_calls[2] = {};  // me2_replay_ab_s: [0] replay off, [1] on  // replayed setter calls by kind (both cvars on)
  std::chrono::steady_clock::time_point start{};
  uint64_t frames0 = 0;
};
Window g_w;
std::unordered_map<uint64_t, Last> g_last;
std::atomic<std::thread::id> g_owner{};   // the one thread whose calls are compared (UE3's render thread)
std::atomic<uint64_t> g_owner_frame{0};   // the frame of its last call (g_sa2_game_frames)
std::atomic<uint64_t> g_other_thread{0};  // calls of other threads while the owner was active
uint64_t g_bad_keys = 0;

}  // namespace

// The observed setters' arguments, at the outermost observed call inside a recorded SetMeshRenderState.
void Note(uint32_t fn, const PPCContext& ctx, const uint8_t* base) {
  Rec& r = *static_cast<Rec*>(t_rec);
  const uint32_t sp = ctx.r1.u32;
  switch (fn) {
    case 0x82A92C50u:
    case 0x82A92D28u: {  // (dev, start, src, count, mask)
      const uint32_t count = ctx.r6.u32;
      r.Kind(fn == 0x82A92C50u ? kVsConst : kPsConst);
      r.Pointer(ctx.r3.u32, sp);
      r.Put(ctx.r4.u32);
      r.Put(count > 512 ? 0u : count);
      r.Put(ctx.r7.u64);
      if (count > 512) {
        r.unreplayable = true;
        break;
      }
      r.Data(base, ctx.r5.u32, count * 16);
      break;
    }
    case 0x822EB710u:
    case 0x822C8C10u: {  // (param, value[, index]): the param's register offset (+2) and size (+4) decide what is read
      const uint32_t param = ctx.r3.u32;
      const bool array = fn == 0x822C8C10u;
      r.Kind(array ? kParam4 : kParam1);
      r.Pointer(param, sp);
      if (Readable(param, 8)) {
        r.Put(L16(base, param + 2));
        r.Put(L16(base, param + 4));
      } else {
        r.Put(uint32_t(0));
        r.unreplayable = true;
      }
      r.Put(ctx.r5.u32);
      r.Data(base, ctx.r4.u32, array ? 16 : 4);
      break;
    }
    case 0x822C8AC8u: {  // (param, matrix)
      r.Kind(kMatrix);
      r.Pointer(ctx.r3.u32, sp);
      r.Data(base, ctx.r4.u32, 64);
      break;
    }
    case 0x827E16A8u: {  // RHISetRasterizerState(initializer): 16 bytes on the caller's stack (826912B8: r1 + 80)
      r.Kind(kRaster);
      r.Data(base, ctx.r3.u32, 16);
      break;
    }
    case 0x8266F820u: {  // the vertex factory set
      r.Kind(kVertexFactory);
      r.Pointer(ctx.r3.u32, sp);
      r.Pointer(ctx.r4.u32, sp);
      r.Pointer(ctx.r5.u32, sp);
      break;
    }
    default:
      break;
  }
}

namespace {

struct Cursor {
  const std::vector<uint8_t>& b;
  size_t i = 0;
  uint32_t U32() {
    uint32_t v = 0;
    if (i + 4 <= b.size()) std::memcpy(&v, &b[i], 4);
    i += 4;
    return v;
  }
  uint16_t U16() {
    uint16_t v = 0;
    if (i + 2 <= b.size()) std::memcpy(&v, &b[i], 2);
    i += 2;
    return v;
  }
  uint64_t U64() {
    uint64_t v = 0;
    if (i + 8 <= b.size()) std::memcpy(&v, &b[i], 8);
    i += 8;
    return v;
  }
  const uint8_t* Bytes(size_t n) {
    const uint8_t* p = i + n <= b.size() ? &b[i] : nullptr;
    i += n;
    return p;
  }
};

std::string Floats(const uint8_t* p, size_t n) {
  std::string f;
  for (size_t j = 0; p && j < n; ++j) {
    const uint32_t w = L32(p, uint32_t(4 * j));
    float x;
    std::memcpy(&x, &w, 4);
    f += fmt::format(" {:.4g}", x);
  }
  return f;
}

// A record decoded entry by entry (diagnostic dump).
std::string Decode(const std::vector<uint8_t>& b) {
  std::string out;
  Cursor c{b};
  while (c.i < b.size()) {
    const uint8_t k = b[c.i++];
    if (k == kVsConst || k == kPsConst) {
      c.U32();
      const uint32_t start = c.U32(), count = c.U32();
      c.U64();
      out += fmt::format(" | {} c{} x{}:{}", k == kVsConst ? "VS" : "PS", start, count,
                         Floats(c.Bytes(size_t(count) * 16), std::min<uint32_t>(count * 4, 8)));
    } else if (k == kParam1 || k == kParam4) {
      const uint32_t param = c.U32();
      const uint16_t reg = c.U16(), size = c.U16();
      const uint32_t index = c.U32();
      const bool arr = k == kParam4;
      out += fmt::format(" | param{} {:08X} reg+{} size {} [{}]:{}", arr ? "[]" : "", param, reg, size, index,
                         Floats(c.Bytes(arr ? 16 : 4), arr ? 4 : 1));
    } else if (k == kMatrix) {
      const uint32_t param = c.U32();
      out += fmt::format(" | matrix {:08X}:{}", param, Floats(c.Bytes(64), 16));
    } else if (k == kRaster) {
      const uint8_t* p = c.Bytes(16);
      out += fmt::format(" | raster {:08X} {:08X}{}", p ? L32(p, 0) : 0, p ? L32(p, 4) : 0,
                         Floats(p ? p + 8 : nullptr, 2));
    } else if (k == kVertexFactory) {
      const uint32_t a = c.U32(), b4 = c.U32(), b5 = c.U32();
      out += fmt::format(" | vf {:08X} {:08X} {:08X}", a, b4, b5);
    } else {
      out += fmt::format(" | ?{}", k);
      break;
    }
  }
  return out;
}

// Calls f with the registers set by `set` and n bytes of data copied below the stack pointer (`set` gets its
// address); the stack pointer is restored afterwards.
template <typename Set>
void CallWithData(PPCFunc* f, PPCContext& ctx, uint8_t* base, const uint8_t* data, uint32_t n, Set set) {
  const uint64_t sp0 = ctx.r1.u64;
  const uint32_t at = (uint32_t(sp0) - 16 - ((n + 15) & ~15u)) & ~15u;  // the data, right below the caller's frame
  const uint32_t sp = at - 128;                                         // the callee's caller frame (back chain, ...)
  if (n && data) std::memcpy(base + at, data, n);
  const uint32_t chain = __builtin_bswap32(uint32_t(sp0));
  std::memcpy(base + sp, &chain, 4);
  ctx.r1.u64 = sp;
  set(at);
  f(ctx, base);
  ctx.r1.u64 = sp0;
}

// Makes the recorded calls.
bool g_kind_timing = false;  // me2_replay and me2_replay_compare: replayed setter calls timed by kind

void Replay(const std::vector<uint8_t>& b, PPCContext& ctx, uint8_t* base) {
  Cursor c{b};
  while (c.i < b.size()) {
    const uint8_t k = b[c.i++];
    struct Timed {
      uint8_t k;
      std::chrono::steady_clock::time_point t0;
      Timed(uint8_t kind) : k(kind), t0(g_kind_timing ? std::chrono::steady_clock::now() : t0) {}
      ~Timed() {
        if (!g_kind_timing || k >= kKinds) return;
        g_w.kind_ns[k] += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        ++g_w.kind_n[k];
      }
    } timed(k);
    switch (k) {
      case kVsConst:
      case kPsConst: {
        const uint32_t dev = c.U32(), start = c.U32(), count = c.U32();
        const uint64_t mask = c.U64();
        const uint8_t* d = c.Bytes(size_t(count) * 16);
        CallWithData(k == kVsConst ? sub_82A92C50 : sub_82A92D28, ctx, base, d, count * 16, [&](uint32_t at) {
          ctx.r3.u64 = dev;
          ctx.r4.u64 = start;
          ctx.r5.u64 = at;
          ctx.r6.u64 = count;
          ctx.r7.u64 = mask;
        });
        break;
      }
      case kParam1:
      case kParam4: {
        const uint32_t param = c.U32();
        c.U16(), c.U16();
        const uint32_t index = c.U32();
        const uint32_t n = k == kParam4 ? 16 : 4;
        const uint8_t* d = c.Bytes(n);
        CallWithData(k == kParam4 ? sub_822C8C10 : sub_822EB710, ctx, base, d, n, [&](uint32_t at) {
          ctx.r3.u64 = param;
          ctx.r4.u64 = at;
          ctx.r5.u64 = index;
        });
        break;
      }
      case kMatrix: {
        const uint32_t param = c.U32();
        const uint8_t* d = c.Bytes(64);
        CallWithData(sub_822C8AC8, ctx, base, d, 64, [&](uint32_t at) {
          ctx.r3.u64 = param;
          ctx.r4.u64 = at;
        });
        break;
      }
      case kRaster: {
        const uint8_t* d = c.Bytes(16);
        CallWithData(sub_827E16A8, ctx, base, d, 16, [&](uint32_t at) { ctx.r3.u64 = at; });
        break;
      }
      case kVertexFactory: {
        const uint32_t r3 = c.U32(), r4 = c.U32(), r5 = c.U32();
        CallWithData(sub_8266F820, ctx, base, nullptr, 0, [&](uint32_t) {
          ctx.r3.u64 = r3;
          ctx.r4.u64 = r4;
          ctx.r5.u64 = r5;
        });
        break;
      }
      default:
        return;  // not reached: records come from Note
    }
  }
}

// The kind of the entry that contains the first differing byte of two records.
uint8_t FirstDiffKind(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  const size_t n = std::min(a.size(), b.size());
  size_t d = 0;
  while (d < n && a[d] == b[d]) ++d;
  Cursor c{a};
  uint8_t kind = 0;
  while (c.i < a.size() && c.i <= d) {
    kind = a[c.i++];
    switch (kind) {
      case kVsConst:
      case kPsConst: {
        c.U32(), c.U32();
        const uint32_t count = c.U32();
        c.U64();
        c.i += size_t(count) * 16;
        break;
      }
      case kParam1: c.i += 12 + 4; break;
      case kParam4: c.i += 12 + 16; break;
      case kMatrix: c.i += 4 + 64; break;
      case kRaster: c.i += 16; break;
      case kVertexFactory: c.i += 12; break;
      default: return 0;
    }
  }
  return kind < kKinds ? kind : 0;
}

void Report(std::chrono::steady_clock::time_point now) {
  Window& w = g_w;
  const double s = std::chrono::duration<double>(now - w.start).count();
  const uint64_t frames = g_sa2_game_frames.load(std::memory_order_relaxed) - w.frames0;
  const double f = double(std::max<uint64_t>(frames, 1));
  std::string by, kinds, offsets;
  for (int i = 0; i < kVariants; ++i)
    if (w.calls_by[i]) by += fmt::format(" {:08X} {:.0f}", kVariantAddr[i], double(w.calls_by[i]) / f);
  for (int k = 1; k < kKinds; ++k)
    if (w.diff_kind[k]) kinds += fmt::format(" {} {};", kKindName[k], w.diff_kind[k]);
  for (const auto& [off, n] : w.fail_offsets) offsets += fmt::format(" +{:X} x{}", off, n);
  const auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
  const auto us = [](uint64_t ns, uint64_t n) { return n ? double(ns) / double(n) / 1000.0 : 0.0; };
  const uint64_t all = w.calls + w.replayed + w.checks;
  REXLOG_INFO("[replay] {:.0f} s: {} mesh setups ({:.0f} a frame); REPLAYED {} ({:.1f} %, {:.2f} us each), checked {} "
              "({:.2f} us each; failed: calls {}, device {}{}), original {} ({:.2f} us each, recorded; a frame by "
              "variant{}); keys never replayed {} (new {}), not replayable records {}",
              s, all, double(all) / f, w.replayed, pct(w.replayed, all), us(w.replay_ns, w.replayed), w.checks,
              us(w.check_ns, w.checks), w.check_fail_calls, w.check_fail_device,
              offsets.empty() ? "" : "; first differing device offsets" + offsets, w.calls, us(w.ns, w.calls), by,
              g_bad_keys, w.newly_bad, w.unreplayable);
  std::string kinds_t;
  for (int k = 1; k < kKinds; ++k)
    if (w.kind_n[k])
      kinds_t += fmt::format(" {} {:.1f} a replay, {:.3f} us each;", kKindName[k],
                             w.replayed ? double(w.kind_n[k]) / double(w.replayed + w.checks) : 0.0,
                             us(w.kind_ns[k], w.kind_n[k]));
  if (!kinds_t.empty()) REXLOG_INFO("[replay]   replayed setter calls:{}", kinds_t);
  if (w.ab_calls[0] && w.ab_calls[1]) {
    const double per_frame = double(w.ab_calls[0] + w.ab_calls[1]) / f;
    const double off = us(w.ab_ns[0], w.ab_calls[0]), on = us(w.ab_ns[1], w.ab_calls[1]);
    REXLOG_INFO("[replay]   A/B: per-mesh setup with the replay {:.3f} us, without {:.3f} us ({} / {} calls); at {:.0f} "
                "a frame: {:.3f} ms a frame with, {:.3f} without, saving {:.3f} ms",
                on, off, w.ab_calls[1], w.ab_calls[0], per_frame, on * per_frame / 1000.0, off * per_frame / 1000.0,
                (off - on) * per_frame / 1000.0);
  }
  REXLOG_INFO("[replay]   originals: {:.1f} entries / {:.0f} bytes recorded each; {} first seen; SAME inputs as last "
              "time {} ({:.1f} %): identical effects {} ({:.2f} % of them), DIFFERENT effects {}{}{}; changed inputs {} "
              "(identical effects anyway {}); {} calls on other threads not compared",
              w.calls ? double(w.entries) / double(w.calls) : 0.0, w.calls ? double(w.bytes) / double(w.calls) : 0.0,
              w.first, w.same_in, pct(w.same_in, w.calls), w.same_in_same_out, pct(w.same_in_same_out, w.same_in),
              w.same_in_diff_out, kinds.empty() ? "" : " (first differing entry:" + kinds + ")",
              w.example.empty() ? "" : " e.g. " + w.example, w.diff_in, w.diff_in_same_out,
              g_other_thread.exchange(0, std::memory_order_relaxed));
  g_w = Window{};
  g_w.start = now;
  g_w.frames0 = g_sa2_game_frames.load(std::memory_order_relaxed);
  if (g_last.size() > 200000) {  // levels change: start over rather than grow without bound
    g_last.clear();
    g_bad_keys = 0;
  }
}

void MarkBad(Last& l) {
  if (!l.bad) {
    l.bad = true;
    ++g_bad_keys;
    ++g_w.newly_bad;
  }
}

uint64_t NsSince(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

template <int V, PPCFunc* Original>
void MeshSetupBody(PPCContext& ctx, uint8_t* base) {
  static const bool compare = REXCVAR_GET(me2_replay_compare);
  static const bool replay = REXCVAR_GET(me2_replay);
  static const uint32_t period_mask = uint32_t(std::max(1, REXCVAR_GET(me2_replay_check_period))) - 1;
  static const bool kind_timing = (g_kind_timing = compare && replay);
  static const bool reserved = (g_last.reserve(1u << 15), true);  // no rehashing at a level start
  (void)kind_timing, (void)reserved;
  if ((!compare && !replay) || t_rec) {
    Original(ctx, base);
    return;
  }
  // Timing (us each in the report) only for the diagnostics: two clock reads are a large part of a replay.
  const auto t0 = compare ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  // One thread at a time: UE3's rendering thread. It is stopped for level loads and a new one started afterwards
  // (console 11-06): a thread takes over once the previous owner has made no call for 30 frames.
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const std::thread::id owner = g_owner.load(std::memory_order_relaxed);
  if (owner != std::this_thread::get_id()) [[unlikely]] {
    if (owner != std::thread::id{} && frame - g_owner_frame.load(std::memory_order_relaxed) < 30) {
      g_other_thread.fetch_add(1, std::memory_order_relaxed);
      Original(ctx, base);
      return;
    }
    g_owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
  }
  if (g_owner_frame.load(std::memory_order_relaxed) != frame) g_owner_frame.store(frame, std::memory_order_relaxed);
  Window& w = g_w;
  // The report: the clock is read every 1024 calls.
  if ((++w.tick & 1023) == 0) [[unlikely]] {
    const auto now = std::chrono::steady_clock::now();
    if (w.start == std::chrono::steady_clock::time_point{}) {
      w.start = now;
      w.frames0 = frame;
    } else if (now - w.start >= std::chrono::seconds(10)) {
      Report(now);
    }
  }
  const uint32_t policy = ctx.r3.u32, view = ctx.r4.u32, r5 = ctx.r5.u32, mesh = ctx.r6.u32, pass = ctx.r7.u32;
  const uint32_t r8 = V > 0 ? ctx.r8.u32 : 0;
  // The view is part of the key (PC 11-06 20:37: one mesh drawn in two views a frame alternated its matrix).
  const uint64_t key = Mix(Mix(Mix(Mix(uint64_t(V) << 8 | (pass & 0xFF), policy), mesh), view), 0x52455031ull);
  // The policy's contents are inputs (PC 11-06 20:41: the same stack address with two different shaders).
  uint64_t sig = Mix(Mix(0, r5), r8);
  if (Readable(mesh, 96)) sig = Mix64Bytes(sig, base + mesh + 32);
  if (Readable(policy, 64)) sig = Mix64Bytes(sig, base + policy);
  auto [it, fresh] = g_last.try_emplace(key);
  Last& l = it->second;

  if (replay && !fresh && !l.bad && !l.unreplayable && l.sig == sig && l.stable >= kStable) {
    const uint32_t n = l.replays++;
    if (n >= kChecked && (n & period_mask) != 0) {
      Replay(l.bytes, ctx, base);
      ++w.replayed;
      if (compare) w.replay_ns += NsSince(t0, std::chrono::steady_clock::now());
      return;
    }
    // Checked: the replay, then the original on top (recorded); the device must not change and the calls must match.
    static thread_local std::vector<uint8_t> after_replay(kDeviceBytes), after_original(kDeviceBytes);
    static thread_local Rec check;
    const uint32_t dev = L32(base, kDevicePtr);
    const bool dev_ok = Readable(dev, kDeviceBytes);
    const auto r3 = ctx.r3, r4 = ctx.r4, r5r = ctx.r5, r6 = ctx.r6, r7 = ctx.r7, r8r = ctx.r8, r9 = ctx.r9,
               r10 = ctx.r10;
    Replay(l.bytes, ctx, base);  // clobbers the argument registers: the original gets them back
    ctx.r3 = r3, ctx.r4 = r4, ctx.r5 = r5r, ctx.r6 = r6, ctx.r7 = r7, ctx.r8 = r8r, ctx.r9 = r9, ctx.r10 = r10;
    if (dev_ok) std::memcpy(after_replay.data(), base + dev, kDeviceBytes);
    check.bytes.clear();
    check.entries = 0;
    check.unreplayable = false;
    t_rec = &check;
    Original(ctx, base);
    t_rec = nullptr;
    if (dev_ok) std::memcpy(after_original.data(), base + dev, kDeviceBytes);
    ++w.checks;
    const bool calls_same = check.bytes == l.bytes;
    uint32_t d = 0;
    if (dev_ok)
      while (d < kDeviceBytes && after_replay[d] == after_original[d]) ++d;
    const bool device_same = !dev_ok || d == kDeviceBytes;
    if (!calls_same || !device_same) {
      if (!calls_same) ++w.check_fail_calls;
      if (!device_same) {
        ++w.check_fail_device;
        ++w.fail_offsets[d & ~3u];
      }
      static int logged = 0;
      if (logged < 6) {
        ++logged;
        REXLOG_INFO("[replay] check failed: {:08X} mesh {:08X} pass {} view {:08X}: calls {}, device {}; record:{}{}",
                    kVariantAddr[V], mesh, pass, view, calls_same ? "same" : "DIFFERENT",
                    device_same ? std::string("same")
                                : fmt::format("differs at +{:X} ({:02X} after the replay, {:02X} after the original)", d,
                                              after_replay[d], after_original[d]),
                    Decode(l.bytes), calls_same ? std::string() : " || original:" + Decode(check.bytes));
      }
      // Different calls: never replayed. A device difference alone: twice in a row. The D3D device also changes under
      // other threads (PC 11-06 21:05: 3 of 66,752 checks, one byte at +2AF3 or +40BB off by one, a different mesh each
      // time: counters of the swap path); a write the replay misses differs at every check.
      if (!calls_same || ++l.device_fails >= 2) MarkBad(l);
    } else {
      l.device_fails = 0;
    }
    if (compare) w.check_ns += NsSince(t0, std::chrono::steady_clock::now());
    return;
  }

  // The original, recorded.
  static thread_local Rec rec;
  rec.bytes.clear();
  rec.entries = 0;
  rec.unreplayable = false;
  t_rec = &rec;
  Original(ctx, base);
  t_rec = nullptr;
  ++w.calls;
  ++w.calls_by[V];
  if (compare) w.ns += NsSince(t0, std::chrono::steady_clock::now());
  w.entries += rec.entries;
  w.bytes += rec.bytes.size();
  if (rec.unreplayable) ++w.unreplayable;
  if (fresh) {
    ++w.first;
    l.sig = sig;
    l.bytes = rec.bytes;
    l.unreplayable = rec.unreplayable;
  } else {
    const bool same_out = l.bytes == rec.bytes;
    if (l.sig == sig) {
      ++w.same_in;
      if (same_out) {
        ++w.same_in_same_out;
        ++l.stable;
      } else {
        ++w.same_in_diff_out;
        MarkBad(l);
        static int dumped = 0;
        if (dumped < 2) {
          ++dumped;
          REXLOG_INFO("[replay] same inputs, different effects: {:08X} mesh {:08X} pass {} view {:08X} r5 {:08X} r8 "
                      "{:08X}; before:{} || now:{}",
                      kVariantAddr[V], mesh, pass, view, r5, r8, Decode(l.bytes), Decode(rec.bytes));
        }
        const uint8_t k = FirstDiffKind(l.bytes, rec.bytes);
        ++w.diff_kind[k];
        if (w.example.empty())
          w.example = fmt::format("[{:08X} mesh {:08X} pass {}, {}]", kVariantAddr[V], mesh, pass, kKindName[k]);
      }
    } else {
      ++w.diff_in;
      if (same_out) ++w.diff_in_same_out;
      l.stable = 0;
      l.replays = 0;
    }
    l.sig = sig;
    if (!same_out) {
      l.bytes = rec.bytes;
      l.stable = 0;
      l.replays = 0;
    }
    l.unreplayable = rec.unreplayable;
  }
}

// me2_replay_ab_s: the whole setup timed, the replay on and off by turns (the owner thread only: UE3's render thread).
template <int V, PPCFunc* Original>
void MeshSetup(PPCContext& ctx, uint8_t* base) {
  static const int ab_s = REXCVAR_GET(me2_replay_ab_s);
  if (ab_s <= 0 || t_rec || g_owner.load(std::memory_order_relaxed) != std::this_thread::get_id()) {
    MeshSetupBody<V, Original>(ctx, base);
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const int on = int(std::chrono::duration_cast<std::chrono::seconds>(t0.time_since_epoch()).count() / ab_s) & 1;
  if (on) {
    MeshSetupBody<V, Original>(ctx, base);
  } else {
    Original(ctx, base);
  }
  g_w.ab_ns[on] += NsSince(t0, std::chrono::steady_clock::now());
  ++g_w.ab_calls[on];
}

}  // namespace
}  // namespace me2::replay

#define ME2_REPLAY_MESH_SETUP(V, ADDR)  \
  REX_EXTERN(__imp__sub_##ADDR);        \
  REX_HOOK_RAW(sub_##ADDR) { me2::replay::MeshSetup<V, __imp__sub_##ADDR>(ctx, base); }
ME2_REPLAY_MESH_SETUP(0, 82679ED0)
ME2_REPLAY_MESH_SETUP(1, 8267A028)
ME2_REPLAY_MESH_SETUP(2, 8267A1A8)
ME2_REPLAY_MESH_SETUP(3, 8267A328)
ME2_REPLAY_MESH_SETUP(4, 8267A4C8)
