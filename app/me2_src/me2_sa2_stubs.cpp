// Stubs for what the kept SA2 renderer code still references from the parts left out of the ME2 build
// (app/CMakeLists.txt, ME2_NATIVE_RENDERER). Filled from the linker's undefined symbols, one by one; each does nothing
// because the feature behind it is SA2's and has no ME2 counterpart yet.

#include "cpu/sa2_enhancements.h"
#include "me/sa2_api.h"

// SA2 declares this in its game-specific native module, which ME2 does not link.
// The shared renderer reads it by name; without registration --sa2_cpu_verify
// was ignored and only its first-4096-use checks ran.
REXCVAR_DEFINE_BOOL(sa2_cpu_verify, false, "ME2",
                    "Check every renderer fast path against its original implementation (diagnostic)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace sa2::api {
// The native Direct3D layer's [api] report line (src/me/sa2_api_*.cpp, left out: it names SA2's Direct3D functions).
void Report() {}
}  // namespace sa2::api

// Codegen checks and instruction-form self-checks use their real implementations.
