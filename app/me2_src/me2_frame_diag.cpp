// me2-nx-deko (2026-10-31): frame diagnostics for the console (steady frame rate work).
//
// The console logs of build 10-30 00:07 (CPU 1785 MHz) showed gameplay frames of 95-170 ms in 16-25 % of the 10 s
// windows, about every 40 s, with no slow file reads near them; the 10 s averages cannot tell what they are. Here:
//
// - me2_diag_gc: UE3's garbage collection, timed. Found from UWorld::Tick's GC block (sub_8247F790, 0x82480884 ..
//   0x82480920): bFullPurgeTriggered (GWorld+392) -> UObject::CollectGarbage sub_827B0160 (r3 = KeepFlags, 64-bit;
//   r4 = bPerformFullPurge); TimeSinceLastPendingKillPurge (GWorld+388) > TimeBetweenPurgingPendingKillObjects
//   (GEngine+764) -> UWorld::PerformGarbageCollection sub_82480988 (calls CollectGarbage); otherwise
//   UObject::IncrementalPurgeGarbage sub_827AFC90 (r3 = bUseTimeLimit), every other tick. GEngine [0x833D5BA0],
//   GWorld [0x833D60B0], as AWorldInfo.execForceGarbageCollection sub_82429B58 writes them. Every CollectGarbage is
//   logged with its time ("[gc]"); the incremental purges are summed per 10 s.
// - me2_diag_spike_ms: a frame (Swap to Swap on the Swap's thread, sa2_renut_pacing.cpp g_frame_observer) at least this
//   long gets a "[spike]" line: the frames before it, and what ran since the previous Swap (garbage collection,
//   incremental purge, GetData waits for occlusion results, the renderer's texture uploads / shader compiles /
//   barriers). Plus a 10 s frame-time histogram ("[frames]": frames over 34 / 50 / 67 / 100 ms, i.e. missed refreshes)
//   with the GetData wait setting of me2_occlusion_wait_cycle, for the occlusion A/B. On the Switch the spikes those
//   counters do not explain are also handed to the stack sampler (RexSwitchPerfHitch): with switch_profile_stacks on,
//   rex_profile.log shows what every thread was doing in them ("during the hitches").
// - me2_gc_min_gap_s (2026-10-31, run 3: 152 collections in 43 min, every ~12-17 s although the ini timer is 60 s;
//   median 70 ms, p90 138 ms at CPU 1326 MHz; 102 of the 290 frames over 70 ms): a collection the world's tick asks
//   for less than this many seconds after the previous one is deferred to the end of the gap, not dropped. The timer /
//   forced path (UWorld::PerformGarbageCollection, which script's ForceGarbageCollection and the ini timer reach) is
//   skipped and the world's timer (GWorld+388) re-armed to fire when the gap ends; a full-purge request of the tick
//   (bFullPurgeTriggered) is the same, and the deferred collection then runs as a full purge. Every other caller of
//   CollectGarbage (map loads, travel) is never held back. Script's ForceGarbageCollection / DelayGarbageCollection /
//   RescheduleGarbageCollectionTimer callers are logged (me2_diag_gc) to see who asks so often.
//
// - me2_gc_stream_gap_s (2026-11-01, runs 5 and 6: 102 of 155 / 87 of 125 collections came from sub_8259ABD0, the
//   level-streaming "sections were unloaded" path, in bursts: 5 in 7 s, 9 in 43 s; median 61-63 ms, full ones ~90 ms):
//   such a collection less than this many seconds after the previous one ends is merged into one collection at the
//   end of the gap (the world's timer re-armed, as above; a full purge asked for stays a full purge), not dropped. Only
//   while the guest's physical memory has at least me2_gc_stream_min_free_mb free: what an unload frees comes back a
//   few seconds later, and the game was made for the 360's 512 MB. Every "[gc]" line gives the free physical memory
//   before and after the collection, the deferrals log one line each.
//
// - me2_diag_stream_wait (2026-11-01, user: black screens while traversing Omega and the Normandy, never in combat):
//   UE3 waits for level streaming when the player reaches an area whose streaming levels are not in yet (Kismet's
//   USeqAct_LevelStreaming with block on load, the streaming volumes), and the screen stays black (or on a loading
//   movie) while the packages load. Such a wait shows as a run of frames with almost no draws (at most
//   me2_diag_stream_wait_draws) or of frames frozen for 250 ms or more. Each run of at least me2_diag_stream_wait_ms
//   gets one "[stream-wait]" line: how long, the SD reads in it (MB, time inside read(), MB/s while reading, how much
//   of the wait was spent reading), LZX decompression, garbage collection, texture uploads, and the files opened
//   during it. That tells whether a wait is the SD, decompression, collection, or something else.
//
// - me2_load_boost (2026-11-01, the mitigation for those waits): the Switch's CPU boost mode (appletSetCpuBoostMode
//   FastLoad: CPU 1785 MHz, GPU at its minimum; what retail games use for their loading screens) while a streaming wait
//   lasts: from its first frame with almost no draws, or from a watchdog when no Swap has come for 200 ms with the
//   game in focus (the game thread blocked in the load), back to normal at the first frame that draws again. UE3's
//   package loading (serialization, LZX) runs on the CPU at 1020 MHz otherwise; the GPU has next to nothing to do in a
//   wait. 2 = A/B: every other wait is boosted, and each "[stream-wait]" line says whether it was. Off by default (user:
//   waiting is fine; the problem was the black screen, see the image hold in renut's d3d hooks/device.cpp).
// - UE3's rendering suspend for blocking loads (2026-11-01): SuspendRenderingDuringLoad (command vtable 0x8222ADF8,
//   Execute sub_829FEDA8) calls sub_827DDB58, which suspends the D3D device (sub_82AA1058), marks it (device +10942
//   bit 2) and hands it over to the loading thread; ResumeRenderingDuringLoad (sub_829FEE98) calls sub_827DDC18. While
//   suspended the 360 kept the last frame on screen; g_me2_d3d_suspended tells the image hold (device.cpp) so.
//
// Except me2_gc_min_gap_s, me2_gc_stream_gap_s and me2_load_boost, everything here only measures.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "renut_engine/gpu/sa2_renut_pacing.h"

#include <rex/platform.h>
#if REX_PLATFORM_SWITCH
#include <switch.h>
// sdk/src/ui/switch_perf.cpp: a long frame's interval (armGetSystemTick ticks); with switch_profile_stacks the 10 s
// report lists what each thread was doing inside those intervals ("during the hitches").
extern "C" void RexSwitchPerfHitch(uint64_t start, uint64_t end);
// 2026-10-31: a CollectGarbage run's interval: "during the garbage collections" in the same report.
extern "C" void RexSwitchPerfGc(uint64_t start, uint64_t end);
#endif
// sdk xthread.cpp: guest threads of an entry point at another priority / cores while 'on' (each applies it itself).
extern "C" void RexSetGuestThreadBoost(uint32_t entry, int priority, uint32_t mask, bool on);

#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>
#endif

REXCVAR_DEFINE_BOOL(me2_diag_gc, false, "ME2/Diagnostics",
                    "Time UE3's garbage collection (CollectGarbage sub_827B0160: a line each run; "
                    "IncrementalPurgeGarbage sub_827AFC90: summed per 10 s)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_gc_min_gap_s, 0, "ME2",
                     "Garbage collections the world's tick asks for (the timer, script's ForceGarbageCollection, "
                     "full-purge requests) at least this many seconds apart: an earlier one waits for the end of the "
                     "gap. Map loads and travel are never held back. 0 = off")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_gc_stream_gap_s, 0, "ME2",
                     "Level-streaming garbage collections (sub_8259ABD0) at least this many seconds after the previous "
                     "collection: an earlier one is merged into one at the end of the gap. 0 = off")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_gc_stream_min_free_mb, 48, "ME2",
                     "me2_gc_stream_gap_s: a streaming collection is only held back while the guest's physical memory "
                     "has at least this many MB free")
    .range(0, 512)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-11-01 (run 21, overclocked: 36 collections in 14 min, 45-55 ms each = a visible hitch every 10-60 s, and all
// but the load ones freed nothing: free memory the same before and after): a held-back collection no longer runs at
// the end of its gap wherever the player is, but at the next quiet frame after it (at most me2_gc_quiet_draws draws:
// a menu, the pause screen, a loading screen, a load), where its hitch does not show; at the latest after
// me2_gc_max_gap_s, and at once when the guest's free memory is below me2_gc_stream_min_free_mb.
REXCVAR_DEFINE_INT32(me2_gc_max_gap_s, 0, "ME2",
                     "Held-back garbage collections (me2_gc_min_gap_s / me2_gc_stream_gap_s) wait past their gap for a "
                     "quiet frame (a menu, a loading screen), at most until this many seconds after the previous "
                     "collection; 0 = they run when the gap ends")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-11-01 (run 22: no quiet frame in 5 minutes of play, so the held collection ran at the 300 s maximum as the full
// purge an early streaming request had asked for: 345 ms in one frame, 206 of it destroying the objects; and while
// held, the streaming code asked ~1,700 times a minute, its unload waiting on the collection): streaming collections
// get a maximum of their own, and a held collection let go by a maximum (not at a quiet frame, not on low memory) runs
// as a normal one, its objects destroyed over the next frames by UE3's incremental purge, never as a full purge.
REXCVAR_DEFINE_INT32(me2_gc_stream_max_gap_s, 0, "ME2",
                     "me2_gc_max_gap_s for level-streaming collections (their unloads wait for one); 0 = the same as "
                     "me2_gc_max_gap_s")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(me2_gc_pool_boost_mask, 0, "ME2",
                      "During each garbage collection, UE3's thread pool threads (its marking tasks) may run on these "
                      "cores (hex mask, e.g. 0xD = cores 0, 2, 3); 0 = their own cores")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_gc_fast_waits, false, "ME2",
                    "During a garbage collection, guest polling loops yield without their 100 us backoff sleep: the "
                    "purge's per-resource waits on the render thread return at once (sdk thread_yield_backoff_us)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#if REX_PLATFORM_SWITCH
extern "C" std::atomic<int> g_rex_yield_no_backoff;  // sdk core/threading_posix.cpp
#endif
REXCVAR_DEFINE_INT32(me2_gc_pool_boost_prio, -1, "ME2",
                     "During each garbage collection, UE3's thread pool threads run at this Horizon priority (e.g. 58 = "
                     "0x3A, above the game threads' 0x3B); -1 = their own")
    .range(-1, 63)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_gc_quiet_draws, 40, "ME2",
                     "me2_gc_max_gap_s: a frame with at most this many draws is a quiet one (menus draw ~3, loading "
                     "screens ~10, gameplay hundreds)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_diag_spike_ms, 0, "ME2/Diagnostics",
                     "Frames (Swap to Swap) at least this long get a [spike] line with what ran in them, and a 10 s "
                     "frame-time histogram is logged ([frames]); 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_sa2_game_frames;                // src/sa2_capture.cpp: counted at each Swap
extern std::atomic<uint64_t> g_me2_draws_total;                // renut d3d hooks/draw.cpp: every draw
extern "C" void RexIoTotals(uint64_t* reads, uint64_t* bytes, uint64_t* us_reads, uint64_t* opens,
                            uint64_t* us_opens);                  // sdk xboxkrnl_io.cpp
extern "C" uint32_t RexIoOpensSince(uint64_t since_us, char* out, uint32_t size);  // sdk xboxkrnl_io.cpp

REXCVAR_DEFINE_BOOL(me2_diag_stream_wait, true, "ME2/Diagnostics",
                    "Log each wait for level streaming (a run of frames with almost no draws, or frozen): how long, "
                    "SD reads, decompression, garbage collection, files opened ('[stream-wait]')")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_diag_stream_wait_draws, 40, "ME2/Diagnostics",
                     "me2_diag_stream_wait: a frame with at most this many draws counts as waiting")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_load_boost, 0, "ME2",
                     "Switch: CPU boost mode (1785 MHz, GPU at its minimum) while the game waits for level streaming "
                     "or a load (frames with almost no draws, or frozen); 0 = off, 2 = A/B (every other wait)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_diag_stream_wait_ms, 500, "ME2/Diagnostics",
                     "me2_diag_stream_wait: only waits at least this long are logged")
    .range(0, 600000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern std::atomic<uint64_t> g_me2_occlusion_wait_total_us;     // me2_occlusion.cpp
extern std::atomic<uint64_t> g_me2_lzx_total_us;                // hot/me_hot_guest.cpp: LZX blocks, any thread
extern std::atomic<uint64_t> g_me2_texture_work_us;             // renut textures.cpp: texture uploads (render thread)
extern std::atomic<int32_t> g_me2_occlusion_wait_setting_us;   // me2_occlusion.cpp
extern "C" bool me2_name_text(const uint8_t* base, uint32_t index, char* out, size_t size);  // me2_cerberus.cpp
extern "C" bool me2_guest_readable(uint32_t va, uint32_t len);                                 // me2_cerberus.cpp

namespace {

using Clock = std::chrono::steady_clock;

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

bool GcOn() {
  static const bool on = REXCVAR_GET(me2_diag_gc);
  return on;
}

// Totals since the start (the frame observer takes differences per frame; the 10 s report per window).
std::atomic<uint64_t> g_gc_us{0}, g_gc_runs{0}, g_gc_max_us{0};          // CollectGarbage
std::atomic<uint64_t> g_purge_us{0}, g_purge_calls{0}, g_purge_max_us{0};  // IncrementalPurgeGarbage (max: per window)

void AtomicMax(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t old = a.load(std::memory_order_relaxed);
  while (v > old && !a.compare_exchange_weak(old, v, std::memory_order_relaxed)) {
  }
}

// ---- frame observer (the Swap's thread) ---------------------------------------------------------------------------
struct FrameState {
  bool init = false;
  Clock::time_point window_start{};
  uint64_t frames = 0, over34 = 0, over50 = 0, over67 = 0, over100 = 0, spikes = 0, spikes_dropped = 0;
  double worst = 0.0, work_sum = 0.0;
  double recent[6] = {};  // the frames before this one, newest first
  // Totals at the previous Swap.
  uint64_t gc_us = 0, gc_runs = 0, purge_us = 0, purge_calls = 0, wait_us = 0, lzx_us = 0, tex_us = 0;
  uint64_t uploads = 0, compiles = 0, compile_ns = 0, barriers_full = 0, barriers_frag = 0;
  // Totals at the window start.
  uint64_t w_gc_us = 0, w_gc_runs = 0, w_purge_us = 0, w_purge_calls = 0, w_wait_us = 0;
};
FrameState g_fs;

// ---- streaming waits (the Swap's thread) ---------------------------------------------------------------------------
struct StreamWait {
  bool active = false;
  uint64_t last_draws = 0;
  // At the start of the current run.
  uint64_t start_us = 0, frame0 = 0, frames = 0, max_draws = 0, draws = 0;
  double longest_ms = 0.0;
  uint64_t reads = 0, bytes = 0, us_reads = 0, opens = 0, us_opens = 0, lzx_us = 0, gc_us = 0, gc_runs = 0, tex_us = 0;
  uint64_t logged = 0;
};
StreamWait g_sw;
std::atomic<uint64_t> g_last_frame_draws{~0ull};  // the last Swap's frame (me2_gc_max_gap_s: quiet frames)

// ---- CPU boost while waiting (me2_load_boost) ----------------------------------------------------------------------
std::mutex g_boost_mutex;
bool g_boosted = false;
std::atomic<uint64_t> g_wait_index{0};      // waits begun so far (the A/B's parity)
std::atomic<uint64_t> g_last_swap_us{0};
std::atomic<bool> g_wait_boosted{false};    // the current wait got the boost (for its line)

int32_t BoostMode() {
  static const int32_t mode = REXCVAR_GET(me2_load_boost);
  return mode;
}
// Whether the coming (or current) wait is one to boost.
bool BoostThisWait() {
  const int32_t mode = BoostMode();
  return mode == 1 || (mode == 2 && (g_wait_index.load(std::memory_order_relaxed) & 1) == 0);
}

void SetBoost(bool on, const char* why) {
#if REX_PLATFORM_SWITCH
  std::lock_guard<std::mutex> lock(g_boost_mutex);
  if (g_boosted == on) return;
  const Result rc = appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
  static int failures = 0;
  if (R_FAILED(rc)) {
    if (failures++ < 3) REXLOG_WARN("[stream-wait] CPU boost {} failed (0x{:X}, {})", on ? "on" : "off", rc, why);
    return;
  }
  g_boosted = on;
  if (on) g_wait_boosted.store(true, std::memory_order_relaxed);
#else
  (void)on;
  (void)why;
#endif
}

// No Swap for 200 ms with the game in focus: the game thread is blocked (a load); boost before the frame comes.
void StartBoostWatchdog() {
#if REX_PLATFORM_SWITCH
  // Not std::call_once (pthread_once): only the Swap's thread calls this, an atomic flag is enough.
  static std::atomic<bool> started{false};
  if (started.load(std::memory_order_relaxed) || BoostMode() == 0) return;
  started.store(true, std::memory_order_relaxed);
  {
    // Left allocated, never detached (libnx has no pthread_detach: detach() throws; see me2_switch_save.cpp).
    new std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t last = g_last_swap_us.load(std::memory_order_relaxed);
        if (!last || NowUs() - last < 200000) continue;
        if (appletGetFocusState() != AppletFocusState_InFocus) continue;
        if (BoostThisWait()) SetBoost(true, "no Swap for 200 ms");
      }
    });
  }
#endif
}

void OnFrameStream(double work_ms) {
  static const bool on = REXCVAR_GET(me2_diag_stream_wait);
  if (!on) {
    static uint64_t before = 0;
    const uint64_t total = g_me2_draws_total.load(std::memory_order_relaxed);
    g_last_frame_draws.store(total - before, std::memory_order_relaxed);
    before = total;
    return;
  }
  static const uint64_t few = uint64_t(std::max(0, REXCVAR_GET(me2_diag_stream_wait_draws)));
  static const uint64_t min_us = uint64_t(std::max(0, REXCVAR_GET(me2_diag_stream_wait_ms))) * 1000;
  StreamWait& w = g_sw;
  const uint64_t draws_total = g_me2_draws_total.load(std::memory_order_relaxed);
  const uint64_t draws = draws_total - w.last_draws;
  w.last_draws = draws_total;
  g_last_frame_draws.store(draws, std::memory_order_relaxed);
  const bool waiting = draws <= few || work_ms >= 250.0;
  const uint64_t now = NowUs();
  uint64_t reads, bytes, us_reads, opens, us_opens;
  RexIoTotals(&reads, &bytes, &us_reads, &opens, &us_opens);
  const uint64_t lzx_us = g_me2_lzx_total_us.load(std::memory_order_relaxed);
  const uint64_t gc_us = g_gc_us.load(std::memory_order_relaxed), gc_runs = g_gc_runs.load(std::memory_order_relaxed);
  const uint64_t tex_us = g_me2_texture_work_us.load(std::memory_order_relaxed);
  g_last_swap_us.store(now, std::memory_order_relaxed);
  StartBoostWatchdog();
  if (waiting) {
    if (!w.active) {
      // The run starts with this frame: its own time is part of it.
      if (BoostThisWait()) SetBoost(true, "a frame with almost no draws");
      w.active = true;
      w.start_us = now - uint64_t(work_ms * 1000.0);
      w.frame0 = g_sa2_game_frames.load(std::memory_order_relaxed);
      w.frames = w.max_draws = w.draws = 0;
      w.longest_ms = 0.0;
      w.reads = reads, w.bytes = bytes, w.us_reads = us_reads, w.opens = opens, w.us_opens = us_opens;
      w.lzx_us = lzx_us, w.gc_us = gc_us, w.gc_runs = gc_runs, w.tex_us = tex_us;
    }
    ++w.frames;
    w.draws += draws;
    w.max_draws = std::max(w.max_draws, draws);
    w.longest_ms = std::max(w.longest_ms, work_ms);
    return;
  }
  // A drawing frame: the boost (from the watchdog, or a wait) ends here.
  SetBoost(false, "drawing again");
  const bool boosted = g_wait_boosted.exchange(false, std::memory_order_relaxed);
  if (!w.active) return;
  w.active = false;
  g_wait_index.fetch_add(1, std::memory_order_relaxed);
  const uint64_t length_us = now - uint64_t(work_ms * 1000.0) - w.start_us;
  if (length_us < min_us || w.logged >= 200) return;
  ++w.logged;
  const double secs = double(length_us) / 1e6;
  const double mb = double(bytes - w.bytes) / (1024.0 * 1024.0);
  const double read_ms = double(us_reads - w.us_reads) / 1000.0;
  char files[1024];
  const uint32_t opened = RexIoOpensSince(w.start_us, files, sizeof(files));
  REXLOG_INFO("[stream-wait] {:.2f} s ({} frames from frame {}, {:.1f} draws a frame, at most {}, longest frame {:.0f} ms): "
              "SD {} reads {:.1f} MB, {:.0f} ms inside read() ({:.0f} % of the wait; {:.1f} MB/s while reading, {:.1f} "
              "MB/s over the wait), {} opens {:.0f} ms; LZX {:.0f} ms (all threads); CollectGarbage {} run(s) {:.0f} ms; "
              "texture uploads {:.0f} ms; CPU boost {}; opened: {}",
              secs, w.frames, w.frame0, w.frames ? double(w.draws) / double(w.frames) : 0.0, w.max_draws, w.longest_ms,
              reads - w.reads, mb, read_ms, secs > 0 ? read_ms / (10.0 * secs) : 0.0,
              read_ms > 0 ? mb / (read_ms / 1000.0) : 0.0, secs > 0 ? mb / secs : 0.0, opens - w.opens,
              double(us_opens - w.us_opens) / 1000.0, double(lzx_us - w.lzx_us) / 1000.0, gc_runs - w.gc_runs,
              double(gc_us - w.gc_us) / 1000.0, double(tex_us - w.tex_us) / 1000.0,
              boosted ? "on" : (BoostMode() == 0 ? "off (me2_load_boost 0)" : "off"),
              opened ? std::string(files) : std::string("nothing"));
}

void OnFrame(double work_ms, double sleep_ms) {
  OnFrameStream(work_ms);
  static const int32_t spike_ms = REXCVAR_GET(me2_diag_spike_ms);
  if (spike_ms <= 0)
    return;
  FrameState& s = g_fs;
  const auto now = Clock::now();
  const uint64_t gc_us = g_gc_us.load(std::memory_order_relaxed), gc_runs = g_gc_runs.load(std::memory_order_relaxed);
  const uint64_t purge_us = g_purge_us.load(std::memory_order_relaxed);
  const uint64_t purge_calls = g_purge_calls.load(std::memory_order_relaxed);
  const uint64_t wait_us = g_me2_occlusion_wait_total_us.load(std::memory_order_relaxed);
  const uint64_t lzx_us = g_me2_lzx_total_us.load(std::memory_order_relaxed);
  const uint64_t tex_us = g_me2_texture_work_us.load(std::memory_order_relaxed);
#if defined(PLUME_DEKO3D)
  const plume::Deko3DStats& dk = plume::GetDeko3DStats();
  const uint64_t uploads = dk.copyUpload.load(std::memory_order_relaxed);
  const uint64_t compiles = dk.shaderCompiles.load(std::memory_order_relaxed);
  const uint64_t compile_ns = dk.shaderCompileNs.load(std::memory_order_relaxed);
  const uint64_t barriers_full = dk.barriersFull.load(std::memory_order_relaxed);
  const uint64_t barriers_frag = dk.barriersFragments.load(std::memory_order_relaxed);
#else
  const uint64_t uploads = 0, compiles = 0, compile_ns = 0, barriers_full = 0, barriers_frag = 0;
#endif
  if (!s.init) {
    s.init = true;
    s.window_start = now;
    s.w_gc_us = gc_us, s.w_gc_runs = gc_runs, s.w_purge_us = purge_us, s.w_purge_calls = purge_calls;
    s.w_wait_us = wait_us;
  } else {
    ++s.frames;
    s.work_sum += work_ms;
    s.worst = std::max(s.worst, work_ms);
    s.over34 += work_ms > 34.0;
    s.over50 += work_ms > 50.0;
    s.over67 += work_ms > 67.0;
    s.over100 += work_ms > 100.0;
    if (work_ms >= double(spike_ms)) {
#if REX_PLATFORM_SWITCH
      // 2026-10-31 (run 3: 136 of 290 frames over 70 ms had no cause in these counters): the spikes that garbage
      // collection, the incremental purge and occlusion waits do not explain go to the stack sampler's hitch section.
      const double known = double(gc_us - s.gc_us + purge_us - s.purge_us + wait_us - s.wait_us) / 1000.0;
      if (known < work_ms * 0.3) {
        const uint64_t freq = armGetSystemTickFreq(), now_tick = armGetSystemTick();
        const uint64_t end = now_tick - uint64_t(sleep_ms * double(freq) / 1000.0);
        const uint64_t length = uint64_t(work_ms * double(freq) / 1000.0);
        RexSwitchPerfHitch(end > length ? end - length : 0, end);
      }
#endif
      // At most 30 lines a 10 s window (a loading screen would flood the log otherwise).
      if (s.spikes < 30) {
        std::string before;
        for (double r : s.recent)
          if (r > 0.0) before += fmt::format(" {:.0f}", r);
        REXLOG_INFO("[spike] frame {}: {:.1f} ms Swap to Swap (paced {:.1f}; frames before:{}); since the previous "
                    "Swap: CollectGarbage {} run(s) {:.1f} ms, incremental purge {} call(s) {:.1f} ms, occlusion GetData "
                    "waits {:.1f} ms (setting {} us); LZX decompression {:.1f} ms (all threads); renderer: texture upload work "
                    "{:.1f} ms, {} texture uploads, {} shader compiles ({:.1f} ms), {} full + {} fragment barriers",
                    g_sa2_game_frames.load(std::memory_order_relaxed), work_ms, sleep_ms, before, gc_runs - s.gc_runs,
                    double(gc_us - s.gc_us) / 1000.0, purge_calls - s.purge_calls,
                    double(purge_us - s.purge_us) / 1000.0, double(wait_us - s.wait_us) / 1000.0,
                    g_me2_occlusion_wait_setting_us.load(std::memory_order_relaxed),
                    double(lzx_us - s.lzx_us) / 1000.0, double(tex_us - s.tex_us) / 1000.0, uploads - s.uploads,
                    compiles - s.compiles, double(compile_ns - s.compile_ns) / 1e6, barriers_full - s.barriers_full,
                    barriers_frag - s.barriers_frag);
      } else {
        ++s.spikes_dropped;
      }
      ++s.spikes;
    }
  }
  for (int i = 5; i > 0; --i)
    s.recent[i] = s.recent[i - 1];
  s.recent[0] = work_ms;
  s.gc_us = gc_us, s.gc_runs = gc_runs, s.purge_us = purge_us, s.purge_calls = purge_calls, s.wait_us = wait_us;
  s.lzx_us = lzx_us;
  s.tex_us = tex_us;
  s.uploads = uploads, s.compiles = compiles, s.compile_ns = compile_ns;
  s.barriers_full = barriers_full, s.barriers_frag = barriers_frag;

  if (now - s.window_start < std::chrono::seconds(10))
    return;
  const double secs = std::chrono::duration<double>(now - s.window_start).count();
  REXLOG_INFO("[frames] {:.1f} s: {} frames, {:.2f} ms a frame Swap to Swap; over 34 ms {}, over 50 ms {}, over 67 ms "
              "{}, over 100 ms {}, worst {:.1f} ms; {} spike lines ({} not logged, me2_diag_spike_ms {}); "
              "CollectGarbage {} run(s) {:.1f} ms, incremental purge {} calls {:.1f} ms (max {:.2f} ms); occlusion "
              "GetData waits {:.1f} ms, wait setting {} us",
              secs, s.frames, s.frames ? s.work_sum / double(s.frames) : 0.0, s.over34, s.over50, s.over67, s.over100,
              s.worst, s.spikes - s.spikes_dropped, s.spikes_dropped, spike_ms, gc_runs - s.w_gc_runs,
              double(gc_us - s.w_gc_us) / 1000.0, purge_calls - s.w_purge_calls,
              double(purge_us - s.w_purge_us) / 1000.0, double(g_purge_max_us.exchange(0, std::memory_order_relaxed)) / 1000.0,
              double(wait_us - s.w_wait_us) / 1000.0, g_me2_occlusion_wait_setting_us.load(std::memory_order_relaxed));
  s.window_start = now;
  s.frames = s.over34 = s.over50 = s.over67 = s.over100 = s.spikes = s.spikes_dropped = 0;
  s.worst = s.work_sum = 0.0;
  s.w_gc_us = gc_us, s.w_gc_runs = gc_runs, s.w_purge_us = purge_us, s.w_purge_calls = purge_calls;
  s.w_wait_us = wait_us;
}

// Registered before main: g_frame_observer is constant-initialised (an atomic of a function pointer).
struct RegisterObserver {
  RegisterObserver() { sa2::renut::g_frame_observer.store(&OnFrame, std::memory_order_relaxed); }
} g_register_observer;

}  // namespace

// ---- UE3 garbage collection -----------------------------------------------------------------------------------------
namespace {

constexpr uint32_t kGEngine = 0x833D5BA0u;  // [ ] = UEngine*: +764 TimeBetweenPurgingPendingKillObjects (float)
constexpr uint32_t kGWorld = 0x833D60B0u;   // [ ] = UWorld*: +388 TimeSinceLastPendingKillPurge, +392 bFullPurgeTriggered

uint32_t Load32(const uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}
float LoadF(const uint8_t* base, uint32_t va) {
  const uint32_t v = Load32(base, va);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t va, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(base + va, &v, 4);
}

int32_t GapS() {
  static const int32_t gap = REXCVAR_GET(me2_gc_min_gap_s);
  return gap;
}
int32_t StreamGapS() {
  static const int32_t gap = REXCVAR_GET(me2_gc_stream_gap_s);
  return gap;
}
// The guest's free physical memory in MB (its 512 MB physical heap), -1 if unknown.
int32_t PhysicalFreeMb() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) return -1;
  auto* heap = kernel->memory()->GetPhysicalHeap();
  if (!heap) return -1;
  return int32_t((uint64_t(heap->unreserved_page_count()) * heap->page_size()) >> 20);
}

// The game thread (UE3's world tick and script run there).
thread_local int t_world_tick = 0;  // inside UWorld::Tick sub_8247F790
thread_local int t_perform = 0;     // inside UWorld::PerformGarbageCollection sub_82480988
// me2-nx-deko (2026-11-06, me2_gc_fast_waits): inside UObject::CollectGarbage on this thread, and the render-command fence
// waits (sub_82335A58) of the current collection (game thread only).
thread_local bool t_in_gc = false;
struct FenceStats {
  uint32_t calls = 0, waited = 0;
  uint64_t us = 0, max_us = 0;
};
FenceStats g_fence;
std::atomic<uint64_t> g_last_gc_end_us{0};
// 2026-10-31 (run 4: 97 of 136 collections came straight from other engine code, not the tick's GC block): the
// innermost of the functions that call CollectGarbage directly (ME2_GC_CALLER wrappers below), 0 = none of them.
thread_local uint32_t t_gc_caller = 0;
struct CallerTally {
  uint32_t runs = 0;
  uint32_t full = 0;
  uint64_t us = 0;
};
std::map<uint32_t, CallerTally> g_gc_callers;  // the 60 s summary, game thread
bool g_deferred_full = false;
uint64_t g_deferred_since_gc = 0;
// me2_gc_stream_gap_s: a streaming collection is waiting for the end of its gap (the timer brings it back), and the
// 60 s tallies of those held back / let through by low memory.
bool g_stream_pending = false;
uint32_t g_stream_deferred_window = 0, g_stream_low_memory_window = 0;
// me2_gc_max_gap_s: how the held collections were let go (60 s tallies).
uint32_t g_quiet_runs_window = 0, g_max_runs_window = 0, g_low_memory_runs_window = 0;
// Why the last GapLeft returned 0: 0 = no holding (no gap, or no maximum), 1 = a quiet frame, 2 = a maximum, 3 = low
// memory. A full purge only goes ahead for 0, 1 and 3 (game thread).
int g_release = 0;
uint32_t g_full_downgraded_window = 0;

// The script's last ForceGarbageCollection, and the 60 s tallies of the GC natives' callers.
struct ForceCall {
  uint64_t us = 0;
  bool full = false;
  std::string who;
};
ForceCall g_last_force;
std::map<std::string, uint32_t> g_callers;  // "Native <- Class.Function"
uint64_t g_summary_from_us = 0;
uint32_t g_runs_by_origin[3] = {};
uint32_t g_deferred_window = 0, g_deferred_full_window = 0;

// "Class.Function" of the script frame that called a native (FFrame: +4 Node = the UFunction, +8 Object).
std::string ScriptCaller(const uint8_t* base, uint32_t frame) {
  if (!me2_guest_readable(frame, 12)) return "?";
  const uint32_t node = Load32(base, frame + 4), object = Load32(base, frame + 8);
  char func[64] = "?", cls[64] = "?";
  if (me2_guest_readable(node, 0x30)) me2_name_text(base, Load32(base, node + 0x2C), func, sizeof(func));
  if (me2_guest_readable(object, 0x38)) {
    const uint32_t klass = Load32(base, object + 0x34);
    if (me2_guest_readable(klass, 0x30)) me2_name_text(base, Load32(base, klass + 0x2C), cls, sizeof(cls));
  }
  return std::string(cls) + "." + func;
}

void NoteNative(const char* native, const std::string& who) {
  ++g_callers[std::string(native) + " <- " + who];
}

void Summary(uint64_t now) {
  if (!GcOn()) return;
  if (!g_summary_from_us) g_summary_from_us = now;
  if (now - g_summary_from_us < 60000000ull) return;
  std::string callers;
  std::vector<std::pair<uint32_t, std::string>> sorted;
  for (const auto& [k, v] : g_callers) sorted.emplace_back(v, k);
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (size_t i = 0; i < sorted.size() && i < 8; ++i)
    callers += fmt::format("; {} x{}", sorted[i].second, sorted[i].first);
  std::string by_caller;
  for (const auto& [addr, t] : g_gc_callers)
    by_caller += fmt::format("; {} {} runs ({} full) {:.0f} ms", addr ? fmt::format("sub_{:08X}", addr) : "other",
                             t.runs, t.full, double(t.us) / 1000.0);
  REXLOG_INFO("[gc] {:.0f} s: collections by origin: {} timer/forced (PerformGarbageCollection), {} full-purge "
              "requests of the world tick, {} other (loads, travel); {} deferred by me2_gc_min_gap_s {} ({} full); "
              "{} streaming collections merged by me2_gc_stream_gap_s {}; held ones let go: {} at a quiet frame, {} at "
              "a maximum ({} full purges made normal ones), {} on low memory; script GC natives{}; by direct caller{}",
              double(now - g_summary_from_us) / 1e6, g_runs_by_origin[0], g_runs_by_origin[1], g_runs_by_origin[2],
              g_deferred_window, GapS(), g_deferred_full_window, g_stream_deferred_window, StreamGapS(),
              g_quiet_runs_window, g_max_runs_window, g_full_downgraded_window, g_low_memory_runs_window,
              callers.empty() ? std::string(": none") : callers,
              by_caller.empty() ? std::string(": none") : by_caller);
  g_callers.clear();
  g_gc_callers.clear();
  g_runs_by_origin[0] = g_runs_by_origin[1] = g_runs_by_origin[2] = 0;
  g_deferred_window = g_deferred_full_window = 0;
  g_stream_deferred_window = g_stream_low_memory_window = 0;
  g_quiet_runs_window = g_max_runs_window = g_low_memory_runs_window = 0;
  g_full_downgraded_window = 0;
  g_summary_from_us = now;
}

bool LowMemory() {
  static const int32_t min_free = REXCVAR_GET(me2_gc_stream_min_free_mb);
  const int32_t free_mb = PhysicalFreeMb();
  return free_mb >= 0 && free_mb < min_free;
}


// Seconds until a held-back collection should ask again, or 0 when it may go now. 'gap_s': its minimum distance from
// the previous collection. With me2_gc_max_gap_s, past the gap it waits for a quiet frame (asking again every second),
// up to the maximum; low memory lets it go at once.
double GapLeft(uint64_t now, int32_t gap_s, int32_t max_gap) {
  g_release = 0;
  const uint64_t last = g_last_gc_end_us.load(std::memory_order_relaxed);
  if (gap_s <= 0 || !last) return 0.0;
  const double since = double(now - last) / 1e6;
  if (since < double(gap_s)) {
    if (LowMemory()) {
      ++g_low_memory_runs_window;
      g_release = 3;
      return 0.0;
    }
    return double(gap_s) - since;
  }
  if (max_gap <= gap_s) return 0.0;
  static const uint64_t quiet = uint64_t(std::max(0, REXCVAR_GET(me2_gc_quiet_draws)));
  if (g_last_frame_draws.load(std::memory_order_relaxed) <= quiet) {
    ++g_quiet_runs_window;
    g_release = 1;
    return 0.0;
  }
  if (LowMemory()) {
    ++g_low_memory_runs_window;
    g_release = 3;
    return 0.0;
  }
  if (since >= double(max_gap)) {
    ++g_max_runs_window;
    g_release = 2;
    return 0.0;
  }
  return std::min(1.0, double(max_gap) - since);
}
int32_t MaxGapS() {
  static const int32_t max_gap = REXCVAR_GET(me2_gc_max_gap_s);
  return max_gap;
}
int32_t StreamMaxGapS() {
  static const int32_t v = REXCVAR_GET(me2_gc_stream_max_gap_s);
  return v > 0 ? v : MaxGapS();
}
double GapLeft(uint64_t now) { return GapLeft(now, GapS(), MaxGapS()); }
double StreamGapLeft(uint64_t now) { return GapLeft(now, StreamGapS(), StreamMaxGapS()); }

// Re-arms the world's timer so that its tick asks again when the gap ends.
void RearmWorldTimer(uint8_t* base, uint32_t world, double left) {
  const uint32_t engine = Load32(base, kGEngine);
  if (!me2_guest_readable(world, 400) || !me2_guest_readable(engine, 768)) return;
  const float between = LoadF(base, engine + 764);
  StoreF(base, world + 388, std::max(0.0f, between - float(left)));
}

}  // namespace

// me2_gc_pool_boost_mask (2026-11-01, runs 24-25 stack samples): every collection hands its marking to 4 tasks of UE3's
// thread pool (sub_827D1DD8 -> sub_827D1BF8 -> FQueuedThreadPool::AddQueuedWork sub_827CB098) and polls with Sleep(0)
// until they are done: 41 % of the game thread's collection time with the pool on cores 2-3 at the game threads'
// priority, 56-71 % with it on cores 0, 2, 3 one step lower (diag_1120: every 0x3B thread then preempted it), while
// UE3's render thread left core 0 82-88 % idle. For the collection only, the pool threads take a higher priority and
// more cores (RexSetGuestThreadBoost, sdk xthread.cpp: each pool thread applies it itself when it wakes for a task,
// and goes back to its own at its next wait after the collection).
constexpr uint32_t kPoolThreadEntry = 0x827CB6D0;  // FQueuedThread's entry point (8 threads)
struct PoolBoostScope {
  bool on = false;
  PoolBoostScope() {
    static const uint32_t mask = REXCVAR_GET(me2_gc_pool_boost_mask);
    static const int32_t prio = REXCVAR_GET(me2_gc_pool_boost_prio);
    if (!mask && prio < 0) return;
    on = true;
    RexSetGuestThreadBoost(kPoolThreadEntry, prio, mask, true);
  }
  ~PoolBoostScope() {
    if (on) RexSetGuestThreadBoost(kPoolThreadEntry, -1, 0, false);
  }
};

REX_EXTERN(__imp__sub_8247F790);
REX_HOOK_RAW(sub_8247F790) {  // UWorld::Tick: its GC block tells full-purge requests from other CollectGarbage calls
  ++t_world_tick;
  __imp__sub_8247F790(ctx, base);
  --t_world_tick;
}

REX_EXTERN(__imp__sub_82480988);
REX_HOOK_RAW(sub_82480988) {  // UWorld::PerformGarbageCollection(this): the ini timer and script's forced collections
  const uint64_t now = NowUs();
  // me2_gc_stream_gap_s: the merged streaming collection, at the end of its (shorter) gap (and then a quiet frame).
  const bool stream_due = g_stream_pending && StreamGapLeft(now) == 0.0;
  if (const double left = stream_due ? 0.0 : GapLeft(now); left > 0.0) {
    RearmWorldTimer(base, ctx.r3.u32, left);
    ++g_deferred_since_gc;
    ++g_deferred_window;
    Summary(now);
    return;
  }
  ++t_perform;
  __imp__sub_82480988(ctx, base);
  --t_perform;
}

REX_EXTERN(__imp__sub_827B0160);
// me2_gc_fast_waits (2026-11-06): for the length of the collection, guest polling loops (Sleep(0), NtYieldExecution ->
// rex::thread::MaybeYield) yield without their 100 us backoff sleep (sdk threading_posix.cpp g_rex_yield_no_backoff). A
// purge waits on the render thread once per destroyed render resource (sub_82919A78 -> fence wait sub_82335A58), both
// threads polling; console 2026-11-06 (MSV Strontium Mule load, 1785 MHz): collections up to 820 ms, the game thread 54 %
// of them in the kernel, 73 % of that in those fence waits, while the render thread did no release work to speak of.
struct FastWaitScope {
  bool on = false;
  FastWaitScope() {
    static const bool fast = REXCVAR_GET(me2_gc_fast_waits);
    t_in_gc = true;
    g_fence = FenceStats{};
#if REX_PLATFORM_SWITCH
    if (fast) {
      on = true;
      g_rex_yield_no_backoff.fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)fast;
#endif
  }
  ~FastWaitScope() {
    t_in_gc = false;
#if REX_PLATFORM_SWITCH
    if (on) g_rex_yield_no_backoff.fetch_sub(1, std::memory_order_relaxed);
#endif
  }
};

// The render-command fence wait (FRenderCommandFence::Wait: while [r3] > r4, a virtual pump and Sleep(0)), counted during
// collections only.
#if REX_PLATFORM_SWITCH
extern "C" int RexSwitchGetCurrentThreadPriority(void);  // sdk core/threading_switch.cpp
#endif
REX_EXTERN(__imp__sub_82335A58);
REX_HOOK_RAW(sub_82335A58) {
  if (!t_in_gc) {
    __imp__sub_82335A58(ctx, base);
    return;
  }
  const uint32_t counter = ctx.r3.u32, target = ctx.r4.u32;
  const bool pending = counter && Load32(base, counter) > target;
  const uint64_t t0 = pending ? NowUs() : 0;
  __imp__sub_82335A58(ctx, base);
  ++g_fence.calls;
  if (pending) {
    const uint64_t us = NowUs() - t0;
    ++g_fence.waited;
    g_fence.us += us;
    g_fence.max_us = std::max(g_fence.max_us, us);
    // me2-nx-deko (2026-11-06): the loads' long collections (console 11-06: 1.7 s, 44 waits of ~30 ms each behind the
    // loading movie, UE3's rendering thread not running): what the wait loop pumps each time round ([0x8337BF0C]'s
    // vtable slot 7, then Sleep(0)) and at what priority the game thread sleeps. A few lines a session.
    static int logged = 0;
    if (us >= 5000 && logged < 6) {
      ++logged;
      const uint32_t obj = Load32(base, 0x8337BF0Cu);
      const uint32_t vt = obj ? Load32(base, obj) : 0;
      const uint32_t fn = vt ? Load32(base, vt + 28) : 0;
#if REX_PLATFORM_SWITCH
      const int prio = RexSwitchGetCurrentThreadPriority();
#else
      const int prio = -1;
#endif
      REXLOG_INFO("[gc]   long fence wait {:.1f} ms: counter {:08X} {} -> {} (target {}); the loop pumps {:08X} (object "
                  "{:08X}, vtable {:08X}); game thread host priority {:#x}",
                  double(us) / 1000.0, counter, Load32(base, counter) > target ? "still above" : "done", Load32(base, counter),
                  target, fn, obj, vt, prio);
    }
  }
}

REX_HOOK_RAW(sub_827B0160) {  // UObject::CollectGarbage(KeepFlags, bPerformFullPurge)
  PoolBoostScope pool_boost;  // me2_gc_pool_boost_mask: UE3's thread pool boosted for this collection
  FastWaitScope fast_waits;   // me2_gc_fast_waits (2026-11-06)
  const bool on = GcOn() || GapS() > 0 || StreamGapS() > 0;
  if (!on) {
    __imp__sub_827B0160(ctx, base);
    return;
  }
  // 0: the timer / forced path, 1: the world tick's full-purge request, 2: anything else (loads, travel).
  const int origin = t_perform ? 0 : (t_world_tick && ctx.r4.u32 == 1 ? 1 : 2);
  const uint64_t t0 = NowUs();
  if (origin == 1) {
    if (const double left = GapLeft(t0); left > 0.0) {
      // The tick clears bFullPurgeTriggered after this call; the re-armed timer brings the collection back as a full
      // purge (g_deferred_full) when the gap ends.
      RearmWorldTimer(base, Load32(base, kGWorld), left);
      g_deferred_full = true;
      ++g_deferred_since_gc;
      ++g_deferred_window;
      ++g_deferred_full_window;
      Summary(t0);
      return;
    }
  }
  // me2_gc_stream_gap_s: a level-streaming collection soon after the previous one waits for the end of the gap.
  if (origin == 2 && t_gc_caller == 0x8259ABD0u && StreamGapS() > 0) {
    const double left = StreamGapLeft(t0);
    if (left > 0.0) {
      const int32_t free_mb = PhysicalFreeMb();
      {
        RearmWorldTimer(base, Load32(base, kGWorld), left);
        const bool was_full = ctx.r4.u32 != 0;
        if (was_full) g_deferred_full = true;
        const bool first = !g_stream_pending;
        g_stream_pending = true;
        ++g_deferred_since_gc;
        ++g_stream_deferred_window;
        // One line a merge (run 21 logged 893, one a held request).
        if (GcOn() && first)
          REXLOG_INFO("[gc] streaming collection (sub_8259ABD0{}) held: merged into one {} (me2_gc_stream_gap_s {}; "
                      "physical memory {} MB free)",
                      was_full ? ", full purge" : "",
                      REXCVAR_GET(me2_gc_max_gap_s) > StreamGapS() ? "at the next quiet frame past the gap"
                                                                    : "at the end of the gap",
                      StreamGapS(), free_mb);
        Summary(t0);
        return;
      }
    }
  }
  const int32_t free_before_mb = GcOn() ? PhysicalFreeMb() : -1;
  bool made_full = false;
  // A held collection let go by a maximum during play: a normal collection (incremental purge), never a full one.
  const bool may_full = g_release != 2;
  if (origin == 0 && g_deferred_full && ctx.r4.u32 == 0 && may_full) {
    ctx.r4.u64 = 1;  // the deferred full-purge request
    made_full = true;
  }
  if (!may_full && ctx.r4.u32 != 0 && (origin == 0 || (origin == 2 && t_gc_caller == 0x8259ABD0u))) {
    ctx.r4.u64 = 0;
    ++g_full_downgraded_window;
  }
  g_release = 0;
  const uint64_t keep = ctx.r3.u64;
  const uint32_t full = ctx.r4.u32;
#if REX_PLATFORM_SWITCH
  const uint64_t gc_tick0 = armGetSystemTick();
#endif
  __imp__sub_827B0160(ctx, base);
#if REX_PLATFORM_SWITCH
  RexSwitchPerfGc(gc_tick0, armGetSystemTick());
#endif
  const uint64_t end = NowUs();
  g_last_gc_end_us.store(end, std::memory_order_relaxed);
  g_stream_pending = false;  // any collection serves a merged streaming one
  if (full) g_deferred_full = false;  // a full collection served a pending full-purge request
  if (!GcOn()) {
    g_deferred_since_gc = 0;
    return;
  }
  const uint64_t us = end - t0;
  g_gc_us.fetch_add(us, std::memory_order_relaxed);
  const uint64_t runs = g_gc_runs.fetch_add(1, std::memory_order_relaxed) + 1;
  AtomicMax(g_gc_max_us, us);
  ++g_runs_by_origin[origin];
  const uint32_t caller = t_gc_caller;
  CallerTally& tally = g_gc_callers[caller];
  ++tally.runs;
  tally.full += full ? 1 : 0;
  tally.us += us;
  static constexpr const char* kOrigin[3] = {"timer/forced", "full-purge request", "other (load/travel)"};
  std::string forced;
  if (g_last_force.us && t0 - g_last_force.us < 5000000ull) {
    forced = fmt::format("; script ForceGarbageCollection{} {:.1f} s before by {}", g_last_force.full ? "(full)" : "",
                         double(t0 - g_last_force.us) / 1e6, g_last_force.who);
    g_last_force.us = 0;
  }
  REXLOG_INFO("[gc] CollectGarbage #{}: {:.1f} ms ({}{}, called from {}; keep flags {:016X}, full purge {}; frame "
              "{}; {} deferred before it; longest so far {:.1f} ms; physical memory free {} -> {} MB){}",
              runs, double(us) / 1000.0, kOrigin[origin], made_full ? ", deferred full purge" : "",
              caller ? fmt::format("sub_{:08X}", caller) : std::string("no listed caller"), keep, full,
              g_sa2_game_frames.load(std::memory_order_relaxed), g_deferred_since_gc,
              double(g_gc_max_us.load(std::memory_order_relaxed)) / 1000.0, free_before_mb, PhysicalFreeMb(),
              forced);
  REXLOG_INFO("[gc]   render fence waits in it: {} calls, {} had to wait, {:.1f} ms in all (longest {:.2f} ms); fast "
              "waits {}",
              g_fence.calls, g_fence.waited, double(g_fence.us) / 1000.0, double(g_fence.max_us) / 1000.0,
              fast_waits.on ? "on" : "off");
  g_deferred_since_gc = 0;
  Summary(end);
}

// The engine functions that call UObject::CollectGarbage directly (callers of sub_827B0160 in the generated code;
// UWorld::Tick sub_8247F790 and PerformGarbageCollection sub_82480988 are hooked above): each only notes itself as the
// current caller around the original, so a collection says which of them asked for it.
#define ME2_GC_CALLER(ADDR)                         \
  REX_EXTERN(__imp__sub_##ADDR);                    \
  REX_HOOK_RAW(sub_##ADDR) {                        \
    const uint32_t outer = t_gc_caller;             \
    t_gc_caller = 0x##ADDR##u;                      \
    __imp__sub_##ADDR(ctx, base);                   \
    t_gc_caller = outer;                            \
  }
ME2_GC_CALLER(82447AA8)
ME2_GC_CALLER(824499C8)
ME2_GC_CALLER(8244A6D0)
ME2_GC_CALLER(825995C8)
ME2_GC_CALLER(82599A38)
ME2_GC_CALLER(8259ABD0)
ME2_GC_CALLER(8259ECA0)
ME2_GC_CALLER(827A8A90)
ME2_GC_CALLER(827D9818)
ME2_GC_CALLER(82863E20)
// sub_8287A818 = the console commands' Exec: me2_me1_import.cpp looks at "importcharacter" there first.
extern "C" void me2_me1_import_console_exec(PPCContext& ctx, uint8_t* base);
REX_EXTERN(__imp__sub_8287A818);
REX_HOOK_RAW(sub_8287A818) {
  me2_me1_import_console_exec(ctx, base);
  const uint32_t outer = t_gc_caller;
  t_gc_caller = 0x8287A818u;
  __imp__sub_8287A818(ctx, base);
  t_gc_caller = outer;
}
ME2_GC_CALLER(8297C628)
ME2_GC_CALLER(829F6990)
ME2_GC_CALLER(829FA268)
#undef ME2_GC_CALLER

// Script's GC natives (exec functions: r3 = the AWorldInfo, r4 = the calling script frame, r5 = the result).
REX_EXTERN(__imp__sub_82429B58);
REX_HOOK_RAW(sub_82429B58) {  // AWorldInfo.execForceGarbageCollection(optional bool bFullPurge)
  if (!GcOn()) {
    __imp__sub_82429B58(ctx, base);
    return;
  }
  const std::string who = ScriptCaller(base, ctx.r4.u32);
  const uint32_t world = Load32(base, kGWorld);
  const bool readable = me2_guest_readable(world, 400);
  const uint32_t before = readable ? Load32(base, world + 392) : 0;
  __imp__sub_82429B58(ctx, base);
  const uint32_t after = readable ? Load32(base, world + 392) : 0;
  g_last_force.us = NowUs();
  g_last_force.full = after != 0 && before == 0;
  g_last_force.who = who;
  NoteNative(g_last_force.full ? "ForceGarbageCollection(full)" : "ForceGarbageCollection", who);
}

REX_EXTERN(__imp__sub_82429C68);
REX_HOOK_RAW(sub_82429C68) {  // AWorldInfo.execDelayGarbageCollection
  if (GcOn()) NoteNative("DelayGarbageCollection", ScriptCaller(base, ctx.r4.u32));
  __imp__sub_82429C68(ctx, base);
}

REX_EXTERN(__imp__sub_82429D00);
REX_HOOK_RAW(sub_82429D00) {  // AWorldInfo.execRescheduleGarbageCollectionTimer
  if (GcOn()) NoteNative("RescheduleGarbageCollectionTimer", ScriptCaller(base, ctx.r4.u32));
  __imp__sub_82429D00(ctx, base);
}

REX_EXTERN(__imp__sub_827AFC90);
REX_HOOK_RAW(sub_827AFC90) {  // UObject::IncrementalPurgeGarbage(bUseTimeLimit)
  if (!GcOn()) {
    __imp__sub_827AFC90(ctx, base);
    return;
  }
  const uint64_t t0 = NowUs();
  __imp__sub_827AFC90(ctx, base);
  const uint64_t us = NowUs() - t0;
  g_purge_us.fetch_add(us, std::memory_order_relaxed);
  g_purge_calls.fetch_add(1, std::memory_order_relaxed);
  AtomicMax(g_purge_max_us, us);
}

// ---- UE3's rendering suspend during blocking loads ------------------------------------------------------------------
std::atomic<bool> g_me2_d3d_suspended{false};
std::atomic<uint64_t> g_me2_d3d_suspends{0};

REX_EXTERN(__imp__sub_827DDB58);
REX_HOOK_RAW(sub_827DDB58) {  // suspend rendering (SuspendRenderingDuringLoad, the render thread's exit)
  g_me2_d3d_suspended.store(true, std::memory_order_relaxed);
  g_me2_d3d_suspends.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_827DDB58(ctx, base);
}
REX_EXTERN(__imp__sub_827DDC18);
REX_HOOK_RAW(sub_827DDC18) {  // resume rendering (ResumeRenderingDuringLoad, ResumeRendering)
  __imp__sub_827DDC18(ctx, base);
  g_me2_d3d_suspended.store(false, std::memory_order_relaxed);
}
