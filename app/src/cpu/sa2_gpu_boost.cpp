// Sonic Adventure 2: the handheld GPU profile (non-CPU enhancement sa2_gpu_boost).
//
// Asks the console's performance manager for the official handheld configuration 0x92220008 (GPU 460.8 MHz, memory
// 1331.2 MHz; the same as UnleashedRecomp-NX, MarathonRecomp-NX and masseffect-nx), through the SDK's switch_apm.cpp,
// which checks with the real clocks for 1.5 s that the memory clock did not move and reverts if it did. Docked: nothing
// is requested.
// WHY IT IS EXACT: a clock, not a setting: the same frames are drawn, sooner.
// RISK: more power draw in handheld mode.
// KEY: sa2_gpu_boost (with sa2_noncpu_enhancements).
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "cpu/sa2_enhancements.h"

#if REX_PLATFORM_SWITCH
extern "C" {
void RexSwitchApmRequestGpuMhz(int mhz);
void RexSwitchApmApply(void);
void RexSwitchApmAllowRam1600(int allow);
}
#endif

SA2_ENHANCEMENT(g_gpu_boost, sa2_gpu_boost, sa2::enh::kNonCpu, true,
                "Handheld: ask for the official 460.8 MHz GPU profile (memory clock checked unchanged, else reverted)");

// ME2 (2026-10-26): the console is guest-CPU bound, so the official "GPU 460.8 + memory 1600" handheld profile
// (0x92220007, not an overclock) is preferred: the game and render threads spend much of a frame in cache misses.
// Same as masseffect-cinnamon's masseffect_switch_ram_1600. false = keep the memory at 1331.2 (the old behaviour).
REXCVAR_DEFINE_BOOL(sa2_switch_ram_1600, true, "SA2 enhancements",
                    "Handheld: with sa2_gpu_boost, take the official GPU 460.8 + memory 1600 MHz profile (costs battery)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace sa2::enh {

void ApplyGpuBoost() {
  if (!g_gpu_boost.active) return;
#if REX_PLATFORM_SWITCH
  RexSwitchApmAllowRam1600(REXCVAR_GET(sa2_switch_ram_1600) ? 1 : 0);
  RexSwitchApmRequestGpuMhz(460);
  RexSwitchApmApply();  // logs what it did on stderr (rex_stderr.log)
  g_gpu_boost.Use();
#endif
}

}  // namespace sa2::enh
