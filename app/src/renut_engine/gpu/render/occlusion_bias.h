#pragma once

#include <cstdint>
#include <optional>

namespace renut::gpu {

struct OcclusionDepthBias {
  int32_t units;
  float clamp;
};

// Pure visibility tests have no framebuffer side effects. They need a
// conservative depth margin even when colour/character depth bias is disabled.
// Keep stencil masks and depth-writing prepasses on their existing policy.
inline std::optional<OcclusionDepthBias> OcclusionTestBias(bool deko, uint32_t color_mask,
                                                         uint32_t depth_control, int32_t units,
                                                         float clamp) {
  if (!deko || color_mask != 0 || (depth_control & 7u) != 2u)
    return std::nullopt;
  const uint32_t compare = (depth_control >> 4) & 7u;
  if (compare == 4 || compare == 6)
    return OcclusionDepthBias{units, clamp};
  if (compare == 1 || compare == 3)
    return OcclusionDepthBias{-units, -clamp};
  return std::nullopt;  // shifting depth cannot conservatively implement EQUAL/NOT_EQUAL
}

}  // namespace renut::gpu
