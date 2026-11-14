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
  enum Reason : unsigned { kRotation = 1, kPosition = 2, kProjection = 4, kInvalid = 8, kGap = 16, kSpike = 32 };
  Me2OcclusionView previous;
  uint64_t epoch = 1, frame = 0;
  bool seen = false;
  // me2-nx-deko (2026-11-04): the largest per-frame change of a view rotation element that still counts as turning, not
  // a cut (me2_occlusion_cut_rot). Was a fixed 0.2 (~12 degrees a frame): at low frame rates a firm stick turn crossed it
  // every frame, each "cut" threw every occlusion answer away, frame rate fell further (console, Kasumi's party: 23 fps
  // looking, 5 fps turning, draws 1,300 -> 4,500 a frame).
  float rot_cut = 0.2f;
  // ME2 (2026-11-12, user: in cutscenes and dialogues models do not appear at once after the camera changes position,
  // and on the Normandy the stars showed through for an instant): a dialogue's reverse shot or a new angle from nearby
  // stays under the absolute limits (60 degrees, 128 units), so the old camera's "occluded" answers lived on. A camera
  // change that is a spike against the last kSpikeHistory frames' motion (more than spike_factor times their largest
  // change, and above spike_min_rot / spike_min_move) is a cut too. A steady turn or walk changes by about the same each
  // frame, so it never is; a turn or a run starting from rest can count once (a few frames of culling lost).
  // spike_factor 0 = off (me2_occlusion_cut_spike_factor).
  static constexpr unsigned kSpikeHistory = 8;
  float spike_factor = 0.0f, spike_min_rot = 0.15f, spike_min_move = 24.0f;
  float recent_rot[kSpikeHistory] = {}, recent_move[kSpikeHistory] = {};
  unsigned recent_count = 0;

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
        if (rotation > rot_cut) reason |= kRotation;
        float a[3], b[3];
        previous.Position(a);
        current.Position(b);
        float distance2 = 0;
        for (unsigned c = 0; c < 3; ++c) distance2 += (a[c] - b[c]) * (a[c] - b[c]);
        if (distance2 > 128.0f * 128.0f) reason |= kPosition;
        const float distance = std::sqrt(distance2);
        if (spike_factor > 0.0f && recent_count >= kSpikeHistory) {
          float max_rot = 0, max_move = 0;
          for (unsigned i = 0; i < kSpikeHistory; ++i) {
            max_rot = std::max(max_rot, recent_rot[i]);
            max_move = std::max(max_move, recent_move[i]);
          }
          if ((rotation > spike_min_rot && rotation > spike_factor * max_rot) ||
              (distance > spike_min_move && distance > spike_factor * max_move))
            reason |= kSpike;
        }
        // The motion history: this frame's change, unless it was a cut (the next frames compare with the shot's own
        // motion, not with the jump).
        if (!(reason & (kRotation | kPosition | kSpike))) {
          recent_rot[recent_count % kSpikeHistory] = rotation;
          recent_move[recent_count % kSpikeHistory] = distance;
          ++recent_count;
        } else {
          recent_count = 0;
        }
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
