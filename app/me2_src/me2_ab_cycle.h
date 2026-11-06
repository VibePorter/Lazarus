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
  kColorHandover,   // me2_renut_color_handover (off = every colour resolve copied as before)
  kLightOcc,        // me2_light_occlusion 2 (off = counted only, nothing left out)
  kCompositeLights, // UE3 CompositeDynamicLights forced on (off = the game's value, False)
  kOcclNoWait,      // me2_occlusion_nowait_visible (off = a late GPU result is waited for, as before)
  kOcclNoWaitShadow,  // me2_occlusion_nowait_shadows (off = a late hidden shadow result is waited for)
  kRasterLean,
  kDeltaRaw,         // me2_renut_watch_raw (off = the marked chunks compared on the UE3 render thread, as before)       // me2_occlusion_raster_lean limits (off = me2_occlusion_raster_min_px / _min_px_per_tri)
  kCount
};

// Whether the rotation has this cut off right now (false without a rotation).
bool Me2AbOff(Me2Ab id);
// me2-nx-deko (2026-11-06): whether the rotation lists this cut (it is then on in every slot but its own "off" one,
// whatever its own setting says).
bool Me2AbInRotation(Me2Ab id);
// Once a frame (the Swap hook): moves the rotation on and writes the "[ab]" line each second.
void Me2AbTick();
// me2-nx-deko (2026-11-06): once a frame after Me2AbTick (the Swap hook): UE3's CompositeDynamicLights as
// me2_composite_lights and the rotation want it.
void Me2CompositeLightsTick(unsigned char* base);
