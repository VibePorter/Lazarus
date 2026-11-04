#include "renut_engine/gpu/render/easu_constants.h"

#include <cassert>
#include <cmath>
#include <cstdint>

// Use AMD's supported CPU setup, rather than maintaining a second formula.
// Vendor types/functions stay local to this translation unit. See ffx_fsr1_LICENSE.txt.
namespace {
#define A_CPU 1
#define A_GCC 1
#define A_STATIC inline
#include "renut_engine/gpu/shaders/host/ffx_a.h"
#include "renut_engine/gpu/shaders/host/ffx_fsr1.h"
#undef A_GCC
#undef A_CPU
#undef A_STATIC
}  // namespace

namespace renut::gpu {

EasuConstants MakeEasuConstants(uint32_t input_width, uint32_t input_height,
                                uint32_t output_width, uint32_t output_height) {
  assert(input_width && input_height && output_width && output_height);
  EasuConstants out{};
  FsrEasuCon(out.con[0], out.con[1], out.con[2], out.con[3], float(input_width), float(input_height),
             float(input_width), float(input_height), float(output_width), float(output_height));
  return out;
}

}  // namespace renut::gpu
