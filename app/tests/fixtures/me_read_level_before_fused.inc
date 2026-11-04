// Frozen pre-optimization ReadLevel from render/textures.cpp for differential regression.
struct LevelSource {
  const uint8_t *base = nullptr;  // start of the level's storage (or its packed tail)
  uint32_t pitch_blocks = 0;      // blocks per row of the storage
  uint32_t height_blocks = 0;     // rows of blocks per depth slice (3D)
  uint32_t x_blocks = 0, y_blocks = 0, z_offset = 0;  // offset inside a packed tail
};

void ReadLevel(const TextureFormatInfo &info, const TextureFetch &fetch, const LevelSource &src,
               uint32_t blocks_w, uint32_t blocks_h, uint32_t depth, uint8_t *dst,
               uint32_t dst_row_pitch, uint32_t dst_slice_pitch) {
  const uint32_t gbpb = info.guest_block_bytes;
  const uint32_t hbpb = info.host_block_bytes;
  const uint32_t bpb_log2 = uint32_t(std::countr_zero(gbpb));
  const u32 endian = fetch.Endian();
  const bool is_3d = fetch.Dimension() == TextureDimension::Tex3D;
  const uint32_t run = is_3d ? 1 : TiledRunBlocks(bpb_log2);

  std::vector<uint8_t> row(size_t(blocks_w) * gbpb);
  // ME2 (2026-10-27, me2_switch_button_icons: app/me2_src/me2_button_icons.cpp): a small uncompressed 32-bit 2D level
  // is gathered untiled first (the guest's bytes, before the endian swap), so g_texture_replace can recognise it by
  // its content and give other texels in the same layout; then it is converted from that copy.
  if (g_texture_replace && !is_3d && depth == 1 && gbpb == 4 && info.block_size == 1 && blocks_w <= 128 &&
      blocks_h <= 128) {
    std::vector<uint8_t> raw(size_t(blocks_w) * blocks_h * 4);
    for (uint32_t y = 0; y < blocks_h; ++y) {
      const uint32_t sy = y + src.y_blocks;
      uint8_t *line = raw.data() + size_t(y) * blocks_w * 4;
      if (fetch.Tiled()) {
        for (uint32_t x = 0; x < blocks_w;) {
          const uint32_t sx = x + src.x_blocks;
          const uint32_t count = std::min(run - sx % run, blocks_w - x);
          std::memcpy(line + size_t(x) * 4, src.base + TiledOffset2D(sx, sy, src.pitch_blocks, bpb_log2),
                      size_t(count) * 4);
          x += count;
        }
      } else {
        std::memcpy(line, src.base + (size_t(sy) * src.pitch_blocks + src.x_blocks) * 4, size_t(blocks_w) * 4);
      }
    }
    g_texture_replace(blocks_w, blocks_h, raw.data());
    for (uint32_t y = 0; y < blocks_h; ++y) {
      std::memcpy(row.data(), raw.data() + size_t(y) * blocks_w * 4, row.size());
      SwapEndian(endian, row.data(), row.data(), row.size());
      uint8_t *out = dst + size_t(y) * dst_row_pitch;
      if (info.conversion == TexelConversion::Copy)
        std::memcpy(out, row.data(), size_t(blocks_w) * hbpb);
      else
        ConvertTexels(info.conversion, row.data(), out, blocks_w);
    }
    return;
  }
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < blocks_h; ++y) {
      const uint32_t sy = y + src.y_blocks;
      const uint32_t sz = z + src.z_offset;
      if (fetch.Tiled()) {
        for (uint32_t x = 0; x < blocks_w;) {
          const uint32_t sx = x + src.x_blocks;
          const uint32_t offset =
              is_3d ? TiledOffset3D(sx, sy, sz, src.pitch_blocks, src.height_blocks, bpb_log2)
                    : TiledOffset2D(sx, sy, src.pitch_blocks, bpb_log2);
          const uint32_t count = std::min(run - sx % run, blocks_w - x);
          std::memcpy(row.data() + size_t(x) * gbpb, src.base + offset, size_t(count) * gbpb);
          x += count;
        }
      } else {
        const size_t offset = (size_t(sz) * src.height_blocks + sy) * src.pitch_blocks * gbpb +
                              size_t(src.x_blocks) * gbpb;
        std::memcpy(row.data(), src.base + offset, row.size());
      }
      SwapEndian(endian, row.data(), row.data(), row.size());
      uint8_t *out = dst + size_t(z) * dst_slice_pitch + size_t(y) * dst_row_pitch;
      if (info.conversion == TexelConversion::Copy)
        std::memcpy(out, row.data(), size_t(blocks_w) * hbpb);
      else if (info.decoded_blocks)
        DecodeBlockRow(info.conversion, row.data(), out, blocks_w, dst_row_pitch / info.block_size);
      else
        ConvertTexels(info.conversion, row.data(), out, blocks_w);
    }
  }
}
