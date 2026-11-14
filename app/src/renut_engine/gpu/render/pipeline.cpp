/**
 * @file    gpu/render/pipeline.cpp
 */
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/render/occlusion_bias.h"
#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/banjo_renut_deko3d.h"  // Dunwall (2026-11-12): DkshReadReport
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <chrono>

#include <fmt/format.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <plume_render_interface_builders.h>
#include <rex/cvar.h>

// banjo-nx (2026-10-17, deko3d renderer): every graphics pipeline's shaders and specialization constants, one line each,
// for tools/deko3d_variant_cache.py (the DKSH cache of the deko3d back end, compiled on the PC). Any back end: the
// constants are the renderer's. Written next to the executable.
REXCVAR_DEFINE_BOOL(banjo_deko3d_variant_log, false, "Banjo/Renut",
                    "Write deko3d_variants.txt next to the executable: each graphics pipeline's vertex and pixel shader "
                    "and specialization constants (tools/deko3d_variant_cache.py builds the deko3d DKSH cache from it)");
#include <xxhash.h>
#include <cstdio>  // banjo-nx (2026-10-20): the pipeline list file
#if defined(_WIN32)
#include <io.h>  // _commit
#else
#include <unistd.h>  // fsync
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/render/format.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/guest/resources.h"
#include "renut_engine/gpu/render/shader_library.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"

REXCVAR_DEFINE_STRING(renut_pipeline_list, "pipeline_list.bin", "Nuts&Bolts/Graphics",
                      "The pipelines the renderer has built, which it builds ahead of use at the "
                      "next start (delete it to start over)");
REXCVAR_DEFINE_INT32(mgr_renut_color_depth_bias, 4096, "MGR/Renderer",
                     "Constant depth bias units a depth-tested colour draw leans toward passing (toward the camera for "
                     "GREATER/GEQUAL), clamped to 1e-6 of depth, so a surface does not fail its own depth prepass by the "
                     "host's extra depth bits")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// me2-nx-deko (2026-10-25): the clamp of mgr_renut_color_depth_bias as a setting. On deko3d uam compiles the base pass's
// and the lighting passes' skinning vertex shaders differently enough that a character's triangles failed their own
// depth by more than 1e-6 (random black triangles while animating, Ryujinx and the console, not the PC).
// 1e-4 (2026-10-25): with 1e-6 the black triangles stayed in Ryujinx, 1e-4 removed them. The bias itself is 4096 units
// of the primitive's own depth (about 5e-4 of it); the clamp only bounds it very near the camera (the faces).
REXCVAR_DEFINE_DOUBLE(me2_renut_color_depth_bias_clamp, 1.0e-4, "ME2/Renderer",
                      "Most depth a depth-tested colour draw leans toward passing (mgr_renut_color_depth_bias's clamp)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(mgr_renut_diag_color_depth_bias, 0, "MGR/Diagnostics",
                     "Diagnostic only: constant depth bias on depth-tested colour draws")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The colour bias is zero on ME2/deko3d to keep eyes behind eyelids. Reusing
// that zero for query boxes removes their conservative depth margin as well.
// 16384 / 2e-4 match the test-only margin of colour bias=4096, which the user's
// Citadel A/B removed the black flashing with, without biasing character draws.
REXCVAR_DEFINE_INT32(me2_renut_occlusion_depth_bias, 16384, "ME2/Renderer",
                     "deko3d: independent depth bias for colourless, depth-read-only visibility tests; 0 disables")
    .range(0, 1048576)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_renut_occlusion_depth_bias_clamp, 2.0e-4, "ME2/Renderer",
                      "deko3d: depth bias clamp for visibility tests, independent of colour/character bias")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(int32_t, mgr_renut_diag_raster_override);

namespace renut::gpu {

namespace {

struct CachedPipeline {
  PipelineKey key;
  std::unique_ptr<plume::RenderPipeline> pipeline;
};

// Render thread (State().mutex).
std::unordered_map<uint64_t, std::vector<CachedPipeline>> g_pipelines;
// The last lookup's: most draws use the pipeline the draw before them did,
// which a compare of the key finds without hashing it.
PipelineKey g_last_key{};
plume::RenderPipeline *g_last_pipeline = nullptr;

// banjo-nx (2026-10-19, CPU proposal 6, banjo_renut_pipeline_memo): what GetPipelineLocked returned for a key, in a
// 64-entry direct-mapped table before the XXH3 hash and the map: the built pipeline; for a specialised key still being
// built, the generic one it draws with until the prebuild epoch moves (kind 1); for one that failed, its generic one
// (kind 2; failures stay failed). Pipelines are never destroyed, so an entry cannot dangle.
struct PipelineMemo {
  PipelineKey key{};
  plume::RenderPipeline *pipeline = nullptr;
  uint64_t epoch = 0;
  uint8_t kind = 0;
};
// banjo-nx (2026-10-19, render-thread proposal 6): 512 sets of 2 ways (was 64 entries direct-mapped: the console binds
// 450-1,050 pipelines a frame out of a few hundred keys, and the memo thrashed). Way 0 is the most recent.
constexpr uint32_t kPipelineMemoSets = 512;
PipelineMemo g_pipeline_memo[kPipelineMemoSets][2];

inline uint32_t PipelineMemoIndex(const PipelineKey &key) {
  uint64_t w[8];
  std::memcpy(w, &key, sizeof(w));  // the shaders, the layout hash, topology, formats
  uint64_t h = w[0] * 0x9E3779B97F4A7C15ull ^ w[1] * 0xC2B2AE3D27D4EB4Full ^ w[2] ^ (w[3] << 17) ^ (w[4] >> 3) ^
               (w[5] << 29) ^ w[6] ^ (w[7] >> 11);
  uint32_t b;
  std::memcpy(&b, reinterpret_cast<const char *>(&key) + offsetof(PipelineKey, bool_values), sizeof(b));
  h ^= uint64_t(b) * 0xFF51AFD7ED558CCDull ^ uint64_t(key.alpha_func) << 40 ^ uint64_t(key.blend_control[0]) << 7 ^
       uint64_t(key.depth_control) << 23;
  return uint32_t((h ^ (h >> 31) ^ (h >> 47)) & (kPipelineMemoSets - 1));
}
// banjo-nx (2026-10-18): bumped each time a prebuild job is done (any thread).
std::atomic<uint64_t> g_prebuild_epoch{0};
// banjo-nx (2026-10-18), render thread: specialised keys a worker is building, with the generic pipeline their draws
// take and the g_prebuild_epoch they were last asked at. Until a job is done, a draw of such a key takes the generic
// pipeline here instead of locking the prebuild queue and searching it again (package 33's first run: hundreds of
// draws a frame waited so for their alpha-specialised pipelines).
struct SpecPending {
  PipelineKey key;
  uint64_t epoch = 0;
  plume::RenderPipeline *generic = nullptr;
};
std::unordered_map<uint64_t, SpecPending> g_spec_pending;

// Every draw looks its pipeline up: a byte-at-a-time hash of the whole key
// cost as much as the rest of the lookup.
uint64_t HashKey(const PipelineKey &key) {
  return XXH3_64bits(&key, sizeof(key));
}

plume::RenderBlendDesc BlendDesc(u32 control, u32 write_mask) {
  // RB_BLENDCONTROL: color src 0-4, op 5-7, dst 8-12; alpha src 16-20,
  // op 21-23, dst 24-28. Xenos has no enable bit: ONE/ZERO/ADD is no blending.
  plume::RenderBlendDesc desc;
  const u32 color_src = control & 0x1F, color_op = (control >> 5) & 7,
            color_dst = (control >> 8) & 0x1F;
  const u32 alpha_src = (control >> 16) & 0x1F, alpha_op = (control >> 21) & 7,
            alpha_dst = (control >> 24) & 0x1F;
  desc.srcBlend = ConvertBlendFactor(color_src);
  desc.dstBlend = ConvertBlendFactor(color_dst);
  desc.blendOp = ConvertBlendOp(color_op);
  desc.srcBlendAlpha = ConvertBlendFactor(alpha_src);
  desc.dstBlendAlpha = ConvertBlendFactor(alpha_dst);
  desc.blendOpAlpha = ConvertBlendOp(alpha_op);
  const bool color_passthrough = color_src == 1 && color_dst == 0 && color_op == 0;
  const bool alpha_passthrough = alpha_src == 1 && alpha_dst == 0 && alpha_op == 0;
  desc.blendEnabled = !(color_passthrough && alpha_passthrough);
  // Alpha factors cannot reference color; D3D12 rejects them.
  auto alpha_only = [](plume::RenderBlend b) {
    switch (b) {
    case plume::RenderBlend::SRC_COLOR:
      return plume::RenderBlend::SRC_ALPHA;
    case plume::RenderBlend::INV_SRC_COLOR:
      return plume::RenderBlend::INV_SRC_ALPHA;
    case plume::RenderBlend::DEST_COLOR:
      return plume::RenderBlend::DEST_ALPHA;
    case plume::RenderBlend::INV_DEST_COLOR:
      return plume::RenderBlend::INV_DEST_ALPHA;
    default:
      return b;
    }
  };
  desc.srcBlendAlpha = alpha_only(desc.srcBlendAlpha);
  desc.dstBlendAlpha = alpha_only(desc.dstBlendAlpha);
  desc.renderTargetWriteMask = uint8_t(write_mask & 0xF);
  return desc;
}

} // namespace

// banjo-nx (banjo_gpu_blend_skip). Vulkan clamps a UNORM target's source and factors to [0, 1] before the blend, so a
// source alpha of 0 or less is 0 there, and a whole source of 0 or less is 0. The target keeps its value where the
// source term is 0 and the destination factor is 1 at that source, on every channel written, with ADD.
uint32_t BlendSkipMode(const PipelineKey &key) {
  if (!key.pixel_shader || key.color_count != 1 || key.alpha_to_coverage)
    return 0;
  // Depth or stencil writes would differ (the discard writes neither).
  if (key.depth_format != plume::RenderFormat::UNKNOWN && (key.depth_control & (1u | 4u)))
    return 0;
  switch (key.color_formats[0]) {
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R8G8_UNORM:
  case plume::RenderFormat::R8_UNORM:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R16_UNORM:
    break;
  default:
    return 0;  // float targets do not clamp; sRGB ones convert
  }
  const plume::RenderBlendDesc b = BlendDesc(key.blend_control[0], key.color_mask);
  if (!b.blendEnabled || (b.renderTargetWriteMask & 0xF) == 0)
    return 0;
  using B = plume::RenderBlend;
  using Op = plume::RenderBlendOperation;
  const bool rgb = (b.renderTargetWriteMask & 7) != 0, alpha = (b.renderTargetWriteMask & 8) != 0;
  // Alpha: the source alpha is 0 in both modes, so its term is 0 whatever its factor; the destination's must be 1.
  if (alpha && !(b.blendOpAlpha == Op::ADD && (b.dstBlendAlpha == B::ONE || b.dstBlendAlpha == B::INV_SRC_ALPHA)))
    return 0;
  if (!rgb)
    return 1;
  if (b.blendOp != Op::ADD)
    return 0;
  // Mode 1: only the alpha is 0 or less: the colour's source factor must be 0 at alpha 0.
  const bool src_zero_at_alpha_zero =
      b.srcBlend == B::ZERO || b.srcBlend == B::SRC_ALPHA || b.srcBlend == B::SRC_ALPHA_SAT;
  const bool dst_one_at_alpha_zero = b.dstBlend == B::ONE || b.dstBlend == B::INV_SRC_ALPHA;
  if (src_zero_at_alpha_zero && dst_one_at_alpha_zero)
    return 1;
  // Mode 2: the whole source is 0: its term is 0 for any factor; the destination's must be 1 at a zero source.
  if (dst_one_at_alpha_zero || b.dstBlend == B::INV_SRC_COLOR)
    return 2;
  return 0;
}

namespace {

// The channels a render target format has (RGBA bits, as the colour write mask). Vulkan drops a component the format
// lacks. Formats not listed: all four (nothing trimmed).
uint32_t FormatChannels(plume::RenderFormat format) {
  using F = plume::RenderFormat;
  switch (format) {
  case F::R8_UNORM:
  case F::R16_UNORM:
  case F::R16_FLOAT:
  case F::R32_FLOAT:
    return 0x1;
  case F::R8G8_UNORM:
  case F::R16G16_UNORM:
  case F::R16G16_FLOAT:
  case F::R32G32_FLOAT:
    return 0x3;
  default:
    return 0xF;
  }
}

bool BlendReadsSourceAlpha(plume::RenderBlend b) {
  using B = plume::RenderBlend;
  return b == B::SRC_ALPHA || b == B::INV_SRC_ALPHA || b == B::SRC_ALPHA_SAT;
}

bool BlendReadsSecondSource(plume::RenderBlend b) {
  using B = plume::RenderBlend;
  return b == B::SRC1_COLOR || b == B::INV_SRC1_COLOR || b == B::SRC1_ALPHA || b == B::INV_SRC1_ALPHA;
}

} // namespace

// banjo-nx (banjo_gpu_output_trim). Vulkan writes a channel only where the write mask has it and the format has it;
// blending reads the source per channel (its own value, SRC_COLOR) and the source alpha (the alpha factors), never
// another colour channel; the alpha factors of the alpha channel read only alphas.
uint32_t OutputTrimMask(const PipelineKey &key) {
  if (!key.pixel_shader)
    return 0;
  uint32_t trim = 0;
  for (uint32_t i = 0; i < d3d::kMaxRenderTargets; ++i) {
    if (i >= key.color_count) {
      trim |= 0xFu << (4 * i);  // no target: an output nothing receives
      continue;
    }
    const uint32_t write = (key.color_mask >> (4 * i)) & 0xF & FormatChannels(key.color_formats[i]);
    uint32_t t = ~write & 0xF;
    const plume::RenderBlendDesc b = BlendDesc(key.blend_control[i], key.color_mask >> (4 * i));
    if (b.blendEnabled) {
      if (BlendReadsSecondSource(b.srcBlend) || BlendReadsSecondSource(b.dstBlend) ||
          BlendReadsSecondSource(b.srcBlendAlpha) || BlendReadsSecondSource(b.dstBlendAlpha))
        t = 0;
      // A written colour channel whose factors read the source alpha.
      if ((write & 7) && (BlendReadsSourceAlpha(b.srcBlend) || BlendReadsSourceAlpha(b.dstBlend)))
        t &= 7;
    }
    if (i == 0 && key.alpha_to_coverage)
      t &= 7;
    trim |= t << (4 * i);
  }
  return trim;
}

namespace {

// Any thread: reads only what device creation set up.
std::unique_ptr<plume::RenderPipeline> Build(const PipelineKey &key, const PipelineInputs &inputs) {
  // banjo-nx (a test, banjo_renut_diag_build_delay_ms): console-slow compiles on the PC.
  if (const int delay = banjo::renut_gpu::DiagBuildDelayMs())
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  banjo::renut_gpu::NotePipelineBuilt();  // banjo-nx (banjo_renut_cache_save_quiet)
  auto &s = State();
  const HostVertexLayout &layout = *inputs.layout;

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.draw_layout.get();
  desc.vertexShader = inputs.vertex_shader;
  desc.pixelShader = inputs.pixel_shader;
  if (key.rectangle_list)
    desc.geometryShader = s.rectangle_list_gs.get();
  if (key.point_sprite)
    desc.geometryShader = s.point_sprite_gs.get();
  desc.primitiveTopology = key.topology;

  // PA_SU_SC_MODE_CNTL: cull front 0, cull back 1, front face clockwise 2.
  // Points have no facing; their sprites are never culled.
  const bool cull_front = !key.point_sprite && (key.mode_control & 1);
  const bool cull_back = !key.point_sprite && (key.mode_control & 2);
  desc.cullMode = cull_front && !cull_back   ? plume::RenderCullMode::FRONT
                  : cull_back && !cull_front ? plume::RenderCullMode::BACK
                                             : plume::RenderCullMode::NONE;
  desc.frontFace = (key.mode_control & 4) ? plume::RenderFrontFace::CLOCKWISE
                                          : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  desc.depthClipEnabled = true;
  if ((REXCVAR_GET(mgr_renut_diag_raster_override) & 512) && key.color_mask)
    desc.depthClipEnabled = false;
  // mgr-nx-deko (2026-10-22): MGR lays depth with a prepass (depth-only vertex shaders) and draws colour with
  // GREATER_EQUAL against it (reversed Z) through other vertex shaders of the same transform. The Xenos stores 20e4
  // float depth, which hides the last bits; the host's 32-bit float keeps them, and the two shaders' positions differ
  // there (their own instruction order), so a part of every surface failed its own prepass: black flecks on Raiden's
  // face. A colour draw's test leans a few units toward passing (+ for GREATER/GEQUAL, - for LESS/LEQUAL), about
  // the precision the Xenos format has.
  // me2-nx-deko (2026-10-25): also a depth-tested draw that writes no colour and no depth (a stencil mark: UE3 marks a
  // character's pixels for its own modulated shadow by drawing it again into the stencil with GREATER_EQUAL). Without the
  // lean every other row failed by a few 1e-9 and the shadow fell on the face in stripes. A depth-only draw that writes
  // depth (a prepass, a shadow map) stays as it is.
  // me2-nx-deko (2026-10-26): a draw that writes the depth it tests leans less than one that only tests it. ME2's base
  // pass writes its depth with colour (no depth-only prepass, unlike MGR); with one lean for both, the depth it stored
  // moved by exactly what the lighting passes leaned, no margin was left, and on deko3d, whose vertex shaders differ by
  // a few 1e-6, lit characters' triangles failed at random (black patches jumping on faces, Ryujinx and the console).
  // A writing draw still leans (against a character's depth prepass: on deko3d the two vertex shaders differed by
  // 1.3e-5 at a depth of 0.11, black dots flickering on Shepard's face): the units, half the clamp. A test-only one
  // leans four times the units and twice the clamp, so it always clears what a writing one stored.
  const bool writes_depth = key.depth_control & 4;
  if ((key.color_mask || !writes_depth) && (key.depth_control & 2)) {
    const u32 func = (key.depth_control >> 4) & 7;
    const int32_t base_units = REXCVAR_GET(mgr_renut_color_depth_bias);
    // The shaders' difference is absolute (a few 2^-23 of z/w, which is near 1 far away), while a constant unit of
    // float depth is 2^-23 of the primitive's own depth: many units, clamped either way.
    static const float base_clamp = float(REXCVAR_GET(me2_renut_color_depth_bias_clamp));
    const int32_t units = writes_depth ? base_units : base_units * 4;
    const float kClamp = writes_depth ? base_clamp * 0.5f : base_clamp * 2.0f;
    if (func == 4 || func == 6) {
      desc.depthBias = units;
      desc.depthBiasClamp = kClamp;
    } else if (func == 1 || func == 3) {
      desc.depthBias = -units;
      desc.depthBiasClamp = -kClamp;
    }
    if (const auto query_bias = OcclusionTestBias(IsDeko3D(), key.color_mask, key.depth_control,
                                                REXCVAR_GET(me2_renut_occlusion_depth_bias),
                                                float(REXCVAR_GET(me2_renut_occlusion_depth_bias_clamp)))) {
      desc.depthBias = query_bias->units;
      desc.depthBiasClamp = query_bias->clamp;
    }
    if (const int32_t diag = REXCVAR_GET(mgr_renut_diag_color_depth_bias))
      desc.depthBias = diag;
  }
  // me2-nx-deko (2026-10-25, me2_renut_poly_offset): the guest's own polygon offset wins.
  if (key.depth_bias || key.depth_bias_slope) {
    desc.depthBias = key.depth_bias;
    desc.depthBiasClamp = std::bit_cast<float>(key.depth_bias_clamp);
    desc.slopeScaledDepthBias = std::bit_cast<float>(key.depth_bias_slope);
  }
  // ME2 (2026-11-12, me2_renut_poly_offset_dynamic): the draw sets the guest's offset (dkCmdBufSetDepthBias), the
  // pipeline does not.
  if (key.reserved & kKeyDynamicBias)
    desc.dynamicDepthBiasEnabled = true;

  // RB_DEPTHCONTROL.
  const u32 dc = key.depth_control;
  if (key.depth_format != plume::RenderFormat::UNKNOWN) {
    desc.depthEnabled = dc & 2;
    desc.depthWriteEnabled = dc & 4;
    // banjo-nx (2026-10-18): a test that always passes and writes nothing has no effect; off, the GPU does not read the
    // depth target for it (full-screen passes over a bound depth target).
    if (desc.depthEnabled && !desc.depthWriteEnabled && ((dc >> 4) & 7) == 7)
      desc.depthEnabled = false;
    desc.depthFunction = ConvertCompareFunction((dc >> 4) & 7);
    desc.stencilEnabled = dc & 1;
    desc.stencilReference = key.stencil_ref_mask & 0xFF;
    desc.stencilReadMask = (key.stencil_ref_mask >> 8) & 0xFF;
    desc.stencilWriteMask = (key.stencil_ref_mask >> 16) & 0xFF;
    desc.stencilFrontFace.compareFunction = ConvertCompareFunction((dc >> 8) & 7);
    desc.stencilFrontFace.failOp = ConvertStencilOp((dc >> 11) & 7);
    desc.stencilFrontFace.passOp = ConvertStencilOp((dc >> 14) & 7);
    desc.stencilFrontFace.depthFailOp = ConvertStencilOp((dc >> 17) & 7);
    if (dc & 0x80) {
      desc.stencilBackFace.compareFunction = ConvertCompareFunction((dc >> 20) & 7);
      desc.stencilBackFace.failOp = ConvertStencilOp((dc >> 23) & 7);
      desc.stencilBackFace.passOp = ConvertStencilOp((dc >> 26) & 7);
      desc.stencilBackFace.depthFailOp = ConvertStencilOp((dc >> 29) & 7);
    } else {
      desc.stencilBackFace = desc.stencilFrontFace;
    }
  }
  desc.depthTargetFormat = key.depth_format;

  desc.renderTargetCount = key.color_count;
  for (uint32_t i = 0; i < key.color_count; ++i) {
    desc.renderTargetFormat[i] = key.color_formats[i];
    desc.renderTargetBlend[i] = BlendDesc(key.blend_control[i], key.color_mask >> (i * 4));
  }
  desc.multisampling = plume::RenderMultisampling(plume::RenderSampleCounts(key.samples));
  // banjo-nx (banjo_renut_alpha_to_mask_test, 2026-10-18): not on a single-sample target, where the draw's shared
  // constants make it an alpha test at 0.5 (draw.cpp). The key keeps the bit: the same keys and shader variants.
  desc.alphaToCoverageEnabled = key.alpha_to_coverage != 0 &&
                                !(key.samples == uint32_t(plume::RenderSampleCount::COUNT_1) &&
                                  banjo::renut_gpu::AlphaToMaskTestOn());

  // banjo-nx (banjo_gpu_bool_spec): the key's bools as the shaders' specialization constants (banjo_bool_spec.h). One
  // list for every stage: a stage takes the ids it declares (vertex and pixel shader bools have distinct ones).
  plume::RenderSpecConstant spec[272];  // 256 bools and 900-908
  uint32_t spec_count = 0;
  for (uint32_t w = 0; w < 8; ++w) {
    for (uint32_t m = key.bool_mask[w]; m; m &= m - 1) {
      const uint32_t b = uint32_t(std::countr_zero(m));
      spec[spec_count++] = plume::RenderSpecConstant(1000 + w * 32 + b, (key.bool_values[w] >> b) & 1u);
    }
  }
  // banjo-nx (banjo_gpu_blend_skip): the pixel shader's discard of pixels the blend would not change.
  if (key.blend_skip)
    spec[spec_count++] = plume::RenderSpecConstant(900, key.blend_skip);
  // banjo-nx (banjo_gpu_output_trim): the colour channels the pixel shader leaves 0.
  if (key.output_trim) {
    spec[spec_count++] = plume::RenderSpecConstant(901, key.output_trim);
    if (banjo::renut_gpu::DiagTrimPoison())
      spec[spec_count++] = plume::RenderSpecConstant(902, 1);  // the test: trimmed channels 60000
  }
  // banjo-nx (banjo_deko3d_alpha_spec, 2026-10-18): the alpha test's function (+1), constant 903 of the deko3d GLSL.
  if (key.alpha_func)
    spec[spec_count++] = plume::RenderSpecConstant(903, key.alpha_func);
  // banjo-nx (banjo_deko3d_tess_spec, 2026-10-18): not a tessellated draw: the vertex shader's patch kind is 0
  // (SharedConstants.tessellation[0], constant 904 + 1). Decided by the key: no new pipelines.
  if (!key.per_instance_streams && banjo::renut_gpu::TessSpecOn())
    spec[spec_count++] = plume::RenderSpecConstant(904, 1);
  // banjo-nx (banjo_deko3d_early_hoist, 2026-10-18): the main body's first texture region ahead of the early alpha test
  // (constant 907, 1 by default in the GLSL); off: after it, as Renut's shaders have it.
  if (!banjo::renut_gpu::EarlyHoistOn())
    spec[spec_count++] = plume::RenderSpecConstant(907, 0);
  // banjo-nx (2026-10-19, GPU): texture sizes from the size buffer (constant 908, 1 by default in the GLSL).
  if (const uint32_t txq = banjo::renut_gpu::TxqMode(); txq != 1u)
    spec[spec_count++] = plume::RenderSpecConstant(908, txq);
  // banjo-nx (2026-10-19, GPU): depth tested before a discarding pixel shader where the draw writes neither depth nor
  // stencil (RB_DEPTHCONTROL bit 2 depth writes; as BlendSkipMode): constant 909 (tools/deko3d_glsl_opt.py earlyz;
  // shaders without a discard do not have it and get no define). 2026-10-20: a stencil test that writes nothing (write
  // mask 0, or every op of both faces KEEP: bits 11-19 front, 23-31 back with bit 7) counts as no stencil writes; the
  // early tests only skip the shader for pixels the tests would have dropped after it (no occlusion queries here).
  if (key.pixel_shader && banjo::renut_gpu::EarlyZOn() && !(dc & 4u)) {
    const bool stencil_ops = ((dc >> 11) & 0x1FF) != 0 || ((dc & 0x80) && ((dc >> 23) & 0x1FF) != 0);
    const bool stencil_writes = (dc & 1u) && ((key.stencil_ref_mask >> 16) & 0xFF) != 0 && stencil_ops;
    if (!stencil_writes)
      spec[spec_count++] = plume::RenderSpecConstant(909, 1);
  }
  // banjo-nx (2026-10-19, GPU, A/B): the listed pixel shaders compiled with 32 GPRs at most (constant 910).
  // (key.reserved bit 1: the off half of the in-run A/B, banjo_renut_ab_target 16.)
  if (key.pixel_shader && banjo::renut_gpu::Gpr32On() && !(key.reserved & 2u))
    spec[spec_count++] = plume::RenderSpecConstant(910, 1);
  // banjo-nx (2026-10-20, GPU A/B): the listed lit shaders' lights in branches (constant 911, pass lightbranch).
  // (key.reserved bit 0: the off half of the in-run A/B, banjo_renut_ab_target 15.)
  if (key.pixel_shader && banjo::renut_gpu::LightBranchOn() && !(key.reserved & 1u))
    spec[spec_count++] = plume::RenderSpecConstant(911, 1);
  // banjo-nx (2026-10-20, diagnostic): the fur family without its pixel work (constant 912, pass nullps).
  if (key.pixel_shader && banjo::renut_gpu::NullPsOn())
    spec[spec_count++] = plume::RenderSpecConstant(912, 1);
  desc.specConstants = spec_count ? spec : nullptr;
  desc.specConstantsCount = spec_count;
  if (REXCVAR_GET(banjo_deko3d_variant_log)) {
    static std::mutex log_mutex;
    static std::unordered_set<std::string> logged;
    static std::ofstream log_file(banjo::renut_gpu::NextToExecutable("deko3d_variants.txt"), std::ios::app);
    const auto *vs_entry = static_cast<const ShaderCacheEntry *>(key.vertex_shader);
    const auto *ps_entry = static_cast<const ShaderCacheEntry *>(key.pixel_shader);
    std::string line = fmt::format("vs {:016X} ps {:016X}", vs_entry ? vs_entry->hash : 0, ps_entry ? ps_entry->hash : 0);
    for (uint32_t i = 0; i < spec_count; ++i)
      line += fmt::format(" {}={}", spec[i].index, spec[i].value);
    // The state the blend skip and the output trim are decided from (tools/deko3d_variant_cache.py skips these).
    line += fmt::format(" | targets {} format0 {} blend0 {:08X} mask {:04X} depth {:08X} a2c {}", key.color_count,
                        int(key.color_formats[0]), key.blend_control[0], key.color_mask, key.depth_control,
                        key.alpha_to_coverage);
    std::lock_guard<std::mutex> lock(log_mutex);
    if (log_file && logged.insert(line).second) {
      log_file << line << '\n';
      log_file.flush();
    }
  }

  plume::RenderInputSlot slots[kMaxVertexStreams + 1];
  uint32_t slot_count = 0;
  for (uint32_t stream = 0; stream < kMaxVertexStreams; ++stream) {
    const HostVertexStream &hs = layout.streams[stream];
    if (hs.used) {
      slots[slot_count++] = plume::RenderInputSlot(
          stream, hs.host_stride,
          key.per_instance_streams ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                                   : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
    }
  }
  slots[slot_count++] = plume::RenderInputSlot(kZeroStreamSlot, 0);
  desc.inputSlots = slots;
  desc.inputSlotsCount = slot_count;
  desc.inputElements = layout.input_elements.data();
  desc.inputElementsCount = uint32_t(layout.input_elements.size());

  auto pipeline = s.device->createGraphicsPipeline(desc);
  if (GraphicsPipelineCreated(pipeline.get())) {
    // Names the guest shaders in captures.
    const auto *vs = static_cast<const ShaderCacheEntry *>(key.vertex_shader);
    const auto *ps = static_cast<const ShaderCacheEntry *>(key.pixel_shader);
    pipeline->setName(fmt::format("vs_{:016x} ps_{:016x}", vs ? vs->hash : 0, ps ? ps->hash : 0));
    return pipeline;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// The pipeline list file
// ---------------------------------------------------------------------------
//
// A header, then records: u32 byte size of the rest of the record, the key
// with the two shader pointers replaced by the shaders' cache hashes, the
// layout's streams (u8 used, u32 host stride, 16 of them), u32 input element
// count, and each element (u8 name length, the name, u32 semantic index,
// location, format, slot, offset). A build whose PipelineKey or layout differs
// bumps kListVersion; a file of another version is started over.

constexpr char kListMagic[4] = {'R', 'N', 'P', 'L'};
// banjo-nx: 2, the key's bool words (banjo_gpu_bool_spec); 3, output_trim; 4, alpha_func (a version 3 list is kept,
// LoadList).
constexpr u32 kListVersion = 4;
constexpr u32 kListV3KeySize = 184;  // the key up to output_trim

struct ListHeader {
  char magic[4];
  u32 version;
  u32 key_size;
  u32 streams;
};

class ByteWriter {
public:
  template <typename T>
  void Put(const T &value) {
    const auto *p = reinterpret_cast<const char *>(&value);
    bytes_.insert(bytes_.end(), p, p + sizeof(T));
  }
  void PutBytes(const void *data, size_t size) {
    const auto *p = static_cast<const char *>(data);
    bytes_.insert(bytes_.end(), p, p + size);
  }
  const std::vector<char> &Bytes() const { return bytes_; }

private:
  std::vector<char> bytes_;
};

class ByteReader {
public:
  ByteReader(const char *data, size_t size) : data_(data), size_(size) {}
  template <typename T>
  bool Get(T &value) {
    if (size_ - at_ < sizeof(T))
      return false;
    std::memcpy(&value, data_ + at_, sizeof(T));
    at_ += sizeof(T);
    return true;
  }
  bool GetBytes(void *out, size_t size) {
    if (size_ - at_ < size)
      return false;
    std::memcpy(out, data_ + at_, size);
    at_ += size;
    return true;
  }

private:
  const char *data_;
  size_t size_;
  size_t at_ = 0;
};

// The key as stored: the shader pointers as their cache hashes.
PipelineKey StoredKey(const PipelineKey &key) {
  PipelineKey stored = key;
  const auto *vs = static_cast<const ShaderCacheEntry *>(key.vertex_shader);
  const auto *ps = static_cast<const ShaderCacheEntry *>(key.pixel_shader);
  const uint64_t vs_hash = vs ? vs->hash : 0, ps_hash = ps ? ps->hash : 0;
  static_assert(sizeof(stored.vertex_shader) == sizeof(uint64_t));
  std::memcpy(&stored.vertex_shader, &vs_hash, sizeof(vs_hash));
  std::memcpy(&stored.pixel_shader, &ps_hash, sizeof(ps_hash));
  return stored;
}

std::vector<char> EncodeRecord(const PipelineKey &key, const HostVertexLayout &layout) {
  ByteWriter w;
  w.Put(StoredKey(key));
  for (u32 s = 0; s < kMaxVertexStreams; ++s) {
    w.Put(u8(layout.streams[s].used ? 1 : 0));
    w.Put(u32(layout.streams[s].host_stride));
  }
  w.Put(u32(layout.input_elements.size()));
  for (const plume::RenderInputElement &e : layout.input_elements) {
    const std::string name = e.semanticName ? e.semanticName : "";
    w.Put(u8(name.size()));
    w.PutBytes(name.data(), name.size());
    w.Put(u32(e.semanticIndex));
    w.Put(u32(e.location));
    w.Put(u32(e.format));
    w.Put(u32(e.slotIndex));
    w.Put(u32(e.alignedByteOffset));
  }
  return w.Bytes();
}

// Dunwall (2026-11-14): a set of 64-bit hashes in one allocation (open addressing, linear probing, doubled at half
// full), for LoadList's "seen" sets and the list's 'listed': an unordered_set allocates a node per insert (2 x 65,561
// at the load), and newlib's malloc is what made the load take 30 s on the console. 0 is kept apart (free slot).
class FlatHashSet {
public:
  explicit FlatHashSet(size_t most = 0) { Reset(most); }
  bool insert(uint64_t h) {  // true: not there before
    if (h == 0) {
      const bool fresh = !zero_;
      zero_ = true;
      return fresh;
    }
    if ((count_ + 1) * 2 > slots_.size())
      Grow();
    for (size_t i = Home(h);; i = (i + 1) & mask_) {
      if (slots_[i] == h)
        return false;
      if (slots_[i] == 0) {
        slots_[i] = h;
        ++count_;
        return true;
      }
    }
  }

private:
  void Reset(size_t most) {
    size_t n = 64;
    while (n < most * 2)
      n <<= 1;
    slots_.assign(n, 0);
    mask_ = n - 1;
    count_ = 0;
  }
  size_t Home(uint64_t h) const { return size_t(h ^ (h >> 29)) & mask_; }
  void Grow() {
    std::vector<uint64_t> old = std::move(slots_);
    Reset(old.size());  // twice the slots
    for (const uint64_t h : old) {
      if (!h)
        continue;
      size_t i = Home(h);
      while (slots_[i])
        i = (i + 1) & mask_;
      slots_[i] = h;
      ++count_;
    }
  }
  std::vector<uint64_t> slots_;
  size_t mask_ = 0, count_ = 0;
  bool zero_ = false;
};

// A listed pipeline, as the worker threads build it.
struct Job {
  PipelineKey key{};
  uint64_t hash = 0;
  HostVertexLayout layout;
  // The elements' semantic names point here. Dunwall (2026-11-14): a vector reserved to the element count before the
  // first name (DecodeRecord), so it never moves its strings; a deque cost two allocations (map and block) a job even
  // for one name, and newlib's malloc made the 65,561-record list take 30 s on the console (see LoadList).
  std::vector<std::string> names;
  enum class Stage { Pending, Building, Done } stage = Stage::Pending;
  std::unique_ptr<plume::RenderPipeline> pipeline;  // Done: nullptr if taken or failed
  // Its shaders the game has not made yet (PipelineShaderLoaded); queued at 0.
  u32 shaders_missing = 0;
};

bool DecodeRecord(const char *data, size_t size, Job &job) {
  ByteReader r(data, size);
  PipelineKey stored;
  if (!r.Get(stored))
    return false;
  uint64_t vs_hash, ps_hash;
  std::memcpy(&vs_hash, &stored.vertex_shader, sizeof(vs_hash));
  std::memcpy(&ps_hash, &stored.pixel_shader, sizeof(ps_hash));
  const ShaderCacheEntry *vs = FindShaderEntry(vs_hash);
  const ShaderCacheEntry *ps = ps_hash ? FindShaderEntry(ps_hash) : nullptr;
  if (!vs || (ps_hash && !ps))
    return false;  // a shader this build lacks
  job.key = stored;
  job.key.vertex_shader = vs;
  job.key.pixel_shader = ps;
  // banjo-nx (2026-10-20): the key as this configuration makes it. A record written with other keys on (Renut's blend
  // skip before package 30, an A/B half) would be built ahead for no draw, and its shader variants compiled at run time
  // (the DKSH cache leaves them out): emulator runs compiled three blend-skip variants at every start.
  if (!banjo::renut_gpu::BlendSkipOn())
    job.key.blend_skip = 0;
  if (!banjo::renut_gpu::OutputTrimOn())
    job.key.output_trim = 0;
  if (!banjo::renut_gpu::AlphaSpecOn())
    job.key.alpha_func = 0;
  if (!banjo::renut_gpu::BoolSpecOn()) {
    std::memset(job.key.bool_mask, 0, sizeof(job.key.bool_mask));
    std::memset(job.key.bool_values, 0, sizeof(job.key.bool_values));
  }
  job.key.reserved &= kKeyDynamicBias;  // a pipeline of the draws that set their bias stays one
  job.hash = HashKey(job.key);
  for (u32 s = 0; s < kMaxVertexStreams; ++s) {
    u8 used;
    u32 stride;
    if (!r.Get(used) || !r.Get(stride))
      return false;
    job.layout.streams[s].used = used != 0;
    job.layout.streams[s].host_stride = stride;
  }
  u32 count;
  if (!r.Get(count) || count > 64)
    return false;
  // Exactly 'count' names follow: reserved, the vector never reallocates, so the c_str() pointers the layout keeps
  // stay valid (short names live inside the string objects).
  job.names.clear();
  job.names.reserve(count);
  job.layout.input_elements.clear();
  job.layout.input_elements.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    u8 length;
    std::string name;
    u32 index, location, format, slot, offset;
    if (!r.Get(length))
      return false;
    name.resize(length);
    if (!r.GetBytes(name.data(), length) || !r.Get(index) || !r.Get(location) || !r.Get(format) ||
        !r.Get(slot) || !r.Get(offset)) {
      return false;
    }
    job.names.push_back(std::move(name));
    job.layout.input_elements.emplace_back(job.names.back().c_str(), index, location,
                                           plume::RenderFormat(format), slot, offset);
  }
  return true;
}

// The list's records, and the file the new ones are appended to. Render
// thread, except the load at the start.
// banjo-nx (2026-10-20, Peggle 2's report on the DKSH cache): a flushed stream only reaches the file system service,
// which may keep it buffered; a game closed with HOME (or stopped in the emulator) lost the records of the run, so the
// same pipelines (and their shader variants) were built again at every start. The file is synced to the card at most
// once a second (kListSyncNs: the records of the last second may still be lost), not per record (a new area appends
// hundreds).
// Dunwall (2026-11-12, the 13 fps / 768 ms frames for ~10 s after Press Start): the render thread appended each
// new pipeline's record with fwrite (records of ~600 bytes against stdio's ~1 KB buffer: an SD write through the FS
// service every second record, the file opened and its size asked at the first) and synced the file once a second
// itself. With ~220 new specialised pipelines as the main menu's scene first draws, the specialised lookups took
// 4.9 s of the render thread in 10 s. Now the render thread only queues the encoded record; a writer thread of its
// own (the background compile core, low priority) opens, appends and syncs (fsync at most once a second, as before).
struct PipelineList {
  FlatHashSet listed;  // key hashes in the file (render thread, and the load at the start)
  static constexpr auto kListSync = std::chrono::seconds(1);
  // The writer's side.
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<char> pending;  // records not written yet: (u32 size, record) each
  bool started = false;
  std::atomic<bool> failed{false};
  std::atomic<uint64_t> queued_ns{0}, queued{0}, written{0}, write_ns{0}, sync_ns{0}, syncs{0}, max_io_ns{0};

  void Append(const PipelineKey &key, uint64_t hash, const HostVertexLayout &layout) {
    if (failed.load(std::memory_order_relaxed) || !listed.insert(hash))
      return;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<char> record = EncodeRecord(key, layout);
    const u32 size = u32(record.size());
    bool start = false;
    {
      std::lock_guard lock(mutex);
      const size_t at = pending.size();
      pending.resize(at + sizeof(size) + record.size());
      std::memcpy(pending.data() + at, &size, sizeof(size));
      std::memcpy(pending.data() + at + sizeof(size), record.data(), record.size());
      start = !started;
      started = true;
    }
    if (start)
      banjo::renut_gpu::StartLongLivedThread(&PipelineList::Writer, 256 * 1024);
    wake.notify_one();
    queued.fetch_add(1, std::memory_order_relaxed);
    queued_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - t0).count()),
                        std::memory_order_relaxed);
  }

  static void Writer();
};
std::string PipelineWaitReport();  // below
PipelineList g_list;

void NoteIo(std::atomic<uint64_t> &total, std::chrono::steady_clock::time_point t0) {
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - t0).count());
  total.fetch_add(ns, std::memory_order_relaxed);
  uint64_t max = g_list.max_io_ns.load(std::memory_order_relaxed);
  while (ns > max && !g_list.max_io_ns.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {
  }
}

void PipelineList::Writer() {
  banjo::renut_gpu::OnBackgroundThreadStart("pipeline list writer");
  PipelineList &l = g_list;
  std::FILE *out = nullptr;
  bool unsynced = false;
  auto last_sync = std::chrono::steady_clock::now();
  std::vector<char> batch;
  std::unique_lock lock(l.mutex);
  for (;;) {
    // Records to write, or the sync a second after the last one.
    if (l.pending.empty())
      l.wake.wait_for(lock, PipelineList::kListSync, [&] { return !l.pending.empty(); });
    batch.clear();
    batch.swap(l.pending);
    lock.unlock();
    if (!batch.empty() && !out && !l.failed.load(std::memory_order_relaxed)) {
      const auto t0 = std::chrono::steady_clock::now();
      const std::filesystem::path path = banjo::renut_gpu::NextToExecutable(REXCVAR_GET(renut_pipeline_list));
      std::error_code ec;
      const bool fresh = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
      out = std::fopen(path.string().c_str(), "ab");
      if (!out) {
        l.failed.store(true, std::memory_order_relaxed);
        RGPU_WARN("Pipeline list {} cannot be written; pipelines are not built ahead next time", path.string());
      } else if (fresh) {
        ListHeader header{};
        std::memcpy(header.magic, kListMagic, 4);
        header.version = kListVersion;
        header.key_size = sizeof(PipelineKey);
        header.streams = kMaxVertexStreams;
        std::fwrite(&header, sizeof(header), 1, out);
      }
      NoteIo(l.write_ns, t0);
    }
    if (!batch.empty() && out) {
      const auto t0 = std::chrono::steady_clock::now();
      std::fwrite(batch.data(), 1, batch.size(), out);
      NoteIo(l.write_ns, t0);
      size_t records = 0;
      for (size_t at = 0; at + sizeof(u32) <= batch.size(); ++records) {
        u32 size;
        std::memcpy(&size, batch.data() + at, sizeof(size));
        at += sizeof(size) + size;
      }
      l.written.fetch_add(records, std::memory_order_relaxed);
      unsynced = true;
    }
    if (out && unsynced && std::chrono::steady_clock::now() - last_sync >= PipelineList::kListSync) {
      const auto t0 = std::chrono::steady_clock::now();
      std::fflush(out);
#if defined(_WIN32)
      _commit(_fileno(out));
#else
      fsync(fileno(out));
#endif
      NoteIo(l.sync_ns, t0);
      l.syncs.fetch_add(1, std::memory_order_relaxed);
      unsynced = false;
      last_sync = std::chrono::steady_clock::now();
    }
    // ME2 (2026-11-12, backported from the Dunwall port): the list's writer, the specialised lookups and the DKSH
    // cache's reads, every 10 s while anything was written.
    static auto last_report = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last_report >= std::chrono::seconds(10)) {
      last_report = std::chrono::steady_clock::now();
      if (l.queued.load(std::memory_order_relaxed))
        RGPU_INFO("[pipelines] 10 s{}", PipelineWaitReport());
    }
    lock.lock();
  }
}

}  // namespace

// banjo-nx (2026-10-20): the list's last records synced to the card a second after they were written. Dunwall
// (2026-11-12): the list's writer thread does that now (PipelineList::Writer); nothing to do on the render thread.
void SyncPipelineListLocked() {}

namespace {

// Dunwall (2026-11-14, the user's console list: 115 MB, but every start loaded the same 21,903 pipelines): a
// session closed while the writer was appending (HOME, power) leaves a part record at the end; the next session
// appends after it, and the load stopped at that spot for good: the 97 MB after it (the pipelines of every later
// session, many listed again and again since they never reached 'listed') were never read. A record is now taken only
// when its bytes parse exactly as EncodeRecord writes them (size, 16 streams with used 0/1, at most 64 elements with
// short printable names, nothing left over); at a damaged spot the load scans on to the next well-formed record. A
// list with damage, a cut-short end or repeated records is written again with each good record once (".tmp", then
// renamed), so the next session appends to a clean file.
bool WellFormedRecord(const char *data, size_t avail, size_t &record_end) {
  if (avail < sizeof(u32))
    return false;
  u32 size;
  std::memcpy(&size, data, sizeof(size));
  constexpr size_t kMin = sizeof(PipelineKey) + kMaxVertexStreams * 5 + sizeof(u32);
  if (size < kMin || size > 65536 || size > avail - sizeof(u32))
    return false;
  const char *p = data + sizeof(u32) + sizeof(PipelineKey);
  const char *end = data + sizeof(u32) + size;
  for (u32 s = 0; s < kMaxVertexStreams; ++s, p += 5) {
    if (u8(p[0]) > 1)
      return false;
  }
  u32 count;
  std::memcpy(&count, p, sizeof(count));
  p += sizeof(count);
  if (count > 64)
    return false;
  for (u32 i = 0; i < count; ++i) {
    if (end - p < 1)
      return false;
    const u8 length = u8(*p++);
    if (length > 64 || end - p < ptrdiff_t(length) + 20)
      return false;
    for (u8 c = 0; c < length; ++c) {
      const char ch = p[c];
      if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
        return false;
    }
    p += length + 20;
  }
  if (p != end)
    return false;
  record_end = sizeof(u32) + size;
  return true;
}

// The records of the list file; empty (and the file started over) when it
// is missing or from another version.
std::vector<std::unique_ptr<Job>> LoadList() {
  std::vector<std::unique_ptr<Job>> jobs;
  const std::filesystem::path path = banjo::renut_gpu::NextToExecutable(REXCVAR_GET(renut_pipeline_list));
  // Dunwall (2026-11-12, the boot: 13-18 s between the device and the prebuild worker on the console, the main
  // thread 87 % waiting in the kernel inside this read): the file was read through std::istreambuf_iterator, a
  // stream buffer's worth (a few KB) per SD read for a list of many MB. Now in blocks of 4 MB.
  const auto t_read = std::chrono::steady_clock::now();
  std::vector<char> bytes;
  {
    std::FILE *in = std::fopen(path.string().c_str(), "rb");
    if (!in)
      return jobs;
    long length = -1;
    if (std::fseek(in, 0, SEEK_END) == 0)
      length = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    if (length > 0) {
      bytes.resize(size_t(length));
      size_t got = 0;
      while (got < bytes.size()) {
        const size_t n = std::fread(bytes.data() + got, 1, std::min<size_t>(4u << 20, bytes.size() - got), in);
        if (n == 0)
          break;
        got += n;
      }
      bytes.resize(got);
    }
    std::fclose(in);
  }
  const double read_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_read).count();
  ListHeader header{};
  if (bytes.size() >= sizeof(header))
    std::memcpy(&header, bytes.data(), sizeof(header));
  const bool header_ok = bytes.size() >= sizeof(header) && std::memcmp(header.magic, kListMagic, 4) == 0 &&
                         header.streams == kMaxVertexStreams;
  const bool valid = header_ok && header.version == kListVersion && header.key_size == sizeof(PipelineKey);
  // banjo-nx (2026-10-18): a version 3 list (before alpha_func): its keys get the fields after output_trim as 0 (the
  // generic pipelines, which the specialised ones draw with until they are built), and the file is written again as
  // version 4.
  const bool v3 = header_ok && header.version == 3 && header.key_size == kListV3KeySize;
  if (!valid && !v3) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return jobs;
  }
  // Dunwall (2026-11-14, the console's 30 s at startup, core 0 at 100 %: 97.6 % of it inside newlib's _malloc_r, the
  // decoding itself 0.5 %): every container here grew a node or a doubling at a time over 65,561 records (~1 KB each),
  // and newlib's allocator slows down with each of the hundreds of thousands of small blocks. Sized once from the file:
  // records are at least a few hundred bytes, so bytes / 256 is a safe upper bound.
  const size_t records_max = bytes.size() / 256 + 16;
  FlatHashSet seen(records_max);
  jobs.reserve(records_max);
  std::vector<char> converted;
  if (v3) {
    ListHeader h4 = header;
    h4.version = kListVersion;
    h4.key_size = sizeof(PipelineKey);
    converted.resize(sizeof(h4));
    std::memcpy(converted.data(), &h4, sizeof(h4));
  }
  size_t at = sizeof(header);
  std::vector<char> record;
  // Dunwall (2026-11-14): damaged spots skipped (WellFormedRecord), each good record kept once for the rewrite.
  std::vector<char> compact;
  FlatHashSet stored_seen(v3 ? 0 : records_max);
  u32 damaged = 0, repeated = 0;
  size_t damaged_bytes = 0;
  if (!v3) {
    compact.reserve(bytes.size());  // at most the file (2026-11-14: was doubled up to 68 MB, each old block freed)
    compact.resize(sizeof(header));
    std::memcpy(compact.data(), &header, sizeof(header));
  }
  while (bytes.size() - at >= sizeof(u32)) {
    if (!v3) {
      size_t record_end = 0;
      if (!WellFormedRecord(bytes.data() + at, bytes.size() - at, record_end)) {
        size_t next = at + 1;
        while (next < bytes.size() && !WellFormedRecord(bytes.data() + next, bytes.size() - next, record_end))
          ++next;
        ++damaged;
        damaged_bytes += next - at;
        at = next;
        if (at >= bytes.size())
          break;
      }
      const uint64_t stored_hash = XXH3_64bits(bytes.data() + at + sizeof(u32), record_end - sizeof(u32));
      if (stored_seen.insert(stored_hash))
        compact.insert(compact.end(), bytes.data() + at, bytes.data() + at + record_end);
      else
        ++repeated;
    }
    u32 size;
    std::memcpy(&size, bytes.data() + at, sizeof(size));
    at += sizeof(size);
    if (bytes.size() - at < size)
      break;  // cut short (the game closed while writing)
    const char *data = bytes.data() + at;
    u32 data_size = size;
    if (v3) {
      if (size < kListV3KeySize)
        break;
      record.assign(data, data + kListV3KeySize);
      record.resize(sizeof(PipelineKey), 0);
      record.insert(record.end(), data + kListV3KeySize, data + size);
      data = record.data();
      data_size = u32(record.size());
      const size_t o = converted.size();
      converted.resize(o + sizeof(u32) + data_size);
      std::memcpy(converted.data() + o, &data_size, sizeof(u32));
      std::memcpy(converted.data() + o + sizeof(u32), data, data_size);
    }
    auto job = std::make_unique<Job>();
    if (DecodeRecord(data, data_size, *job) && seen.insert(job->hash))
      jobs.push_back(std::move(job));
    at += size;
  }
  if (v3) {
    std::ofstream rewrite(path, std::ios::binary | std::ios::trunc);
    if (rewrite)
      rewrite.write(converted.data(), std::streamsize(converted.size()));
    RGPU_INFO("Pipeline list {}: version 3, {} pipelines kept as version 4 (generic alpha test)", path.string(),
              jobs.size());
  }
  if (!v3 && (damaged || repeated || at != bytes.size())) {
    const std::string tmp = path.string() + ".tmp";
    bool ok = false;
    if (std::FILE *out = std::fopen(tmp.c_str(), "wb")) {
      ok = std::fwrite(compact.data(), 1, compact.size(), out) == compact.size();
      ok = std::fflush(out) == 0 && ok;
#if defined(_WIN32)
      _commit(_fileno(out));
#else
      fsync(fileno(out));
#endif
      ok = std::fclose(out) == 0 && ok;
    }
    std::error_code ec;
    if (ok) {
      std::filesystem::remove(path, ec);
      std::filesystem::rename(tmp, path, ec);
      ok = !ec;
    } else {
      std::filesystem::remove(tmp, ec);
    }
    RGPU_INFO("Pipeline list {}: {} damaged spots ({:.1f} MB) skipped, {} repeated records dropped; {} ({:.1f} MB "
              "-> {:.1f} MB)",
              path.string(), damaged, damaged_bytes / 1e6, repeated, ok ? "written again clean" : "NOT rewritten",
              bytes.size() / 1e6, compact.size() / 1e6);
  }
  // Records of shaders this build lacks stay out of 'listed': a later build
  // that has them lists the pipelines again.
  g_list.listed = std::move(seen);
  RGPU_INFO("Pipeline list {}: {} pipelines, {:.1f} MB read in {:.0f} ms, all loaded in {:.0f} ms", path.string(),
            jobs.size(), bytes.size() / 1e6, read_ms,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_read).count());
  return jobs;
}

// ---------------------------------------------------------------------------
// Building the listed pipelines ahead
// ---------------------------------------------------------------------------
//
// A listed pipeline is built once the game has made both of its shaders (a
// level's shaders come with its load), not at the start: the list holds every
// level ever played, and the driver keeps each built pipeline's shaders and
// code in host memory. What is built stays built.

struct Prebuild {
  std::mutex mutex;
  std::condition_variable work;  // a job queued, or stopping
  std::condition_variable done;  // a job reached Done, or a worker ended
  std::vector<std::unique_ptr<Job>> jobs;
  std::unordered_map<uint64_t, std::vector<Job *>> by_hash;
  std::deque<Job *> queue;       // shaders all made; may have been claimed since
  // The shaders the game has made, and the jobs waiting for each of the others.
  std::unordered_set<const ShaderCacheEntry *> loaded;
  std::unordered_map<const ShaderCacheEntry *, std::vector<Job *>> waiting;
  u32 workers = 0;               // still running
  u32 building = 0;              // workers between taking a job and finishing it
  bool stop = false;
  bool started = false;          // banjo-nx: the workers were started (StartWorkersLocked)
  // Host shaders for the queued jobs, shared by the workers; freed whenever the
  // queue runs dry (the pipelines keep their own compiled copies).
  std::unordered_map<const ShaderCacheEntry *, std::unique_ptr<plume::RenderShader>> shaders;
};
Prebuild g_prebuild;

plume::RenderShader *PrebuildShader(const ShaderCacheEntry *entry) {
  if (!entry)
    return nullptr;
  auto &slot = g_prebuild.shaders[entry];
  if (!slot)
    slot = CreateCachedShader(State().device.get(), *entry);
  return slot.get();
}

void PrebuildWorker() {
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"renut pipeline prebuild");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  banjo::renut_gpu::OnPrebuildWorkerStart();  // banjo-nx: its core and priority on the Switch
  std::unique_lock lock(g_prebuild.mutex);
  for (;;) {
    g_prebuild.work.wait(lock, [] { return g_prebuild.stop || !g_prebuild.queue.empty(); });
    if (g_prebuild.stop)
      break;
    Job &job = *g_prebuild.queue.front();
    g_prebuild.queue.pop_front();
    if (job.stage != Job::Stage::Pending)
      continue;  // the render thread claimed it (TakePrebuilt)
    job.stage = Job::Stage::Building;
    ++g_prebuild.building;
    PipelineInputs inputs;
    inputs.vertex_shader = PrebuildShader(static_cast<const ShaderCacheEntry *>(job.key.vertex_shader));
    inputs.pixel_shader = PrebuildShader(static_cast<const ShaderCacheEntry *>(job.key.pixel_shader));
    inputs.layout = &job.layout;
    const bool shaders = inputs.vertex_shader && (!job.key.pixel_shader || inputs.pixel_shader);
    lock.unlock();
    std::unique_ptr<plume::RenderPipeline> pipeline = shaders ? Build(job.key, inputs) : nullptr;
    lock.lock();
    job.pipeline = std::move(pipeline);
    job.stage = Job::Stage::Done;
    g_prebuild_epoch.fetch_add(1, std::memory_order_release);  // banjo-nx: g_spec_pending asks again
    if (--g_prebuild.building == 0 && g_prebuild.queue.empty())
      g_prebuild.shaders.clear();
    g_prebuild.done.notify_all();
  }
  if (--g_prebuild.workers == 0)
    g_prebuild.shaders.clear();
  g_prebuild.done.notify_all();
}

// Requires g_prebuild.mutex. Queues the job once the game has made its shaders,
// or waits for the ones it has not.
void QueueOrWait(Job &job) {
  for (const void *shader : {job.key.vertex_shader, job.key.pixel_shader}) {
    const auto *entry = static_cast<const ShaderCacheEntry *>(shader);
    if (entry && !g_prebuild.loaded.contains(entry)) {
      ++job.shaders_missing;
      g_prebuild.waiting[entry].push_back(&job);
    }
  }
  if (job.shaders_missing == 0)
    g_prebuild.queue.push_back(&job);
}

// banjo-nx: the prebuild workers, started once (by the list at the start, or by the first specialised pipeline).
// Requires g_prebuild.mutex.
void StartWorkersLocked() {
  if (g_prebuild.workers != 0 || g_prebuild.stop || g_prebuild.started)
    return;
  g_prebuild.started = true;
  // Leave the render and game threads their cores.
  const u32 cores = std::max(1u, std::thread::hardware_concurrency());
  const u32 workers = banjo::renut_gpu::PrebuildWorkers(std::clamp(cores / 2, 1u, 4u));  // banjo-nx
  g_prebuild.workers = workers;
  // banjo-nx: not std::thread::detach, which throws on the Switch (package 24's crash); a thread that is never
  // joined, with a stack of its own size there (banjo_renut_tuning.h). StopPipelinePrebuild waits for the workers.
  for (u32 i = 0; i < workers; ++i)
    banjo::renut_gpu::StartLongLivedThread(PrebuildWorker, 512 * 1024);
}

// banjo-nx (banjo_gpu_bool_spec): a specialised pipeline, without waiting. Ready: built (moved to 'out'); Pending:
// queued or building (a job made now if there was none); Failed: its build failed. Render thread.
// 2026-11-10 (console, the rats: the render thread blocked in the pipeline lookup, 80-92 % of its draw time, while uam
// compiled the specialised pipelines' variants): Busy, the prebuild lock is held by another thread; the draw takes the
// generic pipeline and the next one asks again (the render thread never waits for the lock here).
enum class SpecState { Ready, Pending, Failed, Busy };
std::atomic<uint64_t> g_pl_spec_calls{0}, g_pl_spec_ns{0}, g_pl_spec_busy{0}, g_pl_slow_calls{0}, g_pl_slow_ns{0};
// Dunwall (2026-11-12): the time in SpecialisedPipeline itself (the rest of a specialised lookup is the generic
// pipeline's lookup or build, and the list's record).
std::atomic<uint64_t> g_pl_spec_request_ns{0};
SpecState SpecialisedPipeline(const PipelineKey &key, uint64_t hash, const HostVertexLayout &layout,
                              std::unique_ptr<plume::RenderPipeline> &out) {
  std::unique_lock lock(g_prebuild.mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    g_pl_spec_busy.fetch_add(1, std::memory_order_relaxed);
    return SpecState::Busy;
  }
  if (g_prebuild.stop)
    return SpecState::Failed;
  auto &jobs = g_prebuild.by_hash[hash];
  for (Job *j : jobs) {
    if (std::memcmp(&j->key, &key, sizeof(key)) != 0)
      continue;
    if (j->stage != Job::Stage::Done)
      return SpecState::Pending;
    if (!j->pipeline)
      return SpecState::Failed;
    out = std::move(j->pipeline);
    return SpecState::Ready;
  }
  // A job of its own, self-contained (the record form copies the layout's names), as the list's are.
  auto job = std::make_unique<Job>();
  const std::vector<char> record = EncodeRecord(key, layout);
  if (!DecodeRecord(record.data(), record.size(), *job))
    return SpecState::Failed;
  Job *j = job.get();
  g_prebuild.jobs.push_back(std::move(job));
  jobs.push_back(j);
  QueueOrWait(*j);
  StartWorkersLocked();
  g_prebuild.work.notify_one();
  return SpecState::Pending;
}

bool Specialised(const PipelineKey &key) {
  if (key.alpha_func)
    return true;
  for (const uint32_t w : key.bool_mask) {
    if (w)
      return true;
  }
  return false;
}

// Render thread: specialised keys whose build failed (they draw with the generic pipeline).
std::unordered_set<uint64_t> g_spec_failed;

// The listed pipeline for the key, when the list has it: built by a worker
// (waited for when one is building it), or nullptr with 'claimed' set when no
// worker has started it, which the caller then builds itself. Render thread.
// 2026-11-10 (renut_draw_stage_timing): the render thread's waits for a prebuild worker's pipeline and its own builds.
std::atomic<uint64_t> g_pl_waits{0}, g_pl_wait_ns{0}, g_pl_builds{0}, g_pl_build_ns{0}, g_pl_lock_ns{0};

plume::RenderPipeline *TakePrebuilt(const PipelineKey &key, uint64_t hash,
                                    std::unique_ptr<plume::RenderPipeline> &out, bool &claimed) {
  claimed = false;
  const auto t_lock = std::chrono::steady_clock::now();
  std::unique_lock lock(g_prebuild.mutex);
  g_pl_lock_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t_lock).count()), std::memory_order_relaxed);
  auto it = g_prebuild.by_hash.find(hash);
  if (it == g_prebuild.by_hash.end())
    return nullptr;
  Job *job = nullptr;
  for (Job *j : it->second) {
    if (std::memcmp(&j->key, &key, sizeof(key)) == 0)
      job = j;
  }
  if (!job)
    return nullptr;
  if (job->stage == Job::Stage::Pending) {
    job->stage = Job::Stage::Done;  // the render thread's now
    claimed = true;
    return nullptr;
  }
  const auto t0 = std::chrono::steady_clock::now();
  g_prebuild.done.wait(lock, [job] { return job->stage == Job::Stage::Done; });
  g_pl_waits.fetch_add(1, std::memory_order_relaxed);
  g_pl_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
  out = std::move(job->pipeline);
  return out.get();
}

} // namespace

bool BuildDrawLayout(VideoState &s) {
  plume::RenderDescriptorSetBuilder textures;
  textures.begin();
  textures.addTexture(0, kTextureDescriptorCount);
  textures.end(true, kTextureDescriptorCount);

  plume::RenderDescriptorSetBuilder samplers;
  samplers.begin();
  samplers.addSampler(0, kSamplerDescriptorCount);
  samplers.end(true, kSamplerDescriptorCount);

  plume::RenderPipelineLayoutBuilder layout;
  layout.begin(false, true);
  layout.addDescriptorSet(textures);  // space0 Texture2D[]
  layout.addDescriptorSet(textures);  // space1 Texture3D[]
  layout.addDescriptorSet(textures);  // space2 TextureCube[]
  layout.addDescriptorSet(samplers);  // space3 SamplerState[]
  layout.addDescriptorSet(textures);  // space4 Texture2DArray[] (stacked textures)
  plume::RenderDescriptorSetBuilder constants;
  constants.begin();
  constants.addConstantBufferDynamic(0);
  constants.end();
  if (UsesVulkanBindings()) {  // banjo-nx: and deko3d
    // banjo-nx: the guest shaders' constants (sets 5-7, kVertexConstantsSet...).
    layout.addDescriptorSet(constants);
    layout.addDescriptorSet(constants);
    layout.addDescriptorSet(constants);
    // Geometry too: point_sprite_gs reads SharedConstants.
    layout.addPushConstant(0, 4, sizeof(DrawPushConstants),
                           plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::GEOMETRY |
                               plume::RenderShaderStageFlag::PIXEL);
  } else {
    layout.addRootDescriptor(0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    layout.addRootDescriptor(1, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    layout.addRootDescriptor(2, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
    layout.addRootDescriptor(0, 5, plume::RenderRootDescriptorType::SHADER_RESOURCE);
    layout.addRootDescriptor(0, 6, plume::RenderRootDescriptorType::UNORDERED_ACCESS);
  }
  layout.end();
  s.draw_layout = layout.create(s.device.get());
  if (!s.draw_layout) {
    RGPU_ERROR("Draw pipeline layout creation failed");
    return false;
  }
  return true;
}

static plume::RenderPipeline *GetPipelineSlow(const PipelineKey &key, const PipelineInputs &inputs, uint8_t &kind,
                                              uint64_t &memo_epoch);

// sa2-nx (2026-10-20, sa2_renut_chain_reuse): GetPipelineLocked's calls counted, and the prebuild epoch: a draw whose key
// is the previous draw's takes that draw's answer when no lookup was made since and no prebuilt pipeline came in
// (DrawStateLocked). The answer depends on nothing else: the last key and the memo change only in the lookups, and a
// memo entry of a generic pipeline is held only for its prebuild epoch.
uint64_t g_pipeline_lookups = 0;
uint64_t PipelineLookupsLocked() { return g_pipeline_lookups; }
uint64_t PrebuildEpochLocked() { return g_prebuild_epoch.load(std::memory_order_acquire); }

plume::RenderPipeline *GetPipelineLocked(const PipelineKey &key, const PipelineInputs &inputs) {
  ++g_pipeline_lookups;  // sa2-nx
  if (g_last_pipeline && std::memcmp(&g_last_key, &key, sizeof(key)) == 0)
    return g_last_pipeline;
  const bool memo_on = banjo::renut_gpu::PipelineMemoOn();
  const uint32_t set = PipelineMemoIndex(key);
  PipelineMemo *ways = g_pipeline_memo[set];
  if (memo_on) {
    for (uint32_t w = 0; w < 2; ++w) {
      PipelineMemo &m = ways[w];
      if (m.pipeline && std::memcmp(&m.key, &key, sizeof(key)) == 0 &&
          (m.kind != 1 || m.epoch == g_prebuild_epoch.load(std::memory_order_acquire))) {
        if (m.kind)
          banjo::renut_gpu::CountBoolSpec(banjo::renut_gpu::BoolSpecEvent::kGeneric);
        plume::RenderPipeline *hit = m.pipeline;
        if (w)
          std::swap(ways[0], ways[1]);  // the most recent in way 0
        return hit;
      }
    }
  }
  uint8_t kind = 0;
  uint64_t memo_epoch = 0;
  const auto t_slow = std::chrono::steady_clock::now();
  plume::RenderPipeline *result = GetPipelineSlow(key, inputs, kind, memo_epoch);
  g_pl_slow_calls.fetch_add(1, std::memory_order_relaxed);
  g_pl_slow_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t_slow).count()), std::memory_order_relaxed);
  if (memo_on && result) {
    // The least recent (way 1) goes; the new one takes way 0. (GetPipelineSlow may have filled this set for a generic
    // key: both are kept.)
    ways = g_pipeline_memo[set];
    ways[1] = ways[0];
    ways[0].key = key;
    ways[0].pipeline = result;
    ways[0].kind = kind;
    ways[0].epoch = memo_epoch;
  }
  return result;
}

static plume::RenderPipeline *GetPipelineSlow(const PipelineKey &key, const PipelineInputs &inputs, uint8_t &kind,
                                              uint64_t &memo_epoch) {
  const uint64_t hash = HashKey(key);
  auto &bucket = g_pipelines[hash];
  for (auto &cached : bucket) {
    if (std::memcmp(&cached.key, &key, sizeof(key)) == 0) {
      if (cached.pipeline) {
        g_last_key = key;
        g_last_pipeline = cached.pipeline.get();
      }
      return cached.pipeline.get();
    }
  }
  // banjo-nx (banjo_gpu_bool_spec): a specialised pipeline not built yet is built by a prebuild worker while the draws
  // take the generic one (every bool read at run time: the same result), from the first draw after it is done.
  if (Specialised(key)) {
    const uint64_t epoch = g_prebuild_epoch.load(std::memory_order_acquire);
    if (auto pend = g_spec_pending.find(hash); pend != g_spec_pending.end() && pend->second.epoch == epoch &&
                                               pend->second.generic &&
                                               std::memcmp(&pend->second.key, &key, sizeof(key)) == 0) {
      banjo::renut_gpu::CountBoolSpec(banjo::renut_gpu::BoolSpecEvent::kGeneric);
      kind = 1;
      memo_epoch = epoch;
      return pend->second.generic;
    }
    std::unique_ptr<plume::RenderPipeline> ready;
    const auto t_spec = std::chrono::steady_clock::now();
    struct SpecTimer {
      std::chrono::steady_clock::time_point t;
      ~SpecTimer() {
        g_pl_spec_calls.fetch_add(1, std::memory_order_relaxed);
        g_pl_spec_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - t).count()), std::memory_order_relaxed);
      }
    } spec_timer{t_spec};
    const auto t_request = std::chrono::steady_clock::now();
    const SpecState state = g_spec_failed.contains(hash) ? SpecState::Failed
                                                         : SpecialisedPipeline(key, hash, *inputs.layout, ready);
    g_pl_spec_request_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - t_request).count()),
                                   std::memory_order_relaxed);
    if (state != SpecState::Pending && state != SpecState::Busy)
      g_spec_pending.erase(hash);
    if (state == SpecState::Ready && ready) {
      g_list.Append(key, hash, *inputs.layout);
      bucket.push_back({key, std::move(ready)});
      g_last_key = key;
      g_last_pipeline = bucket.back().pipeline.get();
      banjo::renut_gpu::CountBoolSpec(banjo::renut_gpu::BoolSpecEvent::kBuilt);
      return g_last_pipeline;
    }
    if (state == SpecState::Failed)
      g_spec_failed.insert(hash);
    banjo::renut_gpu::CountBoolSpec(banjo::renut_gpu::BoolSpecEvent::kGeneric);
    PipelineKey generic = key;
    std::memset(generic.bool_mask, 0, sizeof(generic.bool_mask));
    std::memset(generic.bool_values, 0, sizeof(generic.bool_values));
    generic.alpha_func = 0;  // banjo-nx (banjo_deko3d_alpha_spec): the function read at run time
    plume::RenderPipeline *pipeline = GetPipelineLocked(generic, inputs);
    if (state == SpecState::Pending) {
      SpecPending &p = g_spec_pending[hash];
      p.key = key;
      p.epoch = epoch;
      p.generic = pipeline;
      kind = 1;
      memo_epoch = epoch;
    } else if (state == SpecState::Busy) {
      kind = 1;
      memo_epoch = epoch - 1;  // never the current epoch: the next draw asks again
    } else {
      kind = 2;  // failed: the generic one from now on
    }
    return pipeline;
  }
  std::unique_ptr<plume::RenderPipeline> pipeline;
  bool claimed = false;
  if (!TakePrebuilt(key, hash, pipeline, claimed)) {
    const auto t0 = std::chrono::steady_clock::now();
    pipeline = Build(key, inputs);
    g_pl_builds.fetch_add(1, std::memory_order_relaxed);
    g_pl_build_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
    if (pipeline && !claimed)
      g_list.Append(key, hash, *inputs.layout);
  }
  if (!pipeline) {
    static uint32_t logged = 0;
    if (!ReportDeviceLost(State().device.get()) && logged++ < 32) {
      const auto *vs = static_cast<const ShaderCacheEntry *>(key.vertex_shader);
      const auto *ps = static_cast<const ShaderCacheEntry *>(key.pixel_shader);
      RGPU_ERROR("Pipeline creation failed (key {:016X}): VS {:016X} (spec constants {:X}), "
                 "PS {:016X} (spec constants {:X}), {} targets [{} {} {} {}], depth format {}, "
                 "{} samples, {} input elements, topology {}",
                 hash, vs ? vs->hash : 0, vs ? vs->specConstantsMask : 0, ps ? ps->hash : 0,
                 ps ? ps->specConstantsMask : 0, key.color_count, u32(key.color_formats[0]),
                 u32(key.color_formats[1]), u32(key.color_formats[2]), u32(key.color_formats[3]),
                 u32(key.depth_format), key.samples, inputs.layout->input_elements.size(),
                 u32(key.topology));
    }
  }
  bucket.push_back({key, std::move(pipeline)});
  plume::RenderPipeline *result = bucket.back().pipeline.get();
  if (result) {
    g_last_key = key;
    g_last_pipeline = result;
  }
  return result;
}

void StartPipelinePrebuild() {
  std::vector<std::unique_ptr<Job>> jobs = LoadList();
  if (jobs.empty())
    return;
  std::lock_guard lock(g_prebuild.mutex);
  g_prebuild.jobs = std::move(jobs);
  for (const auto &job : g_prebuild.jobs) {
    g_prebuild.by_hash[job->hash].push_back(job.get());
    QueueOrWait(*job);
  }
  StartWorkersLocked();  // banjo-nx: shared with the specialised pipelines'
}

void StopPipelinePrebuild() {
  // The workers finish the pipeline each is building (they use the device).
  std::unique_lock lock(g_prebuild.mutex);
  g_prebuild.stop = true;
  g_prebuild.work.notify_all();
  g_prebuild.done.wait(lock, [] { return g_prebuild.workers == 0; });
}

void PipelineShaderLoaded(const ShaderCacheEntry *entry) {
  if (!entry)
    return;
  std::lock_guard lock(g_prebuild.mutex);
  if (!g_prebuild.loaded.insert(entry).second)
    return;
  auto it = g_prebuild.waiting.find(entry);
  if (it == g_prebuild.waiting.end())
    return;
  bool queued = false;
  for (Job *job : it->second) {
    // A job the render thread claimed meanwhile is done with.
    if (--job->shaders_missing == 0 && job->stage == Job::Stage::Pending) {
      g_prebuild.queue.push_back(job);
      queued = true;
    }
  }
  g_prebuild.waiting.erase(it);
  if (queued)
    g_prebuild.work.notify_all();
}

PipelineCounts PipelineCountsLocked() {
  PipelineCounts counts;
  for (const auto &[hash, bucket] : g_pipelines) {
    for (const CachedPipeline &cached : bucket)
      counts.built += cached.pipeline ? 1 : 0;
  }
  std::lock_guard lock(g_prebuild.mutex);
  counts.listed = g_prebuild.jobs.size();
  for (const auto &job : g_prebuild.jobs) {
    counts.prebuilt_unused += job->pipeline ? 1 : 0;
    counts.waiting += job->stage == Job::Stage::Pending && job->shaders_missing ? 1 : 0;
  }
  return counts;
}

} // namespace renut::gpu

namespace renut::gpu {
namespace {
// The 10 s report (PipelineList::Writer): counts since the last call.
std::string PipelineWaitReport() {
  const auto take = [](std::atomic<uint64_t> &a) { return a.exchange(0, std::memory_order_relaxed); };
  const uint64_t waits = take(g_pl_waits), wait_ns = take(g_pl_wait_ns), builds = take(g_pl_builds),
                 build_ns = take(g_pl_build_ns), lock_ns = take(g_pl_lock_ns);
  const uint64_t spec = take(g_pl_spec_calls), spec_ns = take(g_pl_spec_ns), busy = take(g_pl_spec_busy),
                 slow = take(g_pl_slow_calls), slow_ns = take(g_pl_slow_ns);
  // Dunwall (2026-11-12): where a specialised lookup's time goes, the list's writer, the DKSH cache's reads.
  const uint64_t request_ns = take(g_pl_spec_request_ns), queued = take(g_list.queued),
                 queued_ns = take(g_list.queued_ns), written = take(g_list.written), write_ns = take(g_list.write_ns),
                 syncs = take(g_list.syncs), sync_ns = take(g_list.sync_ns), max_io_ns = take(g_list.max_io_ns);
#if defined(PLUME_DEKO3D)
  const std::string dksh = banjo::renut_gpu::deko3d::DkshReadReport();
#else
  const std::string dksh = "no DKSH cache";
#endif
  return fmt::format("; pipelines: {} waits for a prebuild worker ({:.1f} ms), {} built on the render thread "
                     "({:.1f} ms), {:.1f} ms taking the prebuild lock; {} slow lookups ({:.1f} ms), {} of them "
                     "specialised ({:.1f} ms, {:.1f} ms of it asking for the specialised one, {} found the prebuild "
                     "lock busy); list: {} records queued ({:.1f} ms on the render thread), writer {} written "
                     "({:.1f} ms), {} syncs ({:.1f} ms, slowest write or sync {:.1f} ms); {}",
                     waits, double(wait_ns) / 1e6, builds, double(build_ns) / 1e6, double(lock_ns) / 1e6, slow,
                     double(slow_ns) / 1e6, spec, double(spec_ns) / 1e6, double(request_ns) / 1e6, busy, queued,
                     double(queued_ns) / 1e6, written, double(write_ns) / 1e6, syncs, double(sync_ns) / 1e6,
                     double(max_io_ns) / 1e6, dksh);
}
}  // namespace
}  // namespace renut::gpu
