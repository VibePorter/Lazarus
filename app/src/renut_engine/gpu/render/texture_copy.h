#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <simde/arm/neon.h>

namespace renut::gpu {

// Fused source-to-staging copy, like MGR's one-pass stream conversion: each byte
// is loaded and stored once. The caller supplies disjoint buffers and a multiple
// of four bytes, so byte-order groups never cross a tiled-run boundary.
template <uint32_t Endian>
inline void CopyTextureBytes(const uint8_t* source, uint8_t* target, size_t bytes) {
  static_assert(Endian < 4);
  if constexpr (Endian == 0) {
    std::memcpy(target, source, bytes);
    return;
  }
  size_t i = 0;
  for (; bytes - i >= 16; i += 16) {
    auto v = simde_vld1q_u8(source + i);
    if constexpr (Endian == 1) v = simde_vrev16q_u8(v);
    if constexpr (Endian == 2) v = simde_vrev32q_u8(v);
    if constexpr (Endian == 3)
      v = simde_vreinterpretq_u8_u16(simde_vrev32q_u16(simde_vreinterpretq_u16_u8(v)));
    simde_vst1q_u8(target + i, v);
  }
  for (; i < bytes; i += 4) {
    uint32_t v;
    std::memcpy(&v, source + i, 4);
    if constexpr (Endian == 1) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if constexpr (Endian == 2) v = __builtin_bswap32(v);
    if constexpr (Endian == 3) v = (v >> 16) | (v << 16);
    std::memcpy(target + i, &v, 4);
  }
}

}  // namespace renut::gpu
