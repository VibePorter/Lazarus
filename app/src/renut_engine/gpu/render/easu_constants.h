#pragma once

#include <cstdint>

namespace renut::gpu {

// Four uint4s consumed verbatim by AMD's FsrEasuF. Stored as float bit patterns.
struct EasuConstants {
  uint32_t con[4][4];
};
static_assert(sizeof(EasuConstants) == 64);

// The whole base-level texture is the input viewport, matching the original
// pixel shader's GetDimensions + FsrEasuCon. Dimensions must be nonzero.
EasuConstants MakeEasuConstants(uint32_t input_width, uint32_t input_height,
                                uint32_t output_width, uint32_t output_height);

}  // namespace renut::gpu
