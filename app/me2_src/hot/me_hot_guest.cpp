// ME2 (2026-10-26): copied from masseffect-cinnamon (ME1 Switch fork, app/src/native/me_hot_guest.cpp), the self-checked
// native replacements of hot guest functions. Unchanged except where marked ME2.
// Mass Effect - hot guest function replacements (REX_HOOK_RAW hooks behind cvars, default off).
//
// Every hook replaces one recompiled function by an exact native version (src/native/hot/n_<addr>.h) and checks itself:
// for the first me2_hot_guard_calls calls (and every me2_hot_guard_period-th call afterwards) the native
// version runs on a copy of the registers and on a private copy of the guest ranges it writes (its shadow build,
// me_hot_shadow.cpp / me_hot_shadow.h: nothing is written to guest memory), then the original (__imp__sub_X) runs for
// real, and registers, written ranges and the FPU mode are compared. A difference logs "[hot] DIFFERENCE ..." and
// switches that hook off for the rest of the run (the original already produced the correct state). Other guest threads
// only ever see the original's stores (2026-10-09: the previous guard wrote the native result into guest memory and
// rolled it back, a race with threads sharing those bytes; per-hook notes in docs/hot-guard.md).
#include "me_hot_guest.h"
#include "me_hot_shadow.h"

#include <mspack.h>  // ME2 (2026-10-31): n_8279B210 (LZX through libmspack lzxd)
#include <lzx.h>

#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "n/n_827EBC58.h"  // ME2: LZO1X decompressor
#include "n/n_82D5F900.h"  // ME2: CRT wcsicmp
#include "n/n_82D602E8.h"  // ME2: CRT wcscmp
#include "n/n_82D65B30.h"  // ME2: CRT memset
#include "n/n_82D65BD0.h"  // ME2: CRT memcpy
#include "n/n_82C474A8.h"  // ME2: XDK XMemCpy family (3 functions)
#include "n/n_82D61750.h"  // ME2: CRT strncpy
#include "n/n_82C47090.h"  // ME2: strlen
#include "n/n_82D5F6D0.h"  // ME2: CRT wcslen
#include "n/n_82E59DF8.h"  // ME2: Wwise resamplers, mono and stereo
#include "n/n_82E94F28.h"  // ME2: Wwise delay line (2 functions)
#include "n/n_82E95FC8.h"  // ME2 (2026-11-06): Wwise reverb core (4-line feedback delay network)
#include "n/n_82993E40.h"  // ME2: TMap<FName,V>::Find, material parameters (2 functions)
#include "n/n_825464E0.h"  // ME2: bone records <-> skeletal mesh bone data swap
#include "n/n_82A92C50.h"  // ME2: XDK SetVertex/PixelShaderConstantF fast paths (2 functions)
#include "n/n_822EB710.h"  // ME2: UE3 pixel shader parameter setters (2 functions, constant setter inlined)
#include "n/n_827DB288.h"  // ME2: UE3 texture + sampler setter with XDK SetTexture / filter setters inlined
#include "n/n_827DB818.h"  // ME2 (2026-11-06): UE3 static render-state setter with the nine XDK setters inlined
#include "n/n_822CDFF0.h"  // ME2: grouped sphere/frustum test (InitViews)
#include "n/n_8279B210.h"  // ME2: LZX of appDecompressMemory (libmspack lzxd)

REXCVAR_DEFINE_BOOL(me2_hot_guest, true, "ME2",
                    "Enable every hot guest function replacement (src/native/hot); each also has its own "
                    "masseffect_hot_* switch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_hot_guard_calls, 256, "ME2",
                     "Hot guest hooks: the first N calls of each hook are checked against the recompiled original")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_hot_guard_period, 4096, "ME2",
                     "Hot guest hooks: after the first N calls every P-th call is checked (rounded to a power of two)")
    .range(1, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::hot {

void InitHook(Hook& h, bool own, bool umbrella) {
  // May run on several threads at once for the first call: every writer stores the same values.
  const bool on = own || (umbrella && REXCVAR_GET(me2_hot_guest));
  const uint32_t guard_calls = uint32_t(std::max(0, REXCVAR_GET(me2_hot_guard_calls)));
  const uint32_t period = uint32_t(std::max(1, REXCVAR_GET(me2_hot_guard_period)));
  uint32_t p2 = 1;
  while (p2 < period) p2 <<= 1;
  h.guard_calls = guard_calls;
  h.period_mask = p2 - 1;
  if (on) REXLOG_INFO("[hot] {}: native replacement ON (guard: first {} calls, then 1 in {})", h.name, guard_calls, p2);
  h.state.store(on ? kOn : kOff, std::memory_order_release);
}

namespace {

const PPCRegister* Gpr(const PPCContext& c, int n) {
  switch (n) {
#define G(i) case i: return &c.r##i;
    G(0) G(1) G(2) G(3) G(4) G(5) G(6) G(7) G(8) G(9) G(10) G(11) G(12) G(13) G(14) G(15) G(16) G(17) G(18) G(19)
    G(20) G(21) G(22) G(23) G(24) G(25) G(26) G(27) G(28) G(29) G(30) G(31)
#undef G
  }
  return nullptr;
}
const PPCRegister* Fpr(const PPCContext& c, int n) {
  switch (n) {
#define F(i) case i: return &c.f##i;
    F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) F(11) F(12) F(13) F(14) F(15) F(16) F(17) F(18) F(19)
    F(20) F(21) F(22) F(23) F(24) F(25) F(26) F(27) F(28) F(29) F(30) F(31)
#undef F
  }
  return nullptr;
}
const PPCVRegister* Vr(const PPCContext& c, int n) {
  switch (n) {
#define V(i) case i: return &c.v##i;
    V(0) V(1) V(2) V(3) V(4) V(5) V(6) V(7) V(8) V(9) V(10) V(11) V(12) V(13) V(14) V(15) V(16) V(17) V(18) V(19)
    V(20) V(21) V(22) V(23) V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)
#undef V
  }
  return nullptr;
}

// First difference between the native result (a) and the original (b), or an empty string.
std::string CompareRegs(const Cmp& cmp, const PPCContext& a, const PPCContext& b) {
  for (int i = 0; i < 32; ++i) {
    const bool volat = (i == 0 || (i >= 3 && i <= 12));
    const bool want = volat ? ((cmp.gpr >> i) & 1) != 0 : (i == 1 || i >= 13);
    if (!want) continue;
    if (Gpr(a, i)->u64 != Gpr(b, i)->u64)
      return fmt::format("r{} native={:#018x} original={:#018x}", i, Gpr(a, i)->u64, Gpr(b, i)->u64);
  }
  for (int i = 0; i < 32; ++i) {
    const bool want = i < 14 ? ((cmp.fpr >> i) & 1) != 0 : true;
    if (!want) continue;
    if (Fpr(a, i)->u64 != Fpr(b, i)->u64)
      return fmt::format("f{} native={:#018x} original={:#018x}", i, Fpr(a, i)->u64, Fpr(b, i)->u64);
  }
  for (int i = 0; i < 32; ++i) {
    const bool want = i < 14 ? ((cmp.vr >> i) & 1) != 0 : true;
    if (!want) continue;
    if (std::memcmp(Vr(a, i)->u8, Vr(b, i)->u8, 16) != 0) {
      const auto* x = Vr(a, i)->u32;
      const auto* y = Vr(b, i)->u32;
      return fmt::format("v{} native={:08x}{:08x}{:08x}{:08x} original={:08x}{:08x}{:08x}{:08x}", i, x[0], x[1], x[2],
                         x[3], y[0], y[1], y[2], y[3]);
    }
  }
  if (a.fpscr.csr != b.fpscr.csr)
    return fmt::format("fpscr.csr native={:#x} original={:#x}", a.fpscr.csr, b.fpscr.csr);
  return {};
}

// Slack pages of the guard's private copy (me_hot_shadow.h ShadowPageFn): on the Switch through the always-mapped shadow
// alias of a committed page (no fault, no commit, no watched-page read); elsewhere none (the slack stays in the declared
// pages).
#if defined(__SWITCH__)
extern "C" void* RexGmShadowFor(uint64_t window_address);
const uint8_t* GuardPage(const uint8_t* base, uint32_t page) {
  return static_cast<const uint8_t*>(RexGmShadowFor(reinterpret_cast<uint64_t>(base + page)));
}
#else
const uint8_t* GuardPage(const uint8_t*, uint32_t) { return nullptr; }
#endif

}  // namespace

void Check(Hook& h, PPCContext& ctx, uint8_t* base) {
  Writes w;
  h.writes(ctx, base, w);
  if (w.overflow) {  // cannot verify: do not trust the native version for this call
    h.orig(ctx, base);
    return;
  }
  // Private copy of the declared ranges; guest memory is only read here.
  ShadowView view;
  std::vector<uint8_t> storage;
  if (!ShadowBuild(view, w, base, ctx.r1.u32, storage, &GuardPage)) {  // a range wraps around 4 GB: cannot verify
    h.orig(ctx, base);
    return;
  }
  const uint32_t hw_before = ctx.fpscr.getcsr();
  const uint32_t csr_before = ctx.fpscr.csr;
  const PPCContext in = ctx;  // inputs for the report
  PPCContext nat = ctx;
  ShadowView* const outer = t_shadow;  // a guarded call reached from guest code that a shadow native called
  t_shadow = &view;
  const bool handled = h.shadow(nat, base);
  t_shadow = outer;
  const uint32_t hw_native = nat.fpscr.getcsr();
  ctx.fpscr.setcsr(hw_before);
  ctx.fpscr.csr = csr_before;
  h.orig(ctx, base);  // the call for real: the only writer of guest memory
  if (!handled) return;  // declined: the original ran, nothing to compare
  const uint32_t hw_orig = ctx.fpscr.getcsr();

  std::string diff = CompareRegs(h.cmp, nat, ctx);
  if (diff.empty() && hw_native != hw_orig) diff = fmt::format("hardware FPCR native={:#x} original={:#x}", hw_native, hw_orig);
  if (diff.empty()) {
    const ShadowDiff d = ShadowCompare(view, w, base);
    if (d.kind == ShadowDiff::kMemory) {
      diff = fmt::format("memory {:#010x} (+{} of range {:#010x}+{}) native={:#04x} original={:#04x}", d.addr, d.offset,
                         w.r[d.range].addr, w.r[d.range].len, d.native, d.original);
    } else if (d.kind == ShadowDiff::kStore) {
      diff = fmt::format("native stored to {:#010x}, outside its declared write ranges (store dropped)", d.addr);
    } else if (d.kind == ShadowDiff::kSlack) {
      diff = fmt::format("native wrote {:#010x} next to its declared write ranges (native={:#04x} before={:#04x})", d.addr,
                         d.native, d.original);
    }
  }
  const uint64_t n = h.checks.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!diff.empty()) {
    h.failed.store(true, std::memory_order_relaxed);
    REXLOG_ERROR(
        "[hot] DIFFERENCE {}: {}; inputs r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} r8={:#x} r9={:#x} r10={:#x}; "
        "native replacement switched off for the rest of the run",
        h.name, diff, in.r3.u64, in.r4.u64, in.r5.u64, in.r6.u64, in.r7.u64,
        in.r8.u64, in.r9.u64, in.r10.u64);
  } else if (n == h.guard_calls || (n > h.guard_calls && ((n - h.guard_calls) & 63) == 0)) {
    REXLOG_INFO("[hot] {}: guard OK after {} checks", h.name, n);
  }
}

}  // namespace me::hot

// ---------------------------------------------------------------------------------------------------------------------
// ME2's hooks. Each: the function's address in ME2, its own cvar, the namespace of its n/n_<addr>.h header (listed in
// me_hot_shadow.cpp too). Found by matching ME1's functions (masseffect-cinnamon) in ME2's code.
// ---------------------------------------------------------------------------------------------------------------------

// LZO1X decompressor of the package streaming (ME2 sub_827EBC58, called from sub_8279B210; ME1 sub_827E1808): the same
// code instruction for instruction (std r30/r31 at -16/-8, first byte > 17 literal run, a 768-entry state x token jump
// table, the 0 / -8 / -4 returns with *out_len = op - out). ME1's profile: the largest single guest function of the
// process (10.7 and 4.4 ms a frame on two streaming threads in an Eden Prime cutscene).
ME_HOT_CVAR(me2_hot_lzo, "Native sub_827EBC58 (LZO1X decompressor of the package streaming), self-checked");
ME_HOT_HOOK(827EBC58, me2_hot_lzo, me::hot::n_827EBC58)

// XDK C runtime (2026-10-27): the same code as ME1's (cinnamon's set of hooked CRT natives: memcpy, memset, wcscmp,
// wcsicmp; its wcslen / wcsncpy natives are not hooked there either). Found by structure: memcpy is the only function
// starting with std r3,-8(r1); the others match cinnamon's descriptions instruction for instruction. ME2 call sites
// checked with cinnamon's liveness.py: only r3 is read after the call (other hits are mr r8,r8 / subfe / dcbt r0 / twi).
ME_HOT_CVAR(me2_hot_crt_memcpy, "Native sub_82D65BD0 (CRT memcpy), self-checked");
ME_HOT_HOOK(82D65BD0, me2_hot_crt_memcpy, me::hot::n_82D65BD0)
ME_HOT_CVAR(me2_hot_crt_memset, "Native sub_82D65B30 (CRT memset), self-checked");
ME_HOT_HOOK(82D65B30, me2_hot_crt_memset, me::hot::n_82D65B30)
ME_HOT_CVAR(me2_hot_crt_wcscmp, "Native sub_82D602E8 (CRT wcscmp), self-checked");
ME_HOT_HOOK(82D602E8, me2_hot_crt_wcscmp, me::hot::n_82D602E8)
ME_HOT_CVAR(me2_hot_crt_wcsicmp, "Native sub_82D5F900 (CRT wcsicmp), self-checked");
ME_HOT_HOOK(82D5F900, me2_hot_crt_wcsicmp, me::hot::n_82D5F900)

// Found from the PGO profile (2026-10-27): the gcda arc counters of the six Ryujinx PGO sessions mapped back to guest
// functions (GCC's function id = crc32 of "__imp__sub_X" & 0x7fffffff), ranked. Not in ME1's set.
// XDK XMemCpy (general) and the two large-copy variants: plain copies for non-overlapping ranges (n_82C474A8.h); their
// 1 KB vector loops were the 4th and 6th hottest guest functions.
ME_HOT_CVAR(me2_hot_xmemcpy, "Native sub_82C474A8 (XDK XMemCpy, n >= 16, non-overlapping), self-checked");
ME_HOT_HOOK(82C474A8, me2_hot_xmemcpy, me::hot::n_82C474A8)
ME_HOT_CVAR(me2_hot_xmemcpy_large, "Native sub_82C47348 (XDK XMemCpy for large copies), self-checked");
ME_HOT_HOOK(82C47348, me2_hot_xmemcpy_large, me::hot::n_82C47348)
ME_HOT_CVAR(me2_hot_xmemcpy_large_wc, "Native sub_82C473F8 (XDK XMemCpy for large copies, write-combined), self-checked");
ME_HOT_HOOK(82C473F8, me2_hot_xmemcpy_large_wc, me::hot::n_82C473F8)
ME_HOT_CVAR(me2_hot_crt_strncpy, "Native sub_82D61750 (CRT strncpy), self-checked");
ME_HOT_HOOK(82D61750, me2_hot_crt_strncpy, me::hot::n_82D61750)
ME_HOT_CVAR(me2_hot_strlen, "Native sub_82C47090 (strlen), self-checked");
ME_HOT_HOOK(82C47090, me2_hot_strlen, me::hot::n_82C47090)
ME_HOT_CVAR(me2_hot_crt_wcslen, "Native sub_82D5F6D0 (CRT wcslen), self-checked");
ME_HOT_HOOK(82D5F6D0, me2_hot_crt_wcslen, me::hot::n_82D5F6D0)

// Wwise fixed-point linear resampler, mono s16 -> float (pointer table 0x822901B0), 3rd in the PGO ranking.
ME_HOT_CVAR(me2_hot_wwise_resample_mono, "Native sub_82E59DF8 (Wwise linear resampler, mono s16), self-checked");
ME_HOT_HOOK(82E59DF8, me2_hot_wwise_resample_mono, me::hot::n_82E59DF8)
ME_HOT_CVAR(me2_hot_wwise_resample_stereo, "Native sub_82E5A180 (Wwise linear resampler, stereo s16), self-checked");
ME_HOT_HOOK(82E5A180, me2_hot_wwise_resample_stereo, me::hot::n_82E5A180)
// Wwise delay line (ring buffer of floats): swap in place / in -> ring -> out (n_82E94F28.h).
// Wwise reverb core: four filtered delay lines mixed each sample (n_82E95FC8.h); 25.8 % of the Wwise thread at Kasumi's
// party (console 11-06).
ME_HOT_CVAR(me2_hot_wwise_reverb, "Native sub_82E95FC8 (Wwise reverb core, 4-line feedback delay network), self-checked");
ME_HOT_HOOK(82E95FC8, me2_hot_wwise_reverb, me::hot::n_82E95FC8)
ME_HOT_CVAR(me2_hot_wwise_delay_swap, "Native sub_82E94F28 (Wwise delay line, in-place swap), self-checked");
ME_HOT_HOOK(82E94F28, me2_hot_wwise_delay_swap, me::hot::n_82E94F28)
ME_HOT_CVAR(me2_hot_wwise_delay_copy, "Native sub_82E94FE8 (Wwise delay line, in -> ring -> out), self-checked");
ME_HOT_HOOK(82E94FE8, me2_hot_wwise_delay_copy, me::hot::n_82E94FE8)

// UE3 render thread, from the second PGO profile (2026-10-27, 2 Ryujinx sessions on the Normandy, gameplay):
// TMap<FName, V>::Find of the material instance parameters (vector: 28-byte pairs, scalar: 16-byte pairs).
ME_HOT_CVAR(me2_hot_tmap_find_vector, "Native sub_82993E40 (TMap<FName,V>::Find, 28-byte pairs: material vector parameters), self-checked");
ME_HOT_HOOK(82993E40, me2_hot_tmap_find_vector, me::hot::n_82993E40)
ME_HOT_CVAR(me2_hot_tmap_find_scalar, "Native sub_825DB270 (TMap<FName,V>::Find, 16-byte pairs: material scalar parameters), self-checked");
ME_HOT_HOOK(825DB270, me2_hot_tmap_find_scalar, me::hot::n_825DB270)
// Bone records <-> skeletal mesh bone data, swapped in place (4th hottest guest function of that profile).
ME_HOT_CVAR(me2_hot_bone_swap, "Native sub_825464E0 (bone records <-> skeletal mesh bone data swap), self-checked");
ME_HOT_HOOK(825464E0, me2_hot_bone_swap, me::hot::n_825464E0)
// ME2 (2026-10-27): XDK D3D float constant setters (vertex / pixel), the hottest XDK functions of the UE3 render thread in
// the console stack profile of 22:2x (3.0 % + 1.0 % of its samples at 13.4 fps): a copy of float4s into the device and an
// OR of the pending group mask (n_82A92C50.h).
ME_HOT_CVAR(me2_hot_vs_consts, "Native sub_82A92C50 (XDK SetVertexShaderConstantF fast path), self-checked");
ME_HOT_HOOK(82A92C50, me2_hot_vs_consts, me::hot::n_82A92C50)
ME_HOT_CVAR(me2_hot_ps_consts, "Native sub_82A92D28 (XDK SetPixelShaderConstantF fast path), self-checked");
ME_HOT_HOOK(82A92D28, me2_hot_ps_consts, me::hot::n_82A92D28)
// ME2 (2026-11-06): UE3's pixel shader parameter setters with the scratch padding and the XDK constant setter inlined
// (n_822EB710.h): 2.3 % + 2.2 % of the UE3 render thread in the console stacks of 2026-11-05 23:5x.
ME_HOT_CVAR(me2_hot_ps_value, "Native sub_822EB710 (UE3 SetPixelShaderValue, one float), self-checked");
ME_HOT_HOOK(822EB710, me2_hot_ps_value, me::hot::n_822EB710)
ME_HOT_CVAR(me2_hot_ps_values, "Native sub_822C8C10 (UE3 SetPixelShaderValues, a float4 array element), self-checked");
ME_HOT_HOOK(822C8C10, me2_hot_ps_values, me::hot::n_822C8C10)
// ME2 (2026-11-06): UE3's static render-state setter with the nine XDK render-state setters inlined, each device word
// stored once (n_827DB818.h): 1.7-1.9 % of the UE3 render thread in the console stacks of 2026-11-06.
ME_HOT_CVAR(me2_hot_render_states, "Native sub_827DB818 (UE3 static render-state setter, nine XDK setters inlined), self-checked");
ME_HOT_HOOK(827DB818, me2_hot_render_states, me::hot::n_827DB818)
// ME2 (2026-10-27): UE3's texture + sampler setter of the material parameters with the XDK SetTexture and the two
// filter setters inlined (n_827DB288.h): 827DB288 + 82A9AF70 + 82A9D268/82A9D0C0 ~5.5 % of the UE3 render thread in the
// console stacks of 22:2x / 22:5x.
ME_HOT_CVAR(me2_hot_texture_sampler, "Native sub_827DB288 (UE3 texture + sampler setter, XDK SetTexture inlined), self-checked");
// Expanded ME_HOT_HOOK with the me2_texbind_diag bracket (me2_shadow_diag.cpp; returns false when the diag is off).
extern "C" bool Me2TexBindBegin(const uint8_t* base, uint32_t s);
extern "C" void Me2TexBindEnd(const uint8_t* base);
namespace me::hot::n_827DB288 {
bool ShadowNative(PPCContext&, uint8_t*);
}
REX_EXTERN(__imp__sub_827DB288);
// ME2 (2026-11-06): the texture header cache of n_827DB288.h (texcache).
REXCVAR_DEFINE_INT32(me2_texture_header_cache, 0, "ME2",
                     "Texture binds (native sub_827DB288) keep a host copy of the texture header words, valid while the "
                     "store cards of their pages stay clean: 1 = count only (the header is still read and compared; "
                     "'[texcache]' every 10 s), 2 = use the copy (one use in 256 still compared; a difference falls back "
                     "to 1); 0 = off")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern "C" int32_t g_me2_texcache_mode = 0;
static void TexCacheReport() {
  using namespace me::hot::n_827DB288::texcache;
  static auto last = std::chrono::steady_clock::now();
  static uint64_t frame0 = g_sa2_game_frames.load(std::memory_order_relaxed);
  static int32_t configured = REXCVAR_GET(me2_texture_header_cache);
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(10)) return;
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  const double f = double(std::max<uint64_t>(1, frame - frame0));
  const Stats& st = g_stats;
  const double b = double(std::max<uint64_t>(1, st.binds));
  REXLOG_INFO("[texcache] {:.0f} frames, mode {} (configured {}): {:.0f} binds/f; outside the cacheable pages {:.1f} %, "
              "refills {:.1f} %, clean cards {:.1f} % (COPY USED {:.1f} %, checked {:.1f} %: equal {}, STALE {}); "
              "entries confirmed {}",
              f, g_me2_texcache_mode, configured, double(st.binds) / f, 100.0 * double(st.outside) / b,
              100.0 * double(st.refills) / b, 100.0 * double(st.clean) / b, 100.0 * double(st.used) / b,
              100.0 * double(st.checked) / b, st.equal, st.stale, st.confirmed);
  if (st.stale)
    REXLOG_WARN("[texcache] {} STALE copies under clean cards: a writer of texture headers that leaves the cards alone",
                st.stale);
  g_stats = Stats{};
  last = now;
  frame0 = frame;
}
static void Hot827DB288(PPCContext& ctx, uint8_t* base) {
  static thread_local uint32_t n_calls;
  static const bool texcache_init = [] {
    g_me2_texcache_mode = REXCVAR_GET(me2_texture_header_cache);
    return true;
  }();
  (void)texcache_init;
  if (g_me2_texcache_mode > 0 && (me::hot::n_827DB288::texcache::g_stats.binds & 4095u) == 0) TexCacheReport();
  static me::hot::Hook h{"sub_827DB288", &__imp__sub_827DB288, &me::hot::NativeB<&me::hot::n_827DB288::Native>,
                         &me::hot::n_827DB288::ShadowNative, &me::hot::n_827DB288::Writes, me::hot::n_827DB288::kCmp};
  const uint8_t st = h.state.load(std::memory_order_relaxed);
  if (st == me::hot::kOn) [[likely]] {
    me::hot::Dispatch<&me::hot::n_827DB288::Native>(h, n_calls, ctx, base);
    return;
  }
  if (st == me::hot::kUninit) [[unlikely]] {
    me::hot::InitHook(h, REXCVAR_GET(me2_hot_texture_sampler), true);
    if (h.state.load(std::memory_order_relaxed) == me::hot::kOn) {
      me::hot::Dispatch<&me::hot::n_827DB288::Native>(h, n_calls, ctx, base);
      return;
    }
  }
  __imp__sub_827DB288(ctx, base);
}
REX_HOOK_RAW(sub_827DB288) {
  if (!Me2TexBindBegin(base, ctx.r3.u32)) [[likely]] {
    Hot827DB288(ctx, base);
    return;
  }
  Hot827DB288(ctx, base);
  Me2TexBindEnd(base);
}

// Same visibility decisions with less VMX register marshalling on complex levels.
ME_HOT_CVAR(me2_hot_frustum_sphere, "Native sub_822CDFF0 (sphere vs plane groups in InitViews), self-checked");
ME_HOT_HOOK_OWN(822CDFF0, me2_hot_frustum_sphere, me::hot::n_822CDFF0)

// ME2 (2026-10-31): native LZX of UE3's appDecompressMemory (n_8279B210.h: libmspack lzxd instead of the recompiled
// XMemDecompress). Its own hook rather than ME_HOT_HOOK: it times every LZX block by the path it took (native, the
// original, a guarded check, a decline) for the "[lzx]" report, and me2_hot_lzx_ab_s alternates native and original
// on a timer so one run measures both. Zlib / LZO blocks go straight to the original (the LZO native is inside it).
ME_HOT_CVAR(me2_hot_lzx, "Native LZX of appDecompressMemory sub_8279B210 (libmspack lzxd), self-checked");
REXCVAR_DEFINE_INT32(me2_hot_lzx_ab_s, 0, "ME2",
                     "Measurement: every this many seconds me2_hot_lzx alternates between the native LZX and the "
                     "original (the \"[lzx]\" report gives both speeds); 0 = native whenever on")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Every LZX block's time, us, since the start (me2_frame_diag.cpp's spike lines take the difference per frame).
std::atomic<uint64_t> g_me2_lzx_total_us{0};

namespace me::hot::n_8279B210 {
bool ShadowNative(PPCContext&, uint8_t*);
}

namespace {

enum LzxPath { kLzxNative, kLzxOriginal, kLzxGuarded, kLzxDeclined, kLzxPaths };
constexpr const char* kLzxPathNames[kLzxPaths] = {"native", "original", "guarded (both)", "declined -> original"};
struct LzxStats {
  std::atomic<uint64_t> blocks[kLzxPaths]{};
  std::atomic<uint64_t> in_bytes[kLzxPaths]{};
  std::atomic<uint64_t> out_bytes[kLzxPaths]{};
  std::atomic<uint64_t> us[kLzxPaths]{};
  std::atomic<uint64_t> max_us[kLzxPaths]{};
  std::atomic<uint64_t> failed{0};  // blocks the game saw fail (r3 = 0), any path
};
LzxStats g_lzx;
std::atomic<int64_t> g_lzx_report_s{0};

uint64_t LzxNowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void LzxReport(uint64_t now_us) {
  const int64_t now_s = int64_t(now_us / 1000000);
  int64_t last = g_lzx_report_s.load(std::memory_order_relaxed);
  if (last == 0) {
    g_lzx_report_s.compare_exchange_strong(last, now_s);
    return;
  }
  if (now_s - last < 10 || !g_lzx_report_s.compare_exchange_strong(last, now_s)) return;
  std::string paths;
  for (int p = 0; p < kLzxPaths; ++p) {
    const uint64_t n = g_lzx.blocks[p].exchange(0, std::memory_order_relaxed);
    const uint64_t in = g_lzx.in_bytes[p].exchange(0, std::memory_order_relaxed);
    const uint64_t out = g_lzx.out_bytes[p].exchange(0, std::memory_order_relaxed);
    const uint64_t us = g_lzx.us[p].exchange(0, std::memory_order_relaxed);
    const uint64_t mx = g_lzx.max_us[p].exchange(0, std::memory_order_relaxed);
    if (!n) continue;
    paths += fmt::format("; {} {} blocks, {:.1f} MB -> {:.1f} MB in {:.1f} ms ({:.1f} MB/s out, {:.0f} us a block, "
                         "longest {:.1f} ms)",
                         kLzxPathNames[p], n, double(in) / 1e6, double(out) / 1e6, double(us) / 1000.0,
                         us ? double(out) / double(us) : 0.0, double(us) / double(n), double(mx) / 1000.0);
  }
  if (paths.empty()) return;
  const int32_t ab = REXCVAR_GET(me2_hot_lzx_ab_s);
  REXLOG_INFO("[lzx] {} s: LZX blocks of appDecompressMemory by path{}; {} failed (me2_hot_lzx_ab_s {})",
              now_s - last, paths, g_lzx.failed.exchange(0, std::memory_order_relaxed), ab);
}

// me2_hot_lzx_ab_s: the native's turn now?
bool LzxNativeTurn() {
  static const int32_t ab = REXCVAR_GET(me2_hot_lzx_ab_s);
  if (ab <= 0) return true;
  return ((LzxNowUs() / 1000000ull) / uint64_t(ab)) % 2 == 0;
}

}  // namespace

REX_EXTERN(__imp__sub_8279B210);
REX_HOOK_RAW(sub_8279B210) {  // appDecompressMemory(flags, dst, dst size, src, src size)
  if ((ctx.r3.u32 & 15) != 4) {  // zlib / LZO
    __imp__sub_8279B210(ctx, base);
    return;
  }
  static thread_local uint32_t n_calls;
  static me::hot::Hook h{"sub_8279B210", &__imp__sub_8279B210, &me::hot::NativeB<&me::hot::n_8279B210::Native>,
                         &me::hot::n_8279B210::ShadowNative, &me::hot::n_8279B210::Writes, me::hot::n_8279B210::kCmp};
  uint8_t st = h.state.load(std::memory_order_relaxed);
  if (st == me::hot::kUninit) [[unlikely]] {
    me::hot::InitHook(h, REXCVAR_GET(me2_hot_lzx), true);
    st = h.state.load(std::memory_order_relaxed);
  }
  const uint32_t in_len = ctx.r7.u32, out_len = ctx.r5.u32;
  const uint64_t t0 = LzxNowUs();
  LzxPath path = kLzxOriginal;
  if (st != me::hot::kOn || h.failed.load(std::memory_order_relaxed) || !LzxNativeTurn()) {
    __imp__sub_8279B210(ctx, base);
  } else {
    const uint32_t c = n_calls++;
    if (c < h.guard_calls || (c & h.period_mask) == 0) [[unlikely]] {
      me::hot::Check(h, ctx, base);
      path = kLzxGuarded;
    } else if (me::hot::n_8279B210::Native(ctx, base)) {
      path = kLzxNative;
    } else {
      __imp__sub_8279B210(ctx, base);
      path = kLzxDeclined;
    }
  }
  const uint64_t t1 = LzxNowUs(), us = t1 - t0;
  g_lzx.blocks[path].fetch_add(1, std::memory_order_relaxed);
  g_lzx.in_bytes[path].fetch_add(in_len, std::memory_order_relaxed);
  g_lzx.out_bytes[path].fetch_add(out_len, std::memory_order_relaxed);
  g_lzx.us[path].fetch_add(us, std::memory_order_relaxed);
  uint64_t mx = g_lzx.max_us[path].load(std::memory_order_relaxed);
  while (us > mx && !g_lzx.max_us[path].compare_exchange_weak(mx, us, std::memory_order_relaxed)) {
  }
  if (ctx.r3.u32 == 0) g_lzx.failed.fetch_add(1, std::memory_order_relaxed);
  g_me2_lzx_total_us.fetch_add(us, std::memory_order_relaxed);
  LzxReport(t1);
}


// me2-nx-deko (2026-11-07): shader constant uploads with NaN / infinite / huge floats (n_82A92C50.h shader_consts::Set,
// the native path of the XDK SetVertex/PixelShaderConstantF), and draws whose device constants hold such floats
// (Me2BadConstsDraw, from the draw hooks: catches writers that bypass Set). Logged with the host callers: image offsets
// (Switch: ELF addresses, the generated sub_XXXXXXXX functions; Windows: RVAs in sa2.exe / sa2.pdb). The guest link
// registers are not usable here (the recompiled code does not keep them). FLT_MAX is left alone (UE3 uploads it on
// purpose).
REXCVAR_DEFINE_BOOL(me2_diag_bad_consts, false, "ME2/Diagnostics",
                    "Log shader constant uploads and draws with NaN, infinite or huge floats in the constants, with "
                    "the host callers ('[bad-consts]' lines)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" int8_t g_me2_bad_consts = -1;

#if defined(__SWITCH__)
extern "C" char _start;  // the image's base
#elif defined(_WIN32)
extern "C" char __ImageBase;
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void**,
                                                                                   unsigned long*);
#endif

namespace {

bool BadFloatBits(uint32_t u) {
  if (u == 0x7F7FFFFFu || u == 0xFF7FFFFFu) return false;  // +-FLT_MAX: deliberate
  float f;
  std::memcpy(&f, &u, 4);
  return !std::isfinite(f) || std::fabs(f) > 1.0e15f;
}

__attribute__((noinline)) std::string HostCallers() {
  std::string text;
#if defined(__SWITCH__) && defined(__aarch64__)
  const uintptr_t image = reinterpret_cast<uintptr_t>(&_start);
  uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  for (int i = 0; i < 14 && fp; ++i) {
    const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(fp);
    const uintptr_t next = frame[0], lr = frame[1];
    if (!lr || lr < image) break;
    text += fmt::format(" {:X}", lr - image);
    if (next <= fp || next - fp > 0x10000 || (next & 7)) break;
    fp = next;
  }
#elif defined(_WIN32)
  void* frames[16] = {};
  const unsigned short n = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
  const uintptr_t image = reinterpret_cast<uintptr_t>(&__ImageBase);
  for (unsigned short i = 0; i < n; ++i) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
    text += a > image ? fmt::format(" +{:X}", a - image) : fmt::format(" ?{:X}", a);
  }
#endif
  return text;
}

std::string RegText(const uint8_t* base, uint32_t va) {
  using namespace me::hot;
  std::string vals;
  for (int j = 0; j < 4; ++j) {
    const uint32_t u = Ld32(base, va + 4u * uint32_t(j));
    float f;
    std::memcpy(&f, &u, 4);
    vals += fmt::format(" {:g}({:08X})", f, u);
  }
  return vals;
}

bool BadConstsOn() {
  if (g_me2_bad_consts < 0) g_me2_bad_consts = REXCVAR_GET(me2_diag_bad_consts) ? 1 : 0;
  return g_me2_bad_consts != 0;
}

}  // namespace

extern "C" void Me2BadConstsCheck(const PPCContext& ctx, const uint8_t* base, bool vertex, uint32_t src, uint32_t count) {
  if (!BadConstsOn()) return;
  using namespace me::hot;
  int first = -1;
  for (uint32_t i = 0; i < count * 4 && first < 0; ++i)
    if (BadFloatBits(Ld32(base, src + 4 * i))) first = int(i / 4);
  if (first < 0) return;
  // Logged per (shader stage, first register): the first 4 of each, then every 4096th (UE3 uploads some infinities
  // on purpose all the time, e.g. PS c3.y; they must not use up the log before a rare bad upload elsewhere).
  static std::atomic<uint32_t> per_key[512] = {}, total{0};
  const uint32_t n = total.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t key = (vertex ? 0u : 256u) + (ctx.r4.u32 & 255u);
  const uint32_t seen = per_key[key].fetch_add(1, std::memory_order_relaxed);
  if (seen >= 4 && (seen & 4095) != 0) return;
  REXLOG_WARN("[bad-consts] set #{} ({} of this kind) {} c{}+{} from {:08X}: register c{} ={}; host callers{}", n,
              seen + 1, vertex ? "VS" : "PS", ctx.r4.u32, count, src, ctx.r4.u32 + uint32_t(first),
              RegText(base, src + uint32_t(first) * 16), HostCallers());
}

// At each draw (render thread): the device's constant registers (VS c0-c255 at device + (r + 120) * 16, PS at
// device + (r + 376) * 16, see n_82A92C50.h). A register is logged the first time it is bad, and again each time it
// turns bad after having been good (rate-limited), with the draw's callers; "[bad-consts] draws" every ~10 s counts
// the draws that had any bad register.
// device_host = the device's host address (the caller translates it; base + va for the registers below).
extern "C" void Me2BadConstsDraw(const uint8_t* device_host, uint32_t device) {
  if (!BadConstsOn() || device == 0 || device_host == nullptr) return;
  using namespace me::hot;
  const uint8_t* base = device_host - device;
  static thread_local uint8_t was_bad[512] = {};
  static thread_local uint32_t reg_logs[512] = {};
  static thread_local uint64_t draws = 0, bad_draws = 0;
  static thread_local auto since = std::chrono::steady_clock::now();
  static std::atomic<uint32_t> logged{0};
  ++draws;
  bool any = false;
  for (uint32_t k = 0; k < 512; ++k) {
    const uint32_t va = device + (k < 256 ? k + 120 : k - 256 + 376) * 16;
    const bool bad = BadFloatBits(Ld32(base, va)) || BadFloatBits(Ld32(base, va + 4)) ||
                     BadFloatBits(Ld32(base, va + 8)) || BadFloatBits(Ld32(base, va + 12));
    if (bad) {
      any = true;
      if (!was_bad[k] && reg_logs[k] < 6 && logged.load(std::memory_order_relaxed) < 400) {
        ++reg_logs[k];
        logged.fetch_add(1, std::memory_order_relaxed);
        REXLOG_WARN("[bad-consts] draw {} {} c{} turned bad ={}; host callers{}", draws, k < 256 ? "VS" : "PS",
                    k & 255, RegText(base, va), HostCallers());
      }
    }
    was_bad[k] = bad;
  }
  bad_draws += any;
  const auto now = std::chrono::steady_clock::now();
  if (now - since >= std::chrono::seconds(10)) {
    std::string regs;
    for (uint32_t k = 0; k < 512; ++k)
      if (was_bad[k]) regs += fmt::format(" {}c{}", k < 256 ? "VS" : "PS", k & 255);
    REXLOG_WARN("[bad-consts] draws last ~10 s: {} of {} with a bad register; bad now:{}", bad_draws, draws, regs);
    draws = bad_draws = 0;
    since = now;
  }
}
