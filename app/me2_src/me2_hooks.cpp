// Mass Effect 2 (Xbox 360, XDK 2.0.8955): the hooks Renut's renderer shares with the game side
// (renut_engine/gpu/banjo_shared_hooks.h). With Renut's renderer chosen they hand over to it; otherwise the original.
// Addresses: tools/deko/D3D_ME2_RENUT.md in the ME2_Recomp tree.

#include <algorithm>
#include "me2_ab_cycle.h"  // me2-nx-deko (2026-11-04): the draw-count rotation
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>

#include "renut_engine/gpu/sa2_renut_yield.h"
#include "sa2_capture.h"

REXCVAR_DEFINE_BOOL(me2_unlock_framerate, false, "ME2",
                    "Remove the guest presentation-interval frame cap; display VSync remains controlled by "
                    "renut_vsync. Read at startup")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_skip_intro_videos, false, "ME2",
                    "Skip the BioWare and EA startup logo videos using the game's startup-only movie entry point; "
                    "story cinematics and loading movies are preserved. Read at startup")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// FFullScreenMovieSupport's startup playback (ME2 XEX 454108CE). Its own
// -nostartupmovies branch returns with r3 == 1 before playing the first
// StartupMovies entry; the general movie player is a different function.
REX_EXTERN(__imp__sub_822F7FA0);
REX_HOOK_RAW(sub_822F7FA0) {
  static const bool skip = REXCVAR_GET(me2_skip_intro_videos);
  if (skip) {
    ctx.r3.u64 = 1;
    REXLOG_INFO("[me2-options] startup logo videos skipped");
    return;
  }
  __imp__sub_822F7FA0(ctx, base);
}

// Diagnostic (2026-10-26, finding UE3's RenderDPGPrePass in ME2): me2_diag_prepass_stacks_s = N starts, N seconds
// after the first draw, a 5 s window in which every depth-only draw (no colour written, Z written) records the guest
// return-address chain (PPC back chain: frame top = [r1], saved LR = [top - 8]). The 12 most frequent chains are logged
// once. 0 = off.
REXCVAR_DEFINE_INT32(me2_diag_prepass_stacks_s, 0, "ME2/Diagnostics",
                     "Log the guest call chains of depth-only draws for 5 s starting N s after the first draw (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
constexpr uint32_t kMe2DeviceColorMask = 0x28DC;     // m_ValuesPacket.ColorMask
constexpr uint32_t kMe2DeviceDepthControl = 0x2934;  // m_ControlPacket.DepthControl (RB_DEPTHCONTROL)

uint32_t DiagLoad(const uint8_t* base, uint32_t va) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va));
}

// 2026-10-27: the off check inline in the draw hooks (the out-of-line call was 0.2 % of the UE3 render thread).
[[gnu::noinline]] void DiagPrepassStackOn(PPCContext& ctx, uint8_t* base, const char* which, int32_t start_s);
inline void DiagPrepassStack(PPCContext& ctx, uint8_t* base, const char* which) {
  static const int32_t start_s = REXCVAR_GET(me2_diag_prepass_stacks_s);
  if (start_s > 0) [[unlikely]] DiagPrepassStackOn(ctx, base, which, start_s);
}

void DiagPrepassStackOn(PPCContext& ctx, uint8_t* base, const char* which, int32_t start_s) {
  static std::mutex mutex;
  static std::map<std::vector<uint32_t>, uint32_t> chains;
  static uint64_t total = 0, depth_only = 0;
  static std::chrono::steady_clock::time_point first{};
  static bool done = false;
  std::lock_guard lock(mutex);
  if (done) return;
  const auto now = std::chrono::steady_clock::now();
  if (first.time_since_epoch().count() == 0) first = now;
  const double t = std::chrono::duration<double>(now - first).count();
  if (t < start_s) return;
  ++total;
  const uint32_t device = ctx.r3.u32;
  if (device) {
    const uint32_t mask = DiagLoad(base, device + kMe2DeviceColorMask) & 0xFFFF;
    const uint32_t depth = DiagLoad(base, device + kMe2DeviceDepthControl);
    if (mask == 0 && (depth & 4)) {  // Z_WRITE_ENABLE, nothing written to colour
      ++depth_only;
      std::vector<uint32_t> chain;
      chain.push_back(uint32_t(ctx.lr));
      uint32_t sp = ctx.r1.u32;
      for (int i = 0; i < 12 && sp >= 0x10000 && sp < 0xF0000000u; ++i) {
        const uint32_t top = DiagLoad(base, sp);
        if (top <= sp || top - sp > 0x100000) break;
        const uint32_t lr = DiagLoad(base, top - 8);
        if (lr < 0x82000000u || lr >= 0x84000000u) break;
        chain.push_back(lr);
        sp = top;
      }
      ++chains[chain];
    }
  }
  if (t < start_s + 5) return;
  done = true;
  std::vector<std::pair<uint32_t, const std::vector<uint32_t>*>> sorted;
  for (const auto& [c, n] : chains) sorted.emplace_back(n, &c);
  std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first > b.first; });
  REXLOG_INFO("[me2-prepass] 5 s: {} draws, {} depth-only, {} distinct chains (last via {})", total, depth_only,
              chains.size(), which);
  for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
    std::string line;
    for (uint32_t a : *sorted[i].second) line += fmt::format(" {:08X}", a);
    REXLOG_INFO("[me2-prepass] {:6} x{}", sorted[i].first, line);
  }
}
}  // namespace

extern "C" void Me2CerberusOnSwap(const uint8_t* base);  // me2_cerberus.cpp
REX_EXTERN(__imp__sub_82AA5238);
REX_HOOK_RAW(sub_82AA5238) {  // D3DDevice_Swap (called by UE3's RHI sub_827DE208 after SynchronizeToPresentationInterval)
  Me2AbTick();  // me2-nx-deko (2026-11-04): the draw-count rotation (me2_ab_cycle.cpp)
  Me2CompositeLightsTick(base);  // me2-nx-deko (2026-11-06): CompositeDynamicLights (me2_ab_cycle.cpp)
  Me2CerberusOnSwap(base);  // me2_cerberus.cpp (before the yield: with Renut it returns from here)
  SA2_RENUT_YIELD_SHARED(sub_82AA5238);
  __imp__sub_82AA5238(ctx, base);
  g_sa2_game_frames.fetch_add(1, std::memory_order_relaxed);
}

// ME2 (2026-10-26, me2_core_layout = 1): the thread issuing the D3D draws (UE3's render thread, which also runs Renut's
// per-draw hooks: the busiest guest thread in the console profile) alone on core 0, the other guest threads on cores 1-2.
REXCVAR_DECLARE(int32_t, me2_core_layout);  // me2_crowd_cpu.cpp
// me2-nx-deko (2026-11-06): with me2_core_layout 1, the cores of the guest threads other than UE3's render thread (and
// those switch_guest_thread_cores / the audio cores place). 0 = cores 1-2 and, with the NSP forwarder, 3 (as before).
// With the game thread alone on core 2 (switch_guest_thread_cores "82865C98:4:2"): 0xA, cores 1 and 3.
REXCVAR_DEFINE_INT32(me2_core_layout_others, 0, "ME2",
                     "Switch, me2_core_layout 1: core mask of the other guest threads (0 = 0x6 plus core 3 when the "
                     "process has it)")
    .range(0, 15)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern "C" uint32_t RexSetGuestThreadCoreLayoutCaller(int main_core, uint32_t others_mask, int others_preferred);
#if REX_PLATFORM_SWITCH
extern "C" unsigned long long RexSwitchProcessCoreMask(void);  // sdk core/threading_switch.cpp
#endif
namespace {
// 2026-10-26 (console profile with named threads): the first draw came from a loading/movie thread, which got core 0
// while UE3's render thread (X827BD330, 72-78 %) stayed on cores 1-2. Then the first thread with kLayoutDraws draws
// of its own got it, but that thread too ended later (console run 20:05: nothing on core 0, the process stuck at
// 200 %, the render thread on cores 1-2). Now the layout is re-evaluated: every kLayoutDraws draws of its own, a
// thread that is not the current owner of core 0 takes it.
constexpr uint32_t kLayoutDraws = 20000;
thread_local uint32_t t_draws = 0;
std::atomic<const void*> g_layout_owner{nullptr};  // &t_draws of the thread on core 0
std::atomic<uint32_t> g_layout_changes{0};
inline void RenderThreadLayoutOnce() {
#if REX_PLATFORM_SWITCH
  if (++t_draws < kLayoutDraws) return;
  t_draws = 0;
  if (g_layout_owner.load(std::memory_order_relaxed) == &t_draws) return;
  if (REXCVAR_GET(me2_core_layout) != 1) return;
  g_layout_owner.store(&t_draws, std::memory_order_relaxed);
  const uint32_t others_set = uint32_t(REXCVAR_GET(me2_core_layout_others)) & uint32_t(RexSwitchProcessCoreMask());
  const uint32_t others_mask = others_set ? others_set : 0x6u | uint32_t(RexSwitchProcessCoreMask() & 0x8u);
  const int others_pref = (others_mask & 0x4u) ? 2 : __builtin_ctz(others_mask);
  if (g_layout_changes.fetch_add(1, std::memory_order_relaxed) >= 16) {
    RexSetGuestThreadCoreLayoutCaller(0, others_mask, others_pref);
    return;  // quiet after 16 changes
  }
  // With the NSP forwarder (core 3 allowed) the other guest threads may also use the core 3 time the audio threads
  // (switch_audio_core_mask) leave, below Wwise's 0x3A.
  const uint32_t others = others_mask;
  const uint32_t moved = RexSetGuestThreadCoreLayoutCaller(0, others, others_pref);
  REXLOG_INFO("[me2] me2_core_layout: the D3D draw thread (UE3's render thread) alone on core 0, the other guest "
              "threads on cores {:#x} ({} threads; process cores {:#x})", others, moved, RexSwitchProcessCoreMask());
#endif
}
}  // namespace

REX_EXTERN(__imp__sub_82A9F3C0);
REX_HOOK_RAW(sub_82A9F3C0) {  // D3DDevice_DrawIndexedVertices (UE3's RHI sub_826AC170, sub_827DBE38)
  RenderThreadLayoutOnce();
  DiagPrepassStack(ctx, base, "DrawIndexedVertices");
  SA2_RENUT_YIELD_SHARED(sub_82A9F3C0);
  __imp__sub_82A9F3C0(ctx, base);
}

// IDirect3DQuery9::GetData (sub_82A906E0) and Issue (sub_82A90458): me2_occlusion.cpp.

REX_EXTERN(__imp__sub_82A9EFD0);
REX_HOOK_RAW(sub_82A9EFD0) {  // D3DDevice_DrawVertices (sub_826913A0)
  DiagPrepassStack(ctx, base, "DrawVertices");
  SA2_RENUT_YIELD_SHARED(sub_82A9EFD0);
  __imp__sub_82A9EFD0(ctx, base);
}
