/**
 * @file    gpu/render/textures.cpp
 */
#include "renut_engine/gpu/render/textures.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include <rex/cvar.h>

#include "renut_engine/gpu/banjo_renut_tuning.h"

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/render/graphics_options.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/texture_copy.h"
#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/backend/video.h"

// me2-nx-deko (2026-10-27): with the final pass at a finer scale than the scene (me2_renut_ui_height_*), its tonemap
// reads the HDR scene through FSR 1 EASU instead of stretching it bilinearly.
REXCVAR_DEFINE_BOOL(me2_renut_ui_fsr, true, "ME2/Renderer",
                    "With the final pass (tonemap, HUD) finer than the scene (me2_renut_ui_height_*): upscale the scene "
                    "for it with AMD FSR 1 EASU instead of a bilinear stretch. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-10-27): FXAA on that scene at its own resolution, before EASU (AMD: EASU wants anti-aliased input).
REXCVAR_DEFINE_BOOL(me2_renut_fxaa, true, "ME2/Renderer",
                    "With the final pass finer than the scene (me2_renut_ui_height_*): NVIDIA FXAA 3.11 on the scene "
                    "at its own resolution, before me2_renut_ui_fsr's upscale. Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-10-27): ME2's colour textures have the Xenos "gamma" sign on X, Y and Z (fetch constants GGGU in a
// Xenia capture of the main menu: DXT1 / DXT4_5 / 8_8_8_8). Read as plain unorm they came out too bright and washed
// out (the menu's screens, the whole lit scene).
REXCVAR_DEFINE_BOOL(me2_renut_texture_gamma, true, "ME2/Renderer",
                    "Textures whose X, Y and Z have the Xenos gamma sign are sampled through the sRGB curve (sRGB "
                    "host formats); off = read as linear unorm (the old behaviour). Read at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(me2_renut_resolve_pool_mb, 32, "ME2/Renderer",
                     "Deko3d color resolve allocations kept for scene/HUD scale alternation; actual image bytes, "
                     "up to 8 spare targets, idle for at most 120 frames. Zero disables reuse. Read at start")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// me2-nx-deko (2026-10-31): the console logs of 10-31 (run 3) had 33 of the 290 frames over 70 ms in bursts of new
// textures (median 145 ms, up to 1.2 s): every texture a draw binds for the first time is created, hashed, untiled,
// swapped and converted on Renut's render thread inside that draw, all of its levels at once. With a budget, a large
// new 2D texture gets its small levels at once and its large ones over the next frames, at most this many KB of
// staging a frame (smallest first; the sampler clamps to what it has, as UE3's own streaming shows a texture's low
// mips while its high ones load). Small, single-level, 3D / cube / stacked, CPU-written and resolved textures, and
// content changes of an uploaded one, are uploaded whole as before.
REXCVAR_DEFINE_INT32(me2_renut_upload_budget_kb, 0, "ME2/Renderer",
                     "Texture uploads a frame for the levels of large new textures (KB of staging); the rest waits for "
                     "the next frames, smallest levels first (sampled from the levels already there). 0 = whole "
                     "textures at once (the old way)")
    .range(0, 1 << 20)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_renut_upload_stream_min_kb, 256, "ME2/Renderer",
                     "me2_renut_upload_budget_kb: textures smaller than this are always uploaded whole")
    .range(0, 1 << 20)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Texture upload work on Renut's render thread (hash + untile + convert + copy recording), us since the start
// (me2_frame_diag.cpp's spike lines take the difference per frame).
std::atomic<uint64_t> g_me2_texture_work_us{0};

namespace renut::gpu {

namespace {

// Fetch constant bits that describe the texture data (not sampling).
constexpr u32 kDescMask[6] = {
    0xFFC003FFu,  // type, signs, pitch, tiled (not clamp modes)
    0xFFFFFFFFu,  // format, endian, stacked, base address
    0xFFFFFFFFu,  // size
    0x0007FFFFu,  // number format, swizzle, exponent adjust (not filters)
    0x000003FCu,  // mip min / max level
    0xFFFFFE00u,  // dimension, packed mips, mip address (not border color)
};

constexpr uint32_t kMaxTextureSize = 8192;

std::unordered_map<u32, std::unique_ptr<HostTexture>> g_textures;

// Recent g_textures entries by VA: BindTextureLocked looks up every texture
// of every draw. The map's nodes stay put until erased (ReleaseHostTextureLocked
// drops the entry here first).
struct TextureMemo {
  u32 va = 0;
  std::unique_ptr<HostTexture> *slot = nullptr;
};
constexpr u32 kTextureMemoBits = 10;
TextureMemo g_texture_memo[1u << kTextureMemoBits];

TextureMemo &TextureMemoFor(u32 texture_va) {
  return g_texture_memo[(texture_va * 0x9E3779B1u) >> (32 - kTextureMemoBits)];
}

// g_textures[texture_va], through the memo.
std::unique_ptr<HostTexture> &TextureSlot(u32 texture_va) {
  TextureMemo &memo = TextureMemoFor(texture_va);
  if (memo.slot && memo.va == texture_va)
    return *memo.slot;
  auto &slot = g_textures[texture_va];
  memo = {texture_va, &slot};
  return slot;
}
// Depth resolve destinations with guest words, by base address (4 KB pages).
std::unordered_map<u32, HostTexture *> g_depth_words;
// The last resolve destination into each base address (4 KB pages): D3D
// textures are views of memory, and a game may resolve through one header and
// sample the same memory through another.
std::unordered_map<u32, HostTexture *> g_resolve_targets;
std::unordered_map<uint64_t, uint32_t> g_samplers;  // sampler key -> slot
std::vector<std::unique_ptr<plume::RenderSampler>> g_sampler_objects;
uint64_t g_frame = 1;

// Resources D3D::UnlockResource was called on since the last bind that
// looked (NoteTextureUnlocked; any thread, hence the lock of its own).
std::mutex g_unlocked_mutex;
std::vector<u32> g_unlocked;

// The bindless table filling up (past kEvictPressureSlots of its slots), or
// video memory nearing the budget the OS gives the process (past
// kEvictMemoryShare of it): host textures not bound or resolved into for
// kEvictIdleFrames go, checked every kEvictCheckFrames, and are made again
// from guest memory if bound later. Resolve destinations hold what only the
// GPU has and wait kEvictIdleGpuFrames. The section pools' textures normally
// go with their memory (ReleaseHostTexturesInMemoryLocked); this catches
// whatever else piles up: at 4K, resolve destinations of levels played
// earlier (2.2 GB of them after a few levels, 2026-10-15), while past the
// budget the OS moves allocations out to system memory and the frame rate
// sinks until it frees them.
constexpr uint64_t kEvictCheckFrames = 120;
constexpr uint64_t kEvictIdleFrames = 600;
constexpr uint64_t kEvictIdleGpuFrames = 3600;
constexpr uint32_t kEvictPressureSlots = kTextureDescriptorCount * 3 / 4;
constexpr double kEvictMemoryShare = 0.85;

// Linear textures (see BindTextureLocked): how often a lock-driven texture
// is checked for writes that went around an unlock, and a dynamic one for
// having stopped changing; how long after an unlock the content is watched;
// and how many checks in a row must see a change to make a texture dynamic.
constexpr uint64_t kLinearVerifyFrames = 30;
constexpr uint64_t kUnlockWatchFrames = 8;
constexpr uint32_t kDynamicChanges = 2;

uint32_t NullSlot(TextureDimension dimension) {
  switch (dimension) {
  case TextureDimension::Tex3D:
    return kNullTexture3DSlot;
  case TextureDimension::Cube:
    return kNullTextureCubeSlot;
  default:
    return kNullTextureSlot;
  }
}

// Host dimension the shaders sample it as: stacked 2D arrays go through the
// 3D table, like tfetch3D reads them.
TextureDimension HostDimension(const TextureFetch &fetch) {
  if (fetch.Dimension() == TextureDimension::Tex2D && fetch.Stacked())
    return TextureDimension::Tex3D;
  if (fetch.Dimension() == TextureDimension::Tex1D)
    return TextureDimension::Tex2D;
  return fetch.Dimension();
}

// Host layers of a stacked texture: a 2D array view takes two at least.
uint32_t StackedLayers(const HostTexture &t) {
  return std::max(t.depth, 2u);
}

bool AllSigned(const TextureFetch &fetch) {
  for (u32 c = 0; c < 4; ++c) {
    if (fetch.Sign(c) != TextureSign::Signed)
      return false;
  }
  return true;
}

// me2-nx-deko (me2_renut_texture_gamma): the sRGB host format for a texture whose X, Y and Z (before the swizzle, the
// order the host texture keeps) are gamma; UNKNOWN when there is none (an sRGB format never touches W, as the
// Xenos's GGGU). The sRGB curve stands in for the Xenos's piecewise-linear gamma.
plume::RenderFormat GammaFormat(const TextureFetch &fetch, plume::RenderFormat host) {
  static const bool on = REXCVAR_GET(me2_renut_texture_gamma);
  if (!on)
    return plume::RenderFormat::UNKNOWN;
  for (u32 c = 0; c < 3; ++c) {
    if (fetch.Sign(c) != TextureSign::Gamma)
      return plume::RenderFormat::UNKNOWN;
  }
  switch (host) {
  case plume::RenderFormat::R8G8B8A8_UNORM:
    return plume::RenderFormat::R8G8B8A8_UNORM_SRGB;
  case plume::RenderFormat::BC1_UNORM:
    return plume::RenderFormat::BC1_UNORM_SRGB;
  case plume::RenderFormat::BC2_UNORM:
    return plume::RenderFormat::BC2_UNORM_SRGB;
  case plume::RenderFormat::BC3_UNORM:
    return plume::RenderFormat::BC3_UNORM_SRGB;
  default:
    return plume::RenderFormat::UNKNOWN;
  }
}

plume::RenderSwizzle ViewSwizzle(uint32_t component) {
  switch (component) {
  case 0:
    return plume::RenderSwizzle::R;
  case 1:
    return plume::RenderSwizzle::G;
  case 2:
    return plume::RenderSwizzle::B;
  case 3:
    return plume::RenderSwizzle::A;
  case 4:
    return plume::RenderSwizzle::ZERO;
  default:
    return plume::RenderSwizzle::ONE;
  }
}

// Host formats with a single component.
bool SingleComponent(plume::RenderFormat format) {
  using F = plume::RenderFormat;
  switch (format) {
  case F::R8_UNORM:
  case F::R8_SNORM:
  case F::R16_UNORM:
  case F::R16_SNORM:
  case F::R16_FLOAT:
  case F::R32_FLOAT:
  case F::R32_UINT:
  case F::BC4_UNORM:
  case F::BC4_SNORM:
    return true;
  default:
    return false;
  }
}

// The view mapping of a fetch constant swizzle: which stored component each
// shader component reads (0-3 XYZW, 4 zero, 5 one), with X and Z exchanged
// for content stored so (swap_red_blue, see Create). A one-component format
// fetches its value in every component: the game's depth restore
// (ps_beb4ddc38d3b2df5) writes the W of a depth resolve fetched with the
// identity swizzle as its depth, where a host view has 1.
plume::RenderComponentMapping SwizzleMapping(u32 swizzle, bool swap_red_blue,
                                             plume::RenderFormat format) {
  const bool single = SingleComponent(format);
  auto component = [&](uint32_t c) {
    uint32_t source = (swizzle >> (c * 3)) & 7;
    if (single && source < 4)
      source = 0;
    else if (swap_red_blue && (source == 0 || source == 2))
      source ^= 2;
    return ViewSwizzle(source);
  };
  return plume::RenderComponentMapping(component(0), component(1), component(2), component(3));
}

// The bindless slot of 't' seen through 'swizzle': its own view's when that is
// the swizzle it was made with, else a view made for it (kept with 't').
uint32_t SlotForSwizzle(HostTexture &t, u32 swizzle) {
  if (swizzle == t.view_swizzle || !t.texture)
    return t.slot;
  for (const HostTexture::SwizzleView &v : t.swizzle_views) {
    if (v.swizzle == swizzle)
      return v.slot;
  }
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.componentMapping = SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format);
  HostTexture::SwizzleView v;
  v.swizzle = swizzle;
  v.view = t.texture->createTextureView(desc);
  if (!v.view)
    return t.slot;
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    return t.slot;
  }
  Video::SetTextureSlotLocked(v.slot, t.texture.get(), v.view.get());
  t.swizzle_views.push_back(std::move(v));
  return t.swizzle_views.back().slot;
}

// Where a texture's guest data is depends only on the fetch constant bits in
// t.desc, so it is worked out once per texture.
void KnowGuestExtent(const TextureFetch &fetch, HostTexture &t) {
  if (!t.sample_known) {
    TextureGuestExtent(fetch, t.sample_pages, t.sample_sizes);
    t.sample_known = true;
  }
}

// The ends SampleHash reads of a region, and the spans between them.
constexpr uint32_t kSampleEndBytes = 256;
constexpr uint32_t kSampleProbes = 8;
constexpr uint32_t kSampleProbeBytes = 64;

// Cheap content fingerprint of a tiled texture: the first and last 256 bytes
// of the base level and of the mip chain, and spans spread through each. The
// game streams textures into memory another one used, with the same header
// (format, size, address): masks that are white at the edges (the hill's
// blend mask after the clearing's ring stencil) have the same ends, so the
// ends alone kept the old texture. Every bound texture is checked each frame
// and its guest memory is mostly out of the cache, so the cost is the lines
// read: eight spans, gathered and hashed at once. (32 spans hashed one at a
// time were a tenth of the render thread.)
uint64_t SampleHash(const TextureFetch &fetch, HostTexture &t) {
  KnowGuestExtent(fetch, t);
  alignas(64) uint8_t gathered[2 * (2 * kSampleEndBytes + kSampleProbes * kSampleProbeBytes)];
  uint32_t at = 0;
  for (u32 i = 0; i < 2; ++i) {
    const uint32_t page = t.sample_pages[i], size = t.sample_sizes[i];
    if (!page || !size)
      continue;
    const auto *data = GuestPhysicalPtr<const uint8_t>(page << 12);
    const uint32_t n = std::min<uint32_t>(size, kSampleEndBytes);
    std::memcpy(gathered + at, data, n);
    std::memcpy(gathered + at + n, data + size - n, n);
    at += 2 * n;
    if (size > 2 * n + kSampleProbeBytes) {
      const uint32_t span = size - 2 * n - kSampleProbeBytes;
      for (uint32_t p = 1; p <= kSampleProbes; ++p) {
        const uint32_t offset = n + uint32_t(uint64_t(span) * p / (kSampleProbes + 1));
        std::memcpy(gathered + at, data + offset, kSampleProbeBytes);
        at += kSampleProbeBytes;
      }
    }
  }
  return at ? banjo::renut_gpu::ContentHash64(gathered, at) : 0;  // banjo-nx: banjo_renut_pmull_hash
}

// Fingerprint of every guest byte of a texture, for the CPU-written linear
// ones, whose writes land anywhere: N&B's font cache (FontCache.cpp,
// sub_82B9FA68) inflates each new glyph into its cell of a 1920x1920 texture
// between D3DLineTexture_LockRect and D3DArrayTexture_UnlockRect.
uint64_t ContentHash(const TextureFetch &fetch, HostTexture &t) {
  KnowGuestExtent(fetch, t);
  uint64_t hash = 0;
  for (u32 i = 0; i < 2; ++i) {
    const uint32_t page = t.sample_pages[i], size = t.sample_sizes[i];
    if (page && size)
      hash = banjo::renut_gpu::ContentHash64(GuestPhysicalPtr<const uint8_t>(page << 12), size, hash);  // banjo-nx
  }
  return hash;
}

// banjo-nx (2026-10-19, render-thread proposal 1, banjo_renut_store_cards): whether no page of the texture's guest
// extent (both ranges; their cards are read and cleared) was written since interval 'proven'.
bool ExtentClean(HostTexture &t, uint64_t proven) {
  bool clean = proven != UINT64_MAX;
  for (u32 i = 0; i < 2; ++i) {
    if (t.sample_pages[i] && t.sample_sizes[i])
      clean &= GuestRangeCardsClean(GuestPhysicalPtr<const u8>(t.sample_pages[i] << 12), t.sample_sizes[i], proven);
  }
  return clean;
}

// The content check's hash (SampleHash of a tiled texture, ContentHash of a linear one), or the last one taken when
// the cards prove the bytes unchanged since: then the bytes are those hashed, and so would the hash be.
uint64_t CheckedHash(const TextureFetch &fetch, HostTexture &t, bool sampled) {
  KnowGuestExtent(fetch, t);
  const bool cards = banjo::renut_gpu::StoreCardsOn();
  if (cards && ExtentClean(t, t.hash_interval)) {
    t.hash_interval = CardsInterval();
    // The bytes the hash would have read: all of a linear texture; SampleHash's ends and spans of a tiled one.
    uint64_t bytes = 0;
    for (u32 i = 0; i < 2; ++i) {
      const uint32_t size = t.sample_pages[i] ? t.sample_sizes[i] : 0;
      if (!sampled) {
        bytes += size;
      } else if (size) {
        const uint32_t n = std::min<uint32_t>(size, kSampleEndBytes);
        bytes += 2 * n + (size > 2 * n + kSampleProbeBytes ? kSampleProbes * kSampleProbeBytes : 0);
      }
    }
    if (banjo::renut_gpu::StoreCardsVerifyOn()) {
      const uint64_t hash = sampled ? SampleHash(fetch, t) : ContentHash(fetch, t);
      if (hash != t.last_hash) {
        static uint32_t logged = 0;
        if (logged++ < 32)
          RGPU_WARN("[cards] texture {:08X} (physical {:08X}, {} bytes) changed under clean cards: a writer of guest "
                    "memory that marks none (banjo_renut_store_cards_verify)",
                    t.guest_va, t.sample_pages[0] << 12, bytes);
        banjo::renut_gpu::CountTextureCards(bytes, 3);
        t.last_hash = hash;
        return hash;
      }
    }
    banjo::renut_gpu::CountTextureCards(bytes, 0);
    return t.last_hash;
  }
  const uint64_t hash = sampled ? SampleHash(fetch, t) : ContentHash(fetch, t);
  t.last_hash = hash;
  t.hash_interval = cards ? CardsInterval() : UINT64_MAX;
  if (cards)
    banjo::renut_gpu::CountTextureCards(0, 1);
  return hash;
}

struct LevelSource {
  const uint8_t *base = nullptr;  // start of the level's storage (or its packed tail)
  uint32_t pitch_blocks = 0;      // blocks per row of the storage
  uint32_t height_blocks = 0;     // rows of blocks per depth slice (3D)
  uint32_t x_blocks = 0, y_blocks = 0, z_offset = 0;  // offset inside a packed tail
};

// Untiles/copies one subresource (level, slice) into 'dst' rows of host blocks.
// Tiled rows are copied a run of blocks at a time where the tiling keeps
// them together, as the game's UntileSurface does.
}  // namespace

// ME2 (2026-10-27): set by the game's side (app/me2_src/me2_button_icons.cpp); see ReadLevel.
bool (*g_texture_replace)(uint32_t width, uint32_t height, uint8_t *guest_texels) = nullptr;
// me2-nx-deko (2026-11-06): the HD levels of the icon the last g_texture_replace hit gave on this thread (scale code,
// level 0's size, level count, the levels' texels in the guest's layout); false: none (HostTexture::hd_data).
bool (*g_texture_replace_hd)(uint32_t *scale_code, uint32_t *size, uint32_t *levels, const uint8_t **data) = nullptr;
// me2-nx-deko (2026-11-06): set by the game's side with g_texture_replace_hd (me2_hd_button_icons).
bool (*g_hd_icons_on)() = nullptr;
bool HdIconsOn() { return g_hd_icons_on && g_hd_icons_on(); }

namespace {

// Direct detile + endian conversion for the common 32-bit / DXT copy formats.
// No row scratch allocation or second staging copy while an area streams in.
template <uint32_t Endian>
void ReadCopy2D(const TextureFetch& fetch, const LevelSource& src, uint32_t blocks_w,
                uint32_t blocks_h, uint32_t gbpb, uint32_t bpb_log2, uint8_t* dst, uint32_t row_pitch) {
  const uint32_t run = TiledRunBlocks(bpb_log2);
  for (uint32_t y = 0; y < blocks_h; ++y) {
    const uint32_t sy = y + src.y_blocks;
    uint8_t* out = dst + size_t(y) * row_pitch;
    if (!fetch.Tiled()) {
      CopyTextureBytes<Endian>(src.base + ((size_t(src.z_offset) * src.height_blocks + sy) *
                                           src.pitch_blocks + src.x_blocks) * gbpb,
                               out, size_t(blocks_w) * gbpb);
      continue;
    }
    for (uint32_t x = 0; x < blocks_w;) {
      const uint32_t sx = x + src.x_blocks;
      const uint32_t count = std::min(run - sx % run, blocks_w - x);
      CopyTextureBytes<Endian>(src.base + TiledOffset2D(sx, sy, src.pitch_blocks, bpb_log2),
                               out + size_t(x) * gbpb, size_t(count) * gbpb);
      x += count;
    }
  }
}

// me2-nx-deko (2026-11-06): set when ReadLevel gave a level from g_texture_replace (HostTexture::replaced_icon).
bool g_level_replaced = false;

void ReadLevel(const TextureFormatInfo &info, const TextureFetch &fetch, const LevelSource &src,
               uint32_t blocks_w, uint32_t blocks_h, uint32_t depth, uint8_t *dst,
               uint32_t dst_row_pitch, uint32_t dst_slice_pitch) {
  const uint32_t gbpb = info.guest_block_bytes;
  const uint32_t hbpb = info.host_block_bytes;
  const uint32_t bpb_log2 = uint32_t(std::countr_zero(gbpb));
  const u32 endian = fetch.Endian();
  const bool is_3d = fetch.Dimension() == TextureDimension::Tex3D;
  const uint32_t run = is_3d ? 1 : TiledRunBlocks(bpb_log2);

  // ME2 (2026-10-27, me2_switch_button_icons: app/me2_src/me2_button_icons.cpp): a small uncompressed 32-bit 2D level
  // is gathered untiled first (the guest's bytes, before the endian swap), so g_texture_replace can recognise it by
  // its content and give other texels in the same layout; then it is converted from that copy.
  if (g_texture_replace && !is_3d && depth == 1 && gbpb == 4 && info.block_size == 1 && blocks_w <= 128 &&
      blocks_h <= 128) {
    std::vector<uint8_t> row(size_t(blocks_w) * gbpb);
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
    if (g_texture_replace(blocks_w, blocks_h, raw.data()))
      g_level_replaced = true;
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
  if (!is_3d && depth == 1 && info.conversion == TexelConversion::Copy && gbpb == hbpb &&
      gbpb >= 4 && gbpb <= 16 && std::has_single_bit(gbpb)) {
    // Every block is a whole endian group; smaller formats and conversions retain
    // the original row path, including its handling of partial final groups.
    switch (endian & 3) {
      case 0: ReadCopy2D<0>(fetch, src, blocks_w, blocks_h, gbpb, bpb_log2, dst, dst_row_pitch); break;
      case 1: ReadCopy2D<1>(fetch, src, blocks_w, blocks_h, gbpb, bpb_log2, dst, dst_row_pitch); break;
      case 2: ReadCopy2D<2>(fetch, src, blocks_w, blocks_h, gbpb, bpb_log2, dst, dst_row_pitch); break;
      case 3: ReadCopy2D<3>(fetch, src, blocks_w, blocks_h, gbpb, bpb_log2, dst, dst_row_pitch); break;
    }
    return;
  }
  std::vector<uint8_t> row(size_t(blocks_w) * gbpb);
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

// Staging data read from guest memory at submission rather than at the draw,
// the way the GPU reads a texture when it executes (see BindTextureLocked).
struct PendingFill {
  u32 texture_va;  // me2-nx-deko (2026-11-06): its host texture, for HostTexture::replaced_icon
  TextureFormatInfo info;
  TextureFetch fetch;
  LevelSource src;
  uint32_t blocks_w, blocks_h, depth;
  uint8_t *dst;
  uint32_t row_pitch, slice_pitch;
};
std::vector<PendingFill> g_pending_fills;

// Records the copies of every level into 't'. The guest data is read now, or
// when the command list is submitted if 'defer_read'.
bool Upload(HostTexture &t, const TextureFetch &fetch, bool defer_read, uint32_t first_level = 0,
            uint32_t end_level = UINT32_MAX) {
  // banjo-nx (2026-10-19, render-thread proposal 1): the interval the guest bytes are read in: now, or at the next
  // kick's flush (FlushTextureFillsLocked, right after FlushVertexFillsLocked starts a new interval; a later one is
  // only more cautious).
  t.read_interval = CardsInterval() + (defer_read ? 1 : 0);
  // banjo-nx (2026-10-19, render-thread proposal 3): no g_texture_epoch bump: an upload changes content, not a binding.
  t.resolve_signature = 0;  // banjo-nx (banjo_renut_resolve_skip): not what a resolve left any more
  u32 block_width, block_height;
  FormatBlockSize(fetch.Format(), block_width, block_height);

  auto &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  static const uint32_t kProfileLabel = gpu_profile::Label("texture uploads");
  gpu_profile::Mark(s, kProfileLabel);
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  t.layout = plume::RenderTextureLayout::COPY_DEST;

  const bool is_3d_data = fetch.Dimension() == TextureDimension::Tex3D;
  // Host array layers (cube faces, stacked layers); a stacked texture's extra
  // layer (StackedLayers) repeats its last one.
  const uint32_t slices = is_3d_data ? 1 : t.stacked ? StackedLayers(t) : t.depth;
  const uint32_t bs = t.info.block_size;
  for (uint32_t level = first_level; level < std::min(end_level, t.mip_levels); ++level) {
    const uint32_t w = std::max(1u, t.width >> level);
    const uint32_t h = std::max(1u, t.height >> level);
    const uint32_t d = is_3d_data ? std::max(1u, t.depth >> level) : 1;
    const uint32_t blocks_w = (w + bs - 1) / bs;
    const uint32_t blocks_h = (h + bs - 1) / bs;

    // Where the level lives: base or mip memory, possibly inside the packed tail.
    const TextureLevelLayout layout = GetTextureLevelLayout(fetch, level);
    const uint32_t page = layout.page;
    if (!page)
      continue;
    LevelSource src;
    src.pitch_blocks = layout.pitch_texels / block_width;
    src.height_blocks = layout.height / block_height;
    src.x_blocks = layout.tail_x_blocks;
    src.y_blocks = layout.tail_y_blocks;
    src.z_offset = layout.tail_z;
    const uint32_t level_offset = layout.offset;

    const uint32_t row_bytes = blocks_w * t.info.host_block_bytes;
    // Footprint rows are 256-byte aligned: rows of blocks, or for decoded
    // blocks each of the block_size texel rows a row of blocks holds.
    const uint32_t row_align = t.info.decoded_blocks ? 256u * bs : 256u;
    const uint32_t row_pitch = (row_bytes + row_align - 1) / row_align * row_align;
    const uint32_t slice_pitch = row_pitch * blocks_h;
    for (uint32_t slice = 0; slice < slices; ++slice) {
      const uint32_t guest_slice = std::min(slice, t.depth - 1);
      src.base = GuestPhysicalPtr<const uint8_t>((page << 12) + level_offset +
                                                 guest_slice * layout.slice_bytes);
      UploadAllocation staging = s.uploads[s.frame].Allocate(uint64_t(slice_pitch) * d, 512);
      if (!staging)
        return false;
      auto *dst = static_cast<uint8_t *>(staging.cpu);
      if (defer_read) {
        g_pending_fills.push_back({t.guest_va, t.info, fetch, src, blocks_w, blocks_h, d, dst, row_pitch, slice_pitch});
      } else {
        g_level_replaced = false;
        ReadLevel(t.info, fetch, src, blocks_w, blocks_h, d, dst, row_pitch, slice_pitch);
        if (g_level_replaced) {
          t.replaced_icon = true;
          // me2-nx-deko (2026-11-06): its HD levels, for UpgradeToHdIcon after the upload.
          uint32_t code = 0;
          if (level == 0 && g_texture_replace_hd &&
              g_texture_replace_hd(&code, &t.hd_size, &t.hd_count, t.hd_data) && t.hd_count >= 2 &&
              t.hd_count <= 4 && code < RenderScale::kCount)
            t.hd_scale = RenderScale{code};
          else
            t.hd_count = 0;
        }
      }
      // Footprints are in texels, block-aligned for compressed formats. Decoded
      // blocks are plain texels, so a level smaller than a block (a small mip)
      // takes only its own part of the decoded block.
      const uint32_t copy_w = std::min(blocks_w * bs, std::max(1u, t.alloc_width >> level));
      const uint32_t copy_h = std::min(blocks_h * bs, std::max(1u, t.alloc_height >> level));
      const plume::RenderBox level_box(0, 0, int32_t(copy_w), int32_t(copy_h), 0, int32_t(d));
      // plume's Vulkan copy takes its extent from the footprint and ignores the
      // box, and Vulkan allows no extent past the level (a compressed one ends
      // at the level's edge instead of a block's): the footprint is the level's
      // part there. D3D12 wants block-aligned footprints, cut by the box.
      const bool vulkan = UsesVulkanBindings();  // banjo-nx: deko3d copies as Vulkan does (the level's extent)
      const uint32_t row_texels = row_pitch / t.info.host_block_bytes * bs;
      if (vulkan && d > 1 && copy_h != blocks_h * bs) {
        // The footprint's height is also its rows per depth slice there, which
        // the staging has blocks_h * bs of: one depth slice at a time.
        for (uint32_t z = 0; z < d; ++z) {
          cmd->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
              plume::RenderTextureCopyLocation::PlacedFootprint(
                  staging.ref.ref, t.info.host, copy_w, copy_h, 1, row_texels,
                  staging.ref.offset + uint64_t(z) * slice_pitch),
              0, 0, z, nullptr);
        }
      } else {
        cmd->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(t.texture.get(), level, slice),
            plume::RenderTextureCopyLocation::PlacedFootprint(
                staging.ref.ref, t.info.host, vulkan ? copy_w : blocks_w * bs,
                vulkan ? copy_h : blocks_h * bs, d, row_texels, staging.ref.offset),
            0, 0, 0, !vulkan && t.info.decoded_blocks ? &level_box : nullptr);
      }
    }
  }
  return true;
}

// The host levels a scaled resolve destination gets (HostTexture::extra_levels):
// down to a texel of at least the guest's.
uint32_t ExtraLevels(RenderScale scale) {
  const float factor = scale.Factor();
  return factor > 4.0f ? 3 : factor > 2.0f ? 2 : factor > 1.0f ? 1 : 0;
}

// swap_red_blue: the content was stored with X and Z exchanged (a guest
// resolve does that when the destination's X swizzle is not X). 'scale': see
// HostTexture::scale (content the GPU writes only). host_levels: a color
// resolve destination, which gets HostTexture::extra_levels when it is a
// scaled 2D one.
bool Create(HostTexture &t, const TextureFetch &fetch, bool render_target,
            plume::RenderFormat rt_format, bool swap_red_blue = false, RenderScale scale = {},
            bool host_levels = false) {
  ++g_texture_epoch;  // banjo-nx (CPU proposal 2)
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  t.dimension = HostDimension(fetch);
  t.guest_format = fetch.Format();
  t.info = GetTextureFormatInfo(fetch.Format(), AllSigned(fetch));
  if (!render_target) {
    const plume::RenderFormat gamma = GammaFormat(fetch, t.info.host);
    if (gamma != plume::RenderFormat::UNKNOWN)
      t.info.host = gamma;
  }
  t.width = width;
  t.height = height;
  t.depth = depth;
  t.scale = scale;
  // No more levels than the size has (GetMaxMipLevels, 0x82654F20).
  const u32 largest = std::max({width, height, fetch.Dimension() == TextureDimension::Tex3D ? depth : 1u});
  const u32 size_levels = 32 - u32(std::countl_zero(largest));
  t.mip_levels = render_target ? 1 : std::min(fetch.MaxMipLevel() + 1, size_levels);
  t.render_target = render_target;
  plume::RenderFormat format = render_target ? rt_format : t.info.host;
  if (format == plume::RenderFormat::UNKNOWN)
    return false;

  plume::RenderDevice *device = Video::Device();
  plume::RenderTextureDesc desc;
  plume::RenderTextureViewDesc view_desc;
  const uint32_t bs = render_target ? 1 : t.info.block_size;
  const uint32_t w = scale.Size((t.width + bs - 1) / bs * bs);
  const uint32_t h = scale.Size((t.height + bs - 1) / bs * bs);
  t.extra_levels = 0;
  if (render_target && host_levels && t.dimension == TextureDimension::Tex2D &&
      State().texture_mip_ps) {
    // No more than the host size has.
    const uint32_t host_levels_max = 32 - uint32_t(std::countl_zero(std::max(w, h)));
    t.extra_levels = std::min(ExtraLevels(scale), host_levels_max - 1);
    t.mip_levels += t.extra_levels;
  }
  t.stacked = fetch.Dimension() == TextureDimension::Tex2D && fetch.Stacked();
  switch (t.dimension) {
  case TextureDimension::Tex3D:
    if (t.stacked) {
      // Viewed as a 2D array, which takes two layers at least (StackedLayers).
      desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, w, h, 1,
                                               t.mip_levels, StackedLayers(t), format);
      view_desc = plume::RenderTextureViewDesc::Texture2D(format);
    } else {
      desc = plume::RenderTextureDesc::Texture3D(w, h, t.depth, t.mip_levels, format);
      view_desc = plume::RenderTextureViewDesc::Texture3D(format);
    }
    break;
  case TextureDimension::Cube:
    desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, w, h, 1,
                                             t.mip_levels, 6, format,
                                             plume::RenderTextureFlag::CUBE);
    view_desc = plume::RenderTextureViewDesc::TextureCube(format);
    t.depth = 6;
    break;
  default:
    desc = plume::RenderTextureDesc::Texture2D(w, h, t.mip_levels, format);
    view_desc = plume::RenderTextureViewDesc::Texture2D(format);
    t.depth = 1;
    break;
  }
  if (render_target)
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  t.host_format = format;
  t.alloc_width = w;
  t.alloc_height = h;
  // Beyond what the backends create (plume returns an object without a
  // resource then); Xenos textures are at most 8192 wide/high, and the
  // backends' limit is 16384.
  const uint32_t max_size = scale.Factor() > 1.0f ? kMaxTextureSize * 2 : kMaxTextureSize;
  if (w > max_size || h > max_size || t.depth > kMaxTextureSize)
    return false;
  t.texture = device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    ReportDeviceLost(device);
    return false;
  }
  t.texture->setName("renut texture");

  view_desc.mipLevels = t.mip_levels;
  view_desc.componentMapping = SwizzleMapping(fetch.Swizzle(), swap_red_blue, format);
  t.view_desc = view_desc;
  t.view_swizzle = fetch.Swizzle();
  t.view_swap_red_blue = swap_red_blue;
  t.view = t.texture->createTextureView(view_desc);
  if (!t.view)
    return false;
  t.layout = plume::RenderTextureLayout::UNKNOWN;
  t.slot = Video::AllocateTextureSlotLocked();
  if (t.slot == kInvalidDescriptor)
    return false;
  Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
  return true;
}

void MaskedDesc(const TextureFetch &fetch, u32 out[6]) {
  for (int i = 0; i < 6; ++i)
    out[i] = fetch.dword[i] & kDescMask[i];
}

void Retire(HostTexture &t);

// me2-nx-deko (2026-11-06, user: docked at 1080p the button icons look low-resolution): an icon g_texture_replace
// gave (HostTexture::hd_data) is held at its HD size with mip levels down to the game's size, and sampled with that
// many levels (TextureBinding::lod_scale = hd_scale, trilinear with smooth_minify): sharp at 1080p, smooth smaller.
// Right after its upload at the bind, so the copies come before the draw that samples it. False: left as it is.
bool UpgradeToHdIcon(HostTexture &t, const TextureFetch &fetch) {
  if (t.hd_icon || t.hd_count < 2 || t.render_target || t.gpu_written || t.dimension != TextureDimension::Tex2D ||
      t.stacked || t.depth != 1 || t.info.block_size != 1 || t.info.guest_block_bytes != 4 || !t.texture)
    return false;
  auto &s = State();
  plume::RenderCommandList *cmd = s.command_list;
  plume::RenderDevice *device = Video::Device();
  const uint32_t levels = t.hd_count;
  // The staging for every level first: nothing is recorded unless all of it fits.
  UploadAllocation staging[4];
  uint32_t pitch[4] = {};
  for (uint32_t level = 0; level < levels; ++level) {
    const uint32_t w = std::max(1u, t.hd_size >> level);
    pitch[level] = (w * t.info.host_block_bytes + 255u) / 256u * 256u;
    staging[level] = s.uploads[s.frame].Allocate(uint64_t(pitch[level]) * w, 512);
    if (!staging[level])
      return false;
  }
  auto texture = device->createTexture(plume::RenderTextureDesc::Texture2D(t.hd_size, t.hd_size, levels, t.host_format));
  if (!TextureCreated(texture.get()))
    return false;
  texture->setName("renut HD icon");
  plume::RenderTextureViewDesc view_desc = t.view_desc;
  view_desc.mipLevels = levels;
  auto view = texture->createTextureView(view_desc);
  if (!view)
    return false;
  const u32 endian = fetch.Endian();
  std::vector<uint8_t> row;
  cmd->barriers(plume::RenderBarrierStage::COPY,
                plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
  for (uint32_t level = 0; level < levels; ++level) {
    const uint32_t w = std::max(1u, t.hd_size >> level);
    row.resize(size_t(w) * 4);
    auto *dst = static_cast<uint8_t *>(staging[level].cpu);
    for (uint32_t y = 0; y < w; ++y) {
      // As ReadLevel's replacement path: the guest's bytes, its endian swap, the host conversion.
      std::memcpy(row.data(), t.hd_data[level] + size_t(y) * w * 4, row.size());
      SwapEndian(endian, row.data(), row.data(), row.size());
      uint8_t *out = dst + size_t(y) * pitch[level];
      if (t.info.conversion == TexelConversion::Copy)
        std::memcpy(out, row.data(), size_t(w) * t.info.host_block_bytes);
      else
        ConvertTexels(t.info.conversion, row.data(), out, w);
    }
    cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture.get(), level, 0),
                           plume::RenderTextureCopyLocation::PlacedFootprint(
                               staging[level].ref.ref, t.info.host, w, w, 1, pitch[level] / t.info.host_block_bytes,
                               staging[level].ref.offset),
                           0, 0, 0, nullptr);
  }
  // The new image in the texture's slot; views of the old one (other swizzles) go.
  ++g_texture_epoch;
  for (HostTexture::SwizzleView &v : t.swizzle_views) {
    Video::FreeTextureSlotLocked(v.slot);
    Video::RetireLocked(std::move(v.view));
  }
  t.swizzle_views.clear();
  Video::RetireLocked(std::move(t.view));
  Video::RetireLocked(std::move(t.texture));
  t.texture = std::move(texture);
  t.view = std::move(view);
  t.view_desc = view_desc;
  Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
  t.layout = plume::RenderTextureLayout::COPY_DEST;
  t.mip_levels = levels;
  t.alloc_width = t.alloc_height = t.hd_size;
  t.hd_icon = true;
  static uint32_t logged = 0;
  if (logged++ < 32)
    RGPU_INFO("[icons] texture {:08X} ({}x{}) held at {}x{} with {} levels (HD icon)", t.guest_va, t.width, t.height,
              t.hd_size, t.hd_size, levels);
  return true;
}

void Retire(HostTexture &t) {
  ++g_texture_epoch;  // banjo-nx (CPU proposal 2)
  DepthHandOverForgetLocked(t);  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover)
  if (t.slot != UINT32_MAX && t.slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.slot);
  t.slot = UINT32_MAX;
  for (HostTexture::SwizzleView &v : t.swizzle_views) {
    Video::FreeTextureSlotLocked(v.slot);
    Video::RetireLocked(std::move(v.view));
  }
  t.swizzle_views.clear();
  if (t.level0_slot != UINT32_MAX && t.level0_slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.level0_slot);
  t.level0_slot = UINT32_MAX;
  Video::RetireLocked(std::move(t.level0_view));
  Video::RetireLocked(std::move(t.copy_framebuffer));  // banjo-nx (2026-10-18)
  Video::RetireLocked(std::move(t.view));
  Video::RetireLocked(std::move(t.texture));
  if (t.words) {
    for (HostTexture::WordsView &v : t.words_views) {  // banjo-nx (banjo_gpu_depth_words_view)
      if (v.slot != UINT32_MAX && v.slot != kInvalidDescriptor)
        Video::FreeTextureSlotLocked(v.slot);
      Video::RetireLocked(std::move(v.view));
    }
    t.words_views.clear();
    Video::RetireLocked(std::move(t.depth_framebuffer));
    Retire(*t.words);
    t.words.reset();
    std::erase_if(g_depth_words, [&t](const auto &entry) { return entry.second == &t; });
  }
  if (t.render_target)
    std::erase_if(g_resolve_targets, [&t](const auto &entry) { return entry.second == &t; });
}

// Allocations only: a parked target is never an alias of guest memory. Keep
// its views, descriptors and last layout intact for already-recorded draws.
// Reuse stays on the same ordered graphics queue, just as repeated resolves
// into an active target do. Eviction uses the usual fence-delayed retirement.
struct ResolveSpare {
  std::unique_ptr<HostTexture> target;
  uint64_t bytes = 0;
  uint64_t frame = 0;
};
constexpr uint32_t kResolveSpareCount = 8;
constexpr uint64_t kResolveSpareIdleFrames = 120;
ResolveSpare g_resolve_spares[kResolveSpareCount];
uint64_t g_resolve_spare_bytes = 0;
struct ResolvePoolStats {
  uint64_t hits = 0, misses = 0, parked = 0, evicted = 0;
} g_resolve_pool_stats;

void DropResolveSpare(ResolveSpare &spare) {
  if (!spare.target)
    return;
  g_resolve_spare_bytes -= spare.bytes;
  Retire(*spare.target);
  spare = {};
  ++g_resolve_pool_stats.evicted;
}

void DropResolveSparesForVa(u32 va) {
  for (auto &spare : g_resolve_spares)
    if (spare.target && spare.target->guest_va == va)
      DropResolveSpare(spare);
}

bool ResolvePoolAllowed() {
  if (!IsDeko3D() || REXCVAR_GET(me2_renut_resolve_pool_mb) <= 0 ||
      Video::TextureSlotsInUseLocked() >= kEvictPressureSlots)
    return false;
  // Refresh once per frame that actually uses the pool, not once per draw.
  static uint64_t checked_frame = UINT64_MAX;
  static bool allowed = false;
  if (checked_frame != g_frame) {
    checked_frame = g_frame;
    const GpuMemoryUsage memory = QueryGpuMemory(State().device.get());
    const uint64_t reserve = std::max<uint64_t>(64ull << 20, banjo::renut_gpu::TextureReserveBytes());
    allowed = memory.valid && memory.local_budget > memory.local_used &&
              memory.local_budget - memory.local_used >= reserve;
  }
  return allowed;
}

void TrimResolveSpares() {
  if (!g_resolve_spare_bytes)
    return;
  const bool allowed = ResolvePoolAllowed();
  for (auto &spare : g_resolve_spares)
    if (spare.target && (!allowed || g_frame - spare.frame >= kResolveSpareIdleFrames))
      DropResolveSpare(spare);
}

bool ReusableResolveTarget(const HostTexture &t) {
  // Depth targets have words, handover and source-generation state. Leave
  // those, sampled uploads, arrays and cubes on their original lifetime path.
  return t.render_target && t.texture && t.view && t.dimension == TextureDimension::Tex2D &&
         !t.stacked && t.depth == 1 && t.host_format != kDepthResolveFormat && !t.words &&
         !t.depth_framebuffer && !t.handed_from && !t.held && t.slot != UINT32_MAX && t.slot != kInvalidDescriptor;
}

std::unique_ptr<HostTexture> TakeResolveSpare(u32 va, const u32 desc[6], plume::RenderFormat format,
                                            RenderScale scale) {
  for (auto &spare : g_resolve_spares) {
    if (spare.target && spare.target->guest_va == va && spare.target->host_format == format &&
        spare.target->scale == scale && std::memcmp(spare.target->desc, desc, sizeof(spare.target->desc)) == 0) {
      auto target = std::move(spare.target);
      g_resolve_spare_bytes -= spare.bytes;
      spare = {};
      // A different scale has overwritten the logical guest resource since
      // this allocation was parked. Never reuse its pixels or skip a resolve.
      target->resolve_signature = 0;
      target->resolves = 0;
      target->copied_resolve = 0;
      target->gpu_written = false;
      target->uploaded = false;
      target->trace_frame = UINT64_MAX;
      ++g_texture_epoch;
      ++g_resolve_pool_stats.hits;
      return target;
    }
  }
  ++g_resolve_pool_stats.misses;
  return nullptr;
}

bool ParkResolveTarget(std::unique_ptr<HostTexture> &target) {
  const uint64_t bytes = TextureAllocationBytes(target->texture.get());
  const uint64_t cap = uint64_t(REXCVAR_GET(me2_renut_resolve_pool_mb)) << 20;
  if (!bytes || bytes > cap)
    return false;
  ResolveSpare *empty = nullptr;
  while (true) {
    ResolveSpare *oldest = nullptr;
    for (auto &spare : g_resolve_spares) {
      if (!spare.target)
        empty = &spare;
      else if (!oldest || spare.frame < oldest->frame)
        oldest = &spare;
    }
    if (empty && g_resolve_spare_bytes + bytes <= cap)
      break;
    if (!oldest)
      return false;
    DropResolveSpare(*oldest);
  }
  HostTexture *old = target.get();
  std::erase_if(g_resolve_targets, [old](const auto &entry) { return entry.second == old; });
  ++g_texture_epoch;
  empty->target = std::move(target);
  empty->bytes = bytes;
  empty->frame = g_frame;
  g_resolve_spare_bytes += bytes;
  ++g_resolve_pool_stats.parked;
  return true;
}

// Two descriptions of the same data: the same memory, format, endianness, size
// and dimension (fetch dwords 1, 2 and the dimension in 5). Fetch constants of
// the same memory may still differ in pitch, swizzle or mip fields.
bool SameData(const u32 a[6], const u32 b[6]) {
  constexpr u32 kDimensionMask = 0x600u;  // fetch dword 5 bits 9-10
  return a[1] == b[1] && a[2] == b[2] && (a[5] & kDimensionMask) == (b[5] & kDimensionMask);
}

// The resolve destination another header wrote this texture's memory through,
// when it holds the same data (SameData). Only the GPU has what the resolve
// wrote.
HostTexture *ResolvedAlias(u32 texture_va, const TextureFetch &fetch, const u32 desc[6]) {
  auto it = g_resolve_targets.find(fetch.BasePage());
  if (it == g_resolve_targets.end())
    return nullptr;
  HostTexture *resolved = it->second;
  if (resolved->guest_va == texture_va || !resolved->texture)
    return nullptr;
  return SameData(resolved->desc, desc) ? resolved : nullptr;
}

// The depth resolve whose words a texture described by 'fetch' reads: a
// 32-bit 8_8_8_8 texture over a depth resolve destination's memory, of its
// size.
HostTexture *DepthWordsSource(const TextureFetch &fetch) {
  if (fetch.Format() != GpuTextureFormat::Fmt8_8_8_8 || g_depth_words.empty())
    return nullptr;
  auto it = g_depth_words.find(fetch.BasePage());
  if (it == g_depth_words.end())
    return nullptr;
  HostTexture *source = it->second;
  u32 width, height, depth;
  fetch.Size(width, height, depth);
  if (!source->words || source->width != width || source->height != height)
    return nullptr;
  return source;
}

// banjo-nx (banjo_gpu_depth_words_view, package 28): the slot of a view of the depth resolve's words that 't' (a
// texture over its memory) samples instead of its own copy, or UINT32_MAX (then the copy). The copy is of the whole
// words texture into a texture of the same format, size and one level, made at the first bind after each resolve, and
// every draw binds its textures again: the draws after a resolve read its words either way. So only a texture of that
// format, size and one level, sampled through its own view's description with the fetch's swizzle.
uint32_t WordsViewSlot(HostTexture &t, HostTexture &source, u32 swizzle) {
  if (!banjo::renut_gpu::DepthWordsViewOn() || !source.words || !source.words->texture ||
      t.host_format != kDepthResolveWordsFormat || t.dimension != TextureDimension::Tex2D || t.stacked ||
      t.mip_levels != 1 || t.alloc_width != source.words->alloc_width || t.alloc_height != source.words->alloc_height)
    return UINT32_MAX;
  for (const HostTexture::WordsView &v : source.words_views) {
    if (v.swizzle == swizzle)
      return v.slot;
  }
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.mipLevels = 1;
  desc.componentMapping = SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format);
  HostTexture::WordsView v;
  v.swizzle = swizzle;
  v.view = source.words->texture->createTextureView(desc);
  if (!v.view)
    return UINT32_MAX;
  v.slot = Video::AllocateTextureSlotLocked();
  if (v.slot == kInvalidDescriptor) {
    Video::RetireLocked(std::move(v.view));
    return UINT32_MAX;
  }
  Video::SetTextureSlotLocked(v.slot, source.words->texture.get(), v.view.get());
  ++g_texture_epoch;  // banjo-nx (CPU proposal 2): a new view
  source.words_views.push_back(std::move(v));
  return source.words_views.back().slot;
}

// Copies the depth resolve's words into 't' (of the source's scale) when it
// has resolved again.
void CopyDepthWords(HostTexture &t, HostTexture &source) {
  if (t.copied_resolve == source.resolves)
    return;
  ++g_texture_epoch;  // banjo-nx (CPU proposal 2)
  plume::RenderCommandList *cmd = State().command_list;
  TransitionTextureLocked(source.words.get(), plume::RenderTextureLayout::COPY_SOURCE);
  TransitionTextureLocked(&t, plume::RenderTextureLayout::COPY_DEST);
  const plume::RenderBox box(0, 0, int32_t(t.scale.Size(t.width)), int32_t(t.scale.Size(t.height)));
  t.resolve_signature = 0;  // banjo-nx (banjo_renut_resolve_skip)
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t.texture.get(), 0, 0),
                         plume::RenderTextureCopyLocation::Subresource(source.words->texture.get()),
                         0, 0, 0, &box);
  t.copied_resolve = source.resolves;
  t.uploaded = true;
}

// GetResolveTextureLocked; 'fetch' receives the header's fetch constant.
HostTexture *ResolveTarget(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                           const d3d::TextureFetchConstant *header_format, bool depth,
                           TextureFetch &fetch) {
  // banjo-nx (2026-10-19, render-thread proposal 3): no unconditional g_texture_epoch bump: Create and Retire bump it,
  // and so does a resolve target entry that changes (below).
  if (!texture_va)
    return nullptr;
  if (!header_format) {
    const auto *header = GuestPtr<const d3d::BaseTexture>(texture_va);
    if (!header)
      return nullptr;
    header_format = &header->Format;
  }
  fetch = TextureFetch::FromHeader(*header_format);
  if (!fetch.IsTexture())
    return nullptr;
  u32 desc[6];
  MaskedDesc(fetch, desc);

  auto &slot = TextureSlot(texture_va);
  bool acquired = false;
  if (slot && (!slot->render_target || std::memcmp(slot->desc, desc, sizeof(desc)) != 0 ||
               !slot->texture || slot->host_format != format || slot->scale != scale)) {
    // Only an otherwise identical color resolve changing scale may park an
    // allocation. All semantic changes retain the original rebuild path.
    const bool scale_only = !depth && ReusableResolveTarget(*slot) && slot->host_format == format &&
                            slot->scale != scale && std::memcmp(slot->desc, desc, sizeof(desc)) == 0;
    std::unique_ptr<HostTexture> reused;
    if (scale_only && ResolvePoolAllowed()) {
      reused = TakeResolveSpare(texture_va, desc, format, scale);
      ParkResolveTarget(slot);
    }
    if (slot && slot->texture && slot->scale != scale) {
      static uint32_t logged = 0;
      if (logged++ < 24)
        RGPU_WARN("[me2-ui] resolve destination {:08X} rebuilt for another scale ({:.3g}x -> {:.3g}x)", texture_va,
                  slot->scale.Factor(), scale.Factor());
    }
    if (slot)
      Retire(*slot);
    slot = std::move(reused);
    acquired = bool(slot);
  }
  if (!slot) {
    slot = std::make_unique<HostTexture>();
    slot->guest_va = texture_va;
    std::memcpy(slot->desc, desc, sizeof(desc));
    // A depth destination has one component, which the swizzle places.
    if (!Create(*slot, fetch, true, format, !depth && (fetch.Swizzle() & 7) != 0, scale, !depth)) {
      RGPU_ERROR("Resolve destination {:08X}: {}x{} target creation failed", texture_va,
                 slot->width, slot->height);
      Retire(*slot);
      return nullptr;
    }
    acquired = true;
  }
  if (acquired) {
    // A new render target has no defined content, and D3D12 wants it
    // discarded (or cleared) before a resolve or copy writes part of it.
    if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
      TransitionTextureLocked(slot.get(), plume::RenderTextureLayout::COLOR_WRITE);
      cmd->discardTexture(slot->texture.get());
    }
  }
  slot->gpu_written = true;
  slot->uploaded = true;
  slot->used_frame = g_frame;
  HostTexture *&entry = g_resolve_targets[fetch.BasePage()];
  if (entry != slot.get()) {
    entry = slot.get();
    ++g_texture_epoch;  // banjo-nx (2026-10-19, render-thread proposal 3): ResolvedAlias's answer changes
  }
  return slot.get();
}

} // namespace

namespace {

// Marks the host textures of the resources unlocked since the last call.
// Unlocks of anything else (vertex buffers, textures never bound) are dropped.
void ApplyUnlocksLocked() {
  std::vector<u32> unlocked;
  {
    std::lock_guard lock(g_unlocked_mutex);
    if (g_unlocked.empty())
      return;
    unlocked.swap(g_unlocked);
  }
  for (u32 va : unlocked) {
    auto it = g_textures.find(va);
    if (it != g_textures.end() && it->second) {
      it->second->unlocked = true;
      it->second->unlock_frame = g_frame;
      it->second->lock_driven = true;
    }
  }
}

// Linear textures are the CPU-written ones. The GPU reads them when it
// executes, after the CPU is done with the frame, so an upload reads memory
// as it is at submission; reading at the draw can catch writes still in
// progress. Only bytes that changed are worth uploading: converting the font
// cache every frame cost more than anything else in Present, and hashing its
// 7 MB every frame still cost more than half a millisecond. How a texture
// changes decides how it is checked, once per frame it is bound:
// - Lock-driven: the game writes it between a Lock and D3D::UnlockResource
//   (the font cache, sub_82B9FA68). An unlock uploads; the content is watched
//   for kUnlockWatchFrames after one, and checked every kLinearVerifyFrames
//   for a write that went around the unlock (which ends lock-driven).
// - Dynamic: changed at kDynamicChanges checks in a row (the XMV logo movies,
//   whose decoder writes without a Lock and may still be writing at the
//   draw). Uploaded every frame; checked every kLinearVerifyFrames for having
//   stopped changing.
// - Otherwise the content is hashed at every check.
// A write after the check shows the next frame.
void RefreshLinearLocked(HostTexture &t, const TextureFetch &fetch) {
  ApplyUnlocksLocked();
  auto upload = [&](uint64_t hash) {
    t.content_hash = hash;
    t.uploaded = Upload(t, fetch, true);
  };
  const bool verify_due = g_frame - t.verified_frame >= kLinearVerifyFrames;
  // banjo-nx (2026-10-19, render-thread proposal 1): every hash through CheckedHash (the cards).
  if (!t.uploaded) {
    upload(CheckedHash(fetch, t, false));
    t.verified_frame = g_frame;
  } else if (t.lock_driven) {
    const bool watched = g_frame - t.unlock_frame < kUnlockWatchFrames;
    if (t.unlocked) {
      upload(CheckedHash(fetch, t, false));
    } else if (watched || verify_due) {
      const uint64_t hash = CheckedHash(fetch, t, false);
      if (hash != t.content_hash) {
        upload(hash);
        t.lock_driven = watched;
      }
      if (verify_due)
        t.verified_frame = g_frame;
    }
  } else if (t.dynamic) {
    if (verify_due) {
      const uint64_t hash = CheckedHash(fetch, t, false);
      t.dynamic = hash != t.content_hash;
      t.changes = 0;
      t.content_hash = hash;
      t.verified_frame = g_frame;
    }
    // banjo-nx (2026-10-19, render-thread proposal 1): not uploaded again while the cards prove no page written since
    // the last upload read it (the GPU's copy holds those bytes).
    KnowGuestExtent(fetch, t);
    if (banjo::renut_gpu::StoreCardsOn() && !banjo::renut_gpu::StoreCardsVerifyOn() &&
        ExtentClean(t, t.read_interval)) {
      banjo::renut_gpu::CountTextureCards(uint64_t(t.sample_sizes[0]) + t.sample_sizes[1], 2);
    } else {
      t.uploaded = Upload(t, fetch, true);
    }
  } else {
    const uint64_t hash = CheckedHash(fetch, t, false);
    if (hash != t.content_hash) {
      upload(hash);
      t.dynamic = ++t.changes >= kDynamicChanges;
      t.verified_frame = g_frame;
    } else {
      t.changes = 0;
    }
  }
  t.unlocked = false;
}

// The backstop (kEvictPressureSlots, kEvictMemoryShare): with the bindless
// table filling up or video memory near its budget, the host textures idle
// for a while go.
void EvictIdleTexturesLocked() {
  const bool slots = Video::TextureSlotsInUseLocked() >= kEvictPressureSlots;
  GpuMemoryUsage memory;
  if (!slots) {
    memory = QueryGpuMemory(State().device.get());
    if (!memory.valid || !memory.local_budget ||
        double(memory.local_used) < double(memory.local_budget) * kEvictMemoryShare) {
      return;
    }
  }
  std::vector<u32> idle;
  const bool keep_gpu_written = banjo::renut_gpu::KeepGpuWritten();  // banjo-nx
  for (const auto &[va, t] : g_textures) {
    if (t && t->gpu_written && keep_gpu_written)
      continue;  // banjo-nx: only the GPU has its content (banjo_renut_keep_gpu_written)
    const uint64_t limit = t && t->gpu_written ? kEvictIdleGpuFrames : kEvictIdleFrames;
    if (!t || g_frame - t->used_frame >= limit)
      idle.push_back(va);
  }
  for (u32 va : idle)
    ReleaseHostTextureLocked(va);
  if (slots) {
    RGPU_WARN("Textures: bindless table nearly full; {} idle host textures released ({} of {} "
              "slots in use)",
              idle.size(), Video::TextureSlotsInUseLocked(), kTextureDescriptorCount);
  } else if (!idle.empty()) {
    RGPU_WARN("Textures: video memory near its budget ({} of {} MB); {} idle host textures "
              "released",
              memory.local_used >> 20, memory.local_budget >> 20, idle.size());
  }
}

// banjo-nx (2026-10-17): banjo_renut_memory_policy (banjo_renut_tuning.h). Every kColdCheckFrames frames, with less
// than banjo_renut_textures_reserve_mb left of the GPU memory budget (the driver's, VK_EXT_memory_budget: real
// footprints) or the bindless table three quarters full, the cold host textures go: not bound for kColdFrames frames
// (sa2-nx's kFramesNoUsageForDrop), so no command list still to run reads them (2 frames in flight; a slot given back
// is reused only once its frame has finished, DrainSlot) and a texture in use is never re-uploaded over and over. A
// released texture is made again from guest memory when bound, as it was made the first time. Never one the GPU wrote
// (banjo_renut_keep_gpu_written). If every texture is warm nothing goes: better a larger cache than thrash. Renut's
// backstop above stays (85 % of the budget, 600 frames), sized for a PC's video memory.
constexpr uint64_t kColdCheckFrames = 32;
constexpr uint64_t kColdFrames = 120;

void EvictColdTexturesLocked(uint64_t reserve) {
  const bool slots = Video::TextureSlotsInUseLocked() >= kEvictPressureSlots;
  uint64_t left = 0, used = 0, budget = 0;
  if (!slots) {
    const GpuMemoryUsage memory = QueryGpuMemory(State().device.get());
    if (!memory.valid)
      return;
    // The device-local heaps where there are any (a PC's video memory); otherwise the shared ones.
    const bool local = memory.local_budget != 0;
    budget = local ? memory.local_budget : memory.shared_budget;
    used = local ? memory.local_used : memory.shared_used;
    if (!budget)
      return;
    left = budget > used ? budget - used : 0;
    if (left >= reserve)
      return;
  }
  const bool keep_gpu_written = banjo::renut_gpu::KeepGpuWritten();
  std::vector<u32> cold;
  for (const auto &[va, t] : g_textures) {
    if (t && t->gpu_written && keep_gpu_written)
      continue;
    const uint64_t limit = t && t->gpu_written ? kEvictIdleGpuFrames : kColdFrames;
    if (!t || g_frame - t->used_frame >= limit)
      cold.push_back(va);
  }
  for (u32 va : cold)
    ReleaseHostTextureLocked(va);
  banjo::renut_gpu::CountReserveEviction(cold.size());
  static uint32_t logged = 0;
  if (!cold.empty() && logged++ < 32) {
    if (slots) {
      RGPU_WARN("Textures: bindless table three quarters full (banjo_renut_memory_policy); {} cold host textures "
                "released ({} of {} slots in use)",
                cold.size(), Video::TextureSlotsInUseLocked(), kTextureDescriptorCount);
    } else {
      RGPU_WARN("Textures: {} MB of the GPU memory budget left ({} of {} MB used), under the {} MB reserve "
                "(banjo_renut_memory_policy); {} cold host textures released",
                left >> 20, used >> 20, budget >> 20, reserve >> 20, cold.size());
    }
  }
}

} // namespace

uint64_t g_texture_frame = 1;  // banjo-nx (2026-10-19): g_frame, read inline by TextureFrameLocked

uint64_t g_texture_epoch = 1;  // banjo-nx (2026-10-19, CPU proposal 2)
// me2-nx-deko (2026-10-27): the final pass started (surfaces.cpp UiPhaseBeforeDrawLocked): FillTextures' memos from
// before it must not hand the final pass the scene's binding (me2_renut_ui_fsr substitutes it).
void BumpTextureEpochLocked() {
  ++g_texture_epoch;
}
uint64_t g_depth_epoch = 1;    // banjo-nx (2026-10-19, render-thread proposal 3)

namespace {
void StreamReportLocked();  // me2-nx-deko (2026-10-31): defined with the upload queue, before BindTextureLocked
}  // namespace

void AdvanceTextureFrameLocked() {
  ++g_frame;
  g_texture_frame = g_frame;
  ApplyUnlocksLocked();
  TrimResolveSpares();
  if (g_frame % 300 == 0 && (g_resolve_pool_stats.parked || g_resolve_pool_stats.evicted ||
                            g_resolve_pool_stats.hits || g_resolve_pool_stats.misses)) {
    RGPU_INFO("[resolve-pool] 300 frames: {} reuses, {} misses, {} parked, {} evicted; {} KiB retained",
              g_resolve_pool_stats.hits, g_resolve_pool_stats.misses, g_resolve_pool_stats.parked,
              g_resolve_pool_stats.evicted, g_resolve_spare_bytes >> 10);
    g_resolve_pool_stats = {};
  }
  if (g_frame % 300 == 0)
    StreamReportLocked();  // me2-nx-deko (2026-10-31): [tex-stream]
  if (g_frame % kEvictCheckFrames == 0)
    EvictIdleTexturesLocked();
  // banjo-nx: banjo_renut_memory_policy.
  if (const uint64_t reserve = banjo::renut_gpu::TextureReserveBytes(); reserve && g_frame % kColdCheckFrames == 0)
    EvictColdTexturesLocked(reserve);
}

void ReleaseResolvePoolLocked() {
  for (auto &spare : g_resolve_spares)
    DropResolveSpare(spare);
}

void ReleaseHostTexturesInMemoryLocked(u32 physical, u32 size) {
  if (!size)
    return;
  const u32 first_page = (physical & 0x1FFFFFFFu) >> 12;
  const u32 end_page = first_page + ((size + 0xFFFu) >> 12);
  auto inside = [&](u32 page) { return page >= first_page && page < end_page; };
  for (auto &spare : g_resolve_spares) {
    if (spare.target && (inside(spare.target->desc[1] >> 12) ||
        ((spare.target->desc[5] >> 12) && inside(spare.target->desc[5] >> 12))))
      DropResolveSpare(spare);
  }
  std::vector<u32> gone;
  for (const auto &[va, t] : g_textures) {
    if (!t)
      continue;
    const u32 base_page = t->desc[1] >> 12;
    const u32 mip_page = t->desc[5] >> 12;
    if (inside(base_page) || (mip_page && inside(mip_page)))
      gone.push_back(va);
  }
  for (u32 va : gone)
    ReleaseHostTextureLocked(va);
}

void NoteTextureUnlocked(u32 resource_va) {
  std::lock_guard lock(g_unlocked_mutex);
  g_unlocked.push_back(resource_va);
}

void FlushTextureFillsLocked() {
  for (const PendingFill &f : g_pending_fills) {
    g_level_replaced = false;
    ReadLevel(f.info, f.fetch, f.src, f.blocks_w, f.blocks_h, f.depth, f.dst, f.row_pitch,
              f.slice_pitch);
    if (g_level_replaced)
      if (HostTexture *t = FindHostTextureLocked(f.texture_va))
        t->replaced_icon = true;
  }
  g_pending_fills.clear();
}

HostTexture *FindHostTextureLocked(u32 texture_va) {
  auto it = g_textures.find(texture_va);
  return it != g_textures.end() ? it->second.get() : nullptr;
}

void ReleaseHostTextureLocked(u32 texture_va) {
  DropResolveSparesForVa(texture_va);
  auto it = g_textures.find(texture_va);
  if (it == g_textures.end())
    return;
  Retire(*it->second);
  if (TextureMemo &memo = TextureMemoFor(texture_va); memo.va == texture_va)
    memo = {};
  g_textures.erase(it);
}

// banjo-nx (2026-10-19, banjo_deko3d_barrier_lean): a layout only read in, both sides: deko3d has no image layouts,
// and two reads need no ordering.
bool ReadOnlyLayout(plume::RenderTextureLayout layout) {
  return layout == plume::RenderTextureLayout::SHADER_READ || layout == plume::RenderTextureLayout::DEPTH_READ ||
         layout == plume::RenderTextureLayout::COPY_SOURCE || layout == plume::RenderTextureLayout::RESOLVE_SOURCE;
}

void TransitionTextureLocked(HostTexture *texture, plume::RenderTextureLayout layout) {
  if (!texture || !texture->texture || texture->layout == layout)
    return;
  if (IsDeko3D() && banjo::renut_gpu::Deko3DBarrierLeanOn() && ReadOnlyLayout(texture->layout) &&
      ReadOnlyLayout(layout)) {
    texture->layout = layout;
    return;
  }
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd)
    return;
  const bool copy = layout == plume::RenderTextureLayout::COPY_DEST ||
                    layout == plume::RenderTextureLayout::COPY_SOURCE ||
                    layout == plume::RenderTextureLayout::RESOLVE_DEST;
  cmd->barriers(copy ? plume::RenderBarrierStage::COPY : plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(texture->texture.get(), layout));
  texture->layout = layout;
}

namespace {

// ---- me2_renut_upload_budget_kb (2026-10-31) -----------------------------------------------------------------------
uint64_t UploadBudgetBytes() {
  static const int32_t kb = REXCVAR_GET(me2_renut_upload_budget_kb);
  return kb > 0 ? uint64_t(kb) << 10 : 0;
}

// Staging bytes of one level, as Upload lays it out (256-byte aligned rows, every slice / face).
uint64_t LevelBytes(const HostTexture &t, uint32_t level) {
  const uint32_t bs = t.info.block_size ? t.info.block_size : 1;
  const uint32_t w = std::max(1u, t.width >> level), h = std::max(1u, t.height >> level);
  const uint32_t blocks_w = (w + bs - 1) / bs, blocks_h = (h + bs - 1) / bs;
  const uint32_t row_align = t.info.decoded_blocks ? 256u * bs : 256u;
  const uint64_t row_pitch = (uint64_t(blocks_w) * t.info.host_block_bytes + row_align - 1) / row_align * row_align;
  return row_pitch * blocks_h * std::max(1u, t.depth);
}
uint64_t TextureBytes(const HostTexture &t) {
  uint64_t sum = 0;
  for (uint32_t level = 0; level < t.mip_levels; ++level) sum += LevelBytes(t, level);
  return sum;
}

struct StreamItem {
  u32 va = 0;
  uint64_t id = 0;
  TextureFetch fetch;
  uint64_t queued_frame = 0;
};
std::deque<StreamItem> g_stream_queue;
uint64_t g_stream_next_id = 1;
uint64_t g_budget_frame = 0, g_budget_used = 0;
struct StreamStats {
  uint64_t new_whole = 0, new_progressive = 0, changed_whole = 0;  // textures
  uint64_t bytes_whole = 0, bytes_first = 0, bytes_later = 0;      // staging bytes
  uint64_t levels_later = 0, completed = 0, latency_frames = 0, latency_max = 0, dropped = 0;
  uint64_t queue_max = 0;
  uint64_t frame_us = 0, frame_us_max = 0, frame_of_us = 0;  // upload work on the render thread, per frame
  uint64_t work_us = 0;
} g_stream_stats;

void BudgetFrame() {
  if (g_budget_frame != g_frame) {
    g_budget_frame = g_frame;
    g_budget_used = 0;
  }
}

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
void NoteWork(uint64_t us) {
  g_me2_texture_work_us.fetch_add(us, std::memory_order_relaxed);
  StreamStats &s = g_stream_stats;
  s.work_us += us;
  if (s.frame_of_us != g_frame) {
    s.frame_of_us = g_frame;
    s.frame_us = 0;
  }
  s.frame_us += us;
  s.frame_us_max = std::max(s.frame_us_max, s.frame_us);
}

// A large mipmapped 2D texture's first upload, under a budget.
bool Streamable(const HostTexture &t) {
  static const uint64_t min_bytes = uint64_t(std::max(0, REXCVAR_GET(me2_renut_upload_stream_min_kb))) << 10;
  return UploadBudgetBytes() && t.dimension == TextureDimension::Tex2D && !t.stacked && t.mip_levels > 1 &&
         !t.render_target && !t.gpu_written && TextureBytes(t) >= min_bytes;
}

// Its small levels now (each <= 64 KB, always), then larger ones while the frame's budget lasts; the rest queued.
bool UploadProgressive(HostTexture &t, const TextureFetch &fetch) {
  BudgetFrame();
  const uint64_t budget = UploadBudgetBytes();
  uint32_t from = t.mip_levels;
  uint64_t used = 0;
  while (from > 0) {
    const uint64_t bytes = LevelBytes(t, from - 1);
    if (bytes > (64u << 10) && g_budget_used + used + bytes > budget) break;
    used += bytes;
    --from;
  }
  if (!Upload(t, fetch, false, from, t.mip_levels)) return false;
  g_budget_used += used;
  t.resident_from = from;
  t.stream_id = 0;
  StreamStats &s = g_stream_stats;
  if (from == 0) {
    ++s.new_whole;
    s.bytes_whole += used;
    return true;
  }
  ++s.new_progressive;
  s.bytes_first += used;
  t.stream_id = g_stream_next_id++;
  g_stream_queue.push_back({t.guest_va, t.stream_id, fetch, g_frame});
  s.queue_max = std::max<uint64_t>(s.queue_max, g_stream_queue.size());
  return true;
}

// Once a frame (the frame's first texture bind): the queued textures' next levels, oldest texture first, largest
// levels last, within the frame's budget (at least one level a frame, so the queue always moves).
void StreamPumpLocked() {
  static uint64_t pumped = 0;
  if (pumped == g_frame || g_stream_queue.empty()) return;
  pumped = g_frame;
  BudgetFrame();
  const uint64_t budget = UploadBudgetBytes();
  const uint64_t t0 = NowUs();
  StreamStats &s = g_stream_stats;
  bool progressed = false;
  while (!g_stream_queue.empty()) {
    StreamItem item = g_stream_queue.front();
    auto found = g_textures.find(item.va);
    HostTexture *t = found != g_textures.end() ? found->second.get() : nullptr;
    if (!t || t->stream_id != item.id || !t->texture || t->resident_from == 0) {
      g_stream_queue.pop_front();  // released, replaced, or uploaded whole meanwhile
      ++s.dropped;
      continue;
    }
    while (t->resident_from > 0) {
      const uint64_t bytes = LevelBytes(*t, t->resident_from - 1);
      if (progressed && g_budget_used + bytes > budget) break;
      if (!Upload(*t, item.fetch, false, t->resident_from - 1, t->resident_from)) break;  // staging full: next frame
      g_budget_used += bytes;
      s.bytes_later += bytes;
      ++s.levels_later;
      --t->resident_from;
      progressed = true;
    }
    if (t->resident_from > 0) break;  // the budget is spent: the rest next frame, in the same order
    g_stream_queue.pop_front();
    t->stream_id = 0;
    ++s.completed;
    const uint64_t latency = g_frame - item.queued_frame;
    s.latency_frames += latency;
    s.latency_max = std::max(s.latency_max, latency);
  }
  NoteWork(NowUs() - t0);
}

void StreamReportLocked() {
  StreamStats &s = g_stream_stats;
  if (!(s.new_whole || s.new_progressive || s.changed_whole || s.levels_later || s.work_us)) return;
  RGPU_INFO("[tex-stream] 300 frames: upload work on the render thread {:.1f} ms (worst frame {:.1f} ms); new "
            "textures {} whole ({:.1f} MB) + {} progressive ({:.1f} MB at once, {} levels / {:.1f} MB in later frames, "
            "{} completed after {:.1f} frames on average, {} at most; queue at most {}, {} entries dropped), {} content "
            "changes uploaded whole; budget {} KB a frame",
            s.work_us / 1000.0, s.frame_us_max / 1000.0, s.new_whole, s.bytes_whole / 1e6, s.new_progressive,
            s.bytes_first / 1e6, s.levels_later, s.bytes_later / 1e6, s.completed,
            s.completed ? double(s.latency_frames) / double(s.completed) : 0.0, s.latency_max, s.queue_max, s.dropped,
            s.changed_whole, UploadBudgetBytes() >> 10);
  const uint64_t frame_of_us = s.frame_of_us, frame_us = s.frame_us;
  s = StreamStats{};
  s.frame_of_us = frame_of_us;
  s.frame_us = frame_us;
}

}  // namespace

TextureBinding BindTextureLocked(u32 texture_va, const TextureFetch &device_fetch, BindEffects *fx) {
  StreamPumpLocked();  // me2-nx-deko (2026-10-31, me2_renut_upload_budget_kb)
  TextureBinding binding;
  binding.dimension = HostDimension(device_fetch);
  binding.slot = NullSlot(binding.dimension);
  if (!texture_va)
    return binding;
  const auto *header = GuestPtr<const d3d::BaseTexture>(texture_va);
  if (!header)
    return binding;
  TextureFetch fetch = TextureFetch::FromHeader(header->Format);
  // SetTexture copied the header into the device's fetch constant. A header
  // that no longer matches it was a temporary the game has overwritten since
  // (a command buffer runs after its recording thread's stack moved on); the
  // device's copy is what the GPU reads.
  if (!fetch.IsTexture() || fetch.BasePage() != device_fetch.BasePage())
    fetch = device_fetch;
  if (!fetch.IsTexture())
    return binding;

  u32 desc[6];
  MaskedDesc(fetch, desc);
  // banjo-nx (2026-10-18, the one-frame colour flashes): a header read while the game rewrites it (Ryujinx run 34: a
  // bloom level's header with dwords 3-5 still zero, read as a 1D texture of other data) dropped the resolve's texture
  // and made one from guest memory. The device's copy, taken at SetTexture, is what the GPU reads: it wins whenever the
  // two describe different data.
  if (device_fetch.IsTexture() && std::memcmp(fetch.dword, device_fetch.dword, sizeof(fetch.dword)) != 0) {
    u32 device_desc[6];
    MaskedDesc(device_fetch, device_desc);
    if (!SameData(desc, device_desc)) {
      static uint32_t logged = 0;
      if (logged++ < 16)
        RGPU_INFO("[flash] frame {}: texture {:08X} bound by the device's fetch constant: its header describes other "
                  "data (desc {:08X} {:08X} {:08X} vs the device's {:08X} {:08X} {:08X})",
                  g_frame, texture_va, desc[1], desc[2], desc[5], device_desc[1], device_desc[2], device_desc[5]);
      fetch = device_fetch;
      std::memcpy(desc, device_desc, sizeof(desc));
    } else {
      // banjo-nx (2026-10-20, the colour flash, banjo_renut_device_swizzle): the same data, other described fields.
      // SetTexture copies dwords 0, 3 and 5 of the header into the device's fetch constant as they are (signs, pitch,
      // tiling; swizzle, number format, exponent adjust; dimension, packed mips, mip address) and changes only dword
      // 4's mip levels (the sampler's MinMipLevel / MaxMipLevel; d3d_native_state.cpp SetTexture): a header that
      // differs in 0, 3 or 5 is stale. Games build headers as stack temporaries (RenderDoc capture of Ryujinx, frame
      // 5437: a bloom level's header rewritten with the swizzle 1,1,0,0 turned the whole frame yellow); the device's
      // copy is what the GPU reads. The periodic report counts them ("[flash] header vs device", by dword).
      u32 differ = 0;
      for (int i = 0; i < 6; ++i)
        differ |= desc[i] != device_desc[i] ? 1u << i : 0u;
      if (differ) {
        banjo::renut_gpu::CountHeaderDescDiffer(differ);
        if (fetch.Swizzle() != device_fetch.Swizzle())
          banjo::renut_gpu::CountDeviceSwizzle(uint32_t(g_frame));
        if (differ & 0x29u) {
          static uint32_t logged = 0;
          if (logged++ < 16)
            RGPU_INFO("[flash] frame {}: texture {:08X} sampled through the device's fetch constant{}: its header (same "
                      "data) is stale: dword0 {:08X}/{:08X} dword3 {:08X}/{:08X} dword5 {:08X}/{:08X} (header/device)",
                      g_frame, texture_va, banjo::renut_gpu::DeviceSwizzleOn() ? "" : " NOT (key off)", desc[0],
                      device_desc[0], desc[3], device_desc[3], desc[5], device_desc[5]);
          if (banjo::renut_gpu::DeviceSwizzleOn()) {
            fetch = device_fetch;
            std::memcpy(desc, device_desc, sizeof(desc));
          }
        }
      }
    }
  }
  const u32 swizzle = fetch.Swizzle();
  if (HostTexture *resolved = ResolvedAlias(texture_va, fetch, desc)) {
    // me2-nx-deko (2026-10-27, me2_renut_ui_fsr): in the frame's final pass, the HDR scene's FSR copy at its scale.
    if (UiPhaseLocked() && swizzle == resolved->view_swizzle)
      resolved = UiFsrSceneLocked(resolved);
    resolved->used_frame = g_frame;
    if (resolved->words)
      banjo::renut_gpu::CountDepthFloatSampled();  // banjo-nx (2026-10-18): a depth resolve's float target sampled
    TransitionTextureLocked(resolved, plume::RenderTextureLayout::SHADER_READ);
    if (fx) {
      fx->transition[0] = resolved;
      fx->float_sampled = resolved->words != nullptr;
    }
    binding.dimension = resolved->dimension;
    binding.slot = SlotForSwizzle(*resolved, swizzle);
    // banjo-nx (2026-10-19, banjo_deko3d_depth_handover): its content where it is.
    if (resolved->handed_from || resolved->held_current) {
      const uint32_t handed = DepthHandOverSlotLocked(
          *resolved, SwizzleMapping(swizzle, resolved->view_swap_red_blue, resolved->host_format), fx);
      if (handed != UINT32_MAX)
        binding.slot = handed;
    }
    binding.stacked = resolved->stacked;
    binding.scale = resolved->scale;
    binding.lod_scale = resolved->extra_levels ? resolved->scale : RenderScale{};
    return binding;
  }
  auto &slot = TextureSlot(texture_va);
  // A resolve destination sampled through a fetch constant that differs from
  // its header only in fields that leave the data alone (the mip range, say)
  // keeps what the resolve wrote.
  if (slot && std::memcmp(slot->desc, desc, sizeof(desc)) != 0 &&
      !(slot->gpu_written && SameData(slot->desc, desc))) {
    // banjo-nx (2026-10-18, the one-frame colour flashes): a texture only the GPU has the content of, thrown away for a
    // fetch constant that reads the memory otherwise; the new one is filled from guest memory, which the resolve never
    // wrote (zero: green through the game's colour encoding on deko3d; stale on Vulkan).
    if (slot->gpu_written) {
      static uint32_t logged = 0;
      if (logged++ < 64)
        RGPU_WARN("[flash] frame {}: resolve destination {:08X} ({}x{} fmt {}) dropped for a fetch of other data "
                  "(desc {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
                  g_frame, texture_va, slot->width, slot->height, u32(slot->guest_format), slot->desc[0],
                  slot->desc[1], slot->desc[2], slot->desc[3], slot->desc[4], slot->desc[5], desc[0], desc[1], desc[2],
                  desc[3], desc[4], desc[5]);
    }
    Retire(*slot);
    slot.reset();
  }
  // What a depth resolve wrote to the memory (only the GPU has it), which is
  // at the resolve's scale; guest data is at the guest size.
  HostTexture *depth_source = !(slot && slot->gpu_written) ? DepthWordsSource(fetch) : nullptr;
  if (depth_source) {
    EnsureDepthWordsLocked(*depth_source);  // banjo-nx (banjo_renut_depth_words_lazy)
    if (fx)
      fx->depth_source = true;  // banjo-nx (2026-10-19, render-thread proposal 3)
  }
  const RenderScale scale = depth_source ? depth_source->scale : RenderScale{};
  if (slot && !slot->gpu_written && slot->scale != scale) {
    Retire(*slot);
    slot.reset();
  }
  if (!slot) {
    // banjo-nx (2026-10-18, the colour flashes): memory a resolve wrote, sampled as other data than the resolve's
    // (ResolvedAlias said no) and not as its depth words: this texture is made from guest memory, which only has what
    // the CPU wrote there.
    if (!depth_source) {
      auto it = g_resolve_targets.find(fetch.BasePage());
      if (it != g_resolve_targets.end() && it->second && it->second->texture) {
        static uint32_t logged = 0;
        const HostTexture &r = *it->second;
        if (logged++ < 64)
          RGPU_WARN("[flash] frame {}: texture {:08X} (format {}) made from guest memory over resolve destination "
                    "{:08X} ({}x{} format {}; desc {:08X} {:08X} {:08X} vs {:08X} {:08X} {:08X})",
                    g_frame, texture_va, u32(fetch.Format()), r.guest_va, r.width, r.height, u32(r.guest_format),
                    r.desc[1], r.desc[2], r.desc[5], desc[1], desc[2], desc[5]);
      }
    }
    slot = std::make_unique<HostTexture>();
    slot->guest_va = texture_va;
    std::memcpy(slot->desc, desc, sizeof(desc));
    if (!Create(*slot, fetch, false, plume::RenderFormat::UNKNOWN, false, scale)) {
      static uint32_t logged = 0;
      if (logged++ < 16) {
        RGPU_WARN("Texture {:08X}: format {} {}x{}x{} not supported", texture_va,
                  u32(fetch.Format()), slot->width, slot->height, slot->depth);
      }
      Retire(*slot);
      return binding;  // keep the entry so it is not retried every draw
    }
  }
  HostTexture &t = *slot;
  if (banjo::renut_gpu::ResolveTraceOn() && t.gpu_written && t.trace_frame != g_frame) {
    t.trace_frame = g_frame;  // banjo-nx (package 28): banjo_renut_diag_resolve_trace
    RGPU_INFO("[trace] sample resolved {:08X} ({}x{} levels {} fmt {})", texture_va, t.width, t.height,
              t.extra_levels ? 1 + t.extra_levels : 1, u32(t.host_format));
  }
  // One that could not be made is not marked used: it goes once idle, and the
  // next bind tries again (the bindless table may have room by then).
  if (!t.texture)
    return binding;
  t.used_frame = g_frame;

  if (depth_source && !t.gpu_written) {
    // banjo-nx (banjo_gpu_depth_words_view, package 28): the resolve's words themselves, not a copy.
    if (const uint32_t words_slot = WordsViewSlot(t, *depth_source, swizzle); words_slot != UINT32_MAX) {
      t.copied_resolve = 0;  // its own texture has not got them: copied in full if the view is not used later
      TransitionTextureLocked(depth_source->words.get(), plume::RenderTextureLayout::SHADER_READ);
      banjo::renut_gpu::CountDepthWords(true);
      if (fx) {
        fx->transition[0] = depth_source->words.get();
        fx->words_viewed = true;
      }
      if (banjo::renut_gpu::ResolveTraceOn() && t.trace_frame != g_frame) {
        t.trace_frame = g_frame;  // banjo-nx (2026-10-19): which resolves' words are read, by the view
        RGPU_INFO("[trace] sample depth words (view) {:08X} ({}x{}) from the resolve of {:08X}", texture_va, t.width,
                  t.height, depth_source->guest_va);
      }
      binding.dimension = t.dimension;
      binding.slot = words_slot;
      binding.stacked = false;
      binding.scale = t.scale;
      binding.lod_scale = RenderScale{};
      return binding;
    }
    banjo::renut_gpu::CountDepthWords(false);
    if (banjo::renut_gpu::ResolveTraceOn() && t.trace_frame != g_frame) {
      t.trace_frame = g_frame;  // banjo-nx (2026-10-18): which header samples a depth resolve's words
      RGPU_INFO("[trace] sample depth words {:08X} ({}x{}) from the resolve of {:08X}", texture_va, t.width, t.height,
                depth_source->guest_va);
    }
    CopyDepthWords(t, *depth_source);
  } else if (!t.gpu_written && t.checked_frame != g_frame) {
    t.checked_frame = g_frame;
    if (!fetch.Tiled()) {
      RefreshLinearLocked(t, fetch);
    } else {
      const uint64_t t0 = NowUs();  // me2-nx-deko (2026-10-31): the upload work, timed
      const uint64_t hash = CheckedHash(fetch, t, true);  // banjo-nx (2026-10-19, render-thread proposal 1)
      bool worked = false;
      if (!t.uploaded || hash != t.content_hash) {
        worked = true;
        t.content_hash = hash;
        if (!t.uploaded && Streamable(t)) {
          t.uploaded = UploadProgressive(t, fetch);  // me2_renut_upload_budget_kb
        } else {
          if (t.uploaded) ++g_stream_stats.changed_whole;
          else ++g_stream_stats.new_whole, g_stream_stats.bytes_whole += TextureBytes(t);
          t.resident_from = 0;  // whole: any queued levels are moot (StreamPumpLocked drops the entry)
          t.stream_id = 0;
          // me2-nx-deko (2026-11-06): an HD icon whose guest bytes changed is made again at the guest's size first.
          if (t.hd_icon) {
            const u32 slot_va = t.guest_va;
            Retire(t);
            t.hd_icon = false;
            t.hd_count = 0;
            t.replaced_icon = false;
            if (!Create(t, fetch, false, plume::RenderFormat::UNKNOWN, false, RenderScale{})) {
              RGPU_WARN("[icons] texture {:08X}: could not be made again after its HD icon", slot_va);
              return binding;
            }
          }
          t.uploaded = Upload(t, fetch, false);
          if (t.uploaded && t.hd_count && HdIconsOn())
            UpgradeToHdIcon(t, fetch);
          BudgetFrame();
          g_budget_used += TextureBytes(t);
        }
      }
      if (worked) NoteWork(NowUs() - t0);
    }
    // banjo-nx (2026-10-19, the colour flash): a texture neither a resolve nor an upload filled is sampled with what
    // its new image's memory held (deko3d does not clear it).
    if (!t.gpu_written && !t.uploaded) {
      static uint32_t logged = 0;
      if (logged++ < 32)
        RGPU_WARN("[flash] frame {}: texture {:08X} ({}x{} format {}{}) sampled with nothing written to it (the "
                  "upload from guest memory failed)",
                  g_frame, texture_va, t.width, t.height, u32(fetch.Format()), fetch.Tiled() ? ", tiled" : "");
    }
  }
  // me2-nx-deko (2026-10-27, me2_renut_ui_fsr): in the frame's final pass, the HDR scene's FSR copy at its scale (the
  // scene resolved into the texture the tonemap samples, the usual case; ResolvedAlias above is the other).
  if (t.gpu_written && UiPhaseLocked() && swizzle == t.view_swizzle) {
    if (HostTexture *fsr = UiFsrSceneLocked(&t); fsr != &t) {
      TransitionTextureLocked(fsr, plume::RenderTextureLayout::SHADER_READ);
      if (fx) {
        fx->transition[0] = fsr;
        fx->float_sampled = false;
      }
      binding.dimension = fsr->dimension;
      binding.slot = fsr->slot;
      binding.stacked = false;
      binding.scale = fsr->scale;
      binding.lod_scale = RenderScale{};
      return binding;
    }
  }
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
  if (t.words)
    banjo::renut_gpu::CountDepthFloatSampled();  // banjo-nx (2026-10-18): a depth resolve's float target sampled
  if (fx) {
    fx->transition[0] = &t;
    fx->float_sampled = t.words != nullptr;
  }
  binding.dimension = t.dimension;
  binding.slot = SlotForSwizzle(t, swizzle);
  // banjo-nx (2026-10-19, banjo_deko3d_depth_handover): its content where it is.
  if (t.handed_from || t.held_current) {
    const uint32_t handed = DepthHandOverSlotLocked(
        t, SwizzleMapping(swizzle, t.view_swap_red_blue, t.host_format), fx);
    if (handed != UINT32_MAX)
      binding.slot = handed;
  }
  binding.stacked = t.stacked;
  binding.scale = t.scale;
  binding.lod_scale = t.hd_icon ? t.hd_scale : t.extra_levels ? t.scale : RenderScale{};  // me2-nx-deko: HD icons
  binding.min_level = t.resident_from;  // me2-nx-deko (2026-10-31): the levels it has yet
  binding.smooth_minify = t.replaced_icon;  // me2-nx-deko (2026-11-06)
  return binding;
}

void TextureGuestExtent(const TextureFetch &fetch, uint32_t pages[2], uint32_t sizes[2]) {
  // Memo over the fetch constant: while memexport results are pending, every
  // draw asks for its textures' extents (DrawNeedsExportFlush), and the layout
  // math cost more than the rest of the check.
  struct Entry {
    uint32_t key[6];
    uint32_t pages[2];
    uint32_t sizes[2];
    bool valid;
  };
  static_assert(sizeof(fetch.dword) == sizeof(Entry::key));
  // banjo-nx (2026-10-19, render-thread proposal 4): a plain static table of 256 (12 KB): both callers run on the render
  // thread (the draw's export check, KnowGuestExtent), and the thread-local one (48 KB, more than the A57's L1) cost a
  // call to read the thread pointer at every access (-mtp=soft).
  static Entry memo[256] = {};
  uint32_t key[6];
  std::memcpy(key, fetch.dword, sizeof(key));
  uint32_t hash = 0;
  for (uint32_t word : key)
    hash = (hash ^ word) * 0x9E3779B1u;
  Entry &entry = memo[hash >> 24];
  if (entry.valid && std::memcmp(entry.key, key, sizeof(key)) == 0) {
    std::memcpy(pages, entry.pages, sizeof(entry.pages));
    std::memcpy(sizes, entry.sizes, sizeof(entry.sizes));
    return;
  }

  uint32_t base_bytes, mip_bytes;
  GetTextureDataSizes(fetch, base_bytes, mip_bytes);
  pages[0] = fetch.BasePage();
  sizes[0] = base_bytes;
  pages[1] = mip_bytes ? fetch.MipPage() : 0;
  sizes[1] = mip_bytes;
  std::memcpy(entry.key, key, sizeof(key));
  std::memcpy(entry.pages, pages, sizeof(entry.pages));
  std::memcpy(entry.sizes, sizes, sizeof(entry.sizes));
  entry.valid = true;
}

HostTexture *GetResolveTextureLocked(u32 texture_va, plume::RenderFormat format, RenderScale scale,
                                     const d3d::TextureFetchConstant *header_format) {
  TextureFetch fetch;
  return ResolveTarget(texture_va, format, scale, header_format, false, fetch);
}

HostTexture *GetDepthResolveTextureLocked(u32 texture_va, RenderScale scale,
                                          const d3d::TextureFetchConstant *header_format) {
  TextureFetch fetch;
  HostTexture *t =
      ResolveTarget(texture_va, kDepthResolveFormat, scale, header_format, true, fetch);
  if (!t)
    return nullptr;
  if (!t->words) {
    auto words = std::make_unique<HostTexture>();
    words->width = t->width;
    words->height = t->height;
    words->scale = t->scale;
    words->alloc_width = t->alloc_width;
    words->alloc_height = t->alloc_height;
    words->host_format = kDepthResolveWordsFormat;
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
        t->alloc_width, t->alloc_height, 1, kDepthResolveWordsFormat);
    desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
    plume::RenderDevice *device = Video::Device();
    words->texture = device->createTexture(desc);
    if (!TextureCreated(words->texture.get())) {
      ReportDeviceLost(device);
      return nullptr;
    }
    words->texture->setName("renut depth resolve words");
    const plume::RenderTexture *targets[] = {t->texture.get(), words->texture.get()};
    t->depth_framebuffer = device->createFramebuffer(plume::RenderFramebufferDesc(targets, 2));
    if (!t->depth_framebuffer) {
      Video::RetireLocked(std::move(words->texture));
      return nullptr;
    }
    if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
      TransitionTextureLocked(words.get(), plume::RenderTextureLayout::COLOR_WRITE);
      cmd->discardTexture(words->texture.get());
    }
    t->words = std::move(words);
  }
  HostTexture *&words_entry = g_depth_words[fetch.BasePage()];
  if (words_entry != t) {
    words_entry = t;
    ++g_texture_epoch;  // banjo-nx (2026-10-19, render-thread proposal 3): DepthWordsSource's answer changes
  }
  ++g_depth_epoch;  // banjo-nx (2026-10-19): the words of this destination may change with this resolve
  ++t->resolves;
  return t;
}

namespace {

// Single-level color targets the host levels are drawn into (plume's barriers
// cover a whole texture, so a level cannot be drawn while level 0 of the same
// texture is sampled) and multisampled resolves staged in: a bare target
// (texture, layout, size) and its framebuffer, by format and size. Kept for
// reuse.
struct ScratchTarget {
  HostTexture target;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  // Its bindless slot, for the next level down to read (ScratchSlot).
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = UINT32_MAX;
};

uint32_t ScratchSlot(ScratchTarget &scratch) {
  if (scratch.slot != UINT32_MAX)
    return scratch.slot;
  scratch.view = scratch.target.texture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(scratch.target.host_format));
  if (!scratch.view)
    return kInvalidDescriptor;
  scratch.slot = Video::AllocateTextureSlotLocked();
  if (scratch.slot != kInvalidDescriptor)
    Video::SetTextureSlotLocked(scratch.slot, scratch.target.texture.get(), scratch.view.get());
  return scratch.slot;
}
std::unordered_map<uint64_t, std::unique_ptr<ScratchTarget>> g_scratch_targets;
// The level pipelines, by render target format.
std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> g_mip_pipelines;

std::unique_ptr<ScratchTarget> MakeScratchTarget(plume::RenderFormat format, uint32_t width, uint32_t height) {
  auto scratch = std::make_unique<ScratchTarget>();
  HostTexture &t = scratch->target;
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, format);
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  plume::RenderDevice *device = Video::Device();
  t.texture = device->createTexture(desc);
  if (!TextureCreated(t.texture.get())) {
    t.texture.reset();
    ReportDeviceLost(device);
    return nullptr;
  }
  t.texture->setName("renut scratch target");
  t.host_format = format;
  t.width = t.alloc_width = width;
  t.height = t.alloc_height = height;
  const plume::RenderTexture *targets[] = {t.texture.get()};
  scratch->framebuffer = device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
  if (!scratch->framebuffer) {
    Video::RetireLocked(std::move(t.texture));
    return nullptr;
  }
  // Like a resolve destination, defined before anything writes part of it.
  if (plume::RenderCommandList *cmd = Video::CommandListLocked()) {
    TransitionTextureLocked(&t, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->discardTexture(t.texture.get());
  }
  return scratch;
}

ScratchTarget *GetScratchTarget(plume::RenderFormat format, uint32_t width, uint32_t height) {
  const uint64_t key = uint64_t(format) << 40 | uint64_t(width) << 20 | height;
  auto &entry = g_scratch_targets[key];
  if (!entry)
    entry = MakeScratchTarget(format, width, height);
  return entry.get();
}

plume::RenderPipeline *MipPipeline(plume::RenderFormat format) {
  auto &pipeline = g_mip_pipelines[uint32_t(format)];
  if (pipeline)
    return pipeline.get();
  auto &s = State();
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.utility_layout.get();
  desc.vertexShader = s.present_vs.get();
  desc.pixelShader = s.texture_mip_ps.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  pipeline = s.device->createGraphicsPipeline(desc);
  if (!pipeline) {
    RGPU_ERROR("Texture mip pipeline creation failed (format {})", uint32_t(format));
    return nullptr;
  }
  pipeline->setName("renut texture mip");
  return pipeline.get();
}

// Level 0 of 't' without the fetch swizzle, in the texture table.
uint32_t Level0Slot(HostTexture &t) {
  if (t.level0_slot != UINT32_MAX)
    return t.level0_slot;
  plume::RenderTextureViewDesc desc = t.view_desc;
  desc.mipSlice = 0;
  desc.mipLevels = 1;
  desc.componentMapping = plume::RenderComponentMapping();
  t.level0_view = t.texture->createTextureView(desc);
  if (!t.level0_view)
    return kInvalidDescriptor;
  t.level0_slot = Video::AllocateTextureSlotLocked();
  if (t.level0_slot != kInvalidDescriptor)
    Video::SetTextureSlotLocked(t.level0_slot, t.texture.get(), t.level0_view.get());
  return t.level0_slot;
}

} // namespace

HostTexture *ResolveStagingLocked(const HostTexture *dest) {
  ScratchTarget *scratch = GetScratchTarget(dest->host_format, dest->alloc_width, dest->alloc_height);
  return scratch ? &scratch->target : nullptr;
}

bool ScaleTextureRegionLocked(HostTexture *texture, const plume::RenderRect &rect, float scale) {
  auto &s = State();
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!texture || !texture->texture || !texture->render_target || texture->dimension != TextureDimension::Tex2D ||
      texture->stacked || !cmd || !s.resolve_copy_ps || rect.right <= rect.left || rect.bottom <= rect.top)
    return false;
  HostTexture &t = *texture;
  ScratchTarget *scratch = GetScratchTarget(t.host_format, t.alloc_width, t.alloc_height);
  const uint32_t slot = scratch ? ScratchSlot(*scratch) : kInvalidDescriptor;
  if (slot == kInvalidDescriptor)
    return false;
  static const uint32_t kProfileLabel = gpu_profile::Label("resolves: exponent bias");  // me2-nx-deko (2026-10-28)
  gpu_profile::Mark(s, kProfileLabel);
  auto &pipeline = s.resolve_copy_pipelines[uint32_t(t.host_format)];  // the same as DrawResolveCopy's
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = s.present_vs.get();
    desc.pixelShader = s.resolve_copy_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = t.host_format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return false;
    pipeline->setName("renut resolve copy");
  }
  if (!t.copy_framebuffer) {
    const plume::RenderTexture *targets[] = {t.texture.get()};
    t.copy_framebuffer = s.device->createFramebuffer(plume::RenderFramebufferDesc(targets, 1));
    if (!t.copy_framebuffer)
      return false;
  }
  // The region as it is into the scratch target, then back scaled.
  TransitionTextureLocked(&t, plume::RenderTextureLayout::COPY_SOURCE);
  TransitionTextureLocked(&scratch->target, plume::RenderTextureLayout::COPY_DEST);
  const plume::RenderBox box(rect.left, rect.top, rect.right, rect.bottom);
  cmd->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(scratch->target.texture.get()),
                         plume::RenderTextureCopyLocation::Subresource(t.texture.get(), 0, 0), uint32_t(rect.left),
                         uint32_t(rect.top), 0, &box);
  TransitionTextureLocked(&scratch->target, plume::RenderTextureLayout::SHADER_READ);
  TransitionTextureLocked(&t, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(t.copy_framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(t.alloc_width), float(t.alloc_height)));
  cmd->setScissors(rect);
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.texture_index = slot;
  push.sampler_index = kPointClampSamplerSlot;
  push.param0 = scale;
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  return true;
}

// me2-nx-deko (2026-10-27, me2_renut_fxaa / me2_renut_ui_fsr): the HDR scene the frame's final pass reads, anti-aliased
// with FXAA at its own resolution and upscaled with FSR 1 EASU to the final pass's.
namespace {
// One target per role, size and format (the final pass reads one scene a frame): which source it holds, of which frame.
struct UiSceneTarget {
  std::unique_ptr<ScratchTarget> target;
  const HostTexture *source = nullptr;
  uint64_t frame = 0;
};
std::unordered_map<uint64_t, UiSceneTarget> g_ui_scene_targets;
bool g_utility_pass_drawn = false;

// 'source' drawn by 'shader' (present_vs's fullscreen triangle, linear clamp sampler, param0 / param1 the output size)
// into the role's target of 'width' x 'height' at 'scale', once a frame; 'source' itself when that cannot be done.
HostTexture *UiScenePassLocked(uint32_t role, HostTexture *source, plume::RenderShader *shader,
                               std::unique_ptr<plume::RenderPipeline> &pipeline, const char *name, uint32_t width,
                               uint32_t height, RenderScale scale) {
  auto &s = State();
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd || !shader)
    return source;
  UiSceneTarget &entry = g_ui_scene_targets[uint64_t(role) << 56 | uint64_t(source->host_format) << 40 |
                                            uint64_t(width) << 20 | height];
  if (entry.frame == g_frame)
    return entry.source == source ? &entry.target->target : source;  // another scene took it this frame
  if (entry.target && entry.target->target.slot == kInvalidDescriptor)
    return source;
  if (!entry.target) {
    entry.target = MakeScratchTarget(source->host_format, width, height);
    if (!entry.target)
      return source;
    HostTexture &t = entry.target->target;
    t.texture->setName(name);
    t.render_target = true;
    t.dimension = TextureDimension::Tex2D;
    t.width = source->width;  // guest size, as a resolve destination's
    t.height = source->height;
    t.scale = scale;
    // Its base view: identity. The pass reads the source through the source's base view (its swizzle applied), so the
    // copy holds what that view shows; FillTextures substitutes it only for that swizzle (t.view_swizzle).
    t.slot = ScratchSlot(*entry.target);
    if (t.slot == kInvalidDescriptor)
      return source;
  }
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = s.present_vs.get();
    desc.pixelShader = shader;
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = source->host_format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return source;
    pipeline->setName(name);
  }
  HostTexture &t = entry.target->target;
  t.view_swizzle = source->view_swizzle;
  TransitionTextureLocked(source, plume::RenderTextureLayout::SHADER_READ);
  TransitionTextureLocked(&t, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(entry.target->framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.texture_index = source->slot;
  push.sampler_index = kLinearClampSamplerSlot;
  push.param0 = float(width);
  push.param1 = float(height);
  if (shader == s.fsr_easu_ps.get()) {
    const EasuPush easu{push, MakeEasuConstants(source->alloc_width, source->alloc_height, width, height)};
    cmd->setGraphicsPushConstants(0, &easu, 0, sizeof(easu));
  } else {
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  }
  cmd->drawInstanced(3, 1, 0, 0);
  g_utility_pass_drawn = true;
  entry.source = source;
  entry.frame = g_frame;
  t.used_frame = g_frame;
  return &t;
}
}  // namespace

// me2-nx-deko (2026-10-28, me2_renut_ui_tonemap_low): FXAA of a surface's scene-scale target (descriptor 'slot', the
// top-left 'width' x 'height' host pixels: the shader reads 1:1 by pixel position) into a texture of exactly that size,
// which EASU then takes as its whole input (draw.cpp UiUpscaleSurfaceLocked). Null when it cannot be drawn.
HostTexture *UiFxaaSurfaceLocked(uint32_t slot, plume::RenderFormat format, uint32_t width, uint32_t height,
                                 RenderScale scale) {
  auto &s = State();
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!cmd || !s.fxaa_ps || slot == kInvalidDescriptor || !width || !height)
    return nullptr;
  UiSceneTarget &entry = g_ui_scene_targets[uint64_t(2) << 56 | uint64_t(format) << 40 | uint64_t(width) << 20 | height];
  if (entry.target && entry.target->target.slot == kInvalidDescriptor)
    return nullptr;
  if (!entry.target) {
    entry.target = MakeScratchTarget(format, width, height);
    if (!entry.target)
      return nullptr;
    HostTexture &t = entry.target->target;
    t.texture->setName("renut final pass tonemap (FXAA)");
    t.render_target = true;
    t.dimension = TextureDimension::Tex2D;
    t.width = width;
    t.height = height;
    t.scale = scale;
    t.slot = ScratchSlot(*entry.target);
    if (t.slot == kInvalidDescriptor)
      return nullptr;
  }
  static std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines;
  auto &pipeline = pipelines[uint32_t(format)];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.utility_layout.get();
    desc.vertexShader = s.present_vs.get();
    desc.pixelShader = s.fxaa_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return nullptr;
    pipeline->setName("renut final pass tonemap (FXAA)");
  }
  HostTexture &t = entry.target->target;
  TransitionTextureLocked(&t, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(entry.target->framebuffer.get());
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  cmd->setGraphicsPipelineLayout(s.utility_layout.get());
  cmd->setPipeline(pipeline.get());
  cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
  UtilityPush push{};
  push.scale[0] = push.scale[1] = 1.0f;
  push.texture_index = slot;
  push.sampler_index = kLinearClampSamplerSlot;
  push.param0 = float(width);
  push.param1 = float(height);
  cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  g_utility_pass_drawn = true;
  entry.frame = g_frame;
  t.used_frame = g_frame;
  return &t;
}

bool TakeUtilityPassDrawnLocked() {
  const bool drawn = g_utility_pass_drawn;
  g_utility_pass_drawn = false;
  return drawn;
}

HostTexture *UiFsrSceneLocked(HostTexture *source) {
  static const bool fsr = REXCVAR_GET(me2_renut_ui_fsr);
  static const bool fxaa = REXCVAR_GET(me2_renut_fxaa);
  if ((!fsr && !fxaa) || !UiPhaseLocked())
    return source;
  const RenderScale ui = UiScaleLocked();
  // The scene's resolve: at the render resolution (coarser than the final pass), the screen's guest size, 64-bit float
  // (UE3's HDR scene copy; the LDR targets of the final pass itself are 8-bit).
  if (source->scale != ResolutionScale() || !(ui.Factor() > source->scale.Factor()) || !source->render_target ||
      source->dimension != TextureDimension::Tex2D || source->stacked || source->words ||
      source->host_format != plume::RenderFormat::R16G16B16A16_FLOAT || source->width != GuestScreenWidth() ||
      source->height != 720 || source->slot == UINT32_MAX || source->view_swap_red_blue) {
    static uint32_t logged = 0;
    if (source->width == GuestScreenWidth() && source->height == 720 && logged++ < 8)
      RGPU_INFO("[me2-ui] FSR/FXAA skip screen-sized texture {:08X}: scale {:.3g}x (scene {:.3g}x), host format {}, "
                "render target {}, words {}", source->guest_va, source->scale.Factor(), ResolutionScale().Factor(),
                u32(source->host_format), source->render_target, source->words != nullptr);
    return source;
  }
  // me2-nx-deko (2026-11-06, me2_renut_color_handover): the passes below read the scene's own texture.
  if (source->handed_from || source->held_current)
    HandOverMaterialiseLocked(*source);
  auto &s = State();
  HostTexture *scene = source;
  if (fxaa)
    scene = UiScenePassLocked(0, scene, s.fxaa_ps.get(), s.fxaa_pipeline, "renut final pass scene (FXAA)",
                              source->alloc_width, source->alloc_height, source->scale);
  if (fsr)
    scene = UiScenePassLocked(1, scene, s.fsr_easu_ps.get(), s.fsr_easu_pipeline, "renut final pass scene (FSR)",
                              ui.Size(source->width), ui.Size(source->height), ui);
  static uint32_t logged = 0;
  if (scene != source && logged++ < 4)
    RGPU_INFO("[me2-ui] scene {:08X} {}x{}{}{} -> {}x{} for the final pass", source->guest_va, source->alloc_width,
              source->alloc_height, fxaa ? " FXAA" : "", fsr ? " FSR 1 EASU" : "", scene->alloc_width,
              scene->alloc_height);
  return scene;
}

void UpdateResolveMipsLocked(HostTexture *texture, const plume::RenderRect &rect) {
  auto &s = State();
  plume::RenderCommandList *cmd = Video::CommandListLocked();
  if (!texture || !texture->texture || !texture->extra_levels || !cmd || !s.texture_mip_ps)
    return;
  HostTexture &t = *texture;
  plume::RenderPipeline *pipeline = MipPipeline(t.host_format);
  const uint32_t source_slot = Level0Slot(t);
  if (!pipeline || source_slot == kInvalidDescriptor)
    return;
  static const uint32_t kProfileLabel = gpu_profile::Label("resolve host mip levels");
  gpu_profile::Mark(s, kProfileLabel);

  // Each level is the 2x2 box of the one above: level 1 of level 0, the rest
  // of the previous level's scratch target. The rect is first widened to
  // whole texels of the smallest level, so each level's region is exactly
  // what the next one reads (the scratch targets are shared, and hold other
  // textures' texels outside what was drawn now).
  struct Level {
    ScratchTarget *scratch;
    plume::RenderRect region;
  };
  Level levels[kMaxExtraLevels] = {};
  const uint32_t count = std::min(t.extra_levels, uint32_t(std::size(levels)));
  const int32_t align = 1 << count;
  const plume::RenderRect aligned(rect.left & ~(align - 1), rect.top & ~(align - 1),
                                  (rect.right + align - 1) & ~(align - 1),
                                  (rect.bottom + align - 1) & ~(align - 1));
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
  uint32_t source = source_slot;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t level = i + 1;
    const int32_t span = 1 << level;
    const uint32_t width = std::max(1u, t.alloc_width >> level);
    const uint32_t height = std::max(1u, t.alloc_height >> level);
    Level &l = levels[i];
    l.region = plume::RenderRect(std::min(aligned.left >> level, int32_t(width)),
                                 std::min(aligned.top >> level, int32_t(height)),
                                 std::min((aligned.right + span - 1) >> level, int32_t(width)),
                                 std::min((aligned.bottom + span - 1) >> level, int32_t(height)));
    if (l.region.right <= l.region.left || l.region.bottom <= l.region.top)
      break;
    l.scratch = GetScratchTarget(t.host_format, width, height);
    if (!l.scratch || source == kInvalidDescriptor) {
      l.scratch = nullptr;
      break;
    }
    if (i > 0) {
      // The level above, drawn just now, is read.
      TransitionTextureLocked(&levels[i - 1].scratch->target, plume::RenderTextureLayout::SHADER_READ);
    }
    TransitionTextureLocked(&l.scratch->target, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->setFramebuffer(l.scratch->framebuffer.get());
    cmd->setGraphicsPipelineLayout(s.utility_layout.get());
    cmd->setPipeline(pipeline);
    cmd->setGraphicsDescriptorSet(s.texture_set.get(), 0);
    cmd->setGraphicsDescriptorSet(s.sampler_set.get(), 1);
    // Pixel coordinates are texels of the level.
    cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
    cmd->setScissors(l.region);
    UtilityPush push{};
    push.scale[0] = push.scale[1] = 1.0f;
    push.texture_index = source;
    push.sampler_index = kPointClampSamplerSlot;
    push.param0 = 2.0f;
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    cmd->drawInstanced(3, 1, 0, 0);
    source = ScratchSlot(*l.scratch);
  }

  TransitionTextureLocked(&t, plume::RenderTextureLayout::COPY_DEST);
  for (uint32_t i = 0; i < count; ++i) {
    const Level &l = levels[i];
    if (!l.scratch)
      continue;
    TransitionTextureLocked(&l.scratch->target, plume::RenderTextureLayout::COPY_SOURCE);
    const plume::RenderBox box(l.region.left, l.region.top, l.region.right, l.region.bottom);
    cmd->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(t.texture.get(), i + 1, 0),
        plume::RenderTextureCopyLocation::Subresource(l.scratch->target.texture.get()),
        uint32_t(l.region.left), uint32_t(l.region.top), 0, &box);
  }
  TransitionTextureLocked(&t, plume::RenderTextureLayout::SHADER_READ);
}

namespace {

// The host texture's levels in bytes, from its host format.
uint64_t HostTextureBytes(const HostTexture &t) {
  if (!t.texture)
    return 0;
  const uint32_t block = std::max(plume::RenderFormatBlockWidth(t.host_format), 1u);
  const uint64_t block_bytes = plume::RenderFormatSize(t.host_format);
  const uint32_t layers = t.stacked ? StackedLayers(t) : t.depth;
  uint64_t bytes = 0;
  for (uint32_t level = 0; level < t.mip_levels; ++level) {
    const uint64_t w = std::max(1u, t.alloc_width >> level);
    const uint64_t h = std::max(1u, t.alloc_height >> level);
    const uint64_t d = t.dimension == TextureDimension::Tex3D && !t.stacked
                           ? std::max(1u, t.depth >> level)
                           : layers;
    bytes += (w + block - 1) / block * ((h + block - 1) / block) * block_bytes * d;
  }
  if (t.words)
    bytes += HostTextureBytes(*t.words);
  return bytes;
}

} // namespace

TextureMemory TextureMemoryLocked() {
  TextureMemory usage;
  for (const auto &[va, t] : g_textures) {
    if (!t || !t->texture)
      continue;
    const uint64_t bytes = HostTextureBytes(*t);
    ++usage.textures;
    usage.bytes += bytes;
    if (t->render_target) {
      ++usage.resolve_targets;
      usage.resolve_bytes += bytes;
    }
  }
  return usage;
}

namespace {
uint32_t SamplerSlotUncached(const TextureFetch &fetch, RenderScale lod_scale, uint32_t min_level, bool smooth_minify);
} // namespace

uint32_t SamplerSlotLocked(const TextureFetch &fetch, RenderScale lod_scale, uint32_t min_level, bool smooth_minify) {
  // Every draw asks for each of its textures: remember the answer by the
  // fetch constant words the sampler state comes from (clamps in 0, filters,
  // anisotropy and LOD bias in 3, mip range in 4, border color in 5).
  struct Memo {
    bool valid = false;
    u32 words[4] = {};
    RenderScale lod_scale;
    uint32_t min_anisotropy = 1;
    uint32_t min_level = 0;
    bool smooth_minify = false;
    uint32_t slot = 0;
  };
  static Memo memo[256];
  const uint32_t min_anisotropy = MinAnisotropy();
  const u32 words[4] = {fetch.dword[0], fetch.dword[3], fetch.dword[4], fetch.dword[5]};
  Memo &entry = memo[(words[0] * 0x9E3779B1u ^ words[1] * 0x85EBCA6Bu ^ words[2] * 0xC2B2AE35u ^
                      words[3] ^ lod_scale.code * 0x27D4EB2Fu ^ min_level * 0x165667B1u ^
                      (smooth_minify ? 0x5BD1E995u : 0u)) >> 24];
  if (entry.valid && entry.lod_scale == lod_scale && entry.min_anisotropy == min_anisotropy &&
      entry.min_level == min_level && entry.smooth_minify == smooth_minify &&
      std::memcmp(entry.words, words, sizeof(words)) == 0)
    return entry.slot;
  const uint32_t slot = SamplerSlotUncached(fetch, lod_scale, min_level, smooth_minify);
  entry.valid = true;
  std::memcpy(entry.words, words, sizeof(words));
  entry.lod_scale = lod_scale;
  entry.min_anisotropy = min_anisotropy;
  entry.min_level = min_level;
  entry.smooth_minify = smooth_minify;
  entry.slot = slot;
  return slot;
}

namespace {

// The sampler slot for the fetch constant's sampler state, created the first
// time it is asked for.
uint32_t SamplerSlotUncached(const TextureFetch &fetch, RenderScale lod_scale, uint32_t min_level, bool smooth_minify) {
  auto address = [](TextureAddress mode) {
    switch (mode) {
    case TextureAddress::Wrap:
      return plume::RenderTextureAddressMode::WRAP;
    case TextureAddress::Mirror:
      return plume::RenderTextureAddressMode::MIRROR;
    case TextureAddress::Border:
    case TextureAddress::MirrorOnceBorder:
      return plume::RenderTextureAddressMode::BORDER;
    case TextureAddress::MirrorOnce:
    case TextureAddress::MirrorOnceBorderHalf:
      return plume::RenderTextureAddressMode::MIRROR_ONCE;
    default:
      return plume::RenderTextureAddressMode::CLAMP;
    }
  };
  auto filter = [](TextureFilter f) {
    return f == TextureFilter::Point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  };

  plume::RenderSamplerDesc desc;
  desc.addressU = address(fetch.Address(0));
  desc.addressV = address(fetch.Address(1));
  desc.addressW = address(fetch.Address(2));
  desc.magFilter = filter(fetch.MagFilter());
  desc.minFilter = filter(fetch.MinFilter());
  // me2-nx-deko (2026-11-06, TextureBinding::smooth_minify): the Switch button icons are shrunk linearly.
  if (smooth_minify)
    desc.minFilter = plume::RenderFilter::LINEAR;
  desc.mipmapMode = fetch.MipFilter() == TextureFilter::Point ? plume::RenderMipmapMode::NEAREST
                                                              : plume::RenderMipmapMode::LINEAR;
  // MaxAnisotropy stores 2..5 for ratios 2:1..16:1 (its table at 0x82003D68).
  const uint32_t aniso = fetch.Anisotropy();
  desc.anisotropyEnabled = aniso > 1 && aniso <= 5;
  desc.maxAnisotropy = desc.anisotropyEnabled ? (1u << (aniso - 1)) : 1;
  // renut_anisotropic_filtering: the least a mipmapped, linearly filtered
  // texture gets (render targets' host levels keep their own filtering).
  const uint32_t min_anisotropy = MinAnisotropy();
  if (min_anisotropy > desc.maxAnisotropy && lod_scale.code == 0 &&
      desc.minFilter == plume::RenderFilter::LINEAR &&
      desc.magFilter == plume::RenderFilter::LINEAR &&
      fetch.MipFilter() != TextureFilter::BaseMap && fetch.MaxMipLevel() > fetch.MinMipLevel()) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = min_anisotropy;
  }
  desc.minLOD = float(fetch.MinMipLevel());
  desc.maxLOD = fetch.MipFilter() == TextureFilter::BaseMap ? float(fetch.MinMipLevel())
                                                            : float(fetch.MaxMipLevel());
  desc.mipLODBias = float(fetch.LodBias()) / 32.0f;  // MipMapLodBias stores bias * 32
  // A resolve destination with host levels (HostTexture::extra_levels) has
  // the guest's one level: a filtered fetch reaches the host level whose
  // texel is the guest's, blending between them; a point fetch stays at the
  // guest texel it hits.
  if (lod_scale.Factor() > 1.0f) {
    desc.minLOD = 0.0f;
    if (desc.minFilter == plume::RenderFilter::LINEAR) {
      desc.maxLOD = std::log2(lod_scale.Factor());
      desc.mipmapMode = plume::RenderMipmapMode::LINEAR;
    } else {
      desc.maxLOD = 0.0f;
    }
  }
  // me2-nx-deko (2026-10-31, me2_renut_upload_budget_kb): a texture whose large levels are still on their way is
  // sampled from the levels it has (UE3's mip clamps above that floor are kept).
  if (min_level) {
    desc.minLOD = std::max(desc.minLOD, float(min_level));
    desc.maxLOD = std::max(desc.maxLOD, desc.minLOD);
  }
  // BorderColor stores 0 for a zero colour and 1 (white) for any other.
  switch (fetch.BorderColor()) {
  case 1:
    desc.borderColor = plume::RenderBorderColor::OPAQUE_WHITE;
    break;
  case 0:
    desc.borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
    break;
  default:
    desc.borderColor = plume::RenderBorderColor::OPAQUE_BLACK;
    break;
  }

  uint64_t key = uint64_t(desc.addressU) | uint64_t(desc.addressV) << 4 |
                 uint64_t(desc.addressW) << 8 | uint64_t(desc.magFilter) << 12 |
                 uint64_t(desc.minFilter) << 14 | uint64_t(desc.mipmapMode) << 16 |
                 uint64_t(desc.maxAnisotropy) << 18 | uint64_t(fetch.MinMipLevel()) << 24 |
                 uint64_t(fetch.MaxMipLevel()) << 28 | uint64_t(fetch.MipFilter()) << 32 |
                 uint64_t(uint32_t(fetch.LodBias()) & 0x3FF) << 34 | uint64_t(desc.borderColor) << 44 |
                 uint64_t(lod_scale.code) << 48 | uint64_t(min_level & 0xF) << 56;
  auto it = g_samplers.find(key);
  if (it != g_samplers.end())
    return it->second;

  auto &s = State();
  uint32_t slot = UINT32_MAX;
  for (uint32_t i = kFirstDynamicSamplerSlot; i < kSamplerDescriptorCount; ++i) {
    if (!s.sampler_slot_used[i]) {
      slot = i;
      break;
    }
  }
  if (slot == UINT32_MAX) {
    static bool logged = false;
    if (!logged) {
      RGPU_WARN("Sampler table full; falling back to linear clamp");
      logged = true;
    }
    return kLinearClampSamplerSlot;
  }
  auto sampler = s.device->createSampler(desc);
  if (!sampler)
    return kLinearClampSamplerSlot;
  s.sampler_slot_used[slot] = true;
  s.sampler_set->setSampler(slot, sampler.get());
  g_sampler_objects.push_back(std::move(sampler));
  g_samplers.emplace(key, slot);
  return slot;
}

} // namespace

} // namespace renut::gpu
