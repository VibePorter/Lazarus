// me2-nx-deko (2026-11-04): the draw-count rotation (me2_ab_cycle.cpp). A test run turns the listed draw cuts off one at
// a time (all on, the first off, all on, the second off, ...) and logs draws per frame each second, so a player standing
// still gets each cut's saving from neighbouring slots of the same view.
#pragma once

enum class Me2Ab : int {
  kOcclusion,       // Umbra latent answers (me2_occlusion.cpp LatentAnswer): off = every node visible
  kLightSmall,      // me2_light_small_factor
  kLightWhole,      // me2_light_whole_factor
  kLightEdge,       // me2_light_edge
  kUmbraSize,       // me2_cull_small_factor / me2_cull_dynamic_factor on Umbra levels (op 50)
  kSmallMesh,       // me2_cull_small_factor in InitViews (levels without Umbra)
  kPrepassSmall,    // me2_prepass_small_factor
  kLod,             // me2_lod_distance_scale
  kRetestOccluded,  // me2_occlusion_umbra_retest_occluded
  kLightBoost,      // me2_light_small_boost (off = the plain size rule: more light draws left out)
  kRaster,          // me2_occlusion_umbra_cpu 2 (off = mode 1: the GPU boxes / latent answers answer Umbra again)
  kRelink,          // me2_relink_skip_same (off = every primitive transform update executed)
  kMaterial,        // me2_material_params (off = UE3's own material parameter setup sub_825DCDA0)
  kNativeVis,       // me2_native_vis_filter (off = every instance Umbra reports is drawn)
  kReaderRaster,    // me2_occlusion_reader_raster (off = per-primitive occlusion reads wait for the GPU)
  kCount
};

// Whether the rotation has this cut off right now (false without a rotation).
bool Me2AbOff(Me2Ab id);
// Once a frame (the Swap hook): moves the rotation on and writes the "[ab]" line each second.
void Me2AbTick();
