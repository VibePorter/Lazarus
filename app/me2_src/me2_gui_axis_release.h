#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// BioPlayerInput.PreProcessInput skips a GUI stick pair when both buffered axes
// are exactly zero. A normal, sub-dead-zone marker lets the unchanged script
// enter the block and emit two EXACT zeros on one neutral transition. It is
// confined to AxisBuffer; movement/camera axes and the physical pad are untouched.
struct Me2GuiAxisReleasePair {
  bool active = false;
  bool pending = false;
  float marker = 0.0f;

  // Called once immediately before eventPlayerInput, AFTER every binding has
  // copied aGuiStrafe/aGuiBaseY/etc. into AxisBuffer (sub_829B7CB0). The ordinary
  // float accumulator never targets AxisBuffer itself.
  bool Prepare(float& x, float y, float deadzone) {
    if (pending && x == marker && y == 0.0f) return false; // script did not consume it yet
    pending = false;
    const bool now_active = x != 0.0f || y != 0.0f;
    const bool released = active && !now_active;
    active = now_active;
    if (!released || !(deadzone > 0.0f) || !std::isfinite(deadzone)) return false;
    const float value = std::min(0.000001f, deadzone * 0.25f);
    if (value < std::numeric_limits<float>::min() || !(value < deadzone)) return false;
    marker = x = value;
    pending = true;
    return true;
  }
};
