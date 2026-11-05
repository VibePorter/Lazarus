// me2-nx-deko (2026-11-04): the draw-count rotation (see me2_ab_cycle.h).
//
// me2_ab_cycle lists the cuts to rotate ("light_edge,light_whole,..."), me2_ab_cycle_s the seconds of a slot. Slot k of
// the rotation: k even = every cut as configured; k odd = cut k / 2 of the list off. Each second a line
//   [ab] t <s> slot <k> (<s> s in) off <name|-> frames <n> draws <n> (<x> a frame)
// tools/rhi/ab_draws.py pairs each "off" slot with the slots around it (the first second of a slot is left out: UE3's
// caches, Umbra's latent answers and LODs settle).
#include "me2_ab_cycle.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(me2_ab_cycle, "", "ME2/Diagnostics",
                      "Draw-count rotation: comma-separated cuts turned off one at a time (occlusion, light_small, "
                      "light_whole, light_edge, umbra_size, small_mesh, prepass, lod, retest_occluded, light_boost, "
                      "raster = me2_occlusion_umbra_cpu 2 -> 1); \"[ab]\" "
                      "lines "
                      "each second; empty = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_ab_cycle_s, 6, "ME2/Diagnostics", "Draw-count rotation: seconds of a slot")
    .range(2, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_me2_draws_total;        // renut d3d hooks/draw.cpp: every draw
extern std::atomic<uint64_t> g_me2_begin_draws_total;  // ... and the Begin*Vertices draws (Umbra test boxes)

namespace {

constexpr const char* kNames[int(Me2Ab::kCount)] = {"occlusion", "light_small", "light_whole", "light_edge", "umbra_size",
                                                    "small_mesh", "prepass",     "lod",         "retest_occluded", "light_boost",
                                                    "raster",    "relink",      "material",  "native_vis", "reader_raster"};

struct Rotation {
  std::vector<int> cuts;  // Me2Ab values in rotation order
  bool parsed = false;
};

Rotation& Get() {
  static Rotation r = [] {
    Rotation out;
    const std::string list = REXCVAR_GET(me2_ab_cycle);
    size_t at = 0;
    while (at <= list.size()) {
      size_t end = list.find(',', at);
      if (end == std::string::npos) end = list.size();
      std::string name = list.substr(at, end - at);
      while (!name.empty() && name.front() == ' ') name.erase(name.begin());
      while (!name.empty() && name.back() == ' ') name.pop_back();
      if (!name.empty()) {
        int found = -1;
        for (int i = 0; i < int(Me2Ab::kCount); ++i)
          if (name == kNames[i]) found = i;
        if (found >= 0)
          out.cuts.push_back(found);
        else
          REXLOG_WARN("[ab] unknown cut '{}' in me2_ab_cycle (left out)", name);
      }
      at = end + 1;
    }
    out.parsed = true;
    if (!out.cuts.empty()) {
      std::string names;
      for (int c : out.cuts) names += std::string(names.empty() ? "" : ", ") + kNames[c];
      REXLOG_INFO("[ab] draw-count rotation: {} s a slot, off in turn: {}", REXCVAR_GET(me2_ab_cycle_s), names);
    }
    return out;
  }();
  return r;
}

std::atomic<int> g_off{-1};  // the cut off now (-1: none)

}  // namespace

bool Me2AbOff(Me2Ab id) { return g_off.load(std::memory_order_relaxed) == int(id); }

void Me2AbTick() {
  Rotation& r = Get();
  if (r.cuts.empty()) return;
  using clock = std::chrono::steady_clock;
  static const clock::time_point start = clock::now();
  static const int slot_s = REXCVAR_GET(me2_ab_cycle_s);
  static int64_t last_second = -1;
  static uint64_t frames = 0, draws0 = g_me2_draws_total.load(std::memory_order_relaxed);
  static uint64_t begin0 = g_me2_begin_draws_total.load(std::memory_order_relaxed);
  ++frames;
  const int64_t t = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - start).count();
  const int64_t slot = t / slot_s;
  const int64_t period = int64_t(r.cuts.size()) * 2;
  const int64_t k = slot % period;
  const int off = (k & 1) ? r.cuts[size_t(k / 2)] : -1;
  if (t != last_second) {
    if (last_second >= 0) {
      // The second that just ended (its slot and cut).
      const int64_t prev_slot = last_second / slot_s, pk = prev_slot % period;
      const int prev_off = (pk & 1) ? r.cuts[size_t(pk / 2)] : -1;
      const uint64_t draws1 = g_me2_draws_total.load(std::memory_order_relaxed);
      const uint64_t begin1 = g_me2_begin_draws_total.load(std::memory_order_relaxed);
      REXLOG_INFO("[ab] t {} slot {} ({} s in) off {} frames {} draws {} ({:.1f} a frame) begin {} ({:.1f} a frame)",
                  last_second, prev_slot, last_second - prev_slot * slot_s, prev_off < 0 ? "-" : kNames[prev_off],
                  frames, draws1 - draws0, frames ? double(draws1 - draws0) / double(frames) : 0.0, begin1 - begin0,
                  frames ? double(begin1 - begin0) / double(frames) : 0.0);
      draws0 = draws1;
      begin0 = begin1;
    }
    frames = 0;
    last_second = t;
  }
  g_off.store(off, std::memory_order_relaxed);
}
