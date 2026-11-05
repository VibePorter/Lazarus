/**
 * @file    gpu/backend/queued.cpp
 */
#include "renut_engine/gpu/backend/queued.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace {
// ME2 (2026-10-28): 'units' blocks of 64 bytes (the deltas' constant groups, 3.5-6 a draw) copied inline. With the
// size not constant the compiler called the library memcpy each time (memmove: 6 % of the UE3 render thread's own
// samples and 4 % of Renut's in the console log 02:26). Explicit NEON: a plain loop of fixed-size memcpy would be turned
// back into one library call (loop distribution).
inline void CopyUnits64(unsigned char *to, const unsigned char *from, unsigned units) {
#if defined(__aarch64__)
  for (unsigned i = 0; i < units; ++i, to += 64, from += 64) {
    const uint8x16x4_t v = vld1q_u8_x4(from);
    vst1q_u8_x4(to, v);
  }
#else
  std::memcpy(to, from, size_t(units) * 64);
#endif
}
}  // namespace

#include <fmt/format.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/guest/callbacks.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/sa2_line_compare.h"
#include "rex/system/xmemory.h"

#include "../../../../me2_src/me2_watch.h"

// me2-nx-deko (2026-11-04): the watched range (me2_watch.h, tools/watch_marks.py), defined here. The runtime's host
// writers (the CRT memory hooks) reach it through rex::memory::SetWatchedRange.
RexWatchedRange g_me2_watch = {};
static_assert(sizeof(RexWatchedRange) == sizeof(rex::memory::WatchedRange) &&
              offsetof(RexWatchedRange, dirty) == offsetof(rex::memory::WatchedRange, dirty));
extern "C" void Me2CensusDeviceStart();  // me2_write_census.cpp (diagnostic builds; a no-op otherwise)
extern "C" void Me2CensusDeviceTick();

// sa2-nx (2026-10-20): see SnapshotKind below.
REXCVAR_DEFINE_BOOL(sa2_renut_snapshot_deltas, true, "SA2/Renderer",
                    "Renut's renderer: a draw keeps what changed in the device since the device's previous draw (its "
                    "64-byte chunks that differ, and the float constant groups the game marked dirty) instead of a "
                    "copy of the device's first 13,088 bytes; the render thread rebuilds the device from them. Off: "
                    "the whole copy for every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_snapshot_deltas_constants, true, "SA2/Renderer",
                    "Renut's renderer (sa2_renut_snapshot_deltas): the chain's draws tell the constant uploads which "
                    "float constant groups changed since the previous draw, so only those are compared with what the "
                    "GPU holds (as a recorded command buffer's replayed draws do); a frame's first draw compares them "
                    "all. Off: every draw compares every group its shaders read")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_capture_batches, true, "SA2/Renderer",
                    "Renut's renderer (with banjo_renut_capture_pool): the game thread takes its draws' captures from "
                    "the pool, and the render thread gives them back, 32 at a time instead of one lock of the pool's "
                    "per draw on each thread. Off: one at a time")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_snapshot_deltas_narrow, true, "SA2/Renderer",
                    "Renut's renderer (sa2_renut_snapshot_deltas): the game thread compares only the device chunks the "
                    "render thread reads (40 of 77: not the dirty masks, ring pointers and call tables before the fetch "
                    "constants, nor the private words from 11008 to 12223, which only the hooks use on the live "
                    "device). The second console run: comparing all 77 was 9 % of the game thread. Off: all 77")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_deferred_chain, true, "SA2/Renderer",
                    "Renut's renderer (from futari-nx, as peggle2-nx has it): Begin*Vertices draws are deltas of the "
                    "device snapshot chain too, not whole 13 KB copies (sa2_renut_snapshot_deltas). Made and queued "
                    "under the chain's lock; every hook queues its device's waiting Begin*Vertices draw first, so the "
                    "chain stays in order. Off: whole copies outside the chain, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-10-27): console stacks of 22:5x (Omega, 13.9 fps): CaptureDelta still 9 % of the UE3 render thread
// with the batched compare, its samples on the chunk loads themselves: the device and shadow lines (39 + 39) are out of
// L1 again at every draw (the game's code between draws evicts them).
// 23:1x console run (build 23:04): no gain (CaptureDelta 9.1 %, its loads still wait), the prefetch loop itself +2 % of
// the UE3 render thread in the draw hook -> off by default.
REXCVAR_DEFINE_BOOL(me2_renut_delta_prefetch, false, "SA2/Renderer",
                    "Prefetch the device and shadow lines the delta chain compares at the start of QueueDraw, before "
                    "the deferred-draw flush, the ready callbacks and the chain's lock")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-10-31): a measurement for a cheaper CaptureDelta. Only 2-3 of the 39 compared chunks change a draw
// (console logs of 10-30/10-31), yet all 39 lines are loaded each draw (memory-bound: ~9 % of the UE3 render thread).
// The XDK marks what each Set* call changed in the device's D3DTAGCOLLECTION (m_Pending[5]; words 0/1 are the float
// constants this chain already uses). If words 2-4 cover every chunk the compare finds changed, the compare can be
// limited to the tagged chunks. This logs, per changed chunk, how often it changed with no tag set at all and which tag
// bits came with it ("[delta-tags]", every 60 s). Diagnostic only: it changes nothing.
REXCVAR_DEFINE_BOOL(me2_diag_delta_tags, false, "SA2/Renderer",
                    "Diagnostic: relate the device chunks the delta chain finds changed to the XDK's dirty tags "
                    "(m_Pending[2..4]); [delta-tags] lines every 60 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_neon_compare, true, "SA2/Renderer",
                    "Renut's renderer on the Switch: the device chunks of the delta chain and the pushed shared "
                    "constants' lines are compared 64 bytes at a time with NEON (four 128-bit loads a side) instead of "
                    "eight 64-bit words, which GCC left scalar. Off: the 64-bit words")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-04): the watch marks (tools/watch_marks.py). The functions that write the D3D device (app/
// watch_writers.txt, from the device census) mark the 64-byte chunks they write; a delta compares only those instead
// of all 39 (CaptureDelta was ~7 % of UE3's render thread on the console, mostly misses on the device and shadow lines).
// On by default since the PC verify of 2026-11-04 (10.7 M draws of menus, fights and conversations: 0 changed chunks
// left unmarked; ~9 chunks compared a delta instead of 39).
REXCVAR_DEFINE_BOOL(me2_renut_watch_chunks, true, "ME2/Renderer",
                    "A draw's device delta compares only the chunks the instrumented D3D writers marked "
                    "(tools/watch_marks.py; needs a build with app/watch_writers.txt)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_renut_watch_verify, false, "ME2/Renderer",
                    "Tests (me2_renut_watch_chunks): every delta also compares the unmarked chunks, takes those that "
                    "changed anyway and reports them ([watch] unmarked lines; counts in the 10 s device snapshot line)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_snapshot_deltas_verify, false, "SA2/Renderer",
                    "Tests (sa2_renut_snapshot_deltas): every draw also keeps the whole copy, and the render thread "
                    "counts the draws whose rebuilt device differs from it in any byte (the 10 s \"[renut] device "
                    "snapshots\" line)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {

namespace {

// sa2-nx (2026-10-20, sa2_renut_snapshot_deltas): how a queued draw holds its device.
//   Whole:  DrawCapture::state is the device's first kDeviceSnapshotSize bytes (every draw before; the Begin*Vertices
//           draws, which are queued later than they are captured, still).
//   Rebase: the same, and the render thread takes it as its device (the chain's first draw, and each frame's).
//   Delta:  DrawCapture::state is a DeltaHeader and the bytes it names: the render thread applies them to its device.
// SA2 records no command buffers, so every draw was a whole copy: 13 KB a draw, 15-18 MB a frame of City Escape's
// 1,100-1,400 draws written on the game thread, into pooled captures last touched by the render thread on another
// core, then read back by the render thread. A draw changes a few bindings, render states and constants: the deltas are
// those. The chain's order is the queue's: deltas are made and queued under g_delta.mutex, and the render thread runs
// the queue in order.
enum class SnapshotKind : u8 { Whole, Rebase, Delta };

// What a queued draw reads besides guest memory the game keeps until a fence.
struct DrawCapture {
  std::vector<u8> state;  // the device, kDeviceSnapshotSize bytes (or a delta: SnapshotKind)
  std::vector<u8> vertices, indices;
  SnapshotKind snapshot = SnapshotKind::Whole;  // sa2-nx (2026-10-20)
  std::vector<u8> verify;  // sa2-nx: the whole device as well (sa2_renut_snapshot_deltas_verify)
  DrawCall call;
  // banjo-nx (2026-10-19, CPU proposal 23): a queued clear's or resolve's arguments. They come from the same pool as
  // the draws' captures: each made three heap allocations on the game thread (its arguments, a device snapshot, a
  // closure too large for std::function's own storage), freed on the render thread through the one malloc lock.
  u32 flags = 0;
  bool has_rect = false, has_color = false, has_dest_format = false;
  ClearRect rect{};
  float color[4] = {};
  float depth = 0.0f;
  u32 stencil = 0;
  u32 dest_texture_va = 0, dest_level = 0, dest_slice = 0;
  i32 dest_x = 0, dest_y = 0;
  d3d::TextureFetchConstant dest_format{};
};

// banjo-nx (banjo_renut_capture_pool): a draw's capture comes from a pool and goes back to it once the render thread
// has drawn it. Each draw made three heap allocations on the game thread (the capture, its 12.7 KB device snapshot,
// the queued closure) and freed them on the render thread: about 2,000 a frame through the Switch's one malloc lock.
// A pooled capture keeps its snapshot's storage, and the closure holds a plain pointer, which std::function stores
// without allocating. Off: every capture is deleted after use, as before (a new one per draw).
// sa2-nx (2026-10-20, sa2_renut_capture_batches): the pool's lock was taken twice a draw, by the game thread (Take) and
// by the render thread (Give), on two cores: its cache line went back and forth about 1,800 times a City Escape frame.
// Each thread now keeps a few captures of its own and goes to the pool kBatch at a time.
bool CaptureBatchesOn();

class CapturePool {
public:
  // sa2-nx (2026-10-20): 'chain_locked' when the caller holds g_delta.mutex (the chain's draws): its batch is a plain
  // list that lock guards. Thread-local lists cost a call a use here (this toolchain's TLS goes through
  // __aarch64_read_tp: 0.8 % of the render thread in the second console run).
  DrawCapture *Take(bool chain_locked = false) {
    if (CaptureBatchesOn() && chain_locked) {
      std::vector<DrawCapture *> &taken = chain_taken_;
      if (taken.empty()) {
        std::lock_guard lock(mutex_);
        const size_t n = std::min(kBatch, free_.size());
        taken.assign(free_.end() - std::ptrdiff_t(n), free_.end());
        free_.resize(free_.size() - n);
      }
      if (!taken.empty()) {
        DrawCapture *capture = taken.back();
        taken.pop_back();
        // me2-nx-deko (2026-11-03): the next draw's capture was last written by the render thread's core (Give): its
        // first lines start coming now, a whole UE3 draw (~15 us) before that draw takes it.
        if (!taken.empty()) {
          const char *next = reinterpret_cast<const char *>(taken.back());
          __builtin_prefetch(next, 1);
          __builtin_prefetch(next + 64, 1);
          __builtin_prefetch(next + 128, 1);
        }
        return capture;
      }
      return new DrawCapture();
    }
    {
      std::lock_guard lock(mutex_);
      if (!free_.empty()) {
        DrawCapture *capture = free_.back();
        free_.pop_back();
        return capture;
      }
    }
    return new DrawCapture();
  }
  // sa2-nx (2026-10-20): 'render_thread' from the render thread's items (only that thread then touches its batch; a
  // queue run inline at shutdown has the render thread gone).
  void Give(DrawCapture *capture, bool render_thread = false) {
    if (!capture)
      return;
    if (banjo::renut_gpu::CapturePoolOn()) {
      capture->vertices.clear();
      capture->indices.clear();
      if (CaptureBatchesOn() && render_thread) {
        std::vector<DrawCapture *> &given = render_given_;
        given.push_back(capture);
        if (given.size() < kBatch)
          return;
        std::lock_guard lock(mutex_);
        for (DrawCapture *c : given) {
          if (free_.size() < kMaxFree)
            free_.push_back(c);
          else
            delete c;
        }
        given.clear();
        return;
      }
      std::lock_guard lock(mutex_);
      if (free_.size() < kMaxFree) {
        free_.push_back(capture);
        return;
      }
    }
    delete capture;
  }

private:
  static constexpr size_t kMaxFree = 2048;  // more than a frame's draws in flight
  static constexpr size_t kBatch = 32;      // sa2-nx: captures a thread takes or gives back at once
  std::mutex mutex_;
  std::vector<DrawCapture *> free_;
  std::vector<DrawCapture *> chain_taken_;   // sa2-nx: under g_delta.mutex
  std::vector<DrawCapture *> render_given_;  // sa2-nx: the render thread's
};
CapturePool g_capture_pool;

// A Begin*Vertices draw waiting for its vertices, per device.
struct DeferredDraw {
  DrawCapture *capture = nullptr;  // owned (g_capture_pool)
  const u8 *scratch = nullptr;
  u32 bytes = 0;
  const u8 *index_scratch = nullptr;  // me2-nx-deko: BeginIndexedVertices
  u32 index_bytes = 0;
};
std::mutex g_deferred_mutex;
// ME2 (2026-10-28): a few slots by device instead of an unordered_map. Each Begin*Vertices draw (Umbra's ~400 test
// boxes a frame) inserted a map node and its flush erased it: a malloc and a free through the Switch's one malloc lock
// per draw (console stacks: rmutexLock <- _free_r <- _Hashtable <- FlushDeferredVertices). There is one device.
class DeferredSlots {
 public:
  using Slot = std::pair<u32, DeferredDraw>;
  DeferredSlots() { slots_.reserve(8); }
  Slot *begin() { return slots_.data(); }
  Slot *end() { return slots_.data() + slots_.size(); }
  Slot *find(u32 device_va) {
    for (Slot &s : slots_)
      if (s.first == device_va)
        return &s;
    return end();
  }
  void erase(Slot *it) {
    if (it != &slots_.back())
      *it = std::move(slots_.back());
    slots_.pop_back();
  }
  DeferredDraw &operator[](u32 device_va) {
    Slot *it = find(device_va);
    if (it != end())
      return it->second;
    slots_.push_back({device_va, DeferredDraw{}});
    return slots_.back().second;
  }
  size_t size() const { return slots_.size(); }

 private:
  std::vector<Slot> slots_;
};
DeferredSlots g_deferred;
// sa2-nx (2026-10-20, sa2_renut_callbacks_fast): g_deferred.size(), kept under g_deferred_mutex and read without it
// (FlushDeferredVertices: every hook calls it first). A Begin*Vertices draw is flushed by the next hook of its device,
// on the thread that made it, which sees the count in program order.
std::atomic<size_t> g_deferred_count{0};

// sa2-nx (2026-10-20, sa2_renut_snapshot_deltas): the delta chain's layout. Of the device's first kDeviceSnapshotSize
// bytes, the float constants (VertexShaderF, PixelShaderF: 8 KB, two thirds) go by the groups m_Pending marks dirty
// (one bit per four registers, bit 63 - group, as the recorded command buffers keep them: command_buffers.cpp); the
// rest goes by 64-byte chunks (30 before the constants, 47 after them, the last one 32 bytes), those that differ from
// the chain's previous draw (compared on the game thread with its shadow).
constexpr u32 kDeltaVertexF = u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderF));
constexpr u32 kDeltaPixelF = u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, PixelShaderF));
constexpr u32 kDeltaTailBegin = kDeltaPixelF + 256 * 16;
constexpr u32 kDeltaUnitBytes = 64;  // a chunk, and a constant group
constexpr u32 kDeltaHeadChunks = kDeltaVertexF / kDeltaUnitBytes;
constexpr u32 kDeltaTailBytes = d3d::kDeviceSnapshotSize - kDeltaTailBegin;
constexpr u32 kDeltaChunks = kDeltaHeadChunks + (kDeltaTailBytes + kDeltaUnitBytes - 1) / kDeltaUnitBytes;
static_assert(kDeltaVertexF % kDeltaUnitBytes == 0 && kDeltaPixelF == kDeltaVertexF + 256 * 16);
static_assert(kDeltaTailBegin == u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderB)));
static_assert(kDeltaChunks <= 128);
static_assert(kDeltaHeadChunks == kChainHeadChunks && kDeltaTailBegin == kChainTailBegin);  // draw.h's ChainHint

constexpr u32 DeltaChunkOffset(u32 i) {
  return i < kDeltaHeadChunks ? i * kDeltaUnitBytes : kDeltaTailBegin + (i - kDeltaHeadChunks) * kDeltaUnitBytes;
}
constexpr u32 DeltaChunkSize(u32 i) {
  return i < kDeltaHeadChunks ? kDeltaUnitBytes
                              : std::min(kDeltaUnitBytes, kDeltaTailBytes - (i - kDeltaHeadChunks) * kDeltaUnitBytes);
}

// sa2-nx (2026-10-20, sa2_renut_snapshot_deltas_narrow): the device bytes the render thread never reads from a draw's
// device (DrawStateLocked and what it calls: the accessors of render/draw.cpp, textures.cpp, surfaces.cpp, the pipeline
// key): before the fetch constants (the dirty masks, ring pointers and Set/Get*State call tables, which a recorded
// command buffer's replay leaves zero too), and the private words 11008-12223 (the creation thread, the fences, the
// swap flags: the hooks read and write them on the live device). Their chunks are not compared; the render thread's
// device keeps them as the frame's whole copy had them.
constexpr u32 kDeltaUnreadEnd = kDeltaVertexF - 0x300;  // 0x480: the fetch constants' start
constexpr u32 kDeltaPrivateUnreadBegin = 10880, kDeltaPrivateUnreadEnd = 11776;  // me2-nx-deko: N&B/ME2 10880 to the 64-byte chunk before the vertex declaration (11824)
constexpr bool DeltaChunkRead(u32 i) {
  const u32 at = DeltaChunkOffset(i);
  return !(at < kDeltaUnreadEnd || (at >= kDeltaPrivateUnreadBegin && at + 64 <= kDeltaPrivateUnreadEnd));
}
static_assert(kDeltaUnreadEnd == 0x480 && kDeltaUnreadEnd % kDeltaUnitBytes == 0);
static_assert((kDeltaPrivateUnreadBegin - kDeltaTailBegin) % kDeltaUnitBytes == 0 &&
              (kDeltaPrivateUnreadEnd - kDeltaTailBegin) % kDeltaUnitBytes == 0);
static_assert(d3d::kDeviceOffsetVertexDeclaration >= kDeltaPrivateUnreadEnd);
// The chunks compared: all, or the read ones.
struct DeltaChunkList {
  u32 count = 0;
  u8 index[128] = {};
  u16 offset[128] = {};  // me2-nx-deko (2026-10-27): DeltaChunkOffset(index[k]), for Lines64DiffMask16
  u32 whole = 0;         // me2-nx-deko: the leading entries that are whole 64-byte chunks (all but a short last one)
};
constexpr DeltaChunkList MakeDeltaChunkList(bool narrow) {
  DeltaChunkList l{};
  for (u32 i = 0; i < kDeltaChunks; ++i)
    if (!narrow || DeltaChunkRead(i)) {
      l.offset[l.count] = u16(DeltaChunkOffset(i));
      l.index[l.count++] = u8(i);
    }
  l.whole = l.count;
  while (l.whole && DeltaChunkSize(l.index[l.whole - 1]) != kDeltaUnitBytes)
    --l.whole;
  return l;
}
constexpr DeltaChunkList kDeltaAllChunks = MakeDeltaChunkList(false);
constexpr DeltaChunkList kDeltaReadChunks = MakeDeltaChunkList(true);
// me2-nx-deko: 12 of the 30 head chunks (from 0x480) + 27 of the 41 tail chunks (10112-12719 without 10880-11775).
static_assert(kDeltaReadChunks.count == 39);
// Only the device's last chunk is short (48 bytes), and it is the last entry of both lists.
static_assert(kDeltaReadChunks.whole == 38 && kDeltaAllChunks.whole == kDeltaAllChunks.count - 1);
// Whether device byte 'at' is one the render thread reads (the verify mode compares those).
constexpr bool DeltaByteRead(u32 at) {
  return !(at < kDeltaUnreadEnd || (at >= kDeltaPrivateUnreadBegin && at < kDeltaPrivateUnreadEnd));
}

// me2-nx-deko (2026-11-04, me2_renut_watch_chunks): the watched range's chunk b (device bytes 64 b..) is the delta's
// chunk b below the float constants and b - 128 from the bool constants on (the float constants go by m_Pending).
constexpr u32 kWatchTailFirst = kDeltaTailBegin / kDeltaUnitBytes;  // 158
static_assert(kDeltaTailBegin % kDeltaUnitBytes == 0 && kWatchTailFirst - kDeltaHeadChunks == 128);
static_assert(kDeltaHeadChunks == 30 && kDeltaChunks == 71 && d3d::kDeviceSnapshotSize <= 256 * kDeltaUnitBytes);
// The delta chunks a list compares, as a mask (bit i % 64 of word i / 64).
struct DeltaChunkMask {
  uint64_t w[2] = {};
};
constexpr DeltaChunkMask MaskOf(const DeltaChunkList &l) {
  DeltaChunkMask m{};
  for (u32 k = 0; k < l.count; ++k)
    m.w[l.index[k] / 64] |= uint64_t(1) << (l.index[k] % 64);
  return m;
}
constexpr DeltaChunkMask kDeltaReadMask = MaskOf(kDeltaReadChunks), kDeltaAllMask = MaskOf(kDeltaAllChunks);
// The watched chunks marked since the last call, as delta chunks (and clears the marks).
inline DeltaChunkMask TakeWatchMarks() {
  uint64_t (&d)[4] = g_me2_watch.dirty;
  constexpr uint64_t kHead = (uint64_t(1) << kDeltaHeadChunks) - 1;
  // Device chunks 158..191 (word 2 bits 30..63) -> delta 30..63; 192..198 (word 3 bits 0..6) -> delta 64..70.
  DeltaChunkMask m;
  m.w[0] = (d[0] & kHead) | (d[2] & ~kHead);
  m.w[1] = d[3] & ((uint64_t(1) << (kDeltaChunks - 64)) - 1);
  d[0] = d[1] = d[2] = d[3] = 0;
  return m;
}

struct DeltaHeader {
  uint64_t chunks[2];  // chunk i follows: bit i % 64 of word i / 64
  uint64_t vs, ps;     // the float constant groups that follow (bit 63 - group), the vertex shader's first
};
// The most a delta holds: every chunk and every group.
constexpr size_t kDeltaMaxBytes = sizeof(DeltaHeader) + d3d::kDeviceSnapshotSize;

// Calls f(first, count) for each run of consecutive groups in an m_Pending mask (bit 63 - group).
template <typename F>
void ForEachGroupRun(uint64_t mask, F f) {
  while (mask) {
    const u32 first = u32(std::countl_zero(mask));
    const u32 count = u32(std::countl_one(mask << first));
    f(first, count);
    mask = first + count >= 64 ? 0 : mask & (~uint64_t(0) >> (first + count));
  }
}

// Whether two whole chunks differ: fixed-size loads the compiler keeps inline and vectorises.
bool DeltaChunkDiffers(const u8 *a, const u8 *b) {
  uint64_t diff = 0;
  for (u32 k = 0; k < kDeltaUnitBytes; k += 8) {
    uint64_t x, y;
    std::memcpy(&x, a + k, 8);
    std::memcpy(&y, b + k, 8);
    diff |= x ^ y;
  }
  return diff != 0;
}

// The game threads' side of the chain.
struct DeltaChain {
  std::mutex mutex;                     // the chain's draws are made and queued in one order
  const d3d::Device *device = nullptr;  // the device it follows; none: the next draw rebases
  uint64_t vs = 0, ps = 0;              // groups whole captures consumed since its last draw: they follow
  uint64_t deltas = 0, rebases = 0, bytes = 0, chunks = 0, groups = 0;  // the 10 s report's
  uint64_t whole = 0;  // sa2-nx: whole captures outside the chain (Begin*Vertices draws), under the lock too
  alignas(64) u8 shadow[d3d::kDeviceSnapshotSize];  // the device at its last draw (the chunks)
};
DeltaChain g_delta;

// The render thread's side: the device the chain's draws rebuild.
alignas(64) u8 g_delta_device[d3d::kDeviceSnapshotSize];
std::atomic<uint64_t> g_delta_verified{0}, g_delta_differ{0};
// sa2_renut_snapshot_deltas_constants: the chain as a replay of the constant uploads' tracking (draw.h,
// ReplayConstants): its id (a new one at each whole copy; the top bit keeps it apart from the command buffers'
// replays, numbered from 1) and the float constant groups written since the upload last consumed them.
uint64_t g_chain_id = 0, g_chain_count = 0;
uint64_t g_chain_vs_dirty = ~uint64_t(0), g_chain_ps_dirty = ~uint64_t(0);
// sa2_renut_chain_reuse: the number of the chain's last draw (draw.h ChainHint::seq; from 2).
uint64_t g_chain_seq = 1;

bool DeltasOn() {
  static const bool on = REXCVAR_GET(sa2_renut_snapshot_deltas);
  return on;
}
bool ChainConstantsOn() {
  static const bool on = DeltasOn() && REXCVAR_GET(sa2_renut_snapshot_deltas_constants);
  return on;
}
bool CaptureBatchesOn() {
  static const bool on = REXCVAR_GET(sa2_renut_capture_batches);
  return on;
}
bool NeonCompareOn() {
  static const bool on = REXCVAR_GET(sa2_renut_neon_compare);
  return on;
}
// sa2-nx (2026-10-20, from futari-nx via peggle2-nx): sa2_renut_deferred_chain (QueueDeferredDraw).
bool DeferredChainOn() {
  static const bool on = REXCVAR_GET(sa2_renut_deferred_chain);
  return on;
}
bool DeltasNarrowOn() {
  static const bool on = REXCVAR_GET(sa2_renut_snapshot_deltas_narrow);
  return on;
}
bool WatchChunksOn() {
  static const bool on = DeltasOn() && REXCVAR_GET(me2_renut_watch_chunks);
  return on;
}
bool WatchVerifyOn() {
  static const bool on = WatchChunksOn() && REXCVAR_GET(me2_renut_watch_verify);
  return on;
}
// The watch marks' 10 s counts (under g_delta.mutex): deltas, marked chunks, chunks the verify found changed unmarked.
uint64_t g_watch_deltas = 0, g_watch_marked = 0, g_watch_unmarked = 0;
bool DeltasVerifyOn() {
  static const bool on = DeltasOn() && REXCVAR_GET(sa2_renut_snapshot_deltas_verify);
  return on;
}

// The original draw calls consume the device's dirty masks as they write
// their packets; a snapshot taken after this has them clear, as the command
// buffer ops' do.
void ClearPending(d3d::Device *device) {
  // sa2-nx (sa2_renut_snapshot_deltas): the dirty constant groups a whole capture consumes go with the chain's next
  // delta (its callers hold g_delta.mutex when the deltas are on).
  if (device == g_delta.device) {
    g_delta.vs |= uint64_t(device->m_Pending[0]);
    g_delta.ps |= uint64_t(device->m_Pending[1]);
  }
  for (auto &mask : device->m_Pending)
    mask = 0;
}

// ME2 (2026-10-28, me2_occlusion_umbra_defer), render thread: the device bytes that may have changed since the last
// held-back draw's private copy (the chunks and constant groups of every delta since; 'all' after a whole device), so a
// held-back draw whose device equals that copy shares it (Umbra's boxes: ~4,500 a frame on the console; a copy each
// cost the render thread ~60 MB a frame). g_defer_replayed: a replay ran since the chain's last draw, whose memos
// (sa2_renut_chain_reuse) the chain's next draw must not take.
struct DeferAcc {
  bool all = true;
  uint64_t chunks[2] = {};
  uint64_t vs = 0, ps = 0;
} g_defer_acc;
std::shared_ptr<std::vector<u8>> g_defer_last;
bool g_defer_replayed = false;

// sa2-nx (sa2_renut_snapshot_deltas), on the render thread: the device a queued draw reads.
const u8 *DeviceForDraw(const DrawCapture &capture) {
  if (g_defer_replayed) {
    ++g_chain_seq;  // a gap: nothing the replays left in the memos is reused
    g_defer_replayed = false;
  }
  if (capture.snapshot == SnapshotKind::Whole) {
    g_chain_hint = {};  // not a chain draw (sa2_renut_chain_reuse)
    g_defer_acc.all = true;
    return capture.state.data();
  }
  if (capture.snapshot == SnapshotKind::Rebase) {
    g_defer_acc.all = true;
    std::memcpy(g_delta_device, capture.state.data(), d3d::kDeviceSnapshotSize);
    g_chain_id = (uint64_t(1) << 63) | ++g_chain_count;  // its first draw compares every group
    g_chain_vs_dirty = g_chain_ps_dirty = ~uint64_t(0);
    g_chain_hint.seq = ++g_chain_seq;
    g_chain_hint.changed[0] = g_chain_hint.changed[1] = ~uint64_t(0);  // a whole copy: every chunk
  } else {
    const u8 *from = capture.state.data();
    DeltaHeader h;
    std::memcpy(&h, from, sizeof(h));
    from += sizeof(h);
    g_chain_hint.seq = ++g_chain_seq;
    g_chain_hint.changed[0] = h.chunks[0];
    g_chain_hint.changed[1] = h.chunks[1];
    g_chain_hint.consts_changed = (h.vs | h.ps) != 0;
    g_chain_vs_dirty |= h.vs;
    g_chain_ps_dirty |= h.ps;
    g_defer_acc.chunks[0] |= h.chunks[0];
    g_defer_acc.chunks[1] |= h.chunks[1];
    g_defer_acc.vs |= h.vs;
    g_defer_acc.ps |= h.ps;
    for (u32 word = 0; word < 2; ++word) {
      for (uint64_t bits = h.chunks[word]; bits; bits &= bits - 1) {
        const u32 i = word * 64 + u32(std::countr_zero(bits));
        const u32 size = DeltaChunkSize(i);
        // Whole chunks as a fixed-size copy, which stays inline.
        if (size == kDeltaUnitBytes)
          std::memcpy(g_delta_device + DeltaChunkOffset(i), from, kDeltaUnitBytes);
        else
          std::memcpy(g_delta_device + DeltaChunkOffset(i), from, size);
        from += size;
      }
    }
    for (const auto [dirty, base] : {std::pair{h.vs, kDeltaVertexF}, std::pair{h.ps, kDeltaPixelF}}) {
      ForEachGroupRun(dirty, [&](u32 first, u32 count) {
        const u32 bytes = count * kDeltaUnitBytes;
        CopyUnits64(g_delta_device + base + first * kDeltaUnitBytes, from, count);
        from += bytes;
      });
    }
  }
  if (!capture.verify.empty()) {
    g_delta_verified.fetch_add(1, std::memory_order_relaxed);
    // sa2-nx (sa2_renut_snapshot_deltas_narrow): the bytes the render thread reads; with the narrow compare off, all.
    bool same;
    if (DeltasNarrowOn()) {
      same = std::memcmp(g_delta_device + kDeltaUnreadEnd, capture.verify.data() + kDeltaUnreadEnd,
                         kDeltaPrivateUnreadBegin - kDeltaUnreadEnd) == 0 &&
             std::memcmp(g_delta_device + kDeltaPrivateUnreadEnd, capture.verify.data() + kDeltaPrivateUnreadEnd,
                         d3d::kDeviceSnapshotSize - kDeltaPrivateUnreadEnd) == 0;
    } else {
      same = std::memcmp(g_delta_device, capture.verify.data(), d3d::kDeviceSnapshotSize) == 0;
    }
    if (!same) {
      g_delta_differ.fetch_add(1, std::memory_order_relaxed);
      static uint32_t logged = 0;
      if (logged < 8) {
        ++logged;
        u32 at = 0;
        while (at < d3d::kDeviceSnapshotSize &&
               (g_delta_device[at] == capture.verify[at] || (DeltasNarrowOn() && !DeltaByteRead(at))))
          ++at;
        RGPU_WARN("Device snapshot delta: the rebuilt device differs from the device at +{} ({} draw): {:02X}, the "
                  "device {:02X} (sa2_renut_snapshot_deltas_verify)",
                  at, capture.snapshot == SnapshotKind::Rebase ? "a whole" : "a delta", g_delta_device[at],
                  capture.verify[at]);
      }
    }
  }
  return g_delta_device;
}

void QueueCapture(DrawCapture *capture) {
  // sa2-nx (2026-10-20, sa2_renut_prefetch_next): the capture and its device bytes, written on the game thread's core,
  // are fetched into the render thread's cache while it does the item before this one.
  render_thread::QueuePrefetched(capture, capture->state.data(), [capture] {
    DrawCall call = capture->call;
    if (!capture->vertices.empty())
      call.up_vertices = capture->vertices.data();
    if (!capture->indices.empty())
      call.up_indices = capture->indices.data();
    const u8 *device = DeviceForDraw(*capture);
    // ME2 (2026-10-28, me2_occlusion_umbra_defer, draw.h g_defer_draws): held back with a private copy of its device;
    // the chain's next draw sees a gap in its sequence numbers and reuses nothing.
    if (g_defer_draws && g_defer_draws()) {
      g_chain_hint = {};
      // The previous held-back draw's copy when every byte that may have changed since is the same.
      bool same = g_defer_last && !g_defer_acc.all;
      for (u32 word = 0; same && word < 2; ++word)
        for (uint64_t bits = g_defer_acc.chunks[word]; same && bits; bits &= bits - 1) {
          const u32 i = word * 64 + u32(std::countr_zero(bits));
          same = std::memcmp(device + DeltaChunkOffset(i), g_defer_last->data() + DeltaChunkOffset(i),
                             DeltaChunkSize(i)) == 0;
        }
      for (const auto [dirty, base] : {std::pair{g_defer_acc.vs, kDeltaVertexF}, std::pair{g_defer_acc.ps, kDeltaPixelF}})
        if (same)
          ForEachGroupRun(dirty, [&](u32 first, u32 count) {
            same = same && std::memcmp(device + base + first * kDeltaUnitBytes,
                                       g_defer_last->data() + base + first * kDeltaUnitBytes,
                                       count * kDeltaUnitBytes) == 0;
          });
      if (!same)
        g_defer_last = std::make_shared<std::vector<u8>>(device, device + d3d::kDeviceSnapshotSize);
      g_defer_acc = DeferAcc{false};
      auto copy = g_defer_last;
      u32 depth_va;
      std::memcpy(&depth_va, device + d3d::kDeviceOffsetDepthStencil, sizeof(depth_va));
      g_defer_push(
          [copy, call, capture](bool draw, u32 retarget) {
            // The copies are changed below (retarget) or released: later held-back draws take a new one.
            g_defer_last.reset();
            g_defer_acc.all = true;
            if (draw) {
              if (retarget) {
                // Another depth target (the base pass's) and no colour target: Umbra's boxes write no colour.
                const u32 be = __builtin_bswap32(retarget);
                std::memcpy(copy->data() + d3d::kDeviceOffsetDepthStencil, &be, sizeof(be));
                for (u32 i = 0; i < d3d::kMaxRenderTargets; ++i)
                  std::memset(copy->data() + d3d::kDeviceOffsetRenderTargets + i * 4, 0, 4);
              }
              // Consecutive replays of one copy as a chain of their own (memos reused, nothing changed between them).
              static const u8 *last_replayed = nullptr;
              const bool same_copy = copy->data() == last_replayed && g_defer_replayed;
              g_chain_hint.seq = ++g_chain_seq;
              g_chain_hint.changed[0] = g_chain_hint.changed[1] = same_copy ? 0 : ~uint64_t(0);
              g_chain_hint.consts_changed = !same_copy;
              last_replayed = copy->data();
              g_defer_replayed = true;
              DrawStateLocked(copy->data(), call);
              g_chain_hint = {};
            }
            g_capture_pool.Give(capture, true);
          },
          __builtin_bswap32(depth_va));
      return;
    }
    if (capture->snapshot != SnapshotKind::Whole && ChainConstantsOn()) {
      // sa2-nx (sa2_renut_snapshot_deltas_constants): every constant is the rebuilt device's (no running device).
      const ReplayConstants chain{nullptr, ~uint64_t(0), ~uint64_t(0), &g_chain_vs_dirty, &g_chain_ps_dirty,
                                  g_chain_id};
      DrawStateLocked(device, call, &chain);
    } else {
      DrawStateLocked(device, call);
    }
    g_chain_hint = {};  // sa2-nx: only for the draw itself
    g_capture_pool.Give(capture, true);
  });
}

DrawCapture *Capture(u32 device_va, const DrawCall &call) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return nullptr;
  ClearPending(device);
  DrawCapture *capture = g_capture_pool.Take();
  // assign() keeps a pooled capture's storage (Snapshot() made a new vector each time).
  const auto *bytes = reinterpret_cast<const u8 *>(device);
  capture->state.assign(bytes, bytes + d3d::kDeviceSnapshotSize);
  capture->snapshot = SnapshotKind::Whole;  // sa2-nx
  capture->verify.clear();
  capture->call = call;
  capture->call.up_vertices = nullptr;
  capture->call.up_indices = nullptr;
  return capture;
}

// me2-nx-deko (2026-10-31, me2_diag_delta_tags): under g_delta.mutex (CaptureDelta's callers hold it).
struct DeltaTagStats {
  uint32_t changed[128] = {};    // deltas that took chunk i
  uint32_t untagged[128] = {};   // ... with m_Pending[2..4] all zero
  uint32_t cooc[192][128] = {};  // ... with tag bit b (word b / 64 + 2, bit b % 64 from the least significant)
  uint32_t tag_set[192] = {};    // deltas with tag bit b
  uint64_t deltas = 0, tagged = 0;
  std::chrono::steady_clock::time_point since{};
};
DeltaTagStats *g_tag_stats = nullptr;

bool DeltaTagsOn() {
  static const bool on = REXCVAR_GET(me2_diag_delta_tags);
  return on;
}

void NoteDeltaTags(const uint64_t (&tags)[3], const uint64_t (&chunks)[2]) {
  if (!g_tag_stats) g_tag_stats = new DeltaTagStats();
  DeltaTagStats &t = *g_tag_stats;
  const auto now = std::chrono::steady_clock::now();
  if (t.since == std::chrono::steady_clock::time_point{}) t.since = now;
  ++t.deltas;
  const bool any = (tags[0] | tags[1] | tags[2]) != 0;
  t.tagged += any;
  for (u32 w = 0; w < 3; ++w)
    for (uint64_t m = tags[w]; m; m &= m - 1) ++t.tag_set[w * 64 + u32(std::countr_zero(m))];
  for (u32 cw = 0; cw < 2; ++cw) {
    for (uint64_t cm = chunks[cw]; cm; cm &= cm - 1) {
      const u32 i = cw * 64 + u32(std::countr_zero(cm));
      ++t.changed[i];
      if (!any) ++t.untagged[i];
      for (u32 w = 0; w < 3; ++w)
        for (uint64_t m = tags[w]; m; m &= m - 1) ++t.cooc[w * 64 + u32(std::countr_zero(m))][i];
    }
  }
  if (now - t.since < std::chrono::seconds(60)) return;
  RGPU_INFO("[delta-tags] {} deltas, {} with a tag of m_Pending[2..4] set; per changed chunk (device offset): "
                "changes, changes with no tag, the tags most often set with it (bit: count of the changes / count of "
                "all deltas with the bit)", t.deltas, t.tagged);
  for (u32 i = 0; i < 128; ++i) {
    if (!t.changed[i]) continue;
    u32 best[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (u32 b = 0; b < 192; ++b) {
      if (!t.cooc[b][i]) continue;
      for (u32 k = 0; k < 4; ++k) {
        if (best[k] == UINT32_MAX || t.cooc[b][i] > t.cooc[best[k]][i]) {
          for (u32 m = 3; m > k; --m) best[m] = best[m - 1];
          best[k] = b;
          break;
        }
      }
    }
    std::string list;
    for (u32 k = 0; k < 4 && best[k] != UINT32_MAX; ++k)
      list += fmt::format(" P{}:{}={}/{}", best[k] / 64 + 2, best[k] % 64, t.cooc[best[k]][i], t.tag_set[best[k]]);
    RGPU_INFO("[delta-tags] chunk {} (+{}): {} changes, {} untagged;{}", i, DeltaChunkOffset(i), t.changed[i],
                  t.untagged[i], list);
  }
  *g_tag_stats = DeltaTagStats{};
  g_tag_stats->since = now;
}

// sa2-nx (2026-10-20, sa2_renut_snapshot_deltas): a draw of the chain. Requires g_delta.mutex, held until the capture
// is queued.
DrawCapture *CaptureDelta(u32 device_va, const DrawCall &call) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return nullptr;
  DeltaChain &c = g_delta;
  DrawCapture *capture = g_capture_pool.Take(true);
  capture->call = call;
  capture->call.up_vertices = nullptr;
  capture->call.up_indices = nullptr;
  const auto *bytes = reinterpret_cast<const u8 *>(device);
  if (c.device != device) {
    // The chain's first draw (or a frame's): the whole device, which the render thread takes as its own.
    for (auto &mask : device->m_Pending)
      mask = 0;
    capture->state.assign(bytes, bytes + d3d::kDeviceSnapshotSize);
    std::memcpy(c.shadow, bytes, d3d::kDeviceSnapshotSize);
    c.device = device;
    // me2-nx-deko (2026-11-04): the watched range follows the chain's device; the shadow is whole again, so the marks
    // start over.
    if (g_me2_watch.lo != device_va || g_me2_watch.span != d3d::kDeviceSnapshotSize) {
      g_me2_watch.lo = device_va;
      g_me2_watch.span = d3d::kDeviceSnapshotSize;
      rex::memory::SetWatchedRange(reinterpret_cast<rex::memory::WatchedRange *>(&g_me2_watch));
      Me2CensusDeviceStart();
    }
    TakeWatchMarks();
    c.vs = c.ps = 0;
    capture->snapshot = SnapshotKind::Rebase;
    ++c.rebases;
    c.bytes += d3d::kDeviceSnapshotSize;
  } else {
    const uint64_t vs = c.vs | uint64_t(device->m_Pending[0]);
    const uint64_t ps = c.ps | uint64_t(device->m_Pending[1]);
    const uint64_t tags[3] = {uint64_t(device->m_Pending[2]), uint64_t(device->m_Pending[3]),
                              uint64_t(device->m_Pending[4])};  // me2_diag_delta_tags
    c.vs = c.ps = 0;
    for (auto &mask : device->m_Pending)
      mask = 0;
    std::vector<u8> &out = capture->state;
    if (out.size() < kDeltaMaxBytes)
      out.resize(kDeltaMaxBytes);  // once per pooled capture: written below, never cleared
    u8 *const begin = out.data();
    // me2-nx-deko (2026-11-03): a pooled capture's buffer was last read on the render thread's core; a delta writes
    // ~500 bytes of it (console logs: 2 chunks and 5 constant groups a draw). Asked for now, they arrive while the 39
    // chunks are compared below instead of stalling the writes after it.
    for (u32 line = 0; line < 512; line += 64)
      __builtin_prefetch(begin + line, 1);
    u8 *to = begin + sizeof(DeltaHeader);
    DeltaHeader h{};
    u32 chunks = 0;
    const DeltaChunkList &list = DeltasNarrowOn() ? kDeltaReadChunks : kDeltaAllChunks;
    const bool neon = NeonCompareOn();  // sa2-nx (sa2_renut_neon_compare)
    // Chunk i differs: into the shadow and the delta (in increasing i, the order DeviceForDraw reads them back).
    const auto take = [&](u32 i) {
      const u32 at = DeltaChunkOffset(i), size = DeltaChunkSize(i);
      if (size == kDeltaUnitBytes) {
        // me2-nx-deko (2026-10-27): fixed-size copies stay inline (the variable size called the library memcpy twice
        // a chunk: memmove 4 % of the UE3 render thread in the 22:5x stacks).
        std::memcpy(c.shadow + at, bytes + at, kDeltaUnitBytes);
        std::memcpy(to, bytes + at, kDeltaUnitBytes);
      } else {
        std::memcpy(c.shadow + at, bytes + at, size);
        std::memcpy(to, bytes + at, size);
      }
      to += size;
      h.chunks[i / 64] |= uint64_t(1) << (i % 64);
      ++chunks;
    };
    u32 k = 0;
    if (WatchChunksOn()) {
      // me2-nx-deko (2026-11-04): only the marked chunks, compared (a mark means written, maybe with the same value).
      const DeltaChunkMask &read = DeltasNarrowOn() ? kDeltaReadMask : kDeltaAllMask;
      const DeltaChunkMask marked = TakeWatchMarks();
      DeltaChunkMask look{{marked.w[0] & read.w[0], marked.w[1] & read.w[1]}};
      ++g_watch_deltas;
      g_watch_marked += uint64_t(std::popcount(look.w[0]) + std::popcount(look.w[1]));
      if (WatchVerifyOn()) {
        // Every read chunk: the unmarked ones that changed are counted, reported and taken (the picture stays right).
        for (u32 word = 0; word < 2; ++word) {
          for (uint64_t bits = read.w[word] & ~look.w[word]; bits; bits &= bits - 1) {
            const u32 i = word * 64 + u32(std::countr_zero(bits));
            const u32 at = DeltaChunkOffset(i), size = DeltaChunkSize(i);
            if (std::memcmp(bytes + at, c.shadow + at, size) == 0)
              continue;
            ++g_watch_unmarked;
            look.w[word] |= uint64_t(1) << (i % 64);
            static uint32_t logged = 0;
            if (logged < 64) {
              ++logged;
              u32 first = 0;
              while (first < size && bytes[at + first] == c.shadow[at + first])
                ++first;
              RGPU_WARN("[watch] unmarked: device chunk +{:X} changed at +{:X} ({:02X} -> {:02X}) with no mark",
                        at, at + first, c.shadow[at + first], bytes[at + first]);
            }
          }
        }
      }
      // Increasing i: the order DeviceForDraw reads them back.
      for (u32 word = 0; word < 2; ++word) {
        for (uint64_t bits = look.w[word]; bits; bits &= bits - 1) {
          const u32 i = word * 64 + u32(std::countr_zero(bits));
          const u32 at = DeltaChunkOffset(i), size = DeltaChunkSize(i);
          const bool differs = size == kDeltaUnitBytes ? sa2::renut::Line64Differs(bytes + at, c.shadow + at, neon)
                                                       : std::memcmp(bytes + at, c.shadow + at, size) != 0;
          if (differs)
            take(i);
        }
      }
      k = list.count;  // nothing left for the full compare below
    }
#if defined(__aarch64__)
    if (neon && k < list.whole) {
      // me2-nx-deko (2026-10-27): the whole chunks 16 at a time, one mask each (Lines64DiffMask16).
      for (; k < list.whole; k += 16) {
        for (u32 m = sa2::renut::Lines64DiffMask16(bytes, c.shadow, list.offset + k, std::min(16u, list.whole - k)); m;
             m &= m - 1)
          take(list.index[k + u32(std::countr_zero(m))]);
      }
      k = list.whole;
    }
#endif
    for (; k < list.count; ++k) {
      const u32 i = list.index[k];
      const u32 at = DeltaChunkOffset(i), size = DeltaChunkSize(i);
      const bool differs = size == kDeltaUnitBytes ? sa2::renut::Line64Differs(bytes + at, c.shadow + at, neon)
                                                   : std::memcmp(bytes + at, c.shadow + at, size) != 0;
      if (differs)
        take(i);
    }
    for (const auto [dirty, base] : {std::pair{vs, kDeltaVertexF}, std::pair{ps, kDeltaPixelF}}) {
      ForEachGroupRun(dirty, [&](u32 first, u32 count) {
        const u32 group_bytes = count * kDeltaUnitBytes;
        CopyUnits64(to, bytes + base + first * kDeltaUnitBytes, count);
        to += group_bytes;
      });
    }
    if (DeltaTagsOn())
      NoteDeltaTags(tags, h.chunks);
    h.vs = vs;
    h.ps = ps;
    std::memcpy(begin, &h, sizeof(h));
    capture->snapshot = SnapshotKind::Delta;
    ++c.deltas;
    c.bytes += uint64_t(to - begin);
    c.chunks += chunks;
    c.groups += uint64_t(std::popcount(vs) + std::popcount(ps));
  }
  if (DeltasVerifyOn())
    capture->verify.assign(bytes, bytes + d3d::kDeviceSnapshotSize);
  else
    capture->verify.clear();
  return capture;
}

} // namespace

void SnapshotDeltasNewFrame() {
  Me2CensusDeviceTick();  // me2-nx-deko (2026-11-04): the device census' reports (diagnostic builds)
  if (!DeltasOn())
    return;
  std::lock_guard lock(g_delta.mutex);
  g_delta.device = nullptr;  // the frame's first draw rebases: whatever went wrong lasts a frame at most
}

void ForgetSnapshotDeltas() {
  if (!DeltasOn())
    return;
  std::lock_guard lock(g_delta.mutex);
  g_delta.device = nullptr;
}

std::string SnapshotDeltaReport() {
  if (!DeltasOn())
    return {};
  uint64_t deltas, rebases, bytes, chunks, groups, whole;
  {
    std::lock_guard lock(g_delta.mutex);
    whole = g_delta.whole;
    g_delta.whole = 0;
    deltas = g_delta.deltas;
    rebases = g_delta.rebases;
    bytes = g_delta.bytes;
    chunks = g_delta.chunks;
    groups = g_delta.groups;
    g_delta.deltas = g_delta.rebases = g_delta.bytes = g_delta.chunks = g_delta.groups = 0;
  }
  const uint64_t draws = deltas + rebases;
  if (!draws)
    return {};
  const double per_delta = double(std::max<uint64_t>(deltas, 1));
  std::string line = fmt::format(
      "device snapshots (sa2_renut_snapshot_deltas): {} draws, {:.0f} bytes kept a draw on average (the whole device "
      "is {}); {:.1f} chunks and {:.1f} constant groups a delta; {} whole copies (the frames' first draws); {} "
      "Begin*Vertices draws as whole copies",
      draws, double(bytes) / double(draws), d3d::kDeviceSnapshotSize, double(chunks) / per_delta,
      double(groups) / per_delta, rebases, whole);
  if (WatchChunksOn()) {
    uint64_t wd, wm, wu;
    {
      std::lock_guard lock(g_delta.mutex);
      wd = g_watch_deltas;
      wm = g_watch_marked;
      wu = g_watch_unmarked;
      g_watch_deltas = g_watch_marked = g_watch_unmarked = 0;
    }
    line += fmt::format("; watch: {:.1f} marked chunks a delta", double(wm) / double(std::max<uint64_t>(wd, 1)));
    if (WatchVerifyOn())
      line += fmt::format(", {} changed unmarked", wu);
  }
  if (DeltasVerifyOn()) {
    line += fmt::format("; verify: {} draws checked, {} differ", g_delta_verified.exchange(0, std::memory_order_relaxed),
                        g_delta_differ.exchange(0, std::memory_order_relaxed));
  }
  return line;
}

// me2-nx-deko (me2_renut_delta_prefetch): the lines CaptureDelta compares, requested ahead (only the addresses are
// read: the shadow without the chain's lock is fine for a prefetch).
static void PrefetchDelta(u32 device_va) {
  static const bool on = DeltasOn() && REXCVAR_GET(me2_renut_delta_prefetch);
  if (!on)
    return;
  const auto *bytes = reinterpret_cast<const u8 *>(GuestPtr<d3d::Device>(device_va));
  if (!bytes)
    return;
  const DeltaChunkList &list = DeltasNarrowOn() ? kDeltaReadChunks : kDeltaAllChunks;
  for (u32 k = 0; k < list.count; ++k) {
    __builtin_prefetch(bytes + list.offset[k]);
    __builtin_prefetch(g_delta.shadow + list.offset[k]);
  }
}

void QueueDraw(u32 device_va, const DrawCall &call, u32 up_vertex_bytes, u32 up_index_bytes) {
  PrefetchDelta(device_va);
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  // sa2-nx (2026-10-20, sa2_renut_snapshot_deltas): made and queued under the chain's lock.
  std::unique_lock<std::mutex> chain;
  if (DeltasOn())
    chain = std::unique_lock<std::mutex>(g_delta.mutex);
  DrawCapture *capture = DeltasOn() ? CaptureDelta(device_va, call) : Capture(device_va, call);
  if (!capture)
    return;
  if (call.up_vertices)
    capture->vertices.assign(call.up_vertices, call.up_vertices + up_vertex_bytes);
  if (call.up_indices)
    capture->indices.assign(call.up_indices, call.up_indices + up_index_bytes);
  QueueCapture(capture);
}

void QueueDeferredDraw(u32 device_va, const DrawCall &call, const u8 *scratch, u32 bytes, const u8 *index_scratch,
                       u32 index_bytes) {
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  // sa2-nx (2026-10-20, sa2_renut_deferred_chain, from futari-nx via peggle2-nx): a Begin*Vertices draw of the chain
  // too. Made under the chain's lock and queued by FlushDeferredVertices under it as well, before any other draw of the
  // device is made (every hook flushes its device's deferred draw first), so the render thread still applies the chain
  // in order. A draw that will not be queued (no scratch) stays a whole capture outside the chain, as before.
  if (DeltasOn() && DeferredChainOn() && scratch) {
    std::unique_lock<std::mutex> chain(g_delta.mutex);
    {
      std::lock_guard lock(g_deferred_mutex);
      auto it = g_deferred.find(device_va);
      if (it != g_deferred.end()) {
        // One not flushed (FlushDeferredVertices above queues it, so none is expected): dropped as the original
        // replaced it; its delta never reaches the render thread, so the chain rebases.
        g_capture_pool.Give(it->second.capture);
        g_deferred.erase(it);
        g_deferred_count.store(g_deferred.size(), std::memory_order_release);
        g_delta.device = nullptr;
      }
    }
    DrawCapture *delta = CaptureDelta(device_va, call);
    if (!delta)
      return;
    std::lock_guard lock(g_deferred_mutex);
    g_deferred[device_va] = {delta, scratch, bytes, index_scratch, index_bytes};
    g_deferred_count.store(g_deferred.size(), std::memory_order_release);
    return;
  }
  DrawCapture *capture = nullptr;
  {
    // sa2-nx (sa2_renut_snapshot_deltas): a whole capture (queued later than made: outside the chain), whose dirty
    // constant groups ClearPending hands to the chain's next delta.
    std::unique_lock<std::mutex> chain;
    if (DeltasOn())
      chain = std::unique_lock<std::mutex>(g_delta.mutex);
    capture = Capture(device_va, call);
    if (DeltasOn())
      ++g_delta.whole;
  }
  if (!capture)
    return;
  if (!scratch) {
    g_capture_pool.Give(capture);
    return;
  }
  std::lock_guard lock(g_deferred_mutex);
  DeferredDraw &slot = g_deferred[device_va];
  g_capture_pool.Give(slot.capture);  // one not flushed (the original replaced it the same way)
  slot = {capture, scratch, bytes, index_scratch, index_bytes};
  g_deferred_count.store(g_deferred.size(), std::memory_order_release);
}

void FlushDeferredVertices(u32 device_va) {
  FlushRecordedVertices(device_va);
  if (HookFastPathsOn() && g_deferred_count.load(std::memory_order_acquire) == 0)
    return;  // sa2-nx (2026-10-20): none waiting, no lock
  // sa2-nx (2026-10-20, sa2_renut_deferred_chain): a chain draw is queued under the chain's lock (taken before the
  // deferred map's, as QueueDeferredDraw takes them), so no draw of the chain is made or queued between.
  std::unique_lock<std::mutex> chain;
  if (DeltasOn() && DeferredChainOn())
    chain = std::unique_lock<std::mutex>(g_delta.mutex);
  DeferredDraw deferred;
  {
    std::lock_guard lock(g_deferred_mutex);
    auto it = g_deferred.find(device_va);
    if (it == g_deferred.end())
      return;
    deferred = std::move(it->second);
    g_deferred.erase(it);
    g_deferred_count.store(g_deferred.size(), std::memory_order_release);
  }
  deferred.capture->vertices.assign(deferred.scratch, deferred.scratch + deferred.bytes);
  if (deferred.index_scratch)
    deferred.capture->indices.assign(deferred.index_scratch, deferred.index_scratch + deferred.index_bytes);
  else
    deferred.capture->indices.clear();
  QueueCapture(deferred.capture);
}

void QueueClear(u32 device_va, u32 flags, const ClearRect *rect, const float color[4], float depth,
                u32 stencil) {
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  const auto *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  DrawCapture *args = g_capture_pool.Take();  // banjo-nx (2026-10-19): pooled, see DrawCapture
  args->state.assign(device, device + d3d::kDeviceSnapshotSize);
  args->has_rect = rect != nullptr;
  args->rect = rect ? *rect : ClearRect{};
  std::memcpy(args->color, color, sizeof(args->color));
  args->flags = flags;
  args->depth = depth;
  args->stencil = stencil;
  render_thread::Queue([args] {
    ClearStateLocked(args->state.data(), args->flags, args->has_rect ? &args->rect : nullptr, args->color,
                     args->depth, args->stencil);
    g_capture_pool.Give(args, true);
  });
}

void QueueResolve(u32 device_va, u32 flags, const ClearRect *source_rect, u32 dest_texture_va,
                  i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                  const float clear_color[4], float clear_depth, u32 clear_stencil) {
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  const auto *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  // The destination header as it is now: the game often builds it on the
  // stack (as for recorded resolves).
  DrawCapture *args = g_capture_pool.Take();  // banjo-nx (2026-10-19): pooled, see DrawCapture
  args->state.assign(device, device + d3d::kDeviceSnapshotSize);
  args->has_rect = source_rect != nullptr;
  args->rect = source_rect ? *source_rect : ClearRect{};
  args->has_color = clear_color != nullptr;
  if (clear_color)
    std::memcpy(args->color, clear_color, sizeof(args->color));
  const auto *header = GuestPtr<const d3d::BaseTexture>(dest_texture_va);
  args->has_dest_format = header != nullptr;
  if (header)
    args->dest_format = header->Format;
  args->flags = flags;
  args->dest_texture_va = dest_texture_va;
  args->dest_x = dest_x;
  args->dest_y = dest_y;
  args->dest_level = dest_level;
  args->dest_slice = dest_slice;
  args->depth = clear_depth;
  args->stencil = clear_stencil;
  render_thread::Queue([args] {
    ResolveStateLocked(args->state.data(), args->flags, args->has_rect ? &args->rect : nullptr,
                       args->dest_texture_va, args->has_dest_format ? &args->dest_format : nullptr,
                       args->dest_x, args->dest_y, args->dest_level, args->dest_slice,
                       args->has_color ? args->color : nullptr, args->depth, args->stencil);
    g_capture_pool.Give(args, true);
  });
}

} // namespace renut::gpu
