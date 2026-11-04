// me2-nx-deko (2026-11-01): a per-frame cache of UE3's material instance parameter lookups on the render thread.
//
// Run 13's call tree (rex_stacks.log): UE3's render thread is the frame's critical path in heavy scenes, at ~20 us of
// work a draw. Each draw sets its material's shader parameters (sub_825DCDA0: the uniform expressions' Evaluate, then
// the constant / texture setters), and every parameter expression asks the material instance's render proxy for its
// value: GetVectorValue sub_82311C70, GetScalarValue sub_82311D40, GetTextureValue sub_82311DF8 (this r3, const FName&
// r4, out r5, context r6; returns found in r3). Each looks in the instance's own map (+24 / +44 / +64) and, if the name
// is not there, asks its parent (+20) for its render proxy twice (virtual +332) and recurses up the chain: ~4 % of the
// render thread's samples in these three alone, more with what they call.
//
// Within one render command the answers cannot change: parameter values reach the proxies through render commands of
// their own. So the first answer for (proxy, name) is kept and the next draws of the same command take it from here: the
// same 'found' and the same value written to the same out. 2026-11-01 (run 16): keyed by the frame (Swap) first, but
// the self-check of me2_material_params caught a value changing within one frame in the main menu (UI draws are
// commands of their own, with parameter updates between them); now keyed by the render command running
// (Me2RenderCommandEpoch below), so the cache empties itself with every command. The 3D scene is one command
// (FDrawSceneCommand), where nearly all of the lookups are.
//
// Self-checked like the hot natives: the first me2_material_cache_checks hits also run the original and compare its
// result and value, then one hit in me2_material_cache_check_every for the rest of the session; any difference logs
// "[material-cache] DIFFERENCE" and turns the cache off for the session.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

REXCVAR_DEFINE_BOOL(me2_material_cache, false, "ME2",
                    "Cache UE3's material instance parameter lookups (GetVectorValue / GetScalarValue / "
                    "GetTextureValue) for the rest of the frame on the render thread")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_material_cache_ab_s, 0, "ME2",
                     "A/B: me2_material_cache on and off in turn every this many seconds; 0 = always on")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_material_cache_checks, 4096, "ME2",
                     "me2_material_cache: hits also checked against the original (then a difference turns it off)")
    .range(0, 1 << 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_material_cache_check_every, 256, "ME2",
                     "me2_material_cache: after the first checks, one hit in this many is still checked; 0 = none")
    .range(0, 1 << 20)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Render commands run so far. Every UE3 render command's deleting destructor is one function (sub_82348E48: it puts
// back the FRenderCommand vtable 0x820D0FA4 and frees with flag 1; vtables FDrawSceneCommand 0x8217EA68,
// BeginDrawingCommand 0x82109808, FenceCommand 0x820E86E4: [that destructor, Execute, DescribeCommand]), called by
// the render thread's loop right after Execute; also for commands run in place on the render thread.
std::atomic<uint64_t> g_me2_render_commands{0};
std::atomic<uint64_t> g_me2_command_epochs{0};  // changes of Me2RenderCommandEpoch, for the report
extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap

// The render command running now, as an id that changes with it. UE3's render thread loop (sub_82335208,
// RenderingThreadMain) takes each command at GRenderCommandBuffer's read pointer ([0x833EA69C + 20]), calls Execute
// (vtable +4) and the destructor (vtable +0, flags 0), then moves the read pointer past it: during a command the read
// pointer is that command's address. Commands with destructors of their own (members to destroy) do not go through
// sub_82348E48, so the read pointer is what sees every queued command; the destructor counter adds commands run in
// place, and the Swap counter keeps a read pointer seen again after the ring wraps from matching. The id changes
// whenever one of the three does since the caller's last look (per thread: each cache has one owner thread).
uint64_t Me2RenderCommandEpoch(const uint8_t* base) {
  constexpr uint32_t kRingReadPointer = 0x833EA69C + 20;
  thread_local uint32_t last_read = 0;
  thread_local uint64_t last_frames = ~0ull, last_commands = ~0ull, epoch = 0;
  uint32_t read;
  std::memcpy(&read, base + kRingReadPointer, 4);
  const uint64_t frames = g_sa2_game_frames.load(std::memory_order_relaxed);
  const uint64_t commands = g_me2_render_commands.load(std::memory_order_relaxed);
  if (read != last_read || frames != last_frames || commands != last_commands) {
    last_read = read;
    last_frames = frames;
    last_commands = commands;
    ++epoch;  // from 1: 0 marks an empty cache entry
    g_me2_command_epochs.fetch_add(1, std::memory_order_relaxed);
  }
  return epoch;
}

namespace {

enum Kind : uint8_t { kVector = 1, kScalar = 2, kTexture = 3 };
constexpr uint32_t kWords[4] = {0, 4, 1, 1};

struct Entry {
  uint64_t frame = 0;  // Me2RenderCommandEpoch (0 = empty)
  uint32_t proxy = 0, name_index = 0, name_number = 0;
  uint8_t kind = 0, found = 0;
  uint32_t value[4] = {};
};
constexpr uint32_t kEntries = 8192;
Entry g_cache[kEntries];
std::atomic<std::thread::id> g_owner{};
std::atomic<uint64_t> g_owner_frame{0};  // the frame of the owner's last call
bool g_take_over = false;                // a new owner: the table is emptied first (owner thread only)
uint64_t g_owners = 0;                   // threads that owned the table so far
std::atomic<bool> g_disabled{false};
std::atomic<int64_t> g_checks_left{-1};
uint64_t g_hits = 0, g_misses = 0, g_checked = 0;
std::chrono::steady_clock::time_point g_report = std::chrono::steady_clock::now();

uint32_t LoadBE32(const uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}
void StoreBE32(uint8_t* base, uint32_t va, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + va, &v, 4);
}
bool GuestOk(uint32_t p) { return p >= 0x10000 && p < 0xFFFF0000u; }

bool On() {
  static const bool on = REXCVAR_GET(me2_material_cache);
  if (!on || g_disabled.load(std::memory_order_relaxed)) return false;
  static const int32_t ab_s = REXCVAR_GET(me2_material_cache_ab_s);
  if (ab_s > 0) {
    static const auto start = std::chrono::steady_clock::now();
    const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    if ((secs / ab_s) & 1) return false;  // the "off" half of the A/B
  }
  // One thread owns the table (UE3's render thread, the first to draw a material); any other thread goes straight to
  // the original.
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

void Report() {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_report < std::chrono::seconds(10)) return;
  static uint64_t commands_before = 0;
  const uint64_t commands = g_me2_command_epochs.load(std::memory_order_relaxed);
  REXLOG_INFO("[material-cache] last ~10 s: {} parameter lookups answered from the command's cache, {} looked up ({:.0f} "
              "% hits; {} render commands; {} owner thread(s) so far); {} hits checked against the original so far{}",
              g_hits, g_misses, g_hits + g_misses ? 100.0 * double(g_hits) / double(g_hits + g_misses) : 0.0,
              commands - commands_before, g_owners, g_checked,
              g_disabled.load(std::memory_order_relaxed) ? " (OFF after a difference)" : "");
  commands_before = commands;
  g_hits = g_misses = 0;
  g_report = now;
}

// The original, then its answer into the entry.
template <void (*Original)(PPCContext&, uint8_t*)>
void Lookup(PPCContext& ctx, uint8_t* base, Kind kind) {
  if (!On()) {
    Original(ctx, base);
    return;
  }
  if (g_take_over) {
    g_take_over = false;
    for (Entry& e : g_cache) e.frame = 0;
  }
  const uint32_t proxy = ctx.r3.u32, name = ctx.r4.u32, out = ctx.r5.u32;
  if (!GuestOk(name) || !GuestOk(out)) {
    Original(ctx, base);
    return;
  }
  const uint32_t name_index = LoadBE32(base, name), name_number = LoadBE32(base, name + 4);
  const uint64_t frame = Me2RenderCommandEpoch(base);
  uint64_t h = (uint64_t(proxy) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(name_index) * 0xC2B2AE3D27D4EB4Full) ^
               (uint64_t(name_number) << 32) ^ kind;
  h ^= h >> 29;
  Entry& e = g_cache[h & (kEntries - 1)];
  const uint32_t words = kWords[kind];
  if (e.frame == frame && e.proxy == proxy && e.name_index == name_index && e.name_number == name_number &&
      e.kind == kind) {
    int64_t left = g_checks_left.load(std::memory_order_relaxed);
    if (left < 0) {
      left = REXCVAR_GET(me2_material_cache_checks);
      g_checks_left.store(left, std::memory_order_relaxed);
    }
    // After the first checks, one hit in every me2_material_cache_check_every keeps being checked for the whole session
    // (a parameter update landing between two draws of one frame, e.g. the HUD's, would show up there).
    static const int32_t every = REXCVAR_GET(me2_material_cache_check_every);
    static uint32_t since_check = 0;
    const bool sampled = every > 0 && ++since_check >= uint32_t(every);
    if (left > 0 || sampled) {
      // Checked hit: the original answers (and writes the out as it always did); then compare.
      if (sampled) since_check = 0;
      if (left > 0) g_checks_left.store(left - 1, std::memory_order_relaxed);
      ++g_checked;
      Original(ctx, base);
      const uint8_t found = ctx.r3.u32 != 0;
      bool same = found == e.found;
      for (uint32_t i = 0; same && found && i < words; ++i) same = LoadBE32(base, out + 4 * i) == e.value[i];
      if (!same) {
        g_disabled.store(true, std::memory_order_relaxed);
        REXLOG_ERROR("[material-cache] DIFFERENCE: proxy {:08X} name {}:{} kind {} found {} (cached {}): the cache "
                     "is off for this session",
                     proxy, name_index, name_number, int(kind), int(found), int(e.found));
      }
      ++g_hits;
      Report();
      return;
    }
    if (e.found)
      for (uint32_t i = 0; i < words; ++i) StoreBE32(base, out + 4 * i, e.value[i]);
    ctx.r3.u64 = e.found;
    ++g_hits;
    Report();
    return;
  }
  Original(ctx, base);
  e.frame = frame;
  e.proxy = proxy;
  e.name_index = name_index;
  e.name_number = name_number;
  e.kind = kind;
  e.found = ctx.r3.u32 != 0;
  if (e.found)
    for (uint32_t i = 0; i < words; ++i) e.value[i] = LoadBE32(base, out + 4 * i);
  ++g_misses;
  Report();
}

}  // namespace

REX_EXTERN(__imp__sub_82311C70);
REX_HOOK_RAW(sub_82311C70) {  // FMaterialInstanceResource::GetVectorValue
  Lookup<__imp__sub_82311C70>(ctx, base, kVector);
}
REX_EXTERN(__imp__sub_82311D40);
REX_HOOK_RAW(sub_82311D40) {  // ::GetScalarValue
  Lookup<__imp__sub_82311D40>(ctx, base, kScalar);
}
REX_EXTERN(__imp__sub_82311DF8);
REX_HOOK_RAW(sub_82311DF8) {  // ::GetTextureValue
  Lookup<__imp__sub_82311DF8>(ctx, base, kTexture);
}

REX_EXTERN(__imp__sub_82348E48);
REX_HOOK_RAW(sub_82348E48) {  // FRenderCommand's deleting destructor (every render command, after its Execute)
  g_me2_render_commands.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82348E48(ctx, base);
}
