#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

// A centre cell is only a lookup hint, not the identity of an Umbra volume.
// Reusing a zero count is conservative only for a box contained by the tested
// box. Recognize eight axis-aligned corners before using that property.
struct Me2OcclusionBounds {
  // Zero-initialized invalid boxes keep the fixed cache tables in BSS.
  float lo[3] = {};
  float hi[3] = {};

  bool Valid() const {
    for (int c = 0; c < 3; ++c)
      if (!std::isfinite(lo[c]) || !std::isfinite(hi[c]) || !(lo[c] < hi[c])) return false;
    return true;
  }

  static Me2OcclusionBounds FromCorners(const float (&vertices)[8][3]) {
    Me2OcclusionBounds box;
    for (int c = 0; c < 3; ++c) {
      box.lo[c] = std::numeric_limits<float>::infinity();
      box.hi[c] = -std::numeric_limits<float>::infinity();
    }
    for (const auto& v : vertices)
      for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(v[c])) return {};
        box.lo[c] = std::min(box.lo[c], v[c]);
        box.hi[c] = std::max(box.hi[c], v[c]);
      }
    if (!box.Valid()) return {};
    unsigned corners = 0;
    for (const auto& v : vertices) {
      unsigned corner = 0;
      for (int c = 0; c < 3; ++c) {
        if (v[c] == box.hi[c]) corner |= 1u << c;
        else if (v[c] != box.lo[c]) return {};
      }
      corners |= 1u << corner;
    }
    return corners == 0xFF ? box : Me2OcclusionBounds{};
  }

  bool Contains(const Me2OcclusionBounds& current) const {
    if (!Valid() || !current.Valid()) return false;
    for (int c = 0; c < 3; ++c)
      if (current.lo[c] < lo[c] || current.hi[c] > hi[c]) return false;
    return true;
  }
};
