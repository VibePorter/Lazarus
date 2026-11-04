/* masseffect-nx FFmpeg/config.h, restored: the fork lists it as modified (THIRD_PARTY_NOTICES.md, .gitignore) but
 * the public repository does not contain it, so the upstream header ("no config" on Horizon) was used.
 * Upstream platform selection + Horizon (config_switch_aarch64.h) + the fork's WMV3 additions after each platform. */
#if defined(__SWITCH__)
#include "config_switch_aarch64.h"
#elif defined(WIN32) || defined(_WIN32)
#if defined(__aarch64__) || defined(_M_ARM64)
#include "config_windows_aarch64.h"
#else
#include "config_windows_x86_64.h"
#endif
#elif defined(__APPLE__) && defined(__MACH__)
#if defined(__aarch64__) || defined(__arm64__)
#include "config_macos_aarch64.h"
#else
#include "config_macos_x86_64.h"
#endif
#elif defined(__gnu_linux__)
#if defined(__aarch64__)
#include "config_linux_aarch64.h"
#else
#include "config_linux_x86_64.h"
#endif
#elif defined(__ANDROID__)
#if defined(__aarch64__)
#include "config_android_aarch64.h"
#elif defined(__amd64__) || defined(__x86_64__)
#include "config_android_x86_64.h"
#else
#error "android arch unsupported"
#endif
#else
#error "no config"
#endif
#include "config_masseffect_wmv3.h"
