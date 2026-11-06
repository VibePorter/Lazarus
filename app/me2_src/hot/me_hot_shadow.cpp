// ME2 (2026-10-26): copied from masseffect-cinnamon (ME1 Switch fork, app/src/native/me_hot_shadow.cpp), the self-checked
// native replacements of hot guest functions. Unchanged except where marked ME2.
// Mass Effect - hot guest hooks: every native replacement compiled a second time with the shadow accessors, for the
// self-check guard (me_hot_guest.cpp Check; design in me_hot_shadow.h, per-hook table in docs/hot-guard.md).
//
// The n_*.h headers are expanded inside namespace me_hot_shadow_copy with ME_HOT_SHADOW_ACCESSORS defined, so their
// me::hot::Raw / St* resolve to the shadow copy of me_hot_common.h (accessors redirected to the private copy of the
// declared write ranges). Nothing here is used on the unchecked path: those calls run the normal build of the natives,
// compiled in me_hot_guest.cpp. Every system / SDK header the natives include must be included above the wrapper.
// The Russian edition uses this file unchanged: its overlay replaces the n_*.h headers and me_hot_call.h it includes.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <simde/x86/sse4.1.h>
#include <simde/arm/neon.h>

#include <rex/platform.h>
#include <rex/ppc/context.h>

#include <cstdlib>

#include <mspack.h>  // ME2 (2026-10-31): n_8279B210 (LZX through libmspack lzxd), outside the wrapper
#include <lzx.h>

#include "me_hot_shadow.h"

#define ME_HOT_SHADOW_ACCESSORS 1
namespace me_hot_shadow_copy {
#include "me_hot_common.h"
#include "n/n_827EBC58.h"
#include "n/n_82D5F900.h"
#include "n/n_82D602E8.h"
#include "n/n_82D65B30.h"
#include "n/n_82D65BD0.h"
#include "n/n_82C474A8.h"
#include "n/n_82D61750.h"
#include "n/n_82C47090.h"
#include "n/n_82D5F6D0.h"
#include "n/n_82E59DF8.h"
#include "n/n_82E94F28.h"
#include "n/n_82E95FC8.h"
#include "n/n_82993E40.h"
#include "n/n_825464E0.h"
#include "n/n_82A92C50.h"
#include "n/n_822EB710.h"
#include "n/n_827DB288.h"
#include "n/n_827DB818.h"
#include "n/n_822CDFF0.h"
#include "n/n_8279B210.h"
}  // namespace me_hot_shadow_copy
#undef ME_HOT_SHADOW_ACCESSORS

// One line per hooked native (the hooks of me_hot_guest.cpp).
ME_HOT_SHADOW_NATIVE(n_827EBC58)
ME_HOT_SHADOW_NATIVE(n_82D5F900)
ME_HOT_SHADOW_NATIVE(n_82D602E8)
ME_HOT_SHADOW_NATIVE(n_82D65B30)
ME_HOT_SHADOW_NATIVE(n_82D65BD0)
ME_HOT_SHADOW_NATIVE(n_82C474A8)
ME_HOT_SHADOW_NATIVE(n_82C47348)
ME_HOT_SHADOW_NATIVE(n_82C473F8)
ME_HOT_SHADOW_NATIVE(n_82D61750)
ME_HOT_SHADOW_NATIVE(n_82C47090)
ME_HOT_SHADOW_NATIVE(n_82D5F6D0)
ME_HOT_SHADOW_NATIVE(n_82E59DF8)
ME_HOT_SHADOW_NATIVE(n_82E5A180)
ME_HOT_SHADOW_NATIVE(n_82E94F28)
ME_HOT_SHADOW_NATIVE(n_82E95FC8)
ME_HOT_SHADOW_NATIVE(n_82E94FE8)
ME_HOT_SHADOW_NATIVE(n_82993E40)
ME_HOT_SHADOW_NATIVE(n_825DB270)
ME_HOT_SHADOW_NATIVE(n_825464E0)
ME_HOT_SHADOW_NATIVE(n_82A92C50)
ME_HOT_SHADOW_NATIVE(n_82A92D28)
ME_HOT_SHADOW_NATIVE(n_822EB710)
ME_HOT_SHADOW_NATIVE(n_822C8C10)
ME_HOT_SHADOW_NATIVE(n_827DB288)
ME_HOT_SHADOW_NATIVE(n_827DB818)
ME_HOT_SHADOW_NATIVE(n_822CDFF0)
ME_HOT_SHADOW_NATIVE(n_8279B210)
