// Mass Effect 2 - the scene depth prepass is not built by the game. Ported from masseffect-cinnamon (ME1 Switch fork,
// app/src/native/me_guest_prepass.cpp: ~430 of ~2,300 draws a frame, heavy views 20-26 -> 27-30 fps).
//
// WHAT THE ENGINE DOES (ME2, from the disassembly, 2026-10-26)
//   FSceneRenderer::Render (sub_827486B8, reached through a vtable) has UE3's RenderPrePass inlined at
//   0x8274A590-0x8274A648: sub_82747F80 sets the depth-only state, then with a compressed depth range viewport it calls
//   RenderDPGPrePass(this, 2 = SDPG_Foreground) and with the full range RenderDPGPrePass(this, 1 = SDPG_World), i.e.
//   sub_82748018 (its only callers; the return values are ignored). RenderDPGPrePass draws the depth draw lists
//   (sub_826A07E8) and the dynamic occluders through a TDynamicPrimitiveDrawer (virtual call -> sub_8256FA70).
//   Shadow depths are a separate path (FProjectedShadowInfo::RenderDepth = sub_826A59F8), untouched here.
//   Measured with me2_diag_prepass_stacks_s on the PC title screen: the prepass chains were ~28 depth-only draws a
//   frame; the shadow-depth chain ~79.
//
// WHY
//   Renut draws the prepass for real, so each of its draws costs the UE3 render thread (building it), the D3D hooks
//   and Renut's render thread (~13-15 us a draw on the console), plus GPU. ME2's base pass writes depth itself (GEQUAL,
//   Z write on), so the picture does not need the prepass; what is lost is early-Z for the base pass (GPU) and the
//   prepass depth for occlusion queries, which me2_hooks.cpp answers "visible" anyway.
//
// me2_guest_skip_prepass (default on): sub_82748018 returns 0 without drawing. Off: the original.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>

REXCVAR_DEFINE_BOOL(me2_guest_skip_prepass, true, "ME2",
                    "The game does not build the scene depth prepass (RenderDPGPrePass sub_82748018 returns without "
                    "drawing): fewer draws for the UE3 render thread, Renut and the GPU; the base pass writes depth")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
std::atomic<int> g_skip{-1};  // -1 = not read yet (no function-local static guard on a per-frame path)
std::atomic<uint64_t> g_skipped{0};
}  // namespace

#if defined(ME2_WRITE_CENSUS)
extern "C" uint64_t Me2CensusEnter(int phase);
extern "C" void Me2CensusLeave(uint64_t token);
#endif
REX_EXTERN(__imp__sub_82748018);  // FSceneRenderer::RenderDPGPrePass(UINT DPGIndex)
REX_HOOK_RAW(sub_82748018) {
  int skip = g_skip.load(std::memory_order_relaxed);
  if (skip < 0) {
    skip = REXCVAR_GET(me2_guest_skip_prepass) ? 1 : 0;
    g_skip.store(skip, std::memory_order_relaxed);
  }
  if (!skip) {
#if defined(ME2_WRITE_CENSUS)
    const uint64_t census = Me2CensusEnter(3);  // me2_write_census.cpp (diagnostic builds)
#endif
    __imp__sub_82748018(ctx, base);
#if defined(ME2_WRITE_CENSUS)
    Me2CensusLeave(census);
#endif
    return;
  }
  const uint64_t n = g_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || (n & 0xFFFF) == 0)
    REXLOG_INFO("[prepass] scene depth prepass not built by the game ({} RenderDPGPrePass calls skipped)", n);
  ctx.r3.u64 = 0;  // bDirty = FALSE (the caller ignores it)
}

// ME2 (2026-10-26): the gameplay path's twin. The draw log of a prologue frame showed ~330 depth-only draws (Z write
// GEQUAL, no colour, scene-sized target) before the base pass, ~18 % of the frame, while sub_82748018 was already
// skipped; the stack chains (me2_diag_prepass_stacks_s, classic codegen) put them in sub_8269D6A0(this, DPG, view):
// static draw lists (sub_8269F3E8 / sub_8269F540 x2) and the dynamic occluders (sub_8269FF68, a virtual call,
// sub_826A0028), the same body as sub_82748018. Its caller sub_8269D248 (UE3's RenderPrePass of this path: depth clear,
// then DPG 2 and the world group) ORs the results into its return value, which sub_8269D160 passes up and
// sub_8269C9B8 ignores. Same switch: me2_guest_skip_prepass.
// ME2 (2026-10-28): with the prepass back (Umbra's queries need its depth: console Omega 16 -> 27 fps) depth-only
// draws were ~40 % of all draws in the heavy windows. me2_prepass_static_only: only the three static draw lists are
// drawn (walls, floors, the large geometry: Umbra's occluders); the dynamic occluders' loop (the view's dynamic
// primitives flagged as occluders through their virtual DrawDynamicElements, between the drawer sub_8269FF68 and its
// flush sub_826A0028: characters, props; 300-390 light environments in the Omega hub) is left out. The base pass
// writes depth itself, so only some culling behind characters is lost.
REXCVAR_DEFINE_BOOL(me2_prepass_static_only, false, "ME2",
                    "With the prepass on (me2_guest_skip_prepass false): the gameplay-path prepass draws only the "
                    "static draw lists, not the dynamic occluders (characters, props)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8269F3E8);  // static draw list (DPG +48)
REX_EXTERN(__imp__sub_8269F540);  // static draw lists (DPG +76, +104)
REX_EXTERN(__imp__sub_8269D6A0);
REX_HOOK_RAW(sub_8269D6A0) {
  int skip = g_skip.load(std::memory_order_relaxed);
  if (skip < 0) {
    skip = REXCVAR_GET(me2_guest_skip_prepass) ? 1 : 0;
    g_skip.store(skip, std::memory_order_relaxed);
  }
  if (!skip) {
    static const bool static_only = REXCVAR_GET(me2_prepass_static_only);
    if (!static_only) {
      __imp__sub_8269D6A0(ctx, base);
      return;
    }
    // The original's static part: r3 = this, r4 = DPG, r5 = view; each list at [this] + DPG * 308 + {48, 76, 104},
    // called (list, view, view + 636); the results ORed (the caller ORs them into its bDirty).
    const uint32_t self = ctx.r3.u32, dpg = ctx.r4.u32, view = ctx.r5.u32;
    uint64_t dirty = 0;
    auto list = [&](PPCFunc* f, uint32_t offset) {
      const uint32_t lists = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + self));
      ctx.r3.u64 = lists + dpg * 308u + offset;
      ctx.r4.u64 = view;
      ctx.r5.u64 = view + 636u;
      f(ctx, base);
      dirty |= ctx.r3.u64;
    };
    list(__imp__sub_8269F3E8, 48);
    list(__imp__sub_8269F540, 76);
    list(__imp__sub_8269F540, 104);
    ctx.r3.u64 = dirty;
    return;
  }
  static std::atomic<uint64_t> skipped{0};  // render thread; a counter, not a per-call guard
  const uint64_t n = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || (n & 0xFFFF) == 0)
    REXLOG_INFO("[prepass] gameplay-path depth prepass not built by the game ({} sub_8269D6A0 calls skipped)", n);
  ctx.r3.u64 = 0;  // nothing drawn
}
