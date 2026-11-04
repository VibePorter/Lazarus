// me2-nx-deko (2026-11-03): write census of UE3 render-thread phases (experiment: could a phase run on a second thread?).
//
// Diagnostic builds only (CMake ME2_WRITE_CENSUS=ON, never the release NRO). Every guest store outside the stack goes
// through REX_CARD (generated/default/sa2_pch.h, tools/store_cards.py); in a census build REX_CARD also calls
// Me2CensusNote while g_me2_census_on is set. The hooks below set it around three phases of UE3's render thread:
//   bit 0  sub_8269CBD8  dynamic-light passes (~19 % of the UE3 render thread at the Kasumi party, 2026-11-03 stacks)
//   bit 1  sub_82748200  Umbra's base-pass draw of one object (~30 %)
//   bit 2  sub_82748018  depth prepass of one object (~8 %)
// and the census counts each store by (phase, guest 4 KB page, writing host function: the return address into the
// recompiled function that stored). What a phase writes is what two threads running it at once would race on.
//
// Not seen: stores to the guest stack (per thread anyway) and guest memory written by host code (our hooks, natives,
// the renderer's own writes). Report: "[census]" lines every me2_census_report_s; each writer as an offset from
// Me2CensusNote (anchor), to symbolise against the ELF.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

extern "C" volatile unsigned char g_me2_census_on = 0;
extern "C" void Me2CensusNote(unsigned ea);

#if defined(ME2_WRITE_CENSUS)

REXCVAR_DEFINE_INT32(me2_census_phases, 0, "ME2/Diagnostics",
                     "Write census build: phases whose guest stores are counted (bit 0 light passes sub_8269CBD8, "
                     "bit 1 Umbra base-pass draw sub_82748200, bit 2 depth prepass sub_82748018); 0 = off")
    .range(0, 7)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_census_report_s, 60, "ME2/Diagnostics",
                     "Write census build: seconds between [census] reports")
    .range(5, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr const char* kPhaseName[4] = {"-", "lights 8269CBD8", "base 82748200", "prepass 82748018"};

struct Entry {
  uint64_t key = 0;  // phase << 60 | page << 32 | (writer offset + 2^31)
  uint64_t count = 0;
  uint16_t lo = 0xFFFF, hi = 0;  // lowest / highest offset written in the page
};
constexpr uint32_t kEntries = 1u << 16;
Entry g_table[kEntries];
uint64_t g_dropped = 0;
uint64_t g_stores[4] = {}, g_calls[4] = {};
thread_local int t_phase = 0;
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();

int Phases() {
  static const int p = REXCVAR_GET(me2_census_phases);
  return p;
}

void Report() {
  const auto now = std::chrono::steady_clock::now();
  static const int report_s = REXCVAR_GET(me2_census_report_s);
  if (now - g_last_report < std::chrono::seconds(report_s))
    return;
  g_last_report = now;
  std::vector<const Entry*> all;
  for (const Entry& e : g_table)
    if (e.count)
      all.push_back(&e);
  REXLOG_INFO("[census] anchor Me2CensusNote={:016X}; entries {} (dropped {})",
              reinterpret_cast<uintptr_t>(&Me2CensusNote), all.size(), g_dropped);
  for (int ph = 1; ph <= 3; ++ph) {
    if (!g_calls[ph])
      continue;
    std::vector<const Entry*> mine;
    for (const Entry* e : all)
      if (int(e->key >> 60) == ph)
        mine.push_back(e);
    std::sort(mine.begin(), mine.end(), [](const Entry* a, const Entry* b) { return a->count > b->count; });
    REXLOG_INFO("[census] phase {}: {} calls, {} guest stores ({:.1f} a call), {} (page, writer) pairs",
                kPhaseName[ph], g_calls[ph], g_stores[ph], double(g_stores[ph]) / double(g_calls[ph]), mine.size());
    // The pairs with the most stores first (the analysis groups them offline); at most 3,000 a phase.
    if (mine.size() > 3000)
      mine.resize(3000);
    for (const Entry* e : mine) {
      const uint32_t page = uint32_t(e->key >> 32) & 0xFFFFF;
      const int64_t writer = int64_t(uint32_t(e->key)) - (int64_t(1) << 31);
      REXLOG_INFO("[census-w] {} page {:08X} +{:03X}..+{:03X} writer {:+X} stores {}", ph, page << 12, e->lo, e->hi,
                  writer, e->count);
    }
  }
}

struct PhaseScope {
  int previous;
  bool active;
  explicit PhaseScope(int phase) : previous(t_phase), active((Phases() >> (phase - 1)) & 1) {
    if (!active)
      return;
    t_phase = phase;
    ++g_calls[phase];
    g_me2_census_on = 1;
  }
  ~PhaseScope() {
    if (!active)
      return;
    t_phase = previous;
    if (!previous) {
      g_me2_census_on = 0;
      Report();
    }
  }
};

}  // namespace

extern "C" __attribute__((noinline)) void Me2CensusNote(unsigned ea) {
  const int phase = t_phase;
  if (!phase)
    return;  // another thread's store while a phase runs
  ++g_stores[phase];
  const uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  const int64_t off = int64_t(ra) - int64_t(reinterpret_cast<uintptr_t>(&Me2CensusNote));
  const uint64_t key = (uint64_t(phase) << 60) | (uint64_t(ea >> 12) << 32) | uint32_t(off + (int64_t(1) << 31));
  uint32_t h = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 48);
  for (uint32_t i = 0; i < 64; ++i, h = (h + 1) & (kEntries - 1)) {
    Entry& e = g_table[h];
    if (e.key == key || e.count == 0) {
      e.key = key;
      ++e.count;
      const uint16_t in_page = uint16_t(ea & 0xFFF);
      e.lo = std::min(e.lo, in_page);
      e.hi = std::max(e.hi, in_page);
      return;
    }
  }
  ++g_dropped;
}

REX_EXTERN(__imp__sub_8269CBD8);
REX_HOOK_RAW(sub_8269CBD8) {
  PhaseScope scope(1);
  __imp__sub_8269CBD8(ctx, base);
}
REX_EXTERN(__imp__sub_82748200);
REX_HOOK_RAW(sub_82748200) {
  PhaseScope scope(2);
  __imp__sub_82748200(ctx, base);
}
// sub_82748018 (the depth prepass) is hooked by me2_prepass.cpp, which opens phase 3 through these.
extern "C" uint64_t Me2CensusEnter(int phase) {
  const bool active = (Phases() >> (phase - 1)) & 1;
  const int previous = t_phase;
  if (active) {
    t_phase = phase;
    ++g_calls[phase];
    g_me2_census_on = 1;
  }
  return (uint64_t(active) << 32) | uint32_t(previous);
}
extern "C" void Me2CensusLeave(uint64_t token) {
  if (!(token >> 32))
    return;
  const int previous = int(uint32_t(token));
  t_phase = previous;
  if (!previous) {
    g_me2_census_on = 0;
    Report();
  }
}

#else

// Release builds: REX_CARD never calls this.
extern "C" void Me2CensusNote(unsigned) {}

#endif  // ME2_WRITE_CENSUS
