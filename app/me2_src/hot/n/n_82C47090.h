// ME2 (2026-10-27): strlen (8-bit) of the game's own code (not the XDK CRT copy), found from the PGO profile (Ryujinx
// sessions): 0.51 % of the guest arc counts, 652 M loop iterations from only two callers (sub_822EE030, sub_8273F830).
//
//   r3 = string. PPC: r11 = r3; do { r10 = lbz [r11]; r11 += 1 } while (r10 != 0); r3 = rotlwi(r11 - r3 - 1, 0), i.e.
//   the byte count before the first zero, as a 32-bit value zero-extended to 64 bits. Reads only, no FPU, no stack.
// The load address is the 32-bit pointer; a string running across 0xE0000000 with the PC's +0x1000 physical offset is
// not handled specially (none exists; same choice as cinnamon's wcslen n_82AD4228.h). std::strlen never reads past the
// aligned block that holds the terminator.
// Registers: the original leaves r10/r11 as scratch; ME2 call-site liveness: the two call sites read only r3.
#pragma once

#include <cstring>

#include "../me_hot_common.h"

namespace me::hot::n_82C47090 {

inline constexpr Cmp kCmp = kCmpRet;

inline void Native(PPCContext& ctx, uint8_t* base) {
  const char* s = reinterpret_cast<const char*>(Raw(base, ctx.r3.u32));
  ctx.r3.u64 = uint32_t(std::strlen(s));
}

inline void Writes(const PPCContext&, const uint8_t*, me::hot::Writes&) {}

}  // namespace me::hot::n_82C47090
