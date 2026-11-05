// me2-nx-deko (2026-11-04): the watched range, the app's side (tools/watch_marks.py; rex::memory::WatchedRange in the
// SDK, which keeps a pointer to this object for its host writers). One dirty bit per 64-byte chunk of the D3D device:
// the instrumented guest stores, the natives and Renut's hooks mark what they write, and the draw capture
// (backend/queued.cpp CaptureDelta) compares only those chunks with me2_renut_watch_chunks.
#pragma once

#include <cstdint>

#ifndef REX_WATCHED_RANGE_DEFINED
#define REX_WATCHED_RANGE_DEFINED
// The same layout as the generated code's (tools/watch_marks.py) and rex::memory::WatchedRange.
struct RexWatchedRange {
  uint32_t lo;
  uint32_t span;  // bytes; 0: nothing is watched
  uint64_t dirty[4];
};
#endif
extern "C" RexWatchedRange g_me2_watch;

// Marks the chunks [a, a + n) overlaps (a host writer of guest memory).
inline void Me2WatchMark(uint32_t a, uint32_t n) {
  const uint64_t lo = g_me2_watch.lo, span = g_me2_watch.span, end = uint64_t(a) + n;
  if (!span || !n || end <= lo || a >= lo + span)
    return;
  const uint32_t first = a > lo ? uint32_t(a - lo) : 0u;
  const uint32_t last = uint32_t((end - lo < span ? end - lo : span) - 1);
  for (uint32_t c = first >> 6; c <= (last >> 6); ++c)
    g_me2_watch.dirty[c >> 6] |= uint64_t(1) << (c & 63);
}
// Every chunk (a host writer that does not say what it wrote).
inline void Me2WatchMarkAll() {
  for (uint64_t& w : g_me2_watch.dirty)
    w = ~uint64_t(0);
}
