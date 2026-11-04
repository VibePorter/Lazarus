// ME2 (2026-10-27): from the second PGO profile (2 Ryujinx sessions on the Normandy, gameplay): 1.36 % of the arc
// counts, the 4th hottest guest function, 414 M loop iterations. Exchanges a list of per-bone records with a skeletal
// mesh object's bone data, in place (each record trades its values with the bone's; called twice gives the old state).
//
// r3 = object (0: nothing), r4 = TArray of 96-byte records (Data +0, Num +4).
//   limit = obj+128 (int), atoms = obj+124 (68-byte entries), mats = obj+172 (64-byte entries)
//   for each record e (Num > 0, signed), i = e+0 (int): if (i < limit, signed) with a = atoms + 68 i, m = mats + 64 i:
//     swap 12 bytes e+4  <-> a+28    (three words, in that order: loads of a, stores to a, stores to e)
//     swap 16 bytes e+16 <-> a+12    (through a stack copy at r1-160)
//     swap 64 bytes e+32 <-> m       (m -> r1-144, e+32 -> m, r1-144 -> e+32; 8-byte ldu/stdu)
// Plain word moves: the result is the same bytes in other places. No return value, r3 unchanged; it saves r23-r31
// below r1 (savegpr) and uses r1-160..r1-80 as scratch, all callee scratch below r1. 7 direct call sites read no
// volatile register after it (tools/hot/liveness_me2.py); one tail call and one pointer to it in the image.
//
// DECLINED (the original runs, nothing modified): a negative index (its atom / matrix address would wrap), ranges that
// wrap around 4 GB or (PC) cross 0xE0000000, and inputs where the three write ranges overlap each other or the fields
// the original re-reads every iteration (obj+124..+176, the TArray header): there the order of the original's word
// moves would matter.
#pragma once
#include "../me_hot_common.h"

namespace me::hot::n_825464E0 {
inline constexpr Cmp kCmp = {R(3), 0, 0};

struct Plan {
  uint32_t data = 0, num = 0, atoms = 0, mats = 0;
  int32_t limit = 0;
  bool any = false;  // at least one record in range
  uint32_t lo = 0, hi = 0;  // bone indices in range: [lo, hi]
  bool ok = false;
};

inline bool Disjoint(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a1 <= b0 || b1 <= a0; }

inline bool InOneRange(uint64_t a, uint64_t len) {
  return len == 0 || (a + len <= 0x100000000ull && PhysOff(uint32_t(a)) == PhysOff(uint32_t(a + len - 1)));
}

inline Plan Prepare(const PPCContext& ctx, const uint8_t* base) {
  Plan p;
  const uint32_t obj = ctx.r3.u32, arr = ctx.r4.u32;
  if (obj == 0) {
    p.ok = true;
    return p;
  }
  const int32_t num = int32_t(Ld32(base, arr + 4));
  if (num <= 0) {
    p.ok = true;
    return p;
  }
  p.num = uint32_t(num);
  p.data = Ld32(base, arr);
  p.limit = int32_t(Ld32(base, obj + 128));
  p.atoms = Ld32(base, obj + 124);
  p.mats = Ld32(base, obj + 172);
  const uint64_t rec_len = uint64_t(p.num) * 96;
  if (!InOneRange(p.data, rec_len)) return p;
  for (uint32_t k = 0; k < p.num; ++k) {
    const int32_t i = int32_t(Ld32(base, p.data + 96 * k));
    if (i >= p.limit) continue;
    if (i < 0) return p;
    if (!p.any) {
      p.lo = p.hi = uint32_t(i);
      p.any = true;
    } else {
      if (uint32_t(i) < p.lo) p.lo = uint32_t(i);
      if (uint32_t(i) > p.hi) p.hi = uint32_t(i);
    }
  }
  if (p.any) {
    const uint64_t a0 = uint64_t(p.atoms) + 68ull * p.lo, a1 = uint64_t(p.atoms) + 68ull * (p.hi + 1ull);
    const uint64_t m0 = uint64_t(p.mats) + 64ull * p.lo, m1 = uint64_t(p.mats) + 64ull * (p.hi + 1ull);
    const uint64_t d0 = p.data, d1 = p.data + rec_len;
    if (!InOneRange(a0, a1 - a0) || !InOneRange(m0, m1 - m0)) return p;
    if (!Disjoint(a0, a1, m0, m1) || !Disjoint(a0, a1, d0, d1) || !Disjoint(m0, m1, d0, d1)) return p;
    // Fields the original reads again in every iteration.
    const uint64_t o0 = uint64_t(obj) + 124, o1 = uint64_t(obj) + 176, h0 = arr, h1 = uint64_t(arr) + 8;
    const uint64_t ranges[3][2] = {{a0, a1}, {m0, m1}, {d0, d1}};
    for (const auto& r : ranges)
      if (!Disjoint(r[0], r[1], o0, o1) || !Disjoint(r[0], r[1], h0, h1)) return p;
  }
  p.ok = true;
  return p;
}

inline void Swap(uint8_t* x, uint8_t* y, size_t n) {
  uint8_t t[64];
  std::memcpy(t, x, n);
  std::memcpy(x, y, n);
  std::memcpy(y, t, n);
}

inline bool Native(PPCContext& ctx, uint8_t* base) {
  const Plan p = Prepare(ctx, base);
  if (!p.ok) return false;
  if (!p.any) return true;
  // The three ranges are disjoint and each lies in one host block: host pointers from their start are valid across them.
  MarkCards(base, p.data, p.num * 96);  // ME2: the store cards, the ranges Writes() declares
  MarkCards(base, uint32_t(p.atoms + 68 * p.lo), 68 * (p.hi - p.lo + 1));
  MarkCards(base, uint32_t(p.mats + 64 * p.lo), 64 * (p.hi - p.lo + 1));
  uint8_t* const recs = Raw(base, p.data);
  uint8_t* const atoms = Raw(base, uint32_t(p.atoms + 68 * p.lo));
  uint8_t* const mats = Raw(base, uint32_t(p.mats + 64 * p.lo));
  for (uint32_t k = 0; k < p.num; ++k) {
    uint8_t* const e = recs + 96 * size_t(k);
    uint32_t iv;
    std::memcpy(&iv, e, 4);
    const int32_t i = int32_t(__builtin_bswap32(iv));
    if (i >= p.limit) continue;
    uint8_t* const a = atoms + 68 * size_t(uint32_t(i) - p.lo);
    uint8_t* const m = mats + 64 * size_t(uint32_t(i) - p.lo);
    Swap(e + 4, a + 28, 12);
    Swap(e + 16, a + 12, 16);
    Swap(e + 32, m, 64);
  }
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const Plan p = Prepare(ctx, base);
  if (!p.ok || !p.any) return;
  w.Add(p.data, p.num * 96);
  w.Add(uint32_t(p.atoms + 68 * p.lo), 68 * (p.hi - p.lo + 1));
  w.Add(uint32_t(p.mats + 64 * p.lo), 64 * (p.hi - p.lo + 1));
}
}  // namespace me::hot::n_825464E0
