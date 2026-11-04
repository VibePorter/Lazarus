// sa2-nx (2026-10-20): the game's frame pacing with Renut's renderer (sa2_renut_pacing.cpp).
#pragma once

#include <atomic>
#include <cstdint>

namespace sa2::renut {

// Called by the Swap hook before the frame goes to the render thread: waits until the game's presentation interval
// since the previous Swap has passed (the console's vertical blank).
void PaceSwap(uint32_t device_va);

// me2-nx-deko (2026-10-31): an observer called at every Swap's release (the Swap's thread) with the frame's time from
// the previous release to this Swap and the pacing's sleep, ms (me2_src/me2_frame_diag.cpp: spike lines, frame-time
// histogram). Null = none.
using FrameObserver = void (*)(double work_ms, double sleep_ms);
extern std::atomic<FrameObserver> g_frame_observer;

}  // namespace sa2::renut
