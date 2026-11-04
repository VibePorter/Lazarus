// Sonic Adventure 2 - native replacements of hot guest functions (CPU enhancements sa2_cpu_native_*; from
// masseffect-nx's me_hot_guest.cpp).
//
// Every hook replaces one recompiled function by a native version (src/cpu/hot/n_<addr>.h) and checks itself: for the
// first sa2_hot_guard_calls calls of each thread (every call with sa2_cpu_verify) and every sa2_hot_guard_period-th call
// afterwards, the native version runs on a copy of the registers with the guest ranges it writes snapshotted, the
// memory is rolled back, the original (__imp__sub_X) runs for real, and registers, written ranges and the FPU mode are
// compared. A difference logs "[hot] DIFFERENCE ..." and switches that hook off for the rest of the run (the original
// already produced the correct state). A hook also stays off when the loaded game code is not byte for byte the code
// the native was written from.
//
// The profile that chose them: the PC profile of the game thread with the GPU wait asleep (docs/enhancements.md).
// WHY EXACT: the CRT natives reproduce the routines' memory effect and return register (n_827FF400.h etc. say why);
//   the others are localized copies of the recompiled functions themselves (tools/localize_native.py).
// RISK: a native that differs only on inputs the guard did not see. KEY: each sa2_cpu_native_* (sa2_cpu_enhancements).

// The generated code's macros and helpers (the localized copies are made of generated code) ...
#include "../../generated/default/sa2_pch.h"

// ... with plain guest memory: the recompiled code's accesses are volatile, which keeps GCC from pairing and
// scheduling them; a native replacement does not need that (a racing write from another thread is a race either way).
#undef REX_LOAD_U8
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
#define REX_LOAD_U8(x) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U32(x) __builtin_bswap32(*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U64(x) __builtin_bswap64(*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_STORE_U8(x, y) (*(u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
#define REX_STORE_U16(x, y) (*(u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (*(u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (*(u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))

#include "sa2_hot_guest.h"
#include "sa2_hot_copies.h"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../../generated/hot/n_8213ECD8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82146F78.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147000.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147B60.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147BD8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147C08.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147CD8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147DA8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_8213A568.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82139E30.h"  // localized copy, unguarded (tools/localize_native.py)
#include "hot/n_vertex_chunks.h"  // hand-written: sub_8214D198 and sub_8214D380 (the chunk-model vertex loaders)
#include "hot/n_ninja_matrix.h"   // hand-written: njCalcPoint and njMultiMatrix (round 13)
#include "../../generated/hot/n_8214E3B8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82555148.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_821BD000.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82548B00.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_829734D8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_821414C0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82554FF0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82973120.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_829721B8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82551D00.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82973000.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8214D960.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8214BE60.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82975E50.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82140D38.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82140B40.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_8214A490.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_8214A2D8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82146EE8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82146E78.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_821417A0.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82975D88.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_821BA660.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_8214B950.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8214B7A8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8213D2B0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_829729A8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_828415C8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8213D8A8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8214AF30.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82841830.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_828427F0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82832D58.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_821BA890.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82842308.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82842E28.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82825070.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82139930.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82842F08.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82147380.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82564F78.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8213E0F8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82841FB8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82840E78.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_8214B5A8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82565478.h"  // localized copy, called by the renderer's hook
#include "../../generated/hot/n_82555F40.h"  // localized copy, called by the renderer's hook
#include "../../generated/hot/n_82555298.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8251CC80.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_821410F0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82139158.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8217EB38.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_821412E0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82562A90.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8251CB88.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82555BF8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8254D720.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8254CA68.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8217EE68.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8214ED78.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82228340.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8213D680.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82139D20.h"  // localized copy, unguarded (tools/localize_native.py)
#include "hot/n_827FE708.h"
#include "hot/n_d3d_constants.h"
#include "hot/n_827FECF8.h"
#include "hot/n_827FF400.h"

REXCVAR_DEFINE_BOOL(sa2_cpu_verify, false, "SA2 enhancements",
                    "Verify mode: every call of a native replacement runs both versions and compares them (slow; "
                    "'[verify]' lines report the checks and differences), and the renderer's self-checks check every "
                    "use")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_hot_guard_calls, 256, "SA2 enhancements",
                     "Native replacements: the first N calls of each thread are checked against the recompiled original")
    .range(0, 1000000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_hot_guard_period, 4096, "SA2 enhancements",
                     "Native replacements: after the first N calls every P-th call is checked (rounded to a power of two)")
    .range(1, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace sa2::hot {

SA2_ENHANCEMENT(g_counter_shared, sa2_cpu_hook_counter_shared, sa2::enh::kCpu, true,
                "Native replacements: one relaxed call counter per hook shared by all threads instead of a thread_local "
                "one (on the Switch every thread_local access is a call to __aarch64_read_tp, on every hooked call)");

SA2_ENHANCEMENT(g_counter_ctx, sa2_cpu_hook_counter_ctx, sa2::enh::kCpu, true,
                "Native replacements: each hook's call counter lives in the calling thread's own context "
                "(PPCContext::host_counters) instead of one counter shared by all threads: no store into a cache line "
                "every calling thread pulls to its core, and still no thread_local (sasrt-nx's, 2026-10-12)");

void TakeCtxSlot(std::atomic<uint8_t>& slot) {
  if (slot.load(std::memory_order_relaxed) != kNoSlot) return;
  static std::atomic<uint32_t> next_slot{0};
  const uint32_t s = next_slot.fetch_add(1, std::memory_order_relaxed);
  uint8_t none = kNoSlot;
  slot.compare_exchange_strong(none, s < kCtxSlots ? uint8_t(s) : kSlotsFull, std::memory_order_relaxed);
}

void InitHook(Hook& h, uint8_t* base) {
  // May run on several threads at once for the first call: every writer stores the same values.
  TakeCtxSlot(h.slot);
  bool on = h.key->active;
  if (on && !sa2::enh::GuestCodeMatches(base, h.code_address, h.code_bytes, h.code_hash, h.key->key)) on = false;
  const bool verify = REXCVAR_GET(sa2_cpu_verify);
  const uint32_t guard_calls = verify ? 0xFFFFFFFFu : uint32_t(std::max(0, REXCVAR_GET(sa2_hot_guard_calls)));
  const uint32_t period = uint32_t(std::max(1, REXCVAR_GET(sa2_hot_guard_period)));
  uint32_t p2 = 1;
  while (p2 < period) p2 <<= 1;
  h.guard_calls = guard_calls;
  h.period_mask = p2 - 1;
  if (on && h.state.load(std::memory_order_relaxed) == kUninit) {
    if (verify) {
      REXLOG_INFO("[hot] {} ({}): native replacement ON, verify mode (every call checked)", h.name, h.key->key);
    } else {
      REXLOG_INFO("[hot] {} ({}): native replacement ON (guard: first {} calls, then 1 in {})", h.name, h.key->key,
                  guard_calls, p2);
    }
  }
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
const PPCRegister* Fpr(const PPCContext& c, int n) { return &c.f0 + n; }       // f0..f31 are consecutive
const PPCVRegister* Vr(const PPCContext& c, int n) { return &c.v0 + n; }       // v0..v127 are consecutive
const PPCCRRegister* Cr(const PPCContext& c, int n) { return &c.cr0 + n; }     // cr0..cr7 are consecutive

// First difference between the native result (a) and the original (b), or an empty string.
std::string CompareRegs(Cmp cmp, const PPCContext& a, const PPCContext& b) {
  const bool all = CmpAll(cmp);
  const bool nan = CmpNaN(cmp);
  const bool ret_fp = cmp != Cmp::kReturnInt;  // f1 / v1: a float return value
  for (int i = 0; i < 32; ++i) {
    const bool want = all || i == 1 || i == 3 || i >= 13;
    if (want && Gpr(a, i)->u64 != Gpr(b, i)->u64)
      return fmt::format("r{} native={:#018x} original={:#018x}", i, Gpr(a, i)->u64, Gpr(b, i)->u64);
  }
  for (int i = 0; i < 32; ++i) {
    const bool want = all || (ret_fp && i == 1) || i >= 14;
    const uint64_t x = Fpr(a, i)->u64, y = Fpr(b, i)->u64;
    if (want && x != y && !(nan && IsNaN64(x) && IsNaN64(y)))
      return fmt::format("f{} native={:#018x} original={:#018x}", i, x, y);
  }
  for (int i = 0; i < 128; ++i) {
    const bool want = all || (ret_fp && i == 1) || (i >= 14 && i < 32) || i >= 64;
    bool differ = want && std::memcmp(Vr(a, i)->u8, Vr(b, i)->u8, 16) != 0;
    if (differ && nan) {
      differ = false;
      for (int k = 0; k < 4; ++k) {
        const uint32_t x = Vr(a, i)->u32[k], y = Vr(b, i)->u32[k];
        if (x != y && !(IsNaN32(x) && IsNaN32(y))) differ = true;
      }
    }
    if (differ) {
      const auto* x = Vr(a, i)->u32;
      const auto* y = Vr(b, i)->u32;
      return fmt::format("v{} native={:08x}{:08x}{:08x}{:08x} original={:08x}{:08x}{:08x}{:08x}", i, x[0], x[1], x[2],
                         x[3], y[0], y[1], y[2], y[3]);
    }
  }
  if (all) {
    for (int i = 0; i < 8; ++i)
      if (Cr(a, i)->raw() != Cr(b, i)->raw())
        return fmt::format("cr{} native={:x} original={:x}", i, Cr(a, i)->raw(), Cr(b, i)->raw());
    if (a.ctr.u64 != b.ctr.u64) return fmt::format("ctr native={:#x} original={:#x}", a.ctr.u64, b.ctr.u64);
    if (a.xer.so != b.xer.so || a.xer.ov != b.xer.ov || a.xer.ca != b.xer.ca) return "xer";
    if (a.reserved.u64 != b.reserved.u64) return "reserved";
    if (a.lr != b.lr) return fmt::format("lr native={:#x} original={:#x}", a.lr, b.lr);
  }
  // Only the guest-visible bits (rounding, flush): host sticky exception flags (x86 MXCSR) may differ harmlessly.
  constexpr uint32_t kGuest = rex::ppc::FPSCRRegister::GuestMask;
  if ((a.fpscr.csr & kGuest) != (b.fpscr.csr & kGuest))
    return fmt::format("fpscr.csr native={:#x} original={:#x}", a.fpscr.csr, b.fpscr.csr);
  return {};
}

}  // namespace

void Check(Hook& h, PPCContext& ctx, uint8_t* base) {
  Writes w;
  h.writes(ctx, base, w);
  if (w.overflow) {  // cannot verify: do not trust the native version for this call
    h.orig(ctx, base);
    return;
  }
  size_t total = 0;
  for (int i = 0; i < w.n; ++i) total += w.r[i].len;
  std::vector<uint8_t> before(total), native_out(total);
  size_t off = 0;
  for (int i = 0; i < w.n; ++i) {
    std::memcpy(before.data() + off, Raw(base, w.r[i].addr), w.r[i].len);
    off += w.r[i].len;
  }
  const uint32_t hw_before = ctx.fpscr.getcsr();
  const uint32_t csr_before = ctx.fpscr.csr;
  const PPCContext in = ctx;  // inputs for the report
  PPCContext nat = ctx;
  if (!h.native(nat, base)) {  // declined: the original runs, nothing to compare
    h.orig(ctx, base);
    return;
  }
  const uint32_t hw_native = nat.fpscr.getcsr();
  off = 0;
  for (int i = 0; i < w.n; ++i) {
    std::memcpy(native_out.data() + off, Raw(base, w.r[i].addr), w.r[i].len);
    std::memcpy(Raw(base, w.r[i].addr), before.data() + off, w.r[i].len);  // roll back
    off += w.r[i].len;
  }
  ctx.fpscr.setcsr(hw_before);
  ctx.fpscr.csr = csr_before;
  h.orig(ctx, base);  // the call for real
  const uint32_t hw_orig = ctx.fpscr.getcsr();

  std::string diff = CompareRegs(h.cmp, nat, ctx);
  if (diff.empty() && (hw_native & rex::ppc::FPSCRRegister::GuestMask) != (hw_orig & rex::ppc::FPSCRRegister::GuestMask))
    diff = fmt::format("hardware FPCR native={:#x} original={:#x}", hw_native, hw_orig);
  if (diff.empty()) {
    off = 0;
    for (int i = 0; i < w.n && diff.empty(); ++i) {
      const uint8_t* now = Raw(base, w.r[i].addr);
      for (uint32_t k = 0; k < w.r[i].len; ++k) {
        if (now[k] != native_out[off + k]) {
          if (CmpNaN(h.cmp) && k / 4 * 4 + 4 <= w.r[i].len) {  // a float NaN word on both sides is equal
            const uint32_t q = k / 4 * 4;
            uint32_t x, y;
            std::memcpy(&x, native_out.data() + off + q, 4);
            std::memcpy(&y, now + q, 4);
            if (IsNaN32(__builtin_bswap32(x)) && IsNaN32(__builtin_bswap32(y))) {
              k = q + 3;
              continue;
            }
          }
          diff = fmt::format("memory {:#010x} (+{} of range {:#010x}+{}) native={:#04x} original={:#04x}",
                             w.r[i].addr + k, k, w.r[i].addr, w.r[i].len, native_out[off + k], now[k]);
          break;
        }
      }
      off += w.r[i].len;
    }
  }
  const uint64_t n = h.checks.fetch_add(1, std::memory_order_relaxed) + 1;
  sa2::enh::VerifyCheck();
  if (!diff.empty()) {
    h.failed.store(true, std::memory_order_relaxed);
    sa2::enh::VerifyDifference();
    REXLOG_ERROR(
        "[hot] DIFFERENCE {} ({}): {}; inputs r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} f1={:#x} f2={:#x} "
        "f3={:#x}; native replacement switched off for the rest of the run",
        h.name, h.key->key, diff, in.r3.u64, in.r4.u64, in.r5.u64, in.r6.u64, in.r7.u64, in.f1.u64, in.f2.u64,
        in.f3.u64);
  } else if (n == 4096 || n == (1u << 20)) {
    REXLOG_INFO("[hot] {} ({}): {} checks, no difference", h.name, h.key->key, n);
  }
}

}  // namespace sa2::hot

// ---------------------------------------------------------------------------------------------------------------
// The hooks. Writes: the guest ranges each may write (the stack ranges are the frames below r1 the localized copies
// write exactly as the originals do). Cmp: kAll for the leaf localized copies (every register); kCalls for the copies
// that call other functions: the scratch registers a callee leaves depend on whether that callee's own call was a
// checked one (a checked memcpy runs the recompiled original, which leaves r4 advanced; the native one leaves it), so
// they compare what their callers can see: r3/f1, the non-volatile registers, CTR-free state and memory. Their callers
// read no volatile register after the call (tools/call_site_liveness.py).
// The stack range of a function that makes calls must be exactly its own frame (stwu): below it lies its callees'
// dead scratch, which differs legitimately between a native callee and its original (the CRT memcpy saves r3 at
// -8(r1), the native one writes nothing there) whenever only one of the two nested calls of a check is itself checked.
// Round 2 (2026-10-06) found push_matrix (128 for a 112 frame), the rotations (176 for 160) and sub_82140D38 (320 for
// 288) switched off by such false DIFFERENCEs in normal runs; verify mode never shows it (every nested call checked).

namespace {
using sa2::hot::Ld32;
using sa2::hot::Writes;
using PPC = PPCContext;
}  // namespace

// C runtime (profile: memcpy 6.1 % of the busy game thread).
SA2_HOT_HOOK(827FF400, sa2_cpu_native_memcpy, "Native CRT memcpy (sub_827FF400): host memcpy/memmove",
             sa2::hot::n_827FF400, &sa2::hot::n_827FF400::Writes, sa2::hot::Cmp::kReturn, 1160, 0xEE0EAEB1E0584297ull)
SA2_HOT_HOOK(827FECF8, sa2_cpu_native_memset, "Native CRT memset (sub_827FECF8): host memset",
             sa2::hot::n_827FECF8, &sa2::hot::n_827FECF8::Writes, sa2::hot::Cmp::kReturn, 160, 0x54D201DC8A21DABAull)
SA2_HOT_HOOK(827FE708, sa2_cpu_native_memmove, "Native CRT memmove (sub_827FE708): host memmove",
             sa2::hot::n_827FE708, &sa2::hot::n_827FE708::Writes, sa2::hot::Cmp::kReturn, 192, 0x8CFADCBB872C5938ull)

// Ninja matrix library (profile: njCalcPoint 4.2 %, njMultiMatrix 1.9 %; njTranslate / njRotate* / njPushMatrix are
// called from 1,000-2,700 places each).
// Round 13: both hand-written, writing only what callers can see (src/cpu/hot/n_ninja_matrix.h; the localized copies
// moved every register they touch in and out of the context on every call). kReturnInt: callers read no float
// register after either (tools/call_site_liveness.py); njMultiMatrix's -48(r1) copy (out equal to an input) is the
// callee's dead scratch, so it is not compared.
SA2_HOT_HOOK(82146F78, sa2_cpu_native_calc_point, "Native njCalcPoint (sub_82146F78), hand-written: the result only",
             sa2::hot::n_calc_point, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r5.u32, 12); },
             sa2::hot::Cmp::kReturnInt, 136, 0x1A5C740F62335800ull)
SA2_HOT_HOOK(82147000, sa2_cpu_native_multi_matrix, "Native njMultiMatrix (sub_82147000), hand-written: the result only",
             sa2::hot::n_multi_matrix,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32 && c.r4.u32 && c.r5.u32) w.Add(c.r5.u32, 48);
             },
             sa2::hot::Cmp::kReturnInt, 752, 0x6118F26106826963ull)
SA2_HOT_HOOK(82147B60, sa2_cpu_native_translate, "Native njTranslate (sub_82147B60), localized copy",
             sa2::hot::n_82147B60,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32) w.Add(c.r3.u32 + 12, 36);
             },
             sa2::hot::Cmp::kAll, 120, 0x601D72585018A282ull)
SA2_HOT_HOOK(82147DA8, sa2_cpu_native_push_matrix, "Native njPushMatrix (sub_82147DA8), localized copy",
             sa2::hot::n_82147DA8,
             +[](const PPC& c, const uint8_t* base, Writes& w) {
               w.Add(0x839C07DCu, 4);  // the matrix stack pointer
               w.Add(Ld32(base, 0x839C07DCu) + 48, 48);
               w.Add(c.r1.u32 - 112, 112);  // its own frame (stwu -112), not the callees' dead scratch below it
             },
             sa2::hot::Cmp::kCalls, 112, 0x2A8618613032BDB9ull)
SA2_HOT_HOOK(82147BD8, sa2_cpu_native_rotate_x, "Native njRotateX (sub_82147BD8), localized copy",
             sa2::hot::n_82147BD8,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32) w.Add(c.r3.u32, 48);
               w.Add(c.r1.u32 - 160, 160);  // its own frame (stwu -160), not the callees' dead scratch below it
             },
             sa2::hot::Cmp::kCalls, 208, 0x99AED3F258180A60ull)
SA2_HOT_HOOK(82147C08, sa2_cpu_native_rotate_y, "Native njRotateY (sub_82147C08), localized copy",
             sa2::hot::n_82147C08,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32) w.Add(c.r3.u32, 48);
               w.Add(c.r1.u32 - 160, 160);  // its own frame (stwu -160), not the callees' dead scratch below it
             },
             sa2::hot::Cmp::kCalls, 208, 0x85C214CA04B80D48ull)
SA2_HOT_HOOK(82147CD8, sa2_cpu_native_rotate_z, "Native njRotateZ (sub_82147CD8), localized copy",
             sa2::hot::n_82147CD8,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32) w.Add(c.r3.u32, 48);
               w.Add(c.r1.u32 - 160, 160);  // its own frame (stwu -160), not the callees' dead scratch below it
             },
             sa2::hot::Cmp::kCalls, 208, 0x3A5DA27104AF59BBull)

// Engine (profile: 3.0 %): an ordered-tree (std::map) lower-bound search by a 32-bit key; result iterator to [r3].
SA2_HOT_HOOK(8213ECD8, sa2_cpu_native_map_lower_bound, "Native std::map lower bound (sub_8213ECD8), localized copy",
             sa2::hot::n_8213ECD8,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r3.u32, 4);
               w.Add(c.r1.u32 - 16, 8);
             },
             sa2::hot::Cmp::kAll, 144, 0xA0A2745BFA713291ull)

// Default of the unguarded localized copies (the 15 below, round 3's 20 and round 4's 18). On since round 4: with round
// 3's smart sync a call syncs only what its callee can reach, and all of them passed the PC checks (verify mode and the
// lockstep capture, pixel-identical). Before smart sync a copy that makes calls wrote back and reloaded every localized
// register around each call and was slower on the PC (sasrt-nx). configs/sa2_test_copies_off.toml turns them all off
// for the console A/B.
#ifndef SA2_COPIES_DEFAULT
#define SA2_COPIES_DEFAULT true
#endif

// The 15 hottest game functions after the ones above (PC profile of the game thread, busy samples), localized copies.
// They call other functions, so they run unguarded; SA2_COPIES_DEFAULT (above) decides their default.
SA2_HOT_HOOK_COPY(8213A568, sa2_cpu_native_sub_8213A568, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213A568 (4.5 % of the game thread on PC)",
                       sa2::hot::n_8213A568, 1696, 0x7DAC45C2DC3B95D2ull)
SA2_HOT_HOOK_COPY(82139E30, sa2_cpu_native_sub_82139E30, SA2_COPIES_DEFAULT, "Native localized copy of sub_82139E30 (3.2 % of the game thread on PC)",
                       sa2::hot::n_82139E30, 632, 0x06A3E594049A1B65ull)
// sub_8214D198: round 13's hand-written version (below) replaced the localized copy.
SA2_HOT_HOOK_COPY(8214E3B8, sa2_cpu_native_sub_8214E3B8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214E3B8 (2.4 % of the game thread on PC)",
                       sa2::hot::n_8214E3B8, 384, 0x856253660DDC74D0ull)
SA2_COPY_FOR_HOOK(82555148, sa2_cpu_native_sub_82555148, SA2_COPIES_DEFAULT, "Native localized copy of sub_82555148 (2.4 %, Direct3D of the game thread on PC)",
                       sa2::hot::n_82555148, 336, 0x5C91B7E296B6DE66ull)
SA2_HOT_HOOK_COPY(821BD000, sa2_cpu_native_sub_821BD000, SA2_COPIES_DEFAULT, "Native localized copy of sub_821BD000 (2.3 % of the game thread on PC)",
                       sa2::hot::n_821BD000, 920, 0xCB982BEFF627C84Full)
SA2_HOT_HOOK_COPY(82548B00, sa2_cpu_native_sub_82548B00, SA2_COPIES_DEFAULT, "Native localized copy of sub_82548B00 (2.0 %, Direct3D of the game thread on PC)",
                       sa2::hot::n_82548B00, 400, 0x3E4CB7FAF32B3E2Bull)
SA2_HOT_HOOK_COPY(829734D8, sa2_cpu_native_sub_829734D8, SA2_COPIES_DEFAULT, "Native localized copy of sub_829734D8 (1.8 % of the game thread on PC)",
                       sa2::hot::n_829734D8, 200, 0x478F9A3FA2705FDDull)
SA2_HOT_HOOK_COPY(821414C0, sa2_cpu_native_sub_821414C0, SA2_COPIES_DEFAULT, "Native localized copy of sub_821414C0 (1.8 %, VMX matrices of the game thread on PC)",
                       sa2::hot::n_821414C0, 496, 0xE90489A245BCA2C4ull)
SA2_COPY_FOR_HOOK(82554FF0, sa2_cpu_native_sub_82554FF0, SA2_COPIES_DEFAULT, "Native localized copy of sub_82554FF0 (1.5 %, Direct3D of the game thread on PC)",
                       sa2::hot::n_82554FF0, 344, 0xDF67A0D237233D0Bull)
SA2_HOT_HOOK_COPY(82973120, sa2_cpu_native_sub_82973120, SA2_COPIES_DEFAULT, "Native localized copy of sub_82973120 (1.4 % of the game thread on PC)",
                       sa2::hot::n_82973120, 216, 0x37665FDBD2590B41ull)
SA2_HOT_HOOK_COPY(829721B8, sa2_cpu_native_sub_829721B8, SA2_COPIES_DEFAULT, "Native localized copy of sub_829721B8 (1.4 % of the game thread on PC)",
                       sa2::hot::n_829721B8, 360, 0x982E75743E03435Eull)
SA2_HOT_HOOK_COPY(82551D00, sa2_cpu_native_sub_82551D00, SA2_COPIES_DEFAULT, "Native localized copy of sub_82551D00 (1.4 %, Direct3D of the game thread on PC)",
                       sa2::hot::n_82551D00, 288, 0xCD2D456D05D4AC56ull)
SA2_HOT_HOOK_COPY(82973000, sa2_cpu_native_sub_82973000, SA2_COPIES_DEFAULT, "Native localized copy of sub_82973000 (1.3 % of the game thread on PC)",
                       sa2::hot::n_82973000, 288, 0x7C79132A3BE8A43Full)
SA2_HOT_HOOK_COPY(8214D960, sa2_cpu_native_sub_8214D960, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214D960 (1.3 % of the game thread on PC)",
                       sa2::hot::n_8214D960, 240, 0xA6DE8ACA97828B1Dull)

// Round 2: the hottest guardable functions of the CONSOLE profile (cpu_on, 1020 MHz, gameplay under 40 fps,
// 2026-10-06; tools/switch_profile_report.py). Guarded like the first set and on by default. Percentages are of the
// game thread's running samples there.
//
// Direct3D's shader float-constant writers (1.3 % + 1.2 %): hand-written, a copy of 16-byte vectors plus the dirty
// mask (hot/n_d3d_constants.h says why it is exact); they write no register, so the guard compares r3, f1, the
// non-volatile registers and the memory.
SA2_HOT_HOOK(8255CFC8, sa2_cpu_native_d3d_constants_a, "Native Direct3D constant upload (sub_8255CFC8, constants 376+): one copy",
             sa2::hot::n_8255CFC8, &sa2::hot::n_8255CFC8::Writes, sa2::hot::Cmp::kReturn, 216, 0xA97944CBFFA1143Aull)
SA2_HOT_HOOK(8255CEF0, sa2_cpu_native_d3d_constants_b, "Native Direct3D constant upload (sub_8255CEF0, constants 120+): one copy",
             sa2::hot::n_8255CEF0, &sa2::hot::n_8255CEF0::Writes, sa2::hot::Cmp::kReturn, 216, 0x9C7F63BCBF9EE982ull)
// Localized copies (tools/localize_native.py), leaves or calling only the save/restore helpers: kAll.
SA2_HOT_HOOK(8214BE60, sa2_cpu_native_key_search, "Native keyframe search (sub_8214BE60), localized copy",
             sa2::hot::n_8214BE60,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r7.u32, 4);  // the lower neighbour
               w.Add(c.r8.u32, 4);  // the upper neighbour
               w.Add(c.r9.u32, 4);  // the blend factor
               w.Add(c.r1.u32 - 32, 32);
             },
             sa2::hot::Cmp::kAll, 248, 0xBBFF0F4CF33C2132ull)
SA2_HOT_HOOK(82975E50, sa2_cpu_native_records_equal, "Native 24-byte record array compare (sub_82975E50), localized copy",
             sa2::hot::n_82975E50, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r1.u32 - 64, 64); },
             sa2::hot::Cmp::kAll, 224, 0x6B9C3F8175340FC8ull)
SA2_HOT_HOOK(82140B40, sa2_cpu_native_vmx_matrix, "Native VMX 4x4 matrix routine (sub_82140B40), localized copy",
             sa2::hot::n_82140B40,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r1.u32 + 32, 48);       // the caller's parameter area (r5-r10 spilled)
               w.Add(c.r4.u32 & ~15u, 16);     // stvx
               w.Add(c.r3.u32 & ~15u, 64);     // four stvx
             },
             sa2::hot::Cmp::kAllNaN, 520, 0x5D26CDBD1029AE91ull)
SA2_HOT_HOOK(8214A490, sa2_cpu_native_cos, "Native njCos (sub_8214A490), localized copy",
             sa2::hot::n_8214A490, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r1.u32 - 16, 8); },
             sa2::hot::Cmp::kAll, 456, 0xE926619F5281C11Bull)
SA2_HOT_HOOK(8214A2D8, sa2_cpu_native_sin, "Native njSin (sub_8214A2D8), localized copy",
             sa2::hot::n_8214A2D8, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r1.u32 - 16, 8); },
             sa2::hot::Cmp::kAll, 440, 0xEF455A09F926F7DDull)
SA2_HOT_HOOK(82146EE8, sa2_cpu_native_matrix_4x4, "Native 3x4 to 4x4 matrix (sub_82146EE8), localized copy",
             sa2::hot::n_82146EE8,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r4.u32) w.Add(c.r3.u32, 64);
             },
             sa2::hot::Cmp::kAll, 144, 0xC6318134F65E2EDEull)
SA2_HOT_HOOK(82146E78, sa2_cpu_native_matrix_3x4, "Native 4x4 to 3x4 matrix (sub_82146E78), localized copy",
             sa2::hot::n_82146E78,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r4.u32) w.Add(c.r4.u32, 48);
             },
             sa2::hot::Cmp::kAll, 112, 0x5A0112B4F25E6F9Bull)
SA2_HOT_HOOK(821417A0, sa2_cpu_native_store_slot, "Native one-of-seven word store (sub_821417A0), localized copy",
             sa2::hot::n_821417A0, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r3.u32 + 1568, 28); },
             sa2::hot::Cmp::kAll, 184, 0x2480CFD0D6204630ull)
SA2_HOT_HOOK(82975D88, sa2_cpu_native_records_copy, "Native 24-byte record copy (sub_82975D88), localized copy",
             sa2::hot::n_82975D88,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r4.u32 && c.r5.s32 > 0) {
                 const uint64_t bytes = 4 + 24 * uint64_t(uint32_t(c.r5.s32));
                 if (bytes > Writes::kMaxBytes) w.overflow = true;
                 else w.Add(c.r3.u32, uint32_t(bytes));
               }
             },
             sa2::hot::Cmp::kAll, 80, 0x0C525A39B6BED5B8ull)
// Calls memcpy and sub_82140B40 (both hooked: a nested call may itself be a checked one), so kCalls.
SA2_HOT_HOOK(82140D38, sa2_cpu_native_matrix_op, "Native 4x4 matrix routine (sub_82140D38), localized copy",
             sa2::hot::n_82140D38,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r3.u32, 64);              // the result (unaligned stvlx/stvrx)
               w.Add(c.r1.u32 - 288, 288);       // its own frame (the helper's saves and the callees' writes are in it)
             },
             sa2::hot::Cmp::kCallsNaN, 256, 0xADE17C4CA1DC4F4Cull)
// Calls njCalcPoint (hooked) on its own frame. Writes its 112-byte frame (njCalcPoint's output is in it, the saved
// lr/f31 just below the caller's r1) and z to [r5] when r5 is not null. kReturnInt (2026-10-07, was kCalls): it returns
// an int in r3 and its 8 call sites read r3, r8 and r9 after it, never f1 (tools/call_site_liveness.py). Since round 13
// njCalcPoint's native leaves f1 alone where the original leaves a scratch value, so f1 at this function's return
// depended on whether the nested njCalcPoint call was itself a checked one: a DIFFERENCE on f1 alone switched this copy
// off (console "logs11 all on", PC 2026-10-07), with r3, the non-volatile registers and the memory equal.
SA2_HOT_HOOK(821BA660, sa2_cpu_native_visibility, "Native visibility test (sub_821BA660), localized copy",
             sa2::hot::n_821BA660,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r1.u32 - 112, 112);
               if (c.r5.u32) w.Add(c.r5.u32, 4);
             },
             sa2::hot::Cmp::kReturnInt, 280, 0x06E537F621A2EA14ull)

// Round 3: the next 20 hottest game-thread functions (console profile of the round-2 NRO), localized copies with smart
// sync (tools/localize_native.py: each call syncs only what its callee can reach). Unguarded like the 15 above, each
// with its own key; SA2_COPIES_DEFAULT decides whether they ship on (docs/enhancements.md, "Round 3").
SA2_HOT_HOOK_COPY(8214B950, sa2_cpu_native_sub_8214B950, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214B950 (object draw walk, calls the render-state, matrix and model functions (11.7 % innermost))",
                  sa2::hot::n_8214B950, 440, 0x82BAF7A465166843ull)
SA2_HOT_HOOK_COPY(8214B7A8, sa2_cpu_native_sub_8214B7A8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214B7A8 (render-state table: applies changed states through their handlers (2.7 % self))",
                  sa2::hot::n_8214B7A8, 144, 0xE4977AECB394F67Eull)
SA2_HOT_HOOK_COPY(8213D2B0, sa2_cpu_native_sub_8213D2B0, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213D2B0 (model draw setup, one indirect call (3.6 % self))",
                  sa2::hot::n_8213D2B0, 640, 0x3A3A2DC475A4348Cull)
SA2_HOT_HOOK_COPY(829729A8, sa2_cpu_native_sub_829729A8, SA2_COPIES_DEFAULT, "Native localized copy of sub_829729A8 (draw submission, calls the Direct3D draw functions (1.9 % self))",
                  sa2::hot::n_829729A8, 280, 0x129F34B1EE627CF2ull)
SA2_HOT_HOOK_COPY(828415C8, sa2_cpu_native_sub_828415C8, SA2_COPIES_DEFAULT, "Native localized copy of sub_828415C8 (float routine of the 0x8283 group (collision or physics math; 13 self + 19.8 inner))",
                  sa2::hot::n_828415C8, 2544, 0xE7E7AD758AB929D6ull)
SA2_HOT_HOOK_COPY(8213D8A8, sa2_cpu_native_sub_8213D8A8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213D8A8 (matrix setup: matrix copy, transpose and VMX matrix calls)",
                  sa2::hot::n_8213D8A8, 104, 0xCEC87EFDD287C802ull)
SA2_HOT_HOOK_COPY(8214AF30, sa2_cpu_native_sub_8214AF30, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214AF30 (object transform and draw (inclusive 52.8 %))",
                  sa2::hot::n_8214AF30, 360, 0x736AE8C2FF120106ull)
SA2_HOT_HOOK_COPY(82841830, sa2_cpu_native_sub_82841830, SA2_COPIES_DEFAULT, "Native localized copy of sub_82841830 (float routine of the 0x8283 group, only the save/restore helpers)",
                  sa2::hot::n_82841830, 1152, 0xBF8AEF4A4FAFB058ull)
SA2_HOT_HOOK_COPY(828427F0, sa2_cpu_native_sub_828427F0, SA2_COPIES_DEFAULT, "Native localized copy of sub_828427F0 (float routine of the 0x8283 group, calls 828415C8)",
                  sa2::hot::n_828427F0, 1464, 0xDE0E6D2C7FC69E70ull)
SA2_HOT_HOOK_COPY(82832D58, sa2_cpu_native_sub_82832D58, SA2_COPIES_DEFAULT, "Native localized copy of sub_82832D58 (hot game function, one indirect call)",
                  sa2::hot::n_82832D58, 744, 0x5AECFD5F6266ABE4ull)
SA2_HOT_HOOK_COPY(821BA890, sa2_cpu_native_sub_821BA890, SA2_COPIES_DEFAULT, "Native localized copy of sub_821BA890 (object draw wrapper (inclusive 59 %))",
                  sa2::hot::n_821BA890, 128, 0x997B62A9F632926Eull)
SA2_HOT_HOOK_COPY(82842308, sa2_cpu_native_sub_82842308, SA2_COPIES_DEFAULT, "Native localized copy of sub_82842308 (routine of the 0x8283 group, calls 82841FB8)",
                  sa2::hot::n_82842308, 560, 0x1CFEFBE869CCAA91ull)
SA2_HOT_HOOK_COPY(82842E28, sa2_cpu_native_sub_82842E28, SA2_COPIES_DEFAULT, "Native localized copy of sub_82842E28 (wrapper of 828427F0)",
                  sa2::hot::n_82842E28, 128, 0x34037AF73161CA53ull)
SA2_HOT_HOOK_COPY(82825070, sa2_cpu_native_sub_82825070, SA2_COPIES_DEFAULT, "Native localized copy of sub_82825070 (hot game function, indirect calls)",
                  sa2::hot::n_82825070, 1192, 0xE0E6E9A9BD9196F9ull)
SA2_HOT_HOOK_COPY(82139930, sa2_cpu_native_sub_82139930, SA2_COPIES_DEFAULT, "Native localized copy of sub_82139930 (hot game function, one indirect call)",
                  sa2::hot::n_82139930, 536, 0x70051431EDFAD872ull)
SA2_HOT_HOOK_COPY(82842F08, sa2_cpu_native_sub_82842F08, SA2_COPIES_DEFAULT, "Native localized copy of sub_82842F08 (wrapper of 82840E78 and 82842308)",
                  sa2::hot::n_82842F08, 96, 0xE384CE4115B7233Full)
SA2_HOT_HOOK_COPY(82147380, sa2_cpu_native_sub_82147380, SA2_COPIES_DEFAULT, "Native localized copy of sub_82147380 (matrix routine: transpose, VMX matrix and memcpy calls)",
                  sa2::hot::n_82147380, 96, 0x78B2850954A4B571ull)
SA2_COPY_FOR_HOOK(82564F78, sa2_cpu_native_sub_82564F78, SA2_COPIES_DEFAULT, "Native localized copy of sub_82564F78 (Direct3D draw path; called by the D3D API layer's hook)",
                  sa2::hot::n_82564F78, 1216, 0x16CE63397DFBCDCBull)
SA2_HOT_HOOK_COPY(8213E0F8, sa2_cpu_native_sub_8213E0F8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213E0F8 (hot game function, indirect calls)",
                  sa2::hot::n_8213E0F8, 256, 0x9FC6BCE3A1354E06ull)
SA2_HOT_HOOK_COPY(82841FB8, sa2_cpu_native_sub_82841FB8, SA2_COPIES_DEFAULT, "Native localized copy of sub_82841FB8 (routine of the 0x8283 group, only the save/restore helpers)",
                  sa2::hot::n_82841FB8, 784, 0x36D820C7E337CEF3ull)

// Round 4: 20 more game-thread functions of the console profile (round-2 NRO, cpu_on, 1020 MHz), ranked by their own
// samples: a copy speeds up the function's own code, not its callees (docs/enhancements.md, "Round 4").
//
// Two leaves, guarded like the round-2 copies (every register compared; the guard runs both versions):
//  - sub_82840E78 multiplies floats: NaN-tolerant compare (round 2: the sign of a NaN depends on the compilation);
//  - sub_8214B5A8 saves r30/r31 below r1 and writes one record of a table: (entry - 9) * 20 bytes after 0x83853480,
//    entry = the word at 0x83850768 + ((r3 >> 14) & 0x3FC), the same loads the function makes.
SA2_HOT_HOOK(82840E78, sa2_cpu_native_sub_82840E78, "Native localized copy of sub_82840E78 (rescales 16 records of four floats by a limit over the fourth; leaf)",
             sa2::hot::n_82840E78,
             +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r3.u32, 256); },
             sa2::hot::Cmp::kAllNaN, 168, 0x2F043BAEA9B4C012ull)
SA2_HOT_HOOK(8214B5A8, sa2_cpu_native_sub_8214B5A8, "Native localized copy of sub_8214B5A8 (fills one record of a table at 0x83853480 from three lookups indexed by r3; leaf)",
             sa2::hot::n_8214B5A8,
             +[](const PPC& c, const uint8_t* base, Writes& w) {
               w.Add(c.r1.u32 - 16, 16);  // r30 and r31 (no frame of its own)
               const uint32_t entry = Ld32(base, 0x83850768u + ((c.r3.u32 >> 14) & 0x3FCu));
               w.Add(0x83853480u + (entry - 9u) * 20u, 17);  // +0..+16: four words and the byte of r3
             },
             sa2::hot::Cmp::kAll, 128, 0x757EF8F6C1A7A575ull)
// The Direct3D draw and its shader load are hooked by the renderer (src/me/sa2_me_hooks.cpp, NoteDrawCall /
// NoteShaderObject): the hooks call these copies in place of the originals (sub_82565478 was the largest function
// of the game thread without a native: 1.7 % self, 7.5 % with what it calls).
SA2_COPY_FOR_HOOK(82565478, sa2_cpu_native_sub_82565478, SA2_COPIES_DEFAULT, "Native localized copy of sub_82565478 (D3DDevice_DrawVertices: the draw's PM4 packets; called by the renderer's hook)",
                  sa2::hot::n_82565478, 1048, 0x96808E6D756A267Full)
SA2_COPY_FOR_HOOK(82555F40, sa2_cpu_native_sub_82555F40, SA2_COPIES_DEFAULT, "Native localized copy of sub_82555F40 (D3D draw-time shader load; called by the renderer's hook)",
                  sa2::hot::n_82555F40, 2432, 0x1D2AC18C0489EEC1ull)
// Sixteen more that make calls: unguarded, exact by construction, smart sync. (The GPU wait loop is not among them:
// src/cpu/sa2_gpu_wait.cpp hooks it already.)
// Name no other function's address in this file: tools/direct_calls.py and tools/localize_native.py treat every
// 82xxxxxx address the app sources mention as hooked (no direct calls to it, full sync around it in the copies).
SA2_HOT_HOOK_COPY(82555298, sa2_cpu_native_sub_82555298, SA2_COPIES_DEFAULT, "Native localized copy of sub_82555298 (D3D: copies a block of state into the command buffer)",
                  sa2::hot::n_82555298, 1576, 0x2BC39EB4A3C1AA0Dull)
SA2_HOT_HOOK_COPY(8251CC80, sa2_cpu_native_sub_8251CC80, SA2_COPIES_DEFAULT, "Native localized copy of sub_8251CC80 (float routine with indirect calls)",
                  sa2::hot::n_8251CC80, 528, 0x5AAFCDEB139DF728ull)
SA2_HOT_HOOK_COPY(821410F0, sa2_cpu_native_sub_821410F0, SA2_COPIES_DEFAULT, "Native localized copy of sub_821410F0 (one direct and one indirect call)",
                  sa2::hot::n_821410F0, 128, 0x85B45347A7086026ull)
SA2_HOT_HOOK_COPY(82139158, sa2_cpu_native_sub_82139158, SA2_COPIES_DEFAULT, "Native localized copy of sub_82139158 (a wrapper with one direct call)",
                  sa2::hot::n_82139158, 88, 0x5C9A9200D8FF4EF1ull)
SA2_HOT_HOOK_COPY(8217EB38, sa2_cpu_native_sub_8217EB38, SA2_COPIES_DEFAULT, "Native localized copy of sub_8217EB38 (one direct and one indirect call)",
                  sa2::hot::n_8217EB38, 160, 0x430CEE9E1B5B4811ull)
SA2_HOT_HOOK_COPY(821412E0, sa2_cpu_native_sub_821412E0, SA2_COPIES_DEFAULT, "Native localized copy of sub_821412E0 (float routine with six indirect calls)",
                  sa2::hot::n_821412E0, 480, 0x6627FEACAC5B993Cull)
SA2_HOT_HOOK_COPY(82562A90, sa2_cpu_native_sub_82562A90, SA2_COPIES_DEFAULT, "Native localized copy of sub_82562A90 (D3D: VMX and float state into PM4 packets)",
                  sa2::hot::n_82562A90, 1664, 0xD23721070E2EAA54ull)
SA2_HOT_HOOK_COPY(8251CB88, sa2_cpu_native_sub_8251CB88, SA2_COPIES_DEFAULT, "Native localized copy of sub_8251CB88 (object transform: njTranslate, njRotateX/Y/Z and sub_8251CC80)",
                  sa2::hot::n_8251CB88, 248, 0x3DA2C02A2AD65BFDull)
SA2_HOT_HOOK_COPY(82555BF8, sa2_cpu_native_sub_82555BF8, SA2_COPIES_DEFAULT, "Native localized copy of sub_82555BF8 (D3D: memcpy and four D3D helpers)",
                  sa2::hot::n_82555BF8, 344, 0x875D371E7E7931EFull)
SA2_HOT_HOOK_COPY(8254D720, sa2_cpu_native_sub_8254D720, SA2_COPIES_DEFAULT, "Native localized copy of sub_8254D720 (D3D: two direct calls)",
                  sa2::hot::n_8254D720, 528, 0xB162AF7A88554AEBull)
SA2_HOT_HOOK_COPY(8254CA68, sa2_cpu_native_sub_8254CA68, SA2_COPIES_DEFAULT, "Native localized copy of sub_8254CA68 (D3D: three direct calls, the GPU wait loop among them)",
                  sa2::hot::n_8254CA68, 152, 0x036E0642FDE072D7ull)
SA2_HOT_HOOK_COPY(8217EE68, sa2_cpu_native_sub_8217EE68, SA2_COPIES_DEFAULT, "Native localized copy of sub_8217EE68 (calls the 0x8217E family and one indirect call)",
                  sa2::hot::n_8217EE68, 776, 0xE00AD515528C6D6Cull)
SA2_HOT_HOOK_COPY(8214ED78, sa2_cpu_native_sub_8214ED78, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214ED78 (float routine, njCalcPoint twice)",
                  sa2::hot::n_8214ED78, 552, 0x293DD9F7805E8702ull)
SA2_HOT_HOOK_COPY(82228340, sa2_cpu_native_sub_82228340, SA2_COPIES_DEFAULT, "Native localized copy of sub_82228340 (seven direct calls into three modules)",
                  sa2::hot::n_82228340, 512, 0xA50BABE6ECAB795Dull)
SA2_HOT_HOOK_COPY(8213D680, sa2_cpu_native_sub_8213D680, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213D680 (converts a float and dispatches through one indirect call)",
                  sa2::hot::n_8213D680, 96, 0x748F92E966D9F5A0ull)
SA2_HOT_HOOK_COPY(82139D20, sa2_cpu_native_sub_82139D20, SA2_COPIES_DEFAULT, "Native localized copy of sub_82139D20 (three memcpy calls)",
                  sa2::hot::n_82139D20, 272, 0xF1C04D41B8B51FD1ull)

// Round 13 (2026-10-07, the user's request after the 2P console profile): the chunk-model vertex loaders by hand
// (src/cpu/hot/n_vertex_chunks.h). Their njCalcPoint calls were the game thread's largest single cost in the 2P race
// (8.5 %), each one through the hook and a register write-back and reload around it. Lists with skinned chunks (types
// 44 and 35, which call other functions) are declined. Guarded (every call in verify mode), kReturnInt.
SA2_HOT_HOOK(8214D198, sa2_cpu_native_vertex_normals,
             "Native chunk-model vertex loader with normals (sub_8214D198): njCalcPoint inline, the matrix read once",
             sa2::hot::n_vn_points_normals, &sa2::hot::n_vn_points_normals::Writes, sa2::hot::Cmp::kReturnInt, 488,
             0x6A9224B55ADC26CEull)
SA2_HOT_HOOK(8214D380, sa2_cpu_native_vertex_points,
             "Native chunk-model vertex loader, positions (sub_8214D380): njCalcPoint inline, the matrix read once",
             sa2::hot::n_vn_points, &sa2::hot::n_vn_points::Writes, sa2::hot::Cmp::kReturnInt, 400,
             0xEB679F82BD509F40ull)
