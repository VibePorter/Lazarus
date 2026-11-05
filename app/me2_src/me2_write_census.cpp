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
// me2-nx-deko (2026-11-04, me2_census_device): the device census. From the first draw on (Renut's queued.cpp calls
// Me2CensusDeviceStart once it watches the D3D device), every guest store into the device is counted by its source
// line in the generated code (REX_CARD passes __FILE__ / __LINE__), with the 64-byte chunks it wrote. tools/rhi/
// census_dev.py maps the lines to guest functions: the list tools/watch_marks.py instruments (app/watch_writers.txt).
//
// Not seen: stores to the guest stack (per thread anyway) and guest memory written by host code (our hooks, natives,
// the renderer's own writes). Report: "[census]" lines every me2_census_report_s; each writer as an offset from
// Me2CensusNote (anchor), to symbolise against the ELF.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <bit>
#include <string>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" volatile unsigned char g_me2_census_on = 0;
extern "C" void Me2CensusNote(unsigned ea);
extern "C" void Me2CensusNoteAt(unsigned ea, const char* file, unsigned line);
extern "C" void Me2CensusDeviceStart();
extern "C" void Me2CensusDeviceTick();
// The watched range (me2_watch.h; Renut's queued.cpp sets it): the device's guest address and size.
#include "me2_watch.h"

#if defined(ME2_WRITE_CENSUS)

REXCVAR_DEFINE_INT32(me2_census_phases, 0, "ME2/Diagnostics",
                     "Write census build: phases whose guest stores are counted (bit 0 light passes sub_8269CBD8, "
                     "bit 1 Umbra base-pass draw sub_82748200, bit 2 depth prepass sub_82748018, bit 3 a base-pass element's own "
                     "part after its link's DrawShared, me2_shadow_diag.cpp); 0 = off")
    .range(0, 15)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_census_device, false, "ME2/Diagnostics",
                    "Write census build: count every guest store into the D3D device by its generated source line "
                    "(\"[census-dev]\" lines; tools/rhi/census_dev.py)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_census_report_s, 60, "ME2/Diagnostics",
                     "Write census build: seconds between [census] reports")
    .range(5, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr const char* kPhaseName[5] = {"-", "lights 8269CBD8", "base 82748200", "prepass 82748018",
                                       "element after DrawShared"};

struct Entry {
  uint64_t key = 0;  // phase << 60 | page << 32 | (writer offset + 2^31)
  uint64_t count = 0;
  uint16_t lo = 0xFFFF, hi = 0;  // lowest / highest offset written in the page
};
constexpr uint32_t kEntries = 1u << 16;
Entry g_table[kEntries];
uint64_t g_dropped = 0;
uint64_t g_stores[5] = {}, g_calls[5] = {};
thread_local int t_phase = 0;
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();

int Phases() {
  static const int p = REXCVAR_GET(me2_census_phases);
  return p;
}

// The device census (me2_census_device).
bool g_dev_on = false;
struct DevSite {
  uint64_t stores = 0;
  uint64_t chunks[4] = {};
};
struct DevKey {
  const char* file;
  unsigned line;
  bool operator==(const DevKey&) const = default;
};
struct DevKeyHash {
  size_t operator()(const DevKey& k) const { return std::hash<const void*>()(k.file) ^ (size_t(k.line) * 0x9E3779B9u); }
};
std::mutex g_dev_mutex;
std::unordered_map<DevKey, DevSite, DevKeyHash>* g_dev_sites = nullptr;
uint64_t g_dev_stores = 0;
std::chrono::steady_clock::time_point g_dev_last_report{};

void ReportDevice() {
  std::vector<std::pair<DevKey, DevSite>> all;
  {
    std::lock_guard lock(g_dev_mutex);
    if (!g_dev_sites)
      return;
    all.assign(g_dev_sites->begin(), g_dev_sites->end());
  }
  std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second.stores > b.second.stores; });
  REXLOG_INFO("[census-dev] device {:08X} +{}: {} stores at {} source lines", g_me2_watch.lo, g_me2_watch.span,
              g_dev_stores, all.size());
  for (const auto& [k, v] : all) {
    std::string chunks;
    for (uint32_t w = 0; w < 4; ++w)
      for (uint64_t m = v.chunks[w]; m; m &= m - 1)
        chunks += fmt::format(" +{:X}", (w * 64 + uint32_t(std::countr_zero(m))) * 64);
    const char* name = k.file;
    for (const char* p = k.file; *p; ++p)
      if (*p == '/' || *p == '\\')
        name = p + 1;
    REXLOG_INFO("[census-dev] {}:{} stores {} chunks{}", name, k.line, v.stores, chunks);
  }
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
  for (int ph = 1; ph <= 4; ++ph) {
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
    g_me2_census_on |= 1;
  }
  ~PhaseScope() {
    if (!active)
      return;
    t_phase = previous;
    if (!previous) {
      g_me2_census_on = g_dev_on ? 2 : 0;
      Report();
    }
  }
};

}  // namespace

namespace {
void NotePhase(unsigned ea, uintptr_t ra) {
  const int phase = t_phase;
  if (!phase)
    return;  // another thread's store while a phase runs
  ++g_stores[phase];
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
}  // namespace

extern "C" __attribute__((noinline)) void Me2CensusNote(unsigned ea) {
  NotePhase(ea, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}

extern "C" __attribute__((noinline)) void Me2CensusNoteAt(unsigned ea, const char* file, unsigned line) {
  if (g_dev_on) {
    const uint32_t o = ea - g_me2_watch.lo;
    if (o < g_me2_watch.span) {
      std::lock_guard lock(g_dev_mutex);
      if (!g_dev_sites)
        g_dev_sites = new std::unordered_map<DevKey, DevSite, DevKeyHash>();
      DevSite& site = (*g_dev_sites)[DevKey{file, line}];
      ++site.stores;
      site.chunks[(o >> 12) & 3] |= uint64_t(1) << ((o >> 6) & 63);
      ++g_dev_stores;
    }
  }
  // The phase census attributes by the return address into the recompiled function: the same as Me2CensusNote's.
  NotePhase(ea, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}

extern "C" void Me2CensusDeviceStart() {
  static const bool on = REXCVAR_GET(me2_census_device);
  if (!on || g_dev_on || !g_me2_watch.span)
    return;
  g_dev_on = true;
  g_dev_last_report = std::chrono::steady_clock::now();
  g_me2_census_on |= 2;  // bit 1: the device census (REX_CARD checks the range inline)
  REXLOG_INFO("[census-dev] counting the guest stores into the device {:08X} +{}", g_me2_watch.lo, g_me2_watch.span);
}

extern "C" void Me2CensusDeviceTick() {
  if (!g_dev_on)
    return;
  static const int report_s = REXCVAR_GET(me2_census_report_s);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_dev_last_report < std::chrono::seconds(report_s))
    return;
  g_dev_last_report = now;
  ReportDevice();
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
    g_me2_census_on |= 1;
  }
  return (uint64_t(active) << 32) | uint32_t(previous);
}
extern "C" void Me2CensusLeave(uint64_t token) {
  if (!(token >> 32))
    return;
  const int previous = int(uint32_t(token));
  t_phase = previous;
  if (!previous) {
    g_me2_census_on = g_dev_on ? 2 : 0;
    Report();
  }
}

#else

// Release builds: REX_CARD never calls these.
extern "C" void Me2CensusNote(unsigned) {}
extern "C" void Me2CensusNoteAt(unsigned, const char*, unsigned) {}
extern "C" void Me2CensusDeviceStart() {}
extern "C" void Me2CensusDeviceTick() {}

#endif  // ME2_WRITE_CENSUS
