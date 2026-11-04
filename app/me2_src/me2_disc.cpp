// Mass Effect 2 (TU2) - no "insert disc 2" screen (2026-11-03, user: "hide it completely").
//
// The port's game folder holds both discs (merged, Disc 1's default.xex). The game's file table (the BioToC resource)
// has, per file, a value for disc 1 (+8) and one for disc 2 (+12); FFileManagerXenon's lookup sub_827D5E98 (this r3,
// name r4; vtable 0x821A1CD8 slot 0x18) returns the one of the disc in the drive, GDiscNum (0x8337C230, read from the
// XEX's "DiscNum" resource at start: 1 here), and -1 for a file that is not on it. Before a level loads, sub_829F8520
// asks that for the level's package and, on -1, the disc swap screen comes up (sub_829F8440 / sub_829F8668: talk-table
// texts 0x443F3 / 0x46549).
//
// With me2_single_disc, a -1 from the drive's disc falls back to the other disc's value (the file is in the folder):
// no swap screen, and the loader sees the file as present.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <atomic>
#include <cstdint>

REXCVAR_DEFINE_BOOL(me2_single_disc, true, "ME2",
                    "Both discs' files are in the game folder: no 'insert disc 2' screen (the file table's other-disc "
                    "entry answers for a file the current disc lacks)");

namespace {
uint32_t Load32(const uint8_t* base, uint32_t va) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va));
}
constexpr uint32_t kDiscNum = 0x8337C230;  // GDiscNum
std::atomic<uint32_t> g_logged{0};
}  // namespace

REX_EXTERN(__imp__sub_827D5E98);
REX_EXTERN(__imp__sub_827D5CE0);  // the file table's find (this, name) -> entry or 0
REX_HOOK_RAW(sub_827D5E98) {
  const uint32_t self = ctx.r3.u32, name = ctx.r4.u32;
  __imp__sub_827D5E98(ctx, base);
  if (ctx.r3.u32 != 0xFFFFFFFFu || !REXCVAR_GET(me2_single_disc)) return;
  const uint64_t original = ctx.r3.u64;
  ctx.r3.u64 = self;
  ctx.r4.u64 = name;
  __imp__sub_827D5CE0(ctx, base);
  const uint32_t entry = ctx.r3.u32;
  ctx.r3.u64 = original;
  if (!entry) return;  // not in the table at all: the game's answer stays
  const bool disc1 = Load32(base, kDiscNum) == 1;
  const uint32_t other = Load32(base, entry + (disc1 ? 12 : 8));
  if (other == 0xFFFFFFFFu) return;
  ctx.r3.u64 = other;
  if (g_logged.fetch_add(1, std::memory_order_relaxed) < 8)
    REXLOG_INFO("[disc] a disc {} file answered from the disc {} entry ({}): no disc swap", disc1 ? 2 : 1,
                disc1 ? 2 : 1, other);
}
