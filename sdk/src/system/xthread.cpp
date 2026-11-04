/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <cstring>

#include <fmt/format.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/literals.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/platform/exceptions.h>
#include <rex/runtime.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/system/xevent.h>
#include <rex/system/xmutant.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/vec128.h>

#if REX_PLATFORM_SWITCH
#include <string_view>
#include <chrono>
#include <unordered_map>

#include "../core/threading_switch.h"
#if defined(__SWITCH__)
extern "C" char _start;  // me2-nx-deko (diag_wait_sites): the image's base
#endif
#endif

// ME2 (2026-10-26): the Switch ignores the priorities the game gives its threads (every guest thread runs at 0x3B,
// the only time-sliced level). This table gives chosen guest threads, by entry point, another Horizon priority:
// "ENTRY:PRIO,ENTRY:PRIO" in hex, e.g. "82E12D80:3A". Only for threads that block between their work (a thread above
// 0x3B that spins keeps its core from every guest thread). Applied when the thread starts.
REXCVAR_DEFINE_STRING(switch_guest_thread_priority, "", "Kernel",
                      "Switch: Horizon priority of chosen guest threads by entry point, \"ENTRY:PRIO,...\" in hex "
                      "(e.g. \"82E12D80:3A\"); empty = every guest thread at 0x3B")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// ME2 (2026-10-26): the fourth core. With the NSP forwarder (forwarder/build_forwarder.py, NPDM cores 0-3) the
// process may use core 3; the audio threads go there: the host Audio Worker and XMA Decoder, the audio output thread
// and the guest threads listed in switch_audio_guest_entries (ME2: Wwise's sound engine thread 82E12D80, ~28 % of a
// core). Without core 3 in the process mask nothing changes.
// 2026-10-26 22:55, console A/B: core 3 starved Wwise (~34 % of a core while it needs 50-65 % in combat; the system
// takes the rest of core 3): whole-mix gaps 850-1400 frames of 5.33 ms per 10 s from the NSP, ~0-17 from hbmenu
// (cores 0-2). The audio threads now default to cores 1-2; the other guest threads still get core 3 from the NSP.
REXCVAR_DEFINE_UINT32(switch_audio_core_mask, 0x6, "Kernel",
                      "Switch: core mask of the audio threads (Audio Worker, XMA Decoder, audio output, the guest "
                      "threads of switch_audio_guest_entries); only the cores the process may use (core 3 needs the "
                      "NSP forwarder). 0 = unchanged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-11-01): the host XMA Decoder thread alone on other cores (e.g. 0x8 = core 3). It decodes in the
// background after the game's kick (audio_xma_in_worker 1) and the game takes the PCM at its next 5.3 ms tick, so it can
// absorb the system's share of core 3 that starved Wwise's real-time mixing there (2026-10-26). 0 = the audio cores.
// 2026-11-01 run 9: default core 3 (audio dropouts 178/h -> 65/h, the decoder needs ~1 % of the core); trimmed to the
// process's cores, so without the NSP forwarder (no core 3) it falls back to switch_audio_core_mask.
REXCVAR_DEFINE_UINT32(switch_xma_core_mask, 0x8, "Kernel",
                      "Switch: core mask of the host XMA Decoder thread (e.g. 0x8 = core 3); 0 = switch_audio_core_mask")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(switch_audio_guest_entries, "", "Kernel",
                      "Switch: guest thread entry points (hex, comma-separated) that take switch_audio_core_mask")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// ME2 (2026-10-28): chosen guest threads on chosen cores, by entry point: "ENTRY:MASK[:PREFERRED],..." in hex, e.g.
// "830463B0:8" (the PhysX worker alone on core 3, beside the game thread on core 2 instead of taking turns with it).
// Trimmed to the process's cores (core 3 needs the NSP forwarder); the guest core layout leaves these threads there.
REXCVAR_DEFINE_STRING(switch_guest_thread_cores, "", "Kernel",
                      "Switch: core mask of chosen guest threads by entry point, \"ENTRY:MASK[:PREFERRED],...\" in hex "
                      "(e.g. \"830463B0:8\"); empty = the guest core layout for every guest thread")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(ignore_thread_priorities, true, "Kernel",
                    "Ignores game-specified thread priorities");

REXCVAR_DEFINE_BOOL(ignore_thread_affinities, true, "Kernel",
                    "Ignores game-specified thread affinities");

#if REX_PLATFORM_SWITCH
namespace {
// sa2-nx (2026-10-20, RexSetGuestThreadCoreLayout): the guest threads' cores besides the main thread's (0: the
// process's, as before), and the preferred one among them, for the threads created from then on.
std::atomic<uint32_t> g_guest_core_mask{0};
std::atomic<int32_t> g_guest_core_preferred{-1};
// sa2-nx (2026-10-20): each guest thread applies the layout itself (RexSetGuestThreadCoreLayout below): the layout's
// generation (0: none set), the main thread's core, and per thread the generation it applied and what it is (0 not a
// guest thread, 1 the main thread, 2 another guest thread).
std::atomic<uint32_t> g_guest_layout_gen{0};
std::atomic<int32_t> g_guest_main_core{0};
thread_local uint32_t t_layout_applied = 0;
thread_local uint8_t t_guest_kind = 0;
// ME2 (2026-10-26, RexSetGuestThreadCoreLayoutCaller): the thread that gets the main core, named by the address of its
// own t_layout_marker (nullptr: the XEX main thread, as before). In ME2 the XEX main thread idles (~1 % of a core);
// UE3's game thread is another guest thread.
std::atomic<const void*> g_layout_main_marker{nullptr};
thread_local char t_layout_marker = 0;
// ME2 (2026-10-26): the thread runs on the audio cores (switch_audio_core_mask); the guest core layout leaves it there.
thread_local bool t_audio_cores = false;
// ME2 (2026-10-28): the thread runs on its switch_guest_thread_cores mask; the guest core layout leaves it there.
thread_local bool t_fixed_cores = false;
// me2-nx-deko (2026-11-01, RexSetGuestThreadBoost): the guest thread's entry point (0: not a guest thread), and while
// boosted the priority and cores it had before.
thread_local uint32_t t_self_entry = 0;
thread_local bool t_boosted = false;
thread_local uint32_t t_boost_seen = 0;
thread_local int t_saved_prio = -1, t_saved_pref = -1;
thread_local unsigned long long t_saved_mask = 0;
std::atomic<uint32_t> g_boost_gen{0};
std::atomic<uint32_t> g_boost_entry{0}, g_boost_mask{0};
std::atomic<int32_t> g_boost_prio{-1};
std::atomic<bool> g_boost_on{false};

// switch_guest_thread_cores' mask and preferred core for an entry point (mask 0: none).
std::pair<uint32_t, int> GuestEntryCores(uint32_t entry) {
  struct Item {
    uint32_t entry, mask;
    int preferred;
  };
  static const std::vector<Item> table = [] {
    std::vector<Item> t;
    const std::string text = REXCVAR_GET(switch_guest_thread_cores);
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos) end = text.size();
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      const size_t c1 = item.find(':');
      if (c1 == std::string::npos) continue;
      const size_t c2 = item.find(':', c1 + 1);
      const uint32_t e = uint32_t(std::strtoul(item.substr(0, c1).c_str(), nullptr, 16));
      const uint32_t m = uint32_t(std::strtoul(item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1).c_str(), nullptr, 16)) & 0xFu;
      const int pref = c2 == std::string::npos ? -1 : int(std::strtol(item.substr(c2 + 1).c_str(), nullptr, 16));
      if (e && m) t.push_back({e, m, pref});
    }
    return t;
  }();
  for (const Item& i : table)
    if (i.entry == entry) return {i.mask, i.preferred};
  return {0, -1};
}

// The calling thread onto switch_audio_core_mask, trimmed to the process's cores. False (and unchanged) when none
// of them is the process's: core 3 without the NSP forwarder.
bool SetCurrentThreadAudioCores() {
  const unsigned long long use = (REXCVAR_GET(switch_audio_core_mask) & 0xFu) & RexSwitchProcessCoreMask();
  if (!use) return false;
  return RexSwitchSetCurrentThreadMask(__builtin_ctzll(use), use);
}

bool IsAudioGuestEntry(uint32_t entry) {
  static const std::vector<uint32_t> table = [] {
    std::vector<uint32_t> t;
    const std::string text = REXCVAR_GET(switch_audio_guest_entries);
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos) end = text.size();
      if (const uint32_t e = uint32_t(std::strtoul(text.substr(at, end - at).c_str(), nullptr, 16))) t.push_back(e);
      at = end + 1;
    }
    return t;
  }();
  return std::find(table.begin(), table.end(), entry) != table.end();
}
}  // namespace
#endif

namespace rex::system {

// sa2-nx (2026-10-20): RexSetGuestThreadCoreLayout's layout applied by the calling thread to itself, once per layout
// (a guest thread's own svcSetThreadCoreMask: it never waits for another thread). Called at every guest wait.
void ApplyGuestCoreLayoutAtWait() {
#if REX_PLATFORM_SWITCH
  const uint32_t gen = g_guest_layout_gen.load(std::memory_order_acquire);
  if (gen == 0 || gen == t_layout_applied) return;
  t_layout_applied = gen;
  if (t_audio_cores || t_fixed_cores) return;  // ME2: an audio thread / a switch_guest_thread_cores thread stays
  const void* main_marker = g_layout_main_marker.load(std::memory_order_acquire);
  const bool is_main = main_marker ? (main_marker == &t_layout_marker) : (t_guest_kind == 1);
  if (is_main) {
    const int core = g_guest_main_core.load(std::memory_order_relaxed);
    RexSwitchSetCurrentThreadMask(core, 1ull << core);
  } else if (t_guest_kind != 0) {  // ME2: the XEX main thread too, when the caller layout named another thread
    const uint32_t mask = g_guest_core_mask.load(std::memory_order_relaxed);
    if (mask) RexSwitchSetCurrentThreadMask(g_guest_core_preferred.load(std::memory_order_relaxed), mask);
  }
#endif
}

// me2-nx-deko (2026-11-01): RexSetGuestThreadBoost's setting, applied by each guest thread of that entry point to
// itself (as the core layout above: never set by another thread). Called before every guest wait (a boost that ended:
// back to the thread's own priority and cores before it sleeps) and after it (woken for work while a boost is on:
// that work runs boosted). Nothing to do most of the time: one atomic load.
void ApplyGuestBoost() {
#if REX_PLATFORM_SWITCH
  if (!t_self_entry) return;
  const uint32_t gen = g_boost_gen.load(std::memory_order_acquire);
  if (gen == t_boost_seen) return;
  t_boost_seen = gen;
  const bool want = g_boost_on.load(std::memory_order_relaxed) &&
                    g_boost_entry.load(std::memory_order_relaxed) == t_self_entry;
  if (want == t_boosted) return;
  if (want) {
    if (!RexSwitchGetCurrentThreadMask(&t_saved_pref, &t_saved_mask)) return;
    t_saved_prio = RexSwitchGetCurrentThreadPriority();
    const unsigned long long use = g_boost_mask.load(std::memory_order_relaxed) & RexSwitchProcessCoreMask();
    if (use) {
      const int pref = t_saved_pref >= 0 && ((use >> t_saved_pref) & 1) ? t_saved_pref : __builtin_ctzll(use);
      RexSwitchSetCurrentThreadMask(pref, use);
    }
    const int prio = g_boost_prio.load(std::memory_order_relaxed);
    if (prio >= 0) RexSwitchSetCurrentThreadPriority(prio);
    t_boosted = true;
  } else {
    if (t_saved_mask) RexSwitchSetCurrentThreadMask(t_saved_pref, t_saved_mask);
    if (t_saved_prio >= 0) RexSwitchSetCurrentThreadPriority(t_saved_prio);
    t_boosted = false;
  }
#endif
}

namespace {
// ApplyGuestBoost before a wait and after it.
struct GuestBoostAroundWait {
  GuestBoostAroundWait() { ApplyGuestBoost(); }
  ~GuestBoostAroundWait() { ApplyGuestBoost(); }
};
}  // namespace

const uint32_t XAPC::kSize;
const uint32_t XAPC::kDummyKernelRoutine;
const uint32_t XAPC::kDummyRundownRoutine;

using namespace rex::literals;

uint32_t next_xthread_id_ = 0;

XThread::XThread(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType), guest_thread_(true) {}

XThread::XThread(KernelState* kernel_state, uint32_t stack_size, uint32_t xapi_thread_startup,
                 uint32_t start_address, uint32_t start_context, uint32_t creation_flags,
                 bool guest_thread, bool main_thread, uint32_t guest_process)
    : XObject(kernel_state, kObjectType),
      thread_id_(++next_xthread_id_),
      guest_thread_(guest_thread),
      main_thread_(main_thread),
      apc_lock_old_irql_(0) {
  creation_params_.stack_size = stack_size;
  creation_params_.xapi_thread_startup = xapi_thread_startup;
  creation_params_.start_address = start_address;
  creation_params_.start_context = start_context;

  // top 8 bits = processor ID (or 0 for default)
  // bit 0 = 1 to create suspended
  creation_params_.creation_flags = creation_flags;
  creation_params_.guest_process = guest_process;

  // Adjust stack size - min of 16k.
  if (creation_params_.stack_size < 16 * 1024) {
    creation_params_.stack_size = 16 * 1024;
  }

  if (!guest_thread_) {
    host_object_ = true;
  }

  // The kernel does not take a reference. We must unregister in the dtor.
  kernel_state_->RegisterThread(this);
}

XThread::~XThread() {
  if (main_fiber_) {
    main_fiber_->Destroy();
    main_fiber_ = nullptr;
  }

  // Unregister first to prevent lookups while deleting.
  kernel_state_->UnregisterThread(this);

  thread_.reset();

  kernel_state_->memory()->SystemHeapFree(scratch_address_);
  kernel_state_->memory()->SystemHeapFree(tls_static_address_);
  kernel_state_->memory()->SystemHeapFree(pcr_address_);
  FreeStack();

  if (thread_) {
    // TODO(benvanik): platform kill
    REXSYS_ERROR("Thread disposed without exiting");
  }
}

thread_local XThread* current_xthread_tls_ = nullptr;

namespace {

XThread* GetBoundCurrentXThread() {
  return current_xthread_tls_;
}

}  // namespace

bool XThread::IsInThread() {
  return GetBoundCurrentXThread() != nullptr;
}

bool XThread::IsInThread(XThread* other) {
  return GetBoundCurrentXThread() == other;
}

// ---------------------------------------------------------------------------------------------------------------------
// me2-nx-deko (2026-11-01): diag_wait_sites. Run 10's slowest windows (20-24 fps) had UE3's render thread 69-96 % busy
// and the game thread 55-68 %: 40-50 ms frames that neither thread's work fills. This finds what they wait for: each
// wait of a listed guest thread is timed and filed under its guest call chain (the return address into the XDK wrapper
// that called the kernel, then four callers from the guest stack's back chain: saved LR at [back chain - 8]), and the
// thread logs its top sites every 10 s ("[waits]"). Host waits on those threads (Renut's) are tagged by the caller.
REXCVAR_DEFINE_STRING(diag_wait_sites, "", "Kernel",
                      "Diagnostic: guest thread entry points (hex, comma-separated) whose waits are timed by guest "
                      "call site ('[waits]' every 10 s); empty = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
// 2026-11-01 (run 11): the recompiled code does not keep the guest LR (calls are direct C++ calls), so the guest stack
// gave no sites. The chain is the HOST return addresses along the x29 frame chain instead (image offsets on the
// Switch; the generated functions are named sub_XXXXXXXX, so the ELF's symbols name the guest code that waited).
constexpr int kWaitFrames = 10;
struct WaitSite {
  uint64_t chain[kWaitFrames] = {};
  const char* tag = "";
  uint64_t ns = 0;
  uint32_t count = 0;
};
struct WaitProfileState {
  int state = -1;  // -1 not decided yet, 0 off, 1 on
  std::string name;
  uint32_t stack_lo = 0, stack_hi = 0;
  std::unordered_map<uint64_t, WaitSite> sites;
  uint64_t total_ns = 0, total_count = 0, since_ns = 0;
};
thread_local WaitProfileState t_wait_profile;

uint64_t WaitNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
bool WaitProfiled() {
  WaitProfileState& p = t_wait_profile;
  if (p.state >= 0) return p.state == 1;
  p.state = 0;
  static const std::vector<uint32_t> entries = [] {
    std::vector<uint32_t> v;
    const std::string text = REXCVAR_GET(diag_wait_sites);
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos) end = text.size();
      if (const uint32_t e = uint32_t(std::strtoul(text.substr(at, end - at).c_str(), nullptr, 16))) v.push_back(e);
      at = end + 1;
    }
    return v;
  }();
  if (entries.empty()) return false;
  XThread* t = GetBoundCurrentXThread();
  if (!t || !t->is_guest_thread()) return false;
  const uint32_t entry = t->creation_params()->start_address;
  if (std::find(entries.begin(), entries.end(), entry) == entries.end()) return false;
  p.state = 1;
  p.name = t->thread_name().empty() ? fmt::format("{:08X}", entry) : t->thread_name();
  p.stack_lo = t->stack_alloc_base();
  p.stack_hi = t->stack_alloc_base() + t->stack_alloc_size();
  p.since_ns = WaitNowNs();
  return true;
}
}  // namespace

uint64_t WaitProfileBegin() {
  if (t_wait_profile.state == 0) return 0;
  if (!WaitProfiled()) return 0;
  return WaitNowNs();
}

void WaitProfileEnd(uint64_t start_ns, const char* tag) {
  WaitProfileState& p = t_wait_profile;
  const uint64_t now = WaitNowNs();
  const uint64_t ns = now - start_ns;
  WaitSite probe;
  probe.tag = tag;
#if defined(__SWITCH__) && defined(__aarch64__)
  {
    const uintptr_t base = reinterpret_cast<uintptr_t>(&_start);  // the image's base (rex_profile.log "image+0x...")
    uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    for (int i = 0; i < kWaitFrames && fp; ++i) {
      const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(fp);
      const uintptr_t next = frame[0], lr = frame[1];
      if (!lr || lr < base) break;
      probe.chain[i] = lr - base;
      if (next <= fp || next - fp > 0x10000 || (next & 7)) break;
      fp = next;
    }
  }
#endif
  uint64_t key = reinterpret_cast<uintptr_t>(tag) * 0x9E3779B97F4A7C15ull;
  for (uint64_t a : probe.chain) key = (key ^ a) * 0x100000001B3ull;
  WaitSite& site = p.sites[key];
  if (!site.count) {
    std::memcpy(site.chain, probe.chain, sizeof(site.chain));
    site.tag = tag;
  }
  site.ns += ns;
  ++site.count;
  p.total_ns += ns;
  ++p.total_count;
  if (now - p.since_ns < 10'000'000'000ull) return;
  std::vector<const WaitSite*> top;
  for (const auto& [k, v] : p.sites) top.push_back(&v);
  std::sort(top.begin(), top.end(), [](const WaitSite* a, const WaitSite* b) { return a->ns > b->ns; });
  std::string text;
  for (size_t i = 0; i < top.size() && i < 10; ++i) {
    text += fmt::format("\n    {:7.1f} ms {:6} x  {:<10}", double(top[i]->ns) / 1e6, top[i]->count, top[i]->tag);
    for (uint64_t a : top[i]->chain)
      if (a) text += fmt::format(" +{:x}", a);
  }
  REXSYS_INFO("[waits] {} {:.1f} s: {} waits, {:.1f} ms waiting ({:.0f} %); top sites (host return addresses as "
              "image offsets, innermost first):{}",
              p.name, double(now - p.since_ns) / 1e9, p.total_count, double(p.total_ns) / 1e6,
              100.0 * double(p.total_ns) / double(now - p.since_ns), text);
  p.sites.clear();
  p.total_ns = p.total_count = 0;
  p.since_ns = now;
}

XThread* XThread::GetCurrentThread() {
  XThread* thread = GetBoundCurrentXThread();
  if (!thread) {
    assert_always("Attempting to use kernel stuff from a non-kernel thread");
  }
  return thread;
}

void XThread::CheckTitleTermination() {
  XThread* self = GetBoundCurrentXThread();
  if (self && self->is_guest_thread() && self->is_running() &&
      self->kernel_state()->is_terminating_title()) {
    // Unwind cleanly at this safe point. Does not return.
    self->Exit(0);
  }
}

uint32_t XThread::GetCurrentThreadHandle() {
  XThread* thread = XThread::GetCurrentThread();
  return thread->handle();
}

uint32_t XThread::GetCurrentThreadId() {
  XThread* thread = XThread::GetCurrentThread();
  return thread->guest_object<X_KTHREAD>()->thread_id;
}

uint32_t XThread::GetLastError() {
  XThread* thread = XThread::GetCurrentThread();
  return thread->last_error();
}

void XThread::SetLastError(uint32_t error_code) {
  XThread* thread = XThread::GetCurrentThread();
  thread->set_last_error(error_code);
}

uint32_t XThread::last_error() {
  return guest_object<X_KTHREAD>()->last_error;
}

void XThread::set_last_error(uint32_t error_code) {
  guest_object<X_KTHREAD>()->last_error = error_code;
}

void XThread::set_name(const std::string_view name) {
  std::lock_guard<std::mutex> lock(thread_lock_);
  thread_name_ = fmt::format("{} ({:08X})", name, handle());

  if (thread_) {
    // May be getting set before the thread is created.
    // One the thread is ready it will handle it.
    thread_->set_name(thread_name_);
  }
}

static uint8_t next_cpu = 0;
static uint8_t GetFakeCpuNumber(uint8_t proc_mask) {
  // NOTE: proc_mask is logical processors, not physical processors or cores.
  if (!proc_mask) {
    next_cpu = (next_cpu + 1) % 6;
    return next_cpu;  // is this reasonable?
    // TODO(Triang3l): Does the following apply here?
    // https://docs.microsoft.com/en-us/windows/win32/dxtecharts/coding-for-multiple-cores
    // "On Xbox 360, you must explicitly assign software threads to a particular
    //  hardware thread by using XSetThreadProcessor. Otherwise, all child
    //  threads will stay on the same hardware thread as the parent."
  }
  assert_false(proc_mask & 0xC0);

  uint8_t cpu_number = 7 - rex::lzcnt(proc_mask);
  assert_true(1 << cpu_number == proc_mask);
  assert_true(cpu_number < 6);
  return cpu_number;
}

void XThread::InitializeGuestObject() {
  auto guest_thread = guest_object<X_KTHREAD>();
  uint32_t guest_ptr = guest_object();

  guest_thread->header.type = 6;  // ThreadObject
  guest_thread->suspend_count = (creation_params_.creation_flags & X_CREATE_SUSPENDED) ? 1 : 0;

  // Self-referencing pointers for wait timeout timer/block
  guest_thread->unk_10 = guest_ptr + 0x010;
  guest_thread->unk_14 = guest_ptr + 0x010;

  guest_thread->wait_timeout_block.wait_list_entry.flink_ptr = guest_ptr + 0x20;
  guest_thread->wait_timeout_block.wait_list_entry.blink_ptr = guest_ptr + 0x20;
  guest_thread->wait_timeout_block.thread = guest_ptr;
  guest_thread->wait_timeout_block.object = guest_ptr + 0x18;
  guest_thread->wait_timeout_block.wait_result_xstatus = 0x0100;
  guest_thread->wait_timeout_block.wait_type = 0x0201;

  guest_thread->stack_base = stack_base_;
  guest_thread->stack_limit = stack_limit_;
  guest_thread->stack_kernel = stack_base_ - 240;
  guest_thread->tls_address = tls_dynamic_address_;
  guest_thread->thread_state = 0;

  // Initialize APC lists (kernel + user mode)
  guest_thread->apc_lists[0].Initialize(memory());
  guest_thread->apc_lists[1].Initialize(memory());

  // Set process pointer - use guest_process if provided, else default to title process.
  uint32_t process_ptr = creation_params_.guest_process
                             ? creation_params_.guest_process
                             : kernel_state_->process_info_block_address();
  guest_thread->process = process_ptr;
  guest_thread->may_queue_apcs = 1;

  // Set PRCB pointers (derived from this thread's PCR).
  uint32_t kpcrb = pcr_address_ + offsetof(X_KPCR, prcb_data);
  guest_thread->a_prcb_ptr = kpcrb;
  guest_thread->another_prcb_ptr = kpcrb;

  // PPCContext for spinlock helpers (valid before thread runs; r13 set at construction).
  auto* ctx = thread_state_->context();

  // Set per-thread process type and link into process thread list.
  if (process_ptr) {
    auto target_process = memory()->TranslateVirtual<X_KPROCESS*>(process_ptr);
    guest_thread->process_type = target_process->process_type;
    guest_thread->process_type_dup = target_process->process_type;

    auto old_irql =
        kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &target_process->thread_list_spinlock);
    util::XeInsertTailList(&target_process->thread_list, &guest_thread->process_threads, memory());
    target_process->thread_count = target_process->thread_count + 1;
    kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &target_process->thread_list_spinlock, old_irql);
  } else {
    guest_thread->process_type = X_PROCTYPE_USER;
    guest_thread->process_type_dup = X_PROCTYPE_USER;
  }

  guest_thread->msr_mask = 0xFDFFD7FF;
  // current_cpu is expected to be initialized externally via SetActiveCpu.
  guest_thread->stack_alloc_base = stack_base_;
  guest_thread->create_time = chrono::Clock::QueryGuestSystemTime();

  // Initialize timer_list as self-referencing
  guest_thread->timer_list.flink_ptr = guest_ptr + offsetof(X_KTHREAD, timer_list);
  guest_thread->timer_list.blink_ptr = guest_ptr + offsetof(X_KTHREAD, timer_list);

  guest_thread->thread_id = thread_id_;
  guest_thread->start_address = creation_params_.start_address;

  // Initialize unk_154 list as self-referencing
  guest_thread->unk_154.flink_ptr = guest_ptr + offsetof(X_KTHREAD, unk_154);
  guest_thread->unk_154.blink_ptr = guest_ptr + offsetof(X_KTHREAD, unk_154);

  guest_thread->last_error = 0;
  guest_thread->creation_flags = creation_params_.creation_flags;
  guest_thread->unk_17C = 1;
}

bool XThread::AllocateStack(uint32_t size) {
  auto heap = memory()->LookupHeap(kStackAddressRangeBegin);

  auto alignment = heap->page_size();
  auto padding = heap->page_size() * 2;  // Guard page size * 2
  size = rex::round_up(size, alignment);
  auto actual_size = size + padding;

  uint32_t address = 0;
  if (!heap->AllocRange(kStackAddressRangeBegin, kStackAddressRangeEnd, actual_size, alignment,
                        memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                        memory::kMemoryProtectRead | memory::kMemoryProtectWrite, false,
                        &address)) {
    return false;
  }

  stack_alloc_base_ = address;
  stack_alloc_size_ = actual_size;
  stack_limit_ = address + (padding / 2);
  stack_base_ = stack_limit_ + size;

  // Initialize the stack with junk
  memory()->Fill(stack_alloc_base_, actual_size, 0xBE);

  // Setup the guard pages
  heap->Protect(stack_alloc_base_, padding / 2, memory::kMemoryProtectNoAccess);
  heap->Protect(stack_base_, padding / 2, memory::kMemoryProtectNoAccess);

  return true;
}

void XThread::FreeStack() {
  if (stack_alloc_base_) {
    auto heap = memory()->LookupHeap(kStackAddressRangeBegin);
    heap->Release(stack_alloc_base_);

    stack_alloc_base_ = 0;
    stack_alloc_size_ = 0;
    stack_base_ = 0;
    stack_limit_ = 0;
  }
}

#if REX_PLATFORM_SWITCH
namespace {

// Horizon only time-slices threads at the preemption priority, 0x3B; every
// other priority is cooperative, and a thread that never blocks keeps its core
// from every other thread of the same or lower priority. Recompiled guest code
// spin-waits constantly, so guest threads run at 0x3B and host service threads
// above it (a lower number), able to preempt the spinners: audio first, then
// the GPU and the kernel's own workers. This is the scheme MarathonRecomp-NX
// ships with. Host threads are told apart by the name their owner gives them
// before Create().
int SwitchPriorityFor(bool guest_thread, std::string_view name) {
  if (guest_thread) {
    return REX_SWITCH_PRIO_GUEST;
  }
  // set_name() stores the name with the handle appended ("Audio Worker (F8000014)"),
  // so the prefix is compared. With an exact comparison the Audio Worker and the
  // XMA Decoder stayed at the presentation priority (0x2C) instead of the audio one
  // (0x2B), as rex_profile.log showed on the console.
  if (name.starts_with("Audio Worker") || name.starts_with("XMA Decoder")) {
    return REX_SWITCH_PRIO_AUDIO;
  }
  return REX_SWITCH_PRIO_PRESENT;  // GPU Commands, GPU VSync, Kernel Dispatch
}

// ME2 (2026-10-26): switch_guest_thread_priority's priority for a guest thread's entry point, or -1.
int GuestEntryPriority(uint32_t entry) {
  static const std::vector<std::pair<uint32_t, int>> table = [] {
    std::vector<std::pair<uint32_t, int>> t;
    const std::string text = REXCVAR_GET(switch_guest_thread_priority);
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos) end = text.size();
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      const size_t colon = item.find(':');
      if (colon == std::string::npos) continue;
      const uint32_t e = uint32_t(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16));
      const int prio = int(std::strtol(item.substr(colon + 1).c_str(), nullptr, 16));
      if (e && prio >= 0x2C && prio <= 0x3F) t.emplace_back(e, prio);  // never above the host service threads
    }
    return t;
  }();
  for (const auto& [e, prio] : table)
    if (e == entry) return prio;
  return -1;
}

}  // namespace
#endif

X_STATUS XThread::Create() {
  // Thread kernel object.
  if (!CreateNative<X_KTHREAD>()) {
    REXSYS_WARN("Unable to allocate thread object");
    return X_STATUS_NO_MEMORY;
  }

  // Allocate a stack.
  if (!AllocateStack(creation_params_.stack_size)) {
    return X_STATUS_NO_MEMORY;
  }

  // Allocate thread scratch.
  // This is used by interrupts/APCs/etc so we can round-trip pointers through.
  scratch_size_ = 4 * 16;
  scratch_address_ = memory()->SystemHeapAlloc(scratch_size_);

  // Allocate TLS block.
  // Games will specify a certain number of 4b slots that each thread will get.
  xex2_opt_tls_info* tls_header = nullptr;
  auto module = kernel_state_->GetExecutableModule();
  if (module) {
    module->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  }

  const uint32_t kDefaultTlsSlotCount = 1024;
  uint32_t tls_slots = kDefaultTlsSlotCount;
  uint32_t tls_extended_size = 0;
  if (tls_header && tls_header->slot_count) {
    tls_slots = tls_header->slot_count;
    tls_extended_size = tls_header->data_size;
  }

  // Allocate both the slots and the extended data.
  // Some TLS is compiled with the binary (declspec(thread)) vars. The game
  // will directly access those through 0(r13).
  uint32_t tls_slot_size = tls_slots * 4;
  tls_total_size_ = tls_slot_size + tls_extended_size;
  tls_static_address_ = memory()->SystemHeapAlloc(tls_total_size_);
  tls_dynamic_address_ = tls_static_address_ + tls_extended_size;
  if (!tls_static_address_) {
    REXSYS_WARN("Unable to allocate thread local storage block");
    return X_STATUS_NO_MEMORY;
  }

  // Zero all of TLS.
  memory()->Fill(tls_static_address_, tls_total_size_, 0);
  if (tls_extended_size) {
    // If game has extended data, copy in the default values.
    assert_not_zero(tls_header->raw_data_address);
    memory()->Copy(tls_static_address_, tls_header->raw_data_address, tls_header->raw_data_size);
  }

  // Allocate thread state block from heap.
  // https://web.archive.org/web/20170704035330/https://www.microsoft.com/msj/archive/S2CE.aspx
  // This is set as r13 for user code and some special inlined Win32 calls
  // (like GetLastError/etc) will poke it directly.
  // We try to use it as our primary store of data just to keep things all
  // consistent.
  // 0x000: pointer to tls data
  // 0x100: pointer to TEB(?)
  // 0x10C: Current CPU(?)
  // 0x150: if >0 then error states don't get set (DPC active bool?)
  // TEB:
  // 0x14C: thread id
  // 0x160: last error
  // So, at offset 0x100 we have a 4b pointer to offset 200, then have the
  // structure.
  pcr_address_ = memory()->SystemHeapAlloc(0x2D8);
  if (!pcr_address_) {
    REXSYS_WARN("Unable to allocate thread state block");
    return X_STATUS_NO_MEMORY;
  }

  // Create thread state - needed for interrupt callbacks and kernel exports
  thread_state_ =
      std::make_unique<runtime::ThreadState>(thread_id_, stack_base_, pcr_address_, memory());

  REXSYS_NOISY_DEBUG("XThread{:08X} ({:X}) Stack: {:08X}-{:08X}", handle(), thread_id_,
                     stack_limit_, stack_base_);

  uint8_t cpu_index = GetFakeCpuNumber(static_cast<uint8_t>(creation_params_.creation_flags >> 24));

  // Initialize the KTHREAD object.
  InitializeGuestObject();

  X_KPCR* pcr = memory()->TranslateVirtual<X_KPCR*>(pcr_address_);

  pcr->tls_ptr = tls_static_address_;
  pcr->pcr_ptr = pcr_address_;
  pcr->prcb_data.current_thread = guest_object();
  pcr->prcb = pcr_address_ + offsetof(X_KPCR, prcb_data);
  pcr->host_stash = reinterpret_cast<uint64_t>(thread_state_->context());
  pcr->current_irql = 0;

  pcr->stack_base_ptr = stack_base_;
  pcr->stack_end_ptr = stack_limit_;

  pcr->prcb_data.dpc_active = 0;

  // Always retain when starting - the thread owns itself until exited.
  RetainHandle();

  rex::thread::Thread::CreationParameters params;
  params.stack_size = 16_MiB;  // Allocate a big host stack.
  params.create_suspended = true;
  thread_ = rex::thread::Thread::Create(params, [this]() {
    rex::initialize_seh_thread();
#if REX_PLATFORM_SWITCH
    {
      // ME2 (2026-10-26): switch_guest_thread_priority, by the guest thread's entry point.
      const int entry_prio = guest_thread_ ? GuestEntryPriority(creation_params_.start_address) : -1;
      RexSwitchSetCurrentThreadPriority(entry_prio >= 0 ? entry_prio : SwitchPriorityFor(guest_thread_, thread_name_));
      if (entry_prio >= 0)
        REXSYS_INFO("[prio] guest thread entry {:08X} at Horizon priority {:#x} (switch_guest_thread_priority)",
                    creation_params_.start_address, entry_prio);
    }
    // sa2-nx (2026-10-20): known to the pthread layer by its kernel handle, so another thread can set its cores
    // (RexSetGuestThreadCoreLayout: the registry was empty, and setting another thread's mask failed quietly).
    RexSwitchRegisterCurrentThread();
    // sa2-nx (2026-10-20): a guest thread made after the layout was set takes the other guest threads' cores.
    // ME2 (2026-10-26): the audio threads onto switch_audio_core_mask (core 3 with the NSP forwarder).
    {
      const std::string_view n(thread_name_);
      const bool audio = guest_thread_ ? IsAudioGuestEntry(creation_params_.start_address)
                                       : (n.starts_with("Audio Worker") || n.starts_with("XMA Decoder"));
      // me2-nx-deko (2026-11-01, switch_xma_core_mask): the XMA Decoder thread on its own cores.
      const unsigned long long xma_use =
          (!guest_thread_ && n.starts_with("XMA Decoder")) ? ((REXCVAR_GET(switch_xma_core_mask) & 0xFu) &
                                                              RexSwitchProcessCoreMask())
                                                           : 0;
      if (xma_use && RexSwitchSetCurrentThreadMask(__builtin_ctzll(xma_use), xma_use)) {
        t_audio_cores = true;
        REXSYS_INFO("[cores] XMA Decoder on cores {:#x} (switch_xma_core_mask; process cores {:#x})", xma_use,
                    RexSwitchProcessCoreMask());
      } else if (audio && SetCurrentThreadAudioCores()) {
        t_audio_cores = true;
        REXSYS_INFO("[cores] {} {:08X} on the audio cores {:#x} (process cores {:#x})",
                    guest_thread_ ? "guest thread" : thread_name_.c_str(), creation_params_.start_address,
                    REXCVAR_GET(switch_audio_core_mask) & RexSwitchProcessCoreMask(), RexSwitchProcessCoreMask());
      }
    }
    // ME2 (2026-10-28): switch_guest_thread_cores, by the guest thread's entry point.
    if (guest_thread_ && !t_audio_cores) {
      const auto [want, preferred] = GuestEntryCores(creation_params_.start_address);
      const unsigned long long use = want & RexSwitchProcessCoreMask();
      if (use) {
        const int pref = preferred >= 0 && ((use >> preferred) & 1) ? preferred : __builtin_ctzll(use);
        if (RexSwitchSetCurrentThreadMask(pref, use)) {
          t_fixed_cores = true;
          REXSYS_INFO("[cores] guest thread {:08X} on cores {:#x}, preferred {} (switch_guest_thread_cores)",
                      creation_params_.start_address, use, pref);
        }
      }
    }
    if (guest_thread_ && !main_thread_ && !t_audio_cores && !t_fixed_cores) {
      t_layout_applied = g_guest_layout_gen.load(std::memory_order_acquire);
      const uint32_t mask = g_guest_core_mask.load(std::memory_order_acquire);
      if (mask)
        RexSwitchSetCurrentThreadMask(g_guest_core_preferred.load(std::memory_order_relaxed), mask);
    }
    t_guest_kind = guest_thread_ ? (main_thread_ ? 1 : 2) : 0;  // sa2-nx: ApplyGuestCoreLayoutAtWait
    t_self_entry = guest_thread_ ? creation_params_.start_address : 0;  // me2-nx-deko: ApplyGuestBoost
#endif
    runtime::ThreadState::Bind(thread_state_.get());

    // Set thread ID override. This is used by logging.
    rex::thread::set_current_thread_id(handle());

    // Set name immediately, if we have one.
    // ME2: XThread::set_name() may run concurrently, copy the name under its lock.
    std::string name;
    {
      std::lock_guard<std::mutex> lock(thread_lock_);
      name = thread_name_;
      thread_->set_name(name);
    }

    PROFILE_THREAD_ENTER(name.c_str());
    PROFILE_THREAD_CREATED();

    // Execute user code.
    current_xthread_tls_ = this;
    running_ = true;
    Execute();
    running_ = false;
    current_xthread_tls_ = nullptr;

    PROFILE_THREAD_EXITED();
    PROFILE_THREAD_EXIT();

#if REX_PLATFORM_SWITCH
    RexSwitchUnregisterCurrentThread();  // sa2-nx (2026-10-20): see RexSwitchRegisterCurrentThread above
#endif

    // Release the self-reference to the thread.
    ReleaseHandle();
  });

  if (!thread_) {
    // TODO(benvanik): translate error?
    REXSYS_ERROR("CreateThread failed");
    return X_STATUS_NO_MEMORY;
  }

  // Set the thread name based on host ID (for easier debugging).
  if (thread_name_.empty()) {
    // ME2 (2026-10-26): guest threads named by their entry point ("X82E12D80"), so rex_profile.log tells them apart
    // (the host id said nothing, and fits the profiler's 15 characters too).
    if (guest_thread_ && creation_params_.start_address)
      set_name(fmt::format("X{:08X}", creation_params_.start_address));
    else
      set_name(fmt::format("XThread{:04X}", thread_->system_id()));
  }
  // ME2 (2026-10-26, diagnostic): every guest thread with its entry point and creation flags (priority bits 0x60).
  if (guest_thread_)
    REXSYS_INFO("[prio] created guest thread {} entry {:08X} context {:08X} flags {:08X}{}", thread_name_,
                creation_params_.start_address, creation_params_.start_context, creation_params_.creation_flags,
                main_thread_ ? " (XEX main thread)" : "");

#if !REX_PLATFORM_SWITCH
  // On Switch the scheme in SwitchPriorityFor decides instead: this value would
  // reach svcSetThreadPriority as 0 or 1, outside an application's range.
  if (creation_params_.creation_flags & 0x60) {
    thread_->set_priority(creation_params_.creation_flags & 0x20 ? 1 : 0);
  }
#endif

  // Assign the newly created thread to the logical processor, and also set up
  // the current CPU in KPCR and KTHREAD.
  SetActiveCpu(cpu_index);

  // TODO(tomc): do we need thread notifications (related to processor thread management)?

  if ((creation_params_.creation_flags & X_CREATE_SUSPENDED) == 0) {
    // Start the thread now that we're all setup.
    thread_->Resume();
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XThread::Exit(int exit_code) {
  // This may only be called on the thread itself.
  assert_true(XThread::GetCurrentThread() == this);
  // Keep the object alive until Thread::Exit() transitions the host thread
  // into pthread_exit(). Otherwise ReleaseHandle() below may delete `this`
  // while this method is still running.
  auto self = retain_object(this);

  // Mark as terminated before running down APCs.
  auto kthread = guest_object<X_KTHREAD>();
  kthread->terminated = 1;

  // TODO(benvanik): dispatch events? waiters? etc?
  RundownAPCs();

  // Set exit code.
  X_KTHREAD* thread = guest_object<X_KTHREAD>();
  thread->header.signal_state = 1;
  thread->exit_status = exit_code;

  // Unlink thread from process thread list.
  uint32_t process_guest = thread->process;
  if (process_guest) {
    auto* ctx = thread_state_->context();
    auto kprocess = memory()->TranslateVirtual<X_KPROCESS*>(process_guest);
    auto old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &kprocess->thread_list_spinlock);
    util::XeRemoveEntryList(&thread->process_threads, memory());
    kprocess->thread_count = kprocess->thread_count - 1;
    kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &kprocess->thread_list_spinlock, old_irql);
  }

  kernel_state_->OnThreadExit(this);

  // TODO(tomc): do we need thread notifications (related to processor thread management)?

  // The guest stack belongs to the thread's execution lifetime, not the handle
  // lifetime. Games can keep thread handles after exit; holding the stack until
  // object destruction leaks the reserved stack range and eventually makes
  // ExCreateThread fail even though the old threads are no longer running.
  FreeStack();

  // NOTE: unless PlatformExit fails, expect it to never return!
  current_xthread_tls_ = nullptr;
  PROFILE_THREAD_EXIT();

  running_ = false;
  ReleaseHandle();

  // NOTE: this does not return!
  rex::thread::Thread::Exit(exit_code);
  return X_STATUS_SUCCESS;
}

X_STATUS XThread::Terminate(int exit_code) {
  // TODO(benvanik): inform the profiler that this thread is exiting.

  // Set exit code.
  X_KTHREAD* thread = guest_object<X_KTHREAD>();
  thread->header.signal_state = 1;
  thread->exit_status = exit_code;

  // TODO(tomc): do we need thread notifications (related to processor thread management)?

  running_ = false;
  if (XThread::IsInThread(this)) {
    // Same lifetime rule as Exit(): don't allow ReleaseHandle() to destroy
    // the thread object before Thread::Exit() reaches pthread_exit().
    auto self = retain_object(this);
    FreeStack();
    ReleaseHandle();
    rex::thread::Thread::Exit(exit_code);
  } else {
    thread_->Terminate(exit_code);
    FreeStack();
    ReleaseHandle();
  }

  return X_STATUS_SUCCESS;
}

void XThread::Execute() {
  REXSYS_NOISY_DEBUG("Execute thid {} (handle={:08X}, '{}', native={:08X})", thread_id_, handle(),
                     thread_name_, thread_->system_id());

  // Let the kernel know we are starting.
  kernel_state_->OnThreadExecute(this);

  // Dispatch any APCs that were queued before the thread was created first.
  DeliverAPCs();

  uint32_t address;
  std::vector<uint64_t> args;
  bool want_exit_code;
  int exit_code = 0;

  // If a XapiThreadStartup value is present, we use that as a trampoline.
  // Otherwise, we are a raw thread.
  if (creation_params_.xapi_thread_startup) {
    address = creation_params_.xapi_thread_startup;
    args.push_back(creation_params_.start_address);
    args.push_back(creation_params_.start_context);
    want_exit_code = false;
  } else {
    // Run user code.
    address = creation_params_.start_address;
    args.push_back(creation_params_.start_context);
    want_exit_code = true;
  }

  // NOTE(tomc): JIT execution replaced with direct function calls
  // In rexglue, guest code is compiled ahead of time and called directly.
  // The start_address points to a 32bit guest address, for which the function
  // dispatcher maintains a lookup table to retrieve the host function pointer.
  auto* runtime = Runtime::instance();
  if (!runtime || !runtime->function_dispatcher()) {
    REXSYS_ERROR("XThread::Execute - Runtime not initialized");
    return;
  }

  auto* dispatcher = runtime->function_dispatcher();
  auto* memory = runtime->memory();
  PPCFunc* func = dispatcher->GetFunction(address);
  if (!func) {
    REXSYS_ERROR("XThread::Execute - No function registered at {:08X}", address);
    return;
  }

  auto* ctx = thread_state_->context();
  uint8_t* base = memory->virtual_membase();

  // Pass arguments in r3, r4, ... per PPC calling convention
  if (args.size() > 0)
    ctx->r3.u64 = args[0];
  if (args.size() > 1)
    ctx->r4.u64 = args[1];
  if (args.size() > 2)
    ctx->r5.u64 = args[2];
  if (args.size() > 3)
    ctx->r6.u64 = args[3];
  if (args.size() > 4)
    ctx->r7.u64 = args[4];
  if (args.size() > 5)
    ctx->r8.u64 = args[5];
  if (args.size() > 6)
    ctx->r9.u64 = args[6];
  if (args.size() > 7)
    ctx->r10.u64 = args[7];

  ctx->fpscr.InitHost();

  // Convert this host thread to a fiber so SwitchTo works bidirectionally.
  // Required on Windows before any CreateFiber; provides the fallback handle
  // when another fiber switches back to the main execution context.
  main_fiber_ = rex::thread::Fiber::ConvertCurrentThread();

  // Execute the function
  REXSYS_NOISY_DEBUG("XThread::Execute - Calling function at {:08X}", address);
  func(*ctx, base);

  exit_code = static_cast<int>(ctx->r3.u32);

  // If we got here it means the execute completed without an exit being called.
  // Treat the return code as an implicit exit code (if desired).
  Exit(!want_exit_code ? 0 : exit_code);
}

void XThread::EnterCriticalRegion() {
  guest_object<X_KTHREAD>()->apc_disable_count--;
}

void XThread::LeaveCriticalRegion() {
  auto kthread = guest_object<X_KTHREAD>();
  auto apc_disable_count = ++kthread->apc_disable_count;
  if (apc_disable_count == 0) {
    // NOTE: intentionally not calling CheckApcs() here.
    // Delivering APCs here can cause them to fire in wrong contexts.
  }
}

void XThread::LockApc() {
  auto kthread = guest_object<X_KTHREAD>();
  apc_lock_old_irql_ =
      kernel::xboxkrnl::xeKeKfAcquireSpinLock(thread_state_->context(), &kthread->apc_lock);
}

void XThread::UnlockApc(bool queue_delivery) {
  auto kthread = guest_object<X_KTHREAD>();
  auto mem = memory();
  bool needs_apc = !kthread->apc_lists[0].empty(mem) || !kthread->apc_lists[1].empty(mem);
  kernel::xboxkrnl::xeKeKfReleaseSpinLock(thread_state_->context(), &kthread->apc_lock,
                                          apc_lock_old_irql_);
  if (needs_apc && queue_delivery) {
    // Match Edge/Canary behavior: callback is only a wakeup hint.
    // User APC execution happens on alertable wait return paths.
    thread_->QueueUserCallback([]() {});
  }
}

void XThread::EnqueueApc(uint32_t normal_routine, uint32_t normal_context, uint32_t arg1,
                         uint32_t arg2) {
  uint32_t apc_ptr = memory()->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    REXSYS_WARN("EnqueueApc: allocation failed (thid={}, normal={:08X})", thread_id_,
                normal_routine);
    return;
  }
  auto apc = memory()->TranslateVirtual<XAPC*>(apc_ptr);
  kernel::xboxkrnl::xeKeInitializeApc(apc, guest_object(), XAPC::kDummyKernelRoutine,
                                      XAPC::kDummyRundownRoutine, normal_routine,
                                      1 /* user apc mode */, normal_context);

  // Important: use the caller PPC context when queuing to another thread.
  // Using the target thread context here can corrupt APC lock/IRQL bookkeeping.
  PPCContext* queue_ctx =
      runtime::ThreadState::Get() ? runtime::current_ppc_context() : thread_state_->context();

  if (!kernel::xboxkrnl::xeKeInsertQueueApc(apc, arg1, arg2, 0, queue_ctx)) {
    memory()->SystemHeapFree(apc_ptr);
    REXSYS_ERROR(
        "EnqueueApc: queue rejected (thid={}, normal={:08X}, ctx={:08X}, arg1={:08X}, "
        "arg2={:08X})",
        thread_id_, normal_routine, normal_context, arg1, arg2);
    return;
  }
  // Match Edge/Canary behavior: callback is only a wakeup hint.
  // APCs are delivered via alertable wait handling.
  thread_->QueueUserCallback([]() {});
}

void XThread::DeliverAPCs() {
  auto mem = memory();
  auto* ctx = thread_state_->context();
  auto kthread = guest_object<X_KTHREAD>();
  auto* dispatcher = kernel_state_->function_dispatcher();

  auto old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &kthread->apc_lock);
  auto& user_apc_queue = kthread->apc_lists[1];

  while (!user_apc_queue.empty(mem) && kthread->apc_disable_count == 0) {
    PERF_counter_inc(kApcQueueDepth);
    XAPC* apc = user_apc_queue.HeadObject(mem);
    uint32_t apc_ptr = mem->HostToGuestVirtual(apc);
    bool needs_freeing = apc->kernel_routine != XAPC::kDummyKernelRoutine;

    util::XeRemoveEntryList(&apc->list_entry, mem);
    apc->enqueued = 0;

    kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);

    uint8_t* scratch_ptr = mem->TranslateVirtual(scratch_address_);
    memory::store_and_swap<uint32_t>(scratch_ptr + 0, apc->normal_routine);
    memory::store_and_swap<uint32_t>(scratch_ptr + 4, apc->normal_context);
    memory::store_and_swap<uint32_t>(scratch_ptr + 8, apc->arg1);
    memory::store_and_swap<uint32_t>(scratch_ptr + 12, apc->arg2);

    if (apc->kernel_routine != XAPC::kDummyKernelRoutine) {
      if (dispatcher->GetFunction(apc->kernel_routine)) {
        uint64_t kernel_args[] = {apc_ptr, scratch_address_ + 0, scratch_address_ + 4,
                                  scratch_address_ + 8, scratch_address_ + 12};
        dispatcher->ExecuteTrap(thread_state_.get(), apc->kernel_routine, kernel_args,
                                rex::countof(kernel_args));
      } else {
        REXSYS_ERROR("DeliverAPCs: kernel_routine {:08X} not found", uint32_t(apc->kernel_routine));
      }
    } else {
      mem->SystemHeapFree(apc_ptr);
      needs_freeing = false;
    }

    uint32_t normal_routine = memory::load_and_swap<uint32_t>(scratch_ptr + 0);
    uint32_t normal_context = memory::load_and_swap<uint32_t>(scratch_ptr + 4);
    uint32_t arg1 = memory::load_and_swap<uint32_t>(scratch_ptr + 8);
    uint32_t arg2 = memory::load_and_swap<uint32_t>(scratch_ptr + 12);

    if (normal_routine) {
      if (dispatcher->GetFunction(normal_routine)) {
        uint64_t normal_args[] = {normal_context, arg1, arg2};
        dispatcher->ExecuteTrap(thread_state_.get(), normal_routine, normal_args,
                                rex::countof(normal_args));
      } else {
        REXSYS_ERROR("DeliverAPCs: normal_routine {:08X} not found", normal_routine);
      }
    }

    REXSYS_DEBUG("Completed delivery of APC to {:08X} ({:08X}, {:08X}, {:08X})", normal_routine,
                 normal_context, arg1, arg2);

    if (needs_freeing) {
      mem->SystemHeapFree(apc_ptr);
    }

    old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &kthread->apc_lock);
  }

  kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);
}

void XThread::RundownAPCs() {
  assert_true(XThread::GetCurrentThread() == this);
  auto mem = memory();
  auto kthread = guest_object<X_KTHREAD>();
  auto* ctx = thread_state_->context();

  // Rundown both user (1) and kernel (0) APC lists.
  for (int mode = 1; mode >= 0; --mode) {
    auto old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &kthread->apc_lock);
    auto& apc_queue = kthread->apc_lists[mode];

    while (!apc_queue.empty(mem)) {
      XAPC* apc = apc_queue.HeadObject(mem);
      uint32_t apc_ptr = mem->HostToGuestVirtual(apc);
      bool needs_freeing = apc->kernel_routine == XAPC::kDummyKernelRoutine;

      util::XeRemoveEntryList(&apc->list_entry, mem);
      apc->enqueued = 0;

      kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);

      if (apc->rundown_routine == XAPC::kDummyRundownRoutine) {
        // No-op.
      } else if (apc->rundown_routine) {
        auto* dispatcher = kernel_state_->function_dispatcher();
        if (dispatcher->GetFunction(apc->rundown_routine)) {
          uint64_t rundown_args[] = {apc_ptr};
          dispatcher->Execute(thread_state_.get(), apc->rundown_routine, rundown_args,
                              rex::countof(rundown_args));
        } else {
          REXSYS_WARN("RundownAPCs: rundown_routine {:08X} not found",
                      uint32_t(apc->rundown_routine));
        }
      }

      if (needs_freeing) {
        mem->SystemHeapFree(apc_ptr);
      }

      old_irql = kernel::xboxkrnl::xeKeKfAcquireSpinLock(ctx, &kthread->apc_lock);
    }
    kernel::xboxkrnl::xeKeKfReleaseSpinLock(ctx, &kthread->apc_lock, old_irql);
  }
}

int32_t XThread::QueryPriority() {
  return thread_->priority();
}

void XThread::SetPriority(int32_t increment) {
  priority_ = increment;
  // ME2 (2026-10-26, diagnostic): which guest threads the game raises or lowers (the Switch drops these values: every
  // guest thread runs at 0x3B). The first 64 calls are logged with the thread's entry point.
  {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 64)
      REXSYS_INFO("[prio] guest thread {} (entry {:08X}, context {:08X}) SetPriority({})", thread_name_,
                  creation_params_.start_address, creation_params_.start_context, increment);
  }

  // Write priority to guest X_KTHREAD struct.
  auto kthread = guest_object<X_KTHREAD>();
  kthread->priority = static_cast<uint8_t>(std::clamp(increment, 0, 31));

  int32_t target_priority = 0;
  if (increment > 0x22) {
    target_priority = rex::thread::ThreadPriority::kHighest;
  } else if (increment > 0x11) {
    target_priority = rex::thread::ThreadPriority::kAboveNormal;
  } else if (increment < -0x22) {
    target_priority = rex::thread::ThreadPriority::kLowest;
  } else if (increment < -0x11) {
    target_priority = rex::thread::ThreadPriority::kBelowNormal;
  } else {
    target_priority = rex::thread::ThreadPriority::kNormal;
  }
  if (!REXCVAR_GET(ignore_thread_priorities)) {
    thread_->set_priority(target_priority);
  }
}

void XThread::SetAffinity(uint32_t affinity) {
  SetActiveCpu(GetFakeCpuNumber(affinity));
}

uint8_t XThread::active_cpu() const {
  // Prefer reading from guest KTHREAD (always available for guest threads,
  // kept in sync by SetActiveCpu). Avoids dependency on pcr_address_ which
  // may not be set yet if the thread is mid-creation.
  if (is_guest_thread()) {
    auto* kthread = memory()->TranslateVirtual<const X_KTHREAD*>(guest_object());
    return kthread->current_cpu;
  }
  if (!pcr_address_) {
    return 0;
  }
  const X_KPCR& pcr = *memory()->TranslateVirtual<const X_KPCR*>(pcr_address_);
  return pcr.prcb_data.current_cpu;
}

void XThread::SetActiveCpu(uint8_t cpu_index) {
  // May be called during thread creation - don't skip if current == new.

  assert_true(cpu_index < 6);

  // Write to guest KTHREAD (always available for guest threads).
  if (is_guest_thread()) {
    X_KTHREAD& thread_object = *memory()->TranslateVirtual<X_KTHREAD*>(guest_object());
    thread_object.current_cpu = cpu_index;
  }

  // Write to PCR if allocated (may not be during early creation).
  if (pcr_address_) {
    X_KPCR& pcr = *memory()->TranslateVirtual<X_KPCR*>(pcr_address_);
    pcr.prcb_data.current_cpu = cpu_index;
  }

  if (rex::thread::logical_processor_count() >= 6) {
    if (!REXCVAR_GET(ignore_thread_affinities)) {
      thread_->set_affinity_mask(uint64_t(1) << cpu_index);
    }
  } else {
    // Once. With fewer than 6 cores it warned on every call, and the
    // GPU interrupt makes that call on every vblank: on Switch, 60 warnings per
    // second written and flushed to the SD (29,771 in six minutes).
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      REXSYS_WARN("Too few processor cores - scheduling will be wonky");
    }
  }
}

bool XThread::GetTLSValue(uint32_t slot, uint32_t* value_out) {
  if (slot * 4 > tls_total_size_) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  *value_out = memory::load_and_swap<uint32_t>(mem);
  return true;
}

bool XThread::SetTLSValue(uint32_t slot, uint32_t value) {
  if (slot * 4 >= tls_total_size_) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  memory::store_and_swap<uint32_t>(mem, value);
  return true;
}

uint32_t XThread::suspend_count() {
  return guest_object<X_KTHREAD>()->suspend_count;
}

X_STATUS XThread::Resume(uint32_t* out_suspend_count) {
  auto guest_thread = guest_object<X_KTHREAD>();
  uint32_t unused_host_suspend_count = 0;

#if REX_PLATFORM_WIN32
  uint8_t previous_suspend_count =
      reinterpret_cast<std::atomic_uint8_t*>(&guest_thread->suspend_count)->fetch_sub(1);
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }
  return thread_->Resume(&unused_host_suspend_count) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  bool should_resume_host = false;
  {
    std::lock_guard<std::mutex> lock(suspend_mutex_);
    uint8_t previous = guest_thread->suspend_count;
    if (previous > 0) {
      guest_thread->suspend_count--;
    }
    if (out_suspend_count) {
      *out_suspend_count = previous;
    }
    should_resume_host = (guest_thread->suspend_count == 0);
    suspend_cv_.notify_all();
  }

  // Self-suspended threads are resumed via guest suspend count transitions.
  if (should_resume_host) {
    thread_->Resume(&unused_host_suspend_count);
  }
  return X_STATUS_SUCCESS;
#else
  uint8_t previous_suspend_count = guest_thread->suspend_count;
  if (guest_thread->suspend_count > 0) {
    --guest_thread->suspend_count;
  }
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }
  return thread_->Resume(&unused_host_suspend_count) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
#endif
}

X_STATUS XThread::Suspend(uint32_t* out_suspend_count) {
  auto guest_thread = guest_object<X_KTHREAD>();
  uint8_t previous_suspend_count =
      reinterpret_cast<std::atomic_uint8_t*>(&guest_thread->suspend_count)->fetch_add(1);
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }

  uint32_t unused_host_suspend_count = 0;
  // Wrapped to 0 - treat as not suspended.
  if (guest_thread->suspend_count == 0) {
    return X_STATUS_SUCCESS;
  }
  return thread_->Suspend(&unused_host_suspend_count) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
uint32_t XThread::SelfSuspend() {
  auto guest_thread = guest_object<X_KTHREAD>();
  std::unique_lock<std::mutex> lock(suspend_mutex_);
  uint32_t previous = guest_thread->suspend_count;
  guest_thread->suspend_count++;
  suspend_cv_.wait(lock, [guest_thread]() { return guest_thread->suspend_count == 0; });
  return previous;
}
#endif

X_STATUS XThread::Delay(uint32_t processor_mode, uint32_t alertable, uint64_t interval) {
  ApplyGuestCoreLayoutAtWait();  // sa2-nx (2026-10-20)
  GuestBoostAroundWait boost_around;  // me2-nx-deko (2026-11-01)
  WaitProfileScope wait_profile(interval == 0 ? "Sleep(0)" : "Delay");  // me2-nx-deko: diag_wait_sites
  int64_t timeout_ticks = interval;
  uint32_t timeout_ms;
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    timeout_ms = 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    timeout_ms = uint32_t(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    timeout_ms = 0;
  }
  timeout_ms = chrono::Clock::ScaleGuestDurationMillis(timeout_ms);
  CheckTitleTermination();
  if (alertable) {
    auto result = rex::thread::AlertableSleep(std::chrono::milliseconds(timeout_ms));
    CheckTitleTermination();
    switch (result) {
      default:
      case rex::thread::SleepResult::kSuccess:
        return X_STATUS_SUCCESS;
      case rex::thread::SleepResult::kAlerted:
        return X_STATUS_USER_APC;
    }
  } else {
    if (timeout_ms == 0) {
      if (priority_ <= rex::thread::ThreadPriority::kBelowNormal) {
        rex::thread::Sleep(std::chrono::microseconds(100));
      } else {
        rex::thread::MaybeYield();
      }
    } else {
      rex::thread::Sleep(std::chrono::milliseconds(timeout_ms));
    }
    CheckTitleTermination();
  }

  return X_STATUS_SUCCESS;
}

struct ThreadSavedState {
  uint32_t thread_id;
  bool is_main_thread;  // Is this the main thread?
  bool is_running;

  uint32_t apc_head;
  uint32_t tls_static_address;
  uint32_t tls_dynamic_address;
  uint32_t tls_total_size;
  uint32_t pcr_address;
  uint32_t stack_base;        // High address
  uint32_t stack_limit;       // Low address
  uint32_t stack_alloc_base;  // Allocation address
  uint32_t stack_alloc_size;  // Allocation size

  // Context (invalid if not running)
  struct {
    uint64_t lr;
    uint64_t ctr;
    uint64_t r[32];
    double f[32];
    vec128_t v[128];
    uint32_t cr[8];
    uint32_t fpscr;
    uint8_t xer_ca;
    uint8_t xer_ov;
    uint8_t xer_so;
    uint8_t vscr_sat;
    uint32_t pc;
  } context;
};

// Save PPCContext to ThreadSavedState
static void SaveContext(const PPCContext* ctx, ThreadSavedState& state) {
  state.context.lr = ctx->lr;
  state.context.ctr = ctx->ctr.u64;

  // GPRs - copy individually since PPCContext uses named registers
  state.context.r[0] = ctx->r0.u64;
  state.context.r[1] = ctx->r1.u64;
  state.context.r[2] = ctx->r2.u64;
  state.context.r[3] = ctx->r3.u64;
  state.context.r[4] = ctx->r4.u64;
  state.context.r[5] = ctx->r5.u64;
  state.context.r[6] = ctx->r6.u64;
  state.context.r[7] = ctx->r7.u64;
  state.context.r[8] = ctx->r8.u64;
  state.context.r[9] = ctx->r9.u64;
  state.context.r[10] = ctx->r10.u64;
  state.context.r[11] = ctx->r11.u64;
  state.context.r[12] = ctx->r12.u64;
  state.context.r[13] = ctx->r13.u64;
  // r14-r31: contiguous in PPCContext and state array
  std::memcpy(&state.context.r[14], &ctx->r14, 18 * sizeof(uint64_t));

  // FPRs
  state.context.f[0] = ctx->f0.f64;
  state.context.f[1] = ctx->f1.f64;
  state.context.f[2] = ctx->f2.f64;
  state.context.f[3] = ctx->f3.f64;
  state.context.f[4] = ctx->f4.f64;
  state.context.f[5] = ctx->f5.f64;
  state.context.f[6] = ctx->f6.f64;
  state.context.f[7] = ctx->f7.f64;
  state.context.f[8] = ctx->f8.f64;
  state.context.f[9] = ctx->f9.f64;
  state.context.f[10] = ctx->f10.f64;
  state.context.f[11] = ctx->f11.f64;
  state.context.f[12] = ctx->f12.f64;
  state.context.f[13] = ctx->f13.f64;
  // f14-f31: contiguous in PPCContext and state array
  std::memcpy(&state.context.f[14], &ctx->f14, 18 * sizeof(double));

  // VRs (v0-v127)
  std::memcpy(&state.context.v[0], &ctx->v0, sizeof(vec128_t));
  std::memcpy(&state.context.v[1], &ctx->v1, sizeof(vec128_t));
  std::memcpy(&state.context.v[2], &ctx->v2, sizeof(vec128_t));
  std::memcpy(&state.context.v[3], &ctx->v3, sizeof(vec128_t));
  std::memcpy(&state.context.v[4], &ctx->v4, sizeof(vec128_t));
  std::memcpy(&state.context.v[5], &ctx->v5, sizeof(vec128_t));
  std::memcpy(&state.context.v[6], &ctx->v6, sizeof(vec128_t));
  std::memcpy(&state.context.v[7], &ctx->v7, sizeof(vec128_t));
  std::memcpy(&state.context.v[8], &ctx->v8, sizeof(vec128_t));
  std::memcpy(&state.context.v[9], &ctx->v9, sizeof(vec128_t));
  std::memcpy(&state.context.v[10], &ctx->v10, sizeof(vec128_t));
  std::memcpy(&state.context.v[11], &ctx->v11, sizeof(vec128_t));
  std::memcpy(&state.context.v[12], &ctx->v12, sizeof(vec128_t));
  std::memcpy(&state.context.v[13], &ctx->v13, sizeof(vec128_t));
  // v14-v31: contiguous in PPCContext and state array
  std::memcpy(&state.context.v[14], &ctx->v14, 18 * sizeof(vec128_t));
  std::memcpy(&state.context.v[32], &ctx->v32, sizeof(vec128_t));
  std::memcpy(&state.context.v[33], &ctx->v33, sizeof(vec128_t));
  std::memcpy(&state.context.v[34], &ctx->v34, sizeof(vec128_t));
  std::memcpy(&state.context.v[35], &ctx->v35, sizeof(vec128_t));
  std::memcpy(&state.context.v[36], &ctx->v36, sizeof(vec128_t));
  std::memcpy(&state.context.v[37], &ctx->v37, sizeof(vec128_t));
  std::memcpy(&state.context.v[38], &ctx->v38, sizeof(vec128_t));
  std::memcpy(&state.context.v[39], &ctx->v39, sizeof(vec128_t));
  std::memcpy(&state.context.v[40], &ctx->v40, sizeof(vec128_t));
  std::memcpy(&state.context.v[41], &ctx->v41, sizeof(vec128_t));
  std::memcpy(&state.context.v[42], &ctx->v42, sizeof(vec128_t));
  std::memcpy(&state.context.v[43], &ctx->v43, sizeof(vec128_t));
  std::memcpy(&state.context.v[44], &ctx->v44, sizeof(vec128_t));
  std::memcpy(&state.context.v[45], &ctx->v45, sizeof(vec128_t));
  std::memcpy(&state.context.v[46], &ctx->v46, sizeof(vec128_t));
  std::memcpy(&state.context.v[47], &ctx->v47, sizeof(vec128_t));
  std::memcpy(&state.context.v[48], &ctx->v48, sizeof(vec128_t));
  std::memcpy(&state.context.v[49], &ctx->v49, sizeof(vec128_t));
  std::memcpy(&state.context.v[50], &ctx->v50, sizeof(vec128_t));
  std::memcpy(&state.context.v[51], &ctx->v51, sizeof(vec128_t));
  std::memcpy(&state.context.v[52], &ctx->v52, sizeof(vec128_t));
  std::memcpy(&state.context.v[53], &ctx->v53, sizeof(vec128_t));
  std::memcpy(&state.context.v[54], &ctx->v54, sizeof(vec128_t));
  std::memcpy(&state.context.v[55], &ctx->v55, sizeof(vec128_t));
  std::memcpy(&state.context.v[56], &ctx->v56, sizeof(vec128_t));
  std::memcpy(&state.context.v[57], &ctx->v57, sizeof(vec128_t));
  std::memcpy(&state.context.v[58], &ctx->v58, sizeof(vec128_t));
  std::memcpy(&state.context.v[59], &ctx->v59, sizeof(vec128_t));
  std::memcpy(&state.context.v[60], &ctx->v60, sizeof(vec128_t));
  std::memcpy(&state.context.v[61], &ctx->v61, sizeof(vec128_t));
  std::memcpy(&state.context.v[62], &ctx->v62, sizeof(vec128_t));
  std::memcpy(&state.context.v[63], &ctx->v63, sizeof(vec128_t));
  // v64-v127: contiguous in PPCContext and state array
  std::memcpy(&state.context.v[64], &ctx->v64, 64 * sizeof(vec128_t));

  // CR fields
  state.context.cr[0] = ctx->cr0.raw();
  state.context.cr[1] = ctx->cr1.raw();
  state.context.cr[2] = ctx->cr2.raw();
  state.context.cr[3] = ctx->cr3.raw();
  state.context.cr[4] = ctx->cr4.raw();
  state.context.cr[5] = ctx->cr5.raw();
  state.context.cr[6] = ctx->cr6.raw();
  state.context.cr[7] = ctx->cr7.raw();

  // Other state
  state.context.fpscr = ctx->fpscr.csr;
  state.context.xer_ca = ctx->xer.ca;
  state.context.xer_ov = ctx->xer.ov;
  state.context.xer_so = ctx->xer.so;
  state.context.vscr_sat = ctx->vscr_sat;
}

// Load ThreadSavedState into PPCContext
static void LoadContext(PPCContext* ctx, const ThreadSavedState& state) {
  ctx->lr = state.context.lr;
  ctx->ctr.u64 = state.context.ctr;

  // GPRs
  ctx->r0.u64 = state.context.r[0];
  ctx->r1.u64 = state.context.r[1];
  ctx->r2.u64 = state.context.r[2];
  ctx->r3.u64 = state.context.r[3];
  ctx->r4.u64 = state.context.r[4];
  ctx->r5.u64 = state.context.r[5];
  ctx->r6.u64 = state.context.r[6];
  ctx->r7.u64 = state.context.r[7];
  ctx->r8.u64 = state.context.r[8];
  ctx->r9.u64 = state.context.r[9];
  ctx->r10.u64 = state.context.r[10];
  ctx->r11.u64 = state.context.r[11];
  ctx->r12.u64 = state.context.r[12];
  ctx->r13.u64 = state.context.r[13];
  // r14-r31: contiguous in PPCContext and state array
  std::memcpy(&ctx->r14, &state.context.r[14], 18 * sizeof(uint64_t));

  // FPRs
  ctx->f0.f64 = state.context.f[0];
  ctx->f1.f64 = state.context.f[1];
  ctx->f2.f64 = state.context.f[2];
  ctx->f3.f64 = state.context.f[3];
  ctx->f4.f64 = state.context.f[4];
  ctx->f5.f64 = state.context.f[5];
  ctx->f6.f64 = state.context.f[6];
  ctx->f7.f64 = state.context.f[7];
  ctx->f8.f64 = state.context.f[8];
  ctx->f9.f64 = state.context.f[9];
  ctx->f10.f64 = state.context.f[10];
  ctx->f11.f64 = state.context.f[11];
  ctx->f12.f64 = state.context.f[12];
  ctx->f13.f64 = state.context.f[13];
  // f14-f31: contiguous in PPCContext and state array
  std::memcpy(&ctx->f14, &state.context.f[14], 18 * sizeof(double));

  // VRs (v0-v127)
  std::memcpy(&ctx->v0, &state.context.v[0], sizeof(vec128_t));
  std::memcpy(&ctx->v1, &state.context.v[1], sizeof(vec128_t));
  std::memcpy(&ctx->v2, &state.context.v[2], sizeof(vec128_t));
  std::memcpy(&ctx->v3, &state.context.v[3], sizeof(vec128_t));
  std::memcpy(&ctx->v4, &state.context.v[4], sizeof(vec128_t));
  std::memcpy(&ctx->v5, &state.context.v[5], sizeof(vec128_t));
  std::memcpy(&ctx->v6, &state.context.v[6], sizeof(vec128_t));
  std::memcpy(&ctx->v7, &state.context.v[7], sizeof(vec128_t));
  std::memcpy(&ctx->v8, &state.context.v[8], sizeof(vec128_t));
  std::memcpy(&ctx->v9, &state.context.v[9], sizeof(vec128_t));
  std::memcpy(&ctx->v10, &state.context.v[10], sizeof(vec128_t));
  std::memcpy(&ctx->v11, &state.context.v[11], sizeof(vec128_t));
  std::memcpy(&ctx->v12, &state.context.v[12], sizeof(vec128_t));
  std::memcpy(&ctx->v13, &state.context.v[13], sizeof(vec128_t));
  // v14-v31: contiguous in PPCContext and state array
  std::memcpy(&ctx->v14, &state.context.v[14], 18 * sizeof(vec128_t));
  std::memcpy(&ctx->v32, &state.context.v[32], sizeof(vec128_t));
  std::memcpy(&ctx->v33, &state.context.v[33], sizeof(vec128_t));
  std::memcpy(&ctx->v34, &state.context.v[34], sizeof(vec128_t));
  std::memcpy(&ctx->v35, &state.context.v[35], sizeof(vec128_t));
  std::memcpy(&ctx->v36, &state.context.v[36], sizeof(vec128_t));
  std::memcpy(&ctx->v37, &state.context.v[37], sizeof(vec128_t));
  std::memcpy(&ctx->v38, &state.context.v[38], sizeof(vec128_t));
  std::memcpy(&ctx->v39, &state.context.v[39], sizeof(vec128_t));
  std::memcpy(&ctx->v40, &state.context.v[40], sizeof(vec128_t));
  std::memcpy(&ctx->v41, &state.context.v[41], sizeof(vec128_t));
  std::memcpy(&ctx->v42, &state.context.v[42], sizeof(vec128_t));
  std::memcpy(&ctx->v43, &state.context.v[43], sizeof(vec128_t));
  std::memcpy(&ctx->v44, &state.context.v[44], sizeof(vec128_t));
  std::memcpy(&ctx->v45, &state.context.v[45], sizeof(vec128_t));
  std::memcpy(&ctx->v46, &state.context.v[46], sizeof(vec128_t));
  std::memcpy(&ctx->v47, &state.context.v[47], sizeof(vec128_t));
  std::memcpy(&ctx->v48, &state.context.v[48], sizeof(vec128_t));
  std::memcpy(&ctx->v49, &state.context.v[49], sizeof(vec128_t));
  std::memcpy(&ctx->v50, &state.context.v[50], sizeof(vec128_t));
  std::memcpy(&ctx->v51, &state.context.v[51], sizeof(vec128_t));
  std::memcpy(&ctx->v52, &state.context.v[52], sizeof(vec128_t));
  std::memcpy(&ctx->v53, &state.context.v[53], sizeof(vec128_t));
  std::memcpy(&ctx->v54, &state.context.v[54], sizeof(vec128_t));
  std::memcpy(&ctx->v55, &state.context.v[55], sizeof(vec128_t));
  std::memcpy(&ctx->v56, &state.context.v[56], sizeof(vec128_t));
  std::memcpy(&ctx->v57, &state.context.v[57], sizeof(vec128_t));
  std::memcpy(&ctx->v58, &state.context.v[58], sizeof(vec128_t));
  std::memcpy(&ctx->v59, &state.context.v[59], sizeof(vec128_t));
  std::memcpy(&ctx->v60, &state.context.v[60], sizeof(vec128_t));
  std::memcpy(&ctx->v61, &state.context.v[61], sizeof(vec128_t));
  std::memcpy(&ctx->v62, &state.context.v[62], sizeof(vec128_t));
  std::memcpy(&ctx->v63, &state.context.v[63], sizeof(vec128_t));
  // v64-v127: contiguous in PPCContext and state array
  std::memcpy(&ctx->v64, &state.context.v[64], 64 * sizeof(vec128_t));

  // CR fields
  ctx->cr0.set_raw(state.context.cr[0]);
  ctx->cr1.set_raw(state.context.cr[1]);
  ctx->cr2.set_raw(state.context.cr[2]);
  ctx->cr3.set_raw(state.context.cr[3]);
  ctx->cr4.set_raw(state.context.cr[4]);
  ctx->cr5.set_raw(state.context.cr[5]);
  ctx->cr6.set_raw(state.context.cr[6]);
  ctx->cr7.set_raw(state.context.cr[7]);

  // Other state
  ctx->fpscr.csr = state.context.fpscr;
  ctx->xer.ca = state.context.xer_ca;
  ctx->xer.ov = state.context.xer_ov;
  ctx->xer.so = state.context.xer_so;
  ctx->vscr_sat = state.context.vscr_sat;
}

bool XThread::Save(stream::ByteStream* stream) {
  if (!guest_thread_) {
    // Host XThreads are expected to be recreated on their own.
    return false;
  }

  REXSYS_DEBUG("XThread {:08X} serializing...", handle());

  uint32_t pc = 0;
  if (running_) {
    // TODO(tomc): do we need rexglue-compatible thread serialization?
    //             ideally any previous use for this (multi-dvds) are reworked in recomp to be
    //             single xex
    REXSYS_WARN("XThread {:08X} serialization not implemented", handle());
    return false;
  }

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(kThreadSaveSignature);
  stream->Write(thread_name_);

  ThreadSavedState state;
  state.thread_id = thread_id_;
  state.is_main_thread = main_thread_;
  state.is_running = running_;
  state.apc_head = 0;  // APCs now live in guest-memory typed lists
  state.tls_static_address = tls_static_address_;
  state.tls_dynamic_address = tls_dynamic_address_;
  state.tls_total_size = tls_total_size_;
  state.pcr_address = pcr_address_;
  state.stack_base = stack_base_;
  state.stack_limit = stack_limit_;
  state.stack_alloc_base = stack_alloc_base_;
  state.stack_alloc_size = stack_alloc_size_;

  if (running_) {
    auto context = thread_state_->context();
    SaveContext(context, state);
    state.context.pc = pc;
  }

  stream->Write(&state, sizeof(ThreadSavedState));
  return true;
}

object_ref<XThread> XThread::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  // Kind-of a hack, but we need to set the kernel state outside of the object
  // constructor so it doesn't register a handle with the object table.
  auto thread = new XThread(nullptr);
  thread->kernel_state_ = kernel_state;

  if (!thread->RestoreObject(stream)) {
    return nullptr;
  }

  if (stream->Read<uint32_t>() != kThreadSaveSignature) {
    REXSYS_ERROR("Could not restore XThread - invalid magic!");
    return nullptr;
  }

  REXSYS_DEBUG("XThread {:08X}", thread->handle());

  thread->thread_name_ = stream->Read<std::string>();

  ThreadSavedState state;
  stream->Read(&state, sizeof(ThreadSavedState));
  thread->thread_id_ = state.thread_id;
  thread->main_thread_ = state.is_main_thread;
  thread->running_ = state.is_running;
  // state.apc_head is ignored - APCs live in guest-memory typed lists
  thread->tls_static_address_ = state.tls_static_address;
  thread->tls_dynamic_address_ = state.tls_dynamic_address;
  thread->tls_total_size_ = state.tls_total_size;
  thread->pcr_address_ = state.pcr_address;
  thread->stack_base_ = state.stack_base;
  thread->stack_limit_ = state.stack_limit;
  thread->stack_alloc_base_ = state.stack_alloc_base;
  thread->stack_alloc_size_ = state.stack_alloc_size;

  // Register now that we know our thread ID.
  kernel_state->RegisterThread(thread);

  // Create thread state
  thread->thread_state_ = std::make_unique<runtime::ThreadState>(
      thread->thread_id_, thread->stack_base_, thread->pcr_address_, kernel_state->memory());

  if (state.is_running) {
    auto context = thread->thread_state_->context();
    LoadContext(context, state);

    // Always retain when starting - the thread owns itself until exited.
    thread->RetainHandle();

    rex::thread::Thread::CreationParameters params;
    params.create_suspended = true;  // Not done restoring yet.
    params.stack_size = 16_MiB;
    thread->thread_ = rex::thread::Thread::Create(params, [thread, state]() {
      // Set thread ID override. This is used by logging.
      rex::thread::set_current_thread_id(thread->handle());
      runtime::ThreadState::Bind(thread->thread_state_.get());

      // Set name immediately, if we have one.
      // ME2: same race as XThread::Create.
      std::string name;
      {
        std::lock_guard<std::mutex> lock(thread->thread_lock_);
        name = thread->thread_name_;
        thread->thread_->set_name(name);
      }

      PROFILE_THREAD_ENTER(name.c_str());

      current_xthread_tls_ = thread;

      // Acquire any mutants
      for (auto mutant : thread->pending_mutant_acquires_) {
        uint64_t timeout = 0;
        auto status = mutant->Wait(0, 0, 0, &timeout);
        assert_true(status == X_STATUS_SUCCESS);
      }
      thread->pending_mutant_acquires_.clear();

      // Execute user code.
      thread->running_ = true;

      // TODO(tomc): do we need this? threads would need different restoration approach
      //             see XThread::Save
      REXSYS_ERROR("Thread restore not implemented");
      (void)state;

      current_xthread_tls_ = nullptr;

      PROFILE_THREAD_EXIT();

      // Release the self-reference to the thread.
      thread->ReleaseHandle();
    });
    assert_not_null(thread->thread_);

    // NOTE(tomc): if this is kept and processor notification dispatch is implemented,
    //             this needs to send a signal to the processor that a thread was started
  }

  return object_ref<XThread>(thread);
}

XHostThread::XHostThread(KernelState* kernel_state, uint32_t stack_size, uint32_t creation_flags,
                         std::function<int()> host_fn)
    : XThread(kernel_state, stack_size, 0, 0, 0, creation_flags, false), host_fn_(host_fn) {
  // NOTE(tomc): there was a start suspended check here before but I don't think we need it.
}

void XHostThread::Execute() {
  REXSYS_DEBUG("XThread::Execute thid {} (handle={:08X}, '{}', native={:08X}, <host>)", thread_id_,
               handle(), thread_name_, thread_->system_id());

  // Let the kernel know we are starting.
  kernel_state_->OnThreadExecute(this);

  int ret = host_fn_();

  // Exit.
  Exit(ret);
}

}  // namespace rex::system

// sa2-nx (2026-10-20): the Switch's core layout for a game whose main thread does the game's work and its drawing (SA2
// with Renut's renderer: sa2_renut_core_layout). The guest main thread is pinned to 'main_core', every other guest
// thread, now and to come, gets 'others_mask' (preferring 'others_preferred'), so none of them shares the main
// thread's core: at the game threads' priority (0x3B) Horizon time-slices threads of a core in 10 ms slices. The host
// threads keep their own placement (the render thread on core 1). Returns the threads moved. A no-op elsewhere.
//
// sa2-nx (2026-10-20, after banjo-nx's Ryujinx runs: 4 of 10 froze at the first Swap with the layout on, none with it
// off): no thread's cores are set by another thread any more (svcSetThreadCoreMask on a thread running elsewhere waits
// for it to leave its core, and the game's loader threads were busy). The layout is recorded with a new generation;
// the calling thread (the main thread, at its Swap) applies its own at once, every other guest thread its own at its
// next wait (ApplyGuestCoreLayoutAtWait, from XObject::Wait / SignalAndWait / WaitMultiple and XThread::Delay), and
// threads made later at their start. Returns the guest threads that will move.
// ME2 (2026-10-26): the same layout, but the calling guest thread gets 'main_core' alone (not the XEX main thread).
extern "C" uint32_t RexSetGuestThreadCoreLayout(int main_core, uint32_t others_mask, int others_preferred);
extern "C" uint32_t RexSetGuestThreadCoreLayoutCaller(int main_core, uint32_t others_mask, int others_preferred) {
#if REX_PLATFORM_SWITCH
  g_layout_main_marker.store(&t_layout_marker, std::memory_order_release);
#endif
  return RexSetGuestThreadCoreLayout(main_core, others_mask, others_preferred);
}

extern "C" uint32_t RexSetGuestThreadCoreLayout(int main_core, uint32_t others_mask, int others_preferred) {
#if REX_PLATFORM_SWITCH
  using rex::system::XThread;
  g_guest_core_preferred.store(others_preferred, std::memory_order_relaxed);
  g_guest_core_mask.store(others_mask, std::memory_order_relaxed);
  g_guest_main_core.store(main_core, std::memory_order_relaxed);
  g_guest_layout_gen.fetch_add(1, std::memory_order_acq_rel);
  rex::system::ApplyGuestCoreLayoutAtWait();  // the caller's own, now
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return 0;
  uint32_t moved = 0;
  for (auto& thread : kernel->object_table()->GetObjectsByType<XThread>())
    if (thread && thread->is_guest_thread() && thread->thread()) ++moved;
  return moved;
#else
  (void)main_core;
  (void)others_mask;
  (void)others_preferred;
  return 0;
#endif
}

// me2-nx-deko (2026-11-01, diag_wait_sites): for the app's host waits on guest threads (Renut).
extern "C" uint64_t RexWaitProfileBegin() {
  return rex::system::WaitProfileBegin();
}
extern "C" void RexWaitProfileEnd(uint64_t start_ns, const char* tag) {
  if (start_ns) rex::system::WaitProfileEnd(start_ns, tag);
}

// me2-nx-deko (2026-11-01): the guest threads of 'entry' get Horizon priority 'priority' (-1: unchanged) and the cores
// 'mask' (0: unchanged) while 'on', each from its next wait (before it sleeps, or when it wakes for work); off: back to
// what they had. ME2: UE3's thread pool during garbage collection (app/me2_src/me2_frame_diag.cpp).
extern "C" void RexSetGuestThreadBoost(uint32_t entry, int priority, uint32_t mask, bool on) {
#if REX_PLATFORM_SWITCH
  g_boost_entry.store(entry, std::memory_order_relaxed);
  g_boost_prio.store(priority, std::memory_order_relaxed);
  g_boost_mask.store(mask, std::memory_order_relaxed);
  g_boost_on.store(on, std::memory_order_relaxed);
  g_boost_gen.fetch_add(1, std::memory_order_acq_rel);
#else
  (void)entry;
  (void)priority;
  (void)mask;
  (void)on;
#endif
}
