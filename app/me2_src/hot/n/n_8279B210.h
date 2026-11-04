// ME2 (2026-10-31): native LZX for UE3's appDecompressMemory (sub_8279B210), COMPRESS_LZX blocks only.
//
//   r3 = flags (flags & 15: 1 zlib, 2 LZO -> sub_827EBC58, 4 LZX), r4 = dst, r5 = dst size, r6 = src, r7 = src size.
//   Returns r3 = 1 when the block decompressed to exactly dst size bytes, else 0.
//
// The original's LZX path: XMemCreateDecompressionContext(XMEMCODEC_LZX = 1, default parameters, 0, &ctx)
// (sub_82C47828, a guest heap allocation of the 128 KB window and the tables), XMemDecompress(ctx, dst, &size, src,
// src size - 4) (sub_82C477B0 -> sub_82C4DA50 -> sub_82C55550 ... the recompiled LZX decoder, Huffman stage
// sub_82C5C850: the hottest function of the loading-heavy PGO profiles, 7.4 %), XMemDestroyDecompressionContext
// (sub_82C477D8); success = HRESULT >= 0 and size == dst size. A fresh context per call: nothing carries over.
//
// Input layout (XMemCompress native format, checked on 3,905 blocks of Textures.tfc offline: all decoded to their exact
// sizes): frames of [0xFF, u16 uncompressed, u16 compressed] (a short frame) or [u16 compressed] (a full 32 KB frame),
// big-endian, each followed by its LZX bits; a frame with compressed size 0 ends the data (zero padding follows).
// The frames' LZX bits, concatenated, are one LZX stream (window 2^17, XMem's default; no reset interval), which
// libmspack's lzxd decodes (it re-aligns to 16 bits at every 32 KB frame as XMem does).
//
// The native declines (the original runs) for anything else: another codec, a frame sum that is not the dst size, an
// lzxd error, a range that is not contiguous host memory. It decodes into a per-thread buffer and copies to the guest
// only on success, so a decline has written nothing. No guest heap allocation (the original's create / destroy).
// Register state: r3 the only result (the caller tests it); r4-r12 scratch; nothing else is touched.
#pragma once

#include "../me_hot_common.h"

namespace me::hot::n_8279B210 {

// Only r3: the original leaves f1 / v1 to whatever the codec left there.
inline constexpr Cmp kCmp = {R(3), 0, 0};

inline constexpr uint32_t kMaxBlock = 1u << 20;  // UE3 blocks are 128 KB; larger ones are left to the original

namespace detail {

// lzxd's allocations (its state, the window, the input buffer) from three per-thread slots reused call after call:
// no heap traffic per block (the original allocates ~170 KB of guest heap per block, libmspack would of host heap).
struct Arena {
  void* slot[4] = {};
  size_t cap[4] = {};
  int next = 0;
};
inline Arena& ThreadArena() {
  static thread_local Arena a;
  return a;
}
inline void* ArenaAlloc(::mspack_system*, size_t n) {
  Arena& a = ThreadArena();
  if (a.next >= 4) return nullptr;
  const int i = a.next++;
  if (a.cap[i] < n) {
    std::free(a.slot[i]);
    a.slot[i] = std::malloc(n);
    a.cap[i] = a.slot[i] ? n : 0;
    if (!a.slot[i]) return nullptr;
  }
  std::memset(a.slot[i], 0, n);  // calloc semantics, as the SDK's mspack_memory_alloc
  return a.slot[i];
}
inline void ArenaFree(void*) {}
inline void ArenaCopy(void* src, void* dst, size_t n) { std::memcpy(dst, src, n); }

struct MemFile {
  ::mspack_system sys;  // first member: lzxd hands the file back as an mspack_file *
  const uint8_t* in = nullptr;
  uint8_t* out = nullptr;
  size_t size = 0, off = 0;
};
inline int FileRead(::mspack_file* f, void* buf, int n) {
  auto* m = reinterpret_cast<MemFile*>(f);
  const size_t t = std::min<size_t>(size_t(n), m->size - m->off);
  std::memcpy(buf, m->in + m->off, t);
  m->off += t;
  return int(t);
}
inline int FileWrite(::mspack_file* f, void* buf, int n) {
  auto* m = reinterpret_cast<MemFile*>(f);
  const size_t t = std::min<size_t>(size_t(n), m->size - m->off);
  std::memcpy(m->out + m->off, buf, t);
  m->off += t;
  return int(t);
}

struct Buffers {
  uint8_t* stream = nullptr;  // the frames' LZX bits, concatenated
  size_t stream_cap = 0;
  uint8_t* out = nullptr;     // the decoded block (copied to the guest on success)
  size_t out_cap = 0;
};
inline Buffers& ThreadBuffers() {
  static thread_local Buffers b;
  return b;
}
inline bool Reserve(uint8_t*& p, size_t& cap, size_t n) {
  if (cap >= n) return true;
  std::free(p);
  p = static_cast<uint8_t*>(std::malloc(n));
  cap = p ? n : 0;
  return p != nullptr;
}

// XMemCompress frames -> one LZX stream in b.stream; returns its length and the frames' uncompressed total.
inline bool StripFrames(const uint8_t* src, uint32_t len, Buffers& b, size_t& stream_len, uint64_t& total) {
  if (!Reserve(b.stream, b.stream_cap, len)) return false;
  stream_len = 0;
  total = 0;
  uint32_t i = 0;
  while (i + 2 <= len) {
    uint32_t u = 32768, c;
    if (src[i] == 0xFF) {
      if (i + 5 > len) return false;
      u = (uint32_t(src[i + 1]) << 8) | src[i + 2];
      c = (uint32_t(src[i + 3]) << 8) | src[i + 4];
      i += 5;
    } else {
      c = (uint32_t(src[i]) << 8) | src[i + 1];
      i += 2;
    }
    if (c == 0) break;  // the end marker
    if (uint64_t(i) + c > len) return false;
    std::memcpy(b.stream + stream_len, src + i, c);
    stream_len += c;
    i += c;
    total += u;
  }
  return stream_len != 0;
}

inline bool Decode(const uint8_t* stream, size_t stream_len, uint8_t* out, size_t out_len) {
  ::mspack_system sys{};
  sys.read = &FileRead;
  sys.write = &FileWrite;
  sys.alloc = &ArenaAlloc;
  sys.free = &ArenaFree;
  sys.copy = &ArenaCopy;
  MemFile in, dst;
  in.sys = sys;
  in.in = stream;
  in.size = stream_len;
  dst.sys = sys;
  dst.out = out;
  dst.size = out_len;
  ThreadArena().next = 0;
  ::lzxd_stream* s = ::lzxd_init(&sys, reinterpret_cast<::mspack_file*>(&in), reinterpret_cast<::mspack_file*>(&dst),
                                 17, 0, 0x8000, ::off_t(out_len), 0);
  if (!s) return false;
  const int rc = ::lzxd_decompress(s, ::off_t(out_len));
  ::lzxd_free(s);
  return rc == MSPACK_ERR_OK && dst.off == out_len;
}

}  // namespace detail

inline bool Native(PPCContext& ctx, uint8_t* base) {
  if ((ctx.r3.u32 & 15) != 4) return false;  // zlib / LZO: the original (LZO has its own native inside)
  const uint32_t dst = ctx.r4.u32, dst_len = ctx.r5.u32, src = ctx.r6.u32, src_len = ctx.r7.u32;
  if (!src || !dst_len || dst_len > kMaxBlock || src_len < 4 + 2) return false;
  const uint32_t len = src_len - 4;  // as the original passes to XMemDecompress
  if (uint64_t(src) + len > 0x100000000ull || uint64_t(dst) + dst_len > 0x100000000ull) return false;
  if (PhysOff(src) != PhysOff(src + len - 1) || PhysOff(dst) != PhysOff(dst + dst_len - 1)) return false;
  detail::Buffers& b = detail::ThreadBuffers();
  size_t stream_len = 0;
  uint64_t total = 0;
  if (!detail::StripFrames(Raw(base, src), len, b, stream_len, total) || total != dst_len) return false;
  if (!detail::Reserve(b.out, b.out_cap, dst_len)) return false;
  if (!detail::Decode(b.stream, stream_len, b.out, dst_len)) return false;
  std::memcpy(StorePtr(base, dst, dst_len), b.out, dst_len);  // the guard's build: its private copy of the range
  MarkCards(base, dst, dst_len);
  ctx.r3.u64 = 1;
  return true;
}

inline void Writes(const PPCContext& ctx, const uint8_t*, me::hot::Writes& w) {
  if ((ctx.r3.u32 & 15) != 4 || ctx.r5.u32 > kMaxBlock) return;
  w.Add(ctx.r4.u32, ctx.r5.u32);
}

}  // namespace me::hot::n_8279B210
