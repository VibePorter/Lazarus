#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// UE3 row-vector camera matrices. Pointer identity is deliberately irrelevant:
// FSceneView allocations rotate during an otherwise continuous camera shot.
struct Me2OcclusionView {
  float view[16] = {};
  float projection[16] = {};

  bool Valid() const {
    for (unsigned i = 0; i < 16; ++i)
      if (!std::isfinite(view[i]) || !std::isfinite(projection[i])) return false;
    for (unsigned r = 0; r < 3; ++r) {
      float length2 = 0;
      for (unsigned c = 0; c < 3; ++c) length2 += view[r * 4 + c] * view[r * 4 + c];
      if (length2 < 0.5f || length2 > 1.5f) return false;
    }
    return std::fabs(view[15] - 1.0f) < 0.01f;
  }

  void Position(float (&out)[3]) const {
    for (unsigned c = 0; c < 3; ++c)
      out[c] = -(view[12] * view[c * 4] + view[13] * view[c * 4 + 1] + view[14] * view[c * 4 + 2]);
  }
};

struct Me2OcclusionViewHistory {
  enum Reason : unsigned { kRotation = 1, kPosition = 2, kProjection = 4, kInvalid = 8, kGap = 16 };
  Me2OcclusionView previous;
  uint64_t epoch = 1, frame = 0;
  bool seen = false;

  // Conservative discontinuity detection, not an inferred guest bCameraCut bit.
  // Normal walking/turning and small temporal projection jitter keep their cache.
  // Fast movement may also trigger a reset, costing culling only until new counts.
  unsigned Observe(const Me2OcclusionView& current, uint64_t now) {
    unsigned reason = 0;
    if (!current.Valid()) reason |= kInvalid;
    if (seen) {
      if (!previous.Valid()) reason |= kInvalid;
      if (now < frame || now - frame > 1) reason |= kGap;
      if (!(reason & kInvalid)) {
        float rotation = 0;
        for (unsigned r = 0; r < 3; ++r)
          for (unsigned c = 0; c < 3; ++c)
            rotation = std::max(rotation, std::fabs(current.view[r * 4 + c] - previous.view[r * 4 + c]));
        if (rotation > 0.2f) reason |= kRotation;
        float a[3], b[3];
        previous.Position(a);
        current.Position(b);
        float distance2 = 0;
        for (unsigned c = 0; c < 3; ++c) distance2 += (a[c] - b[c]) * (a[c] - b[c]);
        if (distance2 > 128.0f * 128.0f) reason |= kPosition;
        for (unsigned i = 0; i < 16; ++i) {
          const float scale = std::max(1.0f, std::max(std::fabs(previous.projection[i]), std::fabs(current.projection[i])));
          if (std::fabs(current.projection[i] - previous.projection[i]) > 0.05f * scale)
            reason |= kProjection;
        }
      }
    }
    if (reason && ++epoch == 0) epoch = 1;
    previous = current;
    frame = now;
    seen = true;
    return reason;
  }
};
