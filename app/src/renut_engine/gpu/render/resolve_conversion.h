#pragma once

#include <cstdint>
#include <plume_render_interface_types.h>

namespace renut::gpu {

// D3DRESOLVE's signed six-bit exponent adjustment (Flags bits 26..31).
inline constexpr int32_t ResolveExponentBias(uint32_t flags) {
  return int32_t(flags) >> 26;
}

// The unscaled same-format pass retains finite UNORM/FP16 values exactly.
// Nonfinite FP16 output behavior is tested across every input encoding.
// Scaling in that first draw removes only the intermediate round trip;
// averaging, format conversion, sRGB and FP32 denormals are excluded.
inline constexpr bool CanFuseResolveExponentBias(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R8G8_UNORM:
  case plume::RenderFormat::R8_UNORM:
  case plume::RenderFormat::R16_UNORM:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R16_FLOAT:
    return true;
  default:
    return false;
  }
}

} // namespace renut::gpu
