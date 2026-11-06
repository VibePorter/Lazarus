// ME2 (2026-10-27): UE3's texture + sampler setter of the material parameters (FMaterialShaderParameters::Set,
// sub_825DCDA0 -> sub_827DB288(sampler, sampler state, texture param)), with the three XDK functions it calls inlined:
// D3DDevice_SetTexture sub_82A9AF70 and the two filter setters sub_82A9D268 / sub_82A9D0C0. Console stack profile of
// 22:2x / 22:5x (Omega, 13.4-13.9 fps, UE3 render thread X827BD330 100 % on core 0): 827DB288 3.2-4.1 %, 82A9AF70
// 1.4-1.7 %, the filter setters ~0.5 % of that thread's samples, about 4 calls a draw.
//
// sub_827DB288(r3 = s (sampler, u16 from the caller), r4 = p (UE3 sampler state), r5 = t (texture param)):
//   dev = [0x833D074C]; M = (1 << 63) >> (s + 32)            (fetch constant dirty bit, srd: 0 for s + 32 >= 64)
//   SetTexture(dev, s, tex = [t + 8], M); MinFilter(dev, s, [p + 4]); MagFilter(dev, s, [p + 8])
//   fc = dev + 24 * (s + 48)  (fetch constant s: 6 words)
//   fc[3] = rlwimi(fc[3], [p + 12], 23, 7, 8);             dev[24] |= M
//   fc[0] = rlwimi(fc[0], [p + 16], 10, 19, 21);           dev[24] |= M
//   fc[0] = rlwimi(fc[0], [p + 20], 13, 16, 18);           dev[24] |= M
//   fc[0] = rlwimi(fc[0], [p + 24], 16, 13, 15);           dev[24] |= M
//   fc[5] |= 24;                                            dev[24] |= M
// SetTexture(dev, s, tex, M): old = dev[12552 + 4 s]; if tex: fc[0..5] rebuilt from tex[28..48] (bit fields, the
//   per-sampler bytes dev[11958 + s] / dev[11984 + s] clamp the mip range), dev[24] |= M; dev[12552 + 4 s] = tex;
//   then for old != 0: dev[10908] != 0 -> old[8] = dev[10908]; else (dev[10912] & old[0]) != 0 -> a packet is written
//   into the command buffer (and sub_82AA15E8 grows it when full; the packet's top bit comes from an uninitialised
//   stack word): DECLINED here (decided before any write: the original runs).
// Filter setters (dev, s, v): fc[4] / fc[3] bit fields from v, the sampler byte dev[11932 + s] through the table
//   0x822483A8 and dev[12010 + s]; dev[24] |= M. They differ only in the shifts (see Filter below).
// Every word is computed exactly as the generated code does in 32 bits (the 64-bit parts of rlwimi / add / andc never
// reach the stored low words). DECLINED: s >= 32 (never seen; the original handles it).
// Registers: the originals leave r3-r12 / ctr / cr as scratch; tools/hot/liveness_me2.py: none of the 80 direct call
// sites reads a volatile register after the call (one pointer to it in data, 0x822687E0: callers through a pointer
// cannot rely on scratch values).
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_827DB288 {

inline constexpr Cmp kCmp = {0, 0, 0};

inline constexpr uint32_t kDevicePtr = 0x833D074Cu;
inline constexpr uint32_t kFilterTable = 0x822483A8u;

// PPC rotate-and-mask on 32 bits (bit 0 = most significant).
constexpr uint32_t Mask(uint32_t mb, uint32_t me) {
  const uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
  return mb <= me ? (a & b) : (a | b);
}
constexpr uint32_t Rotl(uint32_t v, uint32_t sh) { return sh ? (v << sh) | (v >> (32 - sh)) : v; }
constexpr uint32_t Rlwinm(uint32_t v, uint32_t sh, uint32_t mb, uint32_t me) { return Rotl(v, sh) & Mask(mb, me); }
constexpr uint32_t Rlwimi(uint32_t a, uint32_t v, uint32_t sh, uint32_t mb, uint32_t me) {
  const uint32_t m = Mask(mb, me);
  return (Rotl(v, sh) & m) | (a & ~m);
}

inline uint32_t Fc(uint32_t dev, uint32_t s) { return dev + 24u * (s + 48u); }

// sub_82A9D268 (min filter: kMag false) / sub_82A9D0C0 (mag filter: kMag true), on the fetch constant's words 3 and 4
// held in registers (fc3 / fc4: the values the original reads from and stores to fc + 12 / fc + 16).
template <bool kMag>
inline void Filter(const uint8_t* base, uint32_t dev, uint32_t s, uint32_t v, uint32_t& fc3, uint32_t& fc4) {
  const uint32_t r7 = v >> 2;
  uint32_t r29 = fc4;
  uint32_t r31 = fc3;
  const uint32_t r6 = kMag ? Rlwinm(r29, 22, 31, 31) : Rlwinm(r29, 21, 31, 31);
  const uint32_t r8 = Ld32(base, kFilterTable + 4u * uint32_t(Ld8(base, dev + s + 11932)));
  r29 = kMag ? Rlwimi(r29, r7, 11, 20, 20) : Rlwimi(r29, r7, 10, 21, 21);
  uint32_t r9 = r8 & ~((r6 | r7) - 1u);
  r9 = kMag ? Rlwinm(r9, 4, 0, 27) : Rlwinm(r9, 6, 0, 25);
  r9 = r9 | r7 | v;
  if (kMag) {
    r31 = Rlwimi(r31, r9, 21, 9, 10);
    r31 = Rlwimi(r31, r9, 21, 4, 6);
  } else {
    r31 = Rlwimi(r31, r9, 19, 11, 12);
    r31 = Rlwimi(r31, r9, 19, 4, 6);
  }
  const uint32_t b = Ld8(base, dev + s + 12010);
  const uint32_t lim = (b >> 2) - 1u;
  uint32_t r6b = Rlwimi(r31, r31, 31, 13, 31);
  r6b = Rlwimi(r6b, r31, 31, 1, 11);
  uint32_t r10 = (Rlwinm(r6b, 13, 20, 31) & lim) + (b & ~lim);
  r10 = Rlwimi(r10, r29, 0, 0, 29);
  fc3 = r31;
  fc4 = r10;  // the original stores r29 first, then r10 over it
}

// A guest store without its marks: the lean native below marks each written range once (MarkCards). The shadow build
// (the guard's private copy) goes through St32 / St64 as before.
inline void Put32(uint8_t* base, uint32_t a, uint32_t v) {
#if defined(ME_HOT_SHADOW_ACCESSORS)
  St32(base, a, v);
#else
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, a), &v, 4);
#endif
}
inline void Put64(uint8_t* base, uint32_t a, uint64_t v) {
#if defined(ME_HOT_SHADOW_ACCESSORS)
  St64(base, a, v);
#else
  v = __builtin_bswap64(v);
  std::memcpy(Raw(base, a), &v, 8);
#endif
}

#if !defined(ME_HOT_SHADOW_ACCESSORS)
// ME2 (2026-11-06, me2_texture_header_cache): a host copy of the six header words SetTexture reads (tex +28..+51), so a
// bind skips the texture header's cache miss (console stacks 2026-11-06: this native 8 % of the UE3 render thread, on
// those loads). Valid while the store cards of the words' pages (base[ea >> 12]: every guest store and every host writer
// of guest memory zeroes them) stay non-zero: on a refill the cards are set to 1 BEFORE the words are read, so a write
// after the read zeroes them again. Renut proves its vertex / index copies with the cards of PHYSICAL pages (draw.cpp
// PagesClean: views 0xA0000000+ and 0x7F000000-0x80000000): those pages are never marked here (their headers are not
// cached), so Renut's protocol is untouched. An entry is used only after a later frame found its card still clean and the
// words unchanged (a write that landed while the entry was being filled is caught there). Mode 1 counts only (the words
// are always read and compared: "[texcache]" every 10 s), mode 2 uses the copy and still compares one use in 256 (a
// difference logs and falls back to mode 1 for good).
}  // namespace me::hot::n_827DB288
#include <atomic>
extern "C" int32_t g_me2_texcache_mode;               // me_hot_guest.cpp, from me2_texture_header_cache
extern std::atomic<uint64_t> g_sa2_game_frames;       // src/sa2_capture.cpp: counted at each Swap
namespace me::hot::n_827DB288 {
namespace texcache {
constexpr uint32_t kSlots = 8192;
struct Entry {
  uint32_t tex = 0;
  uint32_t w[6] = {};      // +28, +32, +36, +40, +44, +48
  uint32_t frame = 0;      // the frame of the refill
  uint8_t state = 0;       // 0 empty, 1 filled, 2 confirmed in a later frame
};
inline Entry g_entries[kSlots];
struct Stats {
  uint64_t binds = 0, outside = 0, refills = 0, clean = 0, confirmed = 0, used = 0, equal = 0, stale = 0, checked = 0;
};
inline Stats g_stats;
inline uint32_t g_check = 0;
// Pages Renut never reads the cards of (draw.cpp PhysicalOf: 0x7F000000-0x7FFFFFFF and 0xA0000000+ are physical views).
inline bool Markable(uint32_t va) { return va < 0x7F000000u || (va >= 0x80000000u && va < 0xA0000000u); }
inline void Read(const uint8_t* base, uint32_t tex, uint32_t w[6]) {
  for (int i = 0; i < 6; ++i) w[i] = Ld32(base, tex + 28u + 4u * uint32_t(i));
}
inline void Words(uint8_t* base, uint32_t tex, uint32_t w[6]) {
  const int32_t mode = g_me2_texcache_mode;
  if (mode <= 0) {
    Read(base, tex, w);
    return;
  }
  Stats& st = g_stats;
  ++st.binds;
  const uint32_t first = tex + 28u, last = tex + 51u;
  if (!Markable(first) || !Markable(last) || last < first) {
    ++st.outside;
    Read(base, tex, w);
    return;
  }
  uint8_t* c0 = base + (first >> 12);
  uint8_t* c1 = base + (last >> 12);
  Entry& e = g_entries[((tex >> 2) * 0x9E3779B1u) >> (32 - 13)];
  const uint32_t frame = uint32_t(g_sa2_game_frames.load(std::memory_order_relaxed));
  const bool clean = __atomic_load_n(c0, __ATOMIC_RELAXED) && __atomic_load_n(c1, __ATOMIC_RELAXED);
  if (e.tex == tex && e.state && clean) {
    ++st.clean;
    if (mode >= 2 && e.state == 2 && (++g_check & 255u) != 0) {
      std::memcpy(w, e.w, sizeof(e.w));
      ++st.used;
      return;
    }
    Read(base, tex, w);
    ++st.checked;
    if (std::memcmp(w, e.w, sizeof(e.w)) == 0) {
      ++st.equal;
      if (e.state == 1 && frame != e.frame) {
        e.state = 2;
        ++st.confirmed;
      }
      return;
    }
    // Clean cards over changed words: refill (and, after a confirmation, the cache cannot be trusted).
    if (e.state == 2) {
      ++st.stale;
      if (mode >= 2) g_me2_texcache_mode = 1;
    }
  } else {
    // A refill: the cards first, then the words.
    __atomic_store_n(c0, uint8_t(1), __ATOMIC_RELAXED);
    __atomic_store_n(c1, uint8_t(1), __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    Read(base, tex, w);
    ++st.refills;
  }
  e.tex = tex;
  std::memcpy(e.w, w, sizeof(e.w));
  e.frame = frame;
  e.state = 1;
}
}  // namespace texcache
#endif

// sub_82A9AF70 for a call that needs no command buffer packet (checked by the caller), on the fetch constant's words
// in registers (fc[0..5]); returns whether a texture was set (the words 1 and 2 are then written too).
inline bool SetTexture(uint8_t* base, uint32_t dev, uint32_t s, uint32_t tex, uint32_t (&fc)[6]) {
  if (!tex) return false;
#if !defined(ME_HOT_SHADOW_ACCESSORS)
  uint32_t hw[6];
  texcache::Words(base, tex, hw);  // me2_texture_header_cache (mode 0: the plain loads)
  const uint32_t t28 = hw[0], t32 = hw[1], t36 = hw[2], t40 = hw[3], t44 = hw[4], t48 = hw[5];
#else
  const uint32_t t48 = Ld32(base, tex + 48), t32 = Ld32(base, tex + 32), t44 = Ld32(base, tex + 44);
  const uint32_t t28 = Ld32(base, tex + 28), t36 = Ld32(base, tex + 36), t40 = Ld32(base, tex + 40);
#endif
  const uint32_t hi48 = (Rlwinm(t48, 12, 20, 31) + 512u) & 0x1000u;
  const uint32_t hi32 = (Rlwinm(t32, 12, 20, 31) + 512u) & 0x1000u;
  const uint32_t w5 = Rlwimi(hi48 + Rlwinm(t48, 0, 3, 22), fc[5], 0, 23, 31);
  const uint32_t w1 = Rlwimi(hi32 + (t32 & 0x1FFFFFFFu), fc[1], 0, 20, 20);
  uint32_t w4 = Rlwimi(t44, fc[4], 0, 30, 21);
  const uint32_t w0 = Rlwimi(t28, fc[0], 0, 10, 21);
  const uint32_t w3 = Rlwimi(t40, fc[3], 0, 1, 12);
  const uint32_t lod_lo = std::max(Rlwinm(t44, 30, 28, 31), uint32_t(Ld8(base, dev + s + 11958)));
  const uint32_t lod_hi = std::min(Rlwinm(t44, 26, 28, 31), uint32_t(Ld8(base, dev + s + 11984)));
  w4 = Rlwimi(w4, lod_lo, 2, 26, 29);
  w4 = Rlwimi(w4, lod_hi, 6, 22, 25);
  fc[0] = w0, fc[1] = w1, fc[2] = t36, fc[3] = w3, fc[4] = w4, fc[5] = w5;
  return true;
}

// Whether SetTexture would write a command buffer packet (the declined path).
inline bool NeedsPacket(const uint8_t* base, uint32_t dev, uint32_t s) {
  const uint32_t old = Ld32(base, dev + 4u * (s + 3138u));
  return old && Ld32(base, dev + 10908) == 0 && (Ld32(base, dev + 10912) & Ld32(base, old)) != 0;
}

// ME2 (2026-11-06, lean): the original's ~20 stores (the fetch constant rewritten by SetTexture, both filters and the
// final bit fields; the dirty mask ORed four times) as one pass in registers and one store per written word, the store
// cards and the watched device chunks marked once per range. Console stacks 2026-11-06 19:44: the per-store version was
// 7.2 % of the UE3 render thread (~3.3 us a bind), flat over 11 KB of code (every St32 reloads the watch range after
// the aliasing byte stores). Same results: the guard compares this code (shadow build: St32 / St64) with the original.
inline bool Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32, p = ctx.r4.u32, t = ctx.r5.u32;
  const uint32_t dev = Ld32(base, kDevicePtr);
  if (s >= 32 || !dev || NeedsPacket(base, dev, s)) return false;
  const uint64_t m = (uint64_t(1) << 63) >> (s + 32);
  const uint32_t fca = Fc(dev, s);
  const uint32_t slot = dev + 4u * (s + 3138u);
  // Every input first (nothing below writes what is read here).
  const uint32_t tex = Ld32(base, t + 8);
  const uint32_t min_v = Ld32(base, p + 4), mag_v = Ld32(base, p + 8), p12 = Ld32(base, p + 12);
  const uint32_t p16 = Ld32(base, p + 16), p20 = Ld32(base, p + 20), p24 = Ld32(base, p + 24);
  const uint32_t old = Ld32(base, slot);
  const uint32_t fence = old ? Ld32(base, dev + 10908) : 0u;
  const uint64_t dirty = Ld64(base, dev + 24) | m;
  uint32_t fc[6];
  for (uint32_t i = 0; i < 6; ++i) fc[i] = Ld32(base, fca + 4u * i);
  const bool set = SetTexture(base, dev, s, tex, fc);
  Filter<false>(base, dev, s, min_v, fc[3], fc[4]);
  Filter<true>(base, dev, s, mag_v, fc[3], fc[4]);
  fc[3] = Rlwimi(fc[3], p12, 23, 7, 8);
  fc[0] = Rlwimi(fc[0], p16, 10, 19, 21);
  fc[0] = Rlwimi(fc[0], p20, 13, 16, 18);
  fc[0] = Rlwimi(fc[0], p24, 16, 13, 15);
  fc[5] |= 24u;
  // The stores (words 1 and 2 only with a texture, as the original).
  Put32(base, fca + 0, fc[0]);
  if (set) {
    Put32(base, fca + 4, fc[1]);
    Put32(base, fca + 8, fc[2]);
  }
  Put32(base, fca + 12, fc[3]);
  Put32(base, fca + 16, fc[4]);
  Put32(base, fca + 20, fc[5]);
  Put64(base, dev + 24, dirty);
  Put32(base, slot, tex);
  if (fence) Put32(base, old + 8, fence);
  MarkCards(base, fca, 24);
  MarkCards(base, dev + 24, 8);
  MarkCards(base, slot, 4);
  if (fence) MarkCards(base, old + 8, 4);
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t* base, me::hot::Writes& w) {
  const uint32_t s = ctx.r3.u32;
  const uint32_t dev = Ld32(base, kDevicePtr);
  if (s >= 32 || !dev) return;
  w.Add(Fc(dev, s), 24);
  w.Add(dev + 24, 8);
  w.Add(dev + 4u * (s + 3138u), 4);
  const uint32_t old = Ld32(base, dev + 4u * (s + 3138u));
  if (old) w.Add(old + 8, 4);
}

}  // namespace me::hot::n_827DB288
