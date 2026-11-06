// Mass Effect 2 (TU2) - the start-up movies before the Press Start screen (2026-11-03, user: the main menu theme and a
// pistol-like sound start during the BioWare logo).
//
// The engine plays the [FullScreenMovie] StartupMovies (BWLogo, ME_EAsig_720p_v2_raw, publisher) while it loads the
// entry menu behind them. From the DVD that load outlasted the logos; here it takes ~2 s, so BioSFHandler_Splash's
// OnPanelAdded (PlayGuiMusic 'MainMenu') and SFXGame.PlayTransientSound (its intro sounds) run under the BioWare logo.
//
// With me2_startup_movies_first, the Press Start screen's music (BioSFHandler/BioSFManager.PlayGuiMusic, from
// BioSFHandler_Splash.OnPanelAdded; me2_sound_diag.cpp's hooks call WaitForStartupMovies on the game thread) waits
// while one of the StartupMovies is playing, as the slow load did. Not a looping or any other movie (the ME1 import
// relaunch's ME1_CharImport_LoadLoop is in ImportStartupMovies): only the ini's start-up list. Movies play on without
// the game thread (the loading movies do). A logo-to-logo gap is bridged by a short grace; at most
// me2_startup_movies_max_s in all. Once: after the first check, never again.
// 2026-11-04 (user: a long black screen between the logos and the menu): the wait used to be at the FIRST sound, an
// SFXGame.PlayTransientSound in the middle of the entry menu's load, so the rest of the load (Conversation and HUD
// movies, BIOG_UIWorld, the player's packages: ~2.7 s on the console) only ran after the logos. Now the load goes on
// under the logos and waits at the music, when it is done; a Wwise sound started while a start-up movie plays
// (UWwiseAudioComponent::Play, sub_8280BA28, the C++ under WwiseAudioComponent.execPlay) is dropped instead (it
// returns 0, as when the component cannot play).
//
// GFullScreenMovie (0x833E94B0, FFullScreenMovieSupport*; the singleton itself at 0x8340680C): current movie name
// FString at +28 (data, num), StartupMovies TArray<FString> at +140; vtable slot 10 (+40) GameThreadIsMoviePlaying
// (this, name) = sub_822F7E50 (the derived player keeps the base's).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

REXCVAR_DEFINE_BOOL(me2_startup_movies_first, true, "ME2",
                    "The start-up logos (BioWare, EA) end before the Press Start screen and its music begin: the first "
                    "sound the game starts waits for them, as the DVD's slower load did");
REXCVAR_DEFINE_INT32(me2_startup_movies_max_s, 30, "ME2", "me2_startup_movies_first waits at most this many seconds")
    .range(1, 120)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_822F7E50);  // FFullScreenMovieSupport::GameThreadIsMoviePlaying(this, const TCHAR*)

namespace {
constexpr uint32_t kGFullScreenMovie = 0x833E94B0, kPlayerSingleton = 0x8340680C;
std::atomic<bool> g_done{false};

uint32_t Load32(const uint8_t* base, uint32_t va) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va));
}
bool Readable(uint32_t va, uint32_t len) {
  if (va < 0x00010000u || va >= 0xA0000000u || va + len < va) return false;
  rex::memory::BaseHeap* heap = REX_KERNEL_MEMORY()->LookupHeap(va);
  return heap && (int(heap->QueryRangeAccess(va, va + len - 1)) & int(rex::memory::PageAccess::kReadOnly)) != 0;
}

// A guest wide (big-endian UTF-16) string, ASCII only, lower-cased; empty when unreadable.
std::string Wide(const uint8_t* base, uint32_t p, uint32_t max_chars = 128) {
  std::string s;
  for (uint32_t i = 0; i < max_chars && Readable(p + 2 * i, 2); ++i) {
    const uint16_t c = uint16_t(base[p + 2 * i] << 8 | base[p + 2 * i + 1]);
    if (!c) break;
    s += (c < 128) ? char(c >= 'A' && c <= 'Z' ? c + 32 : c) : '?';
  }
  return s;
}

uint32_t Player(const uint8_t* base) {
  for (const uint32_t g : {kGFullScreenMovie, kPlayerSingleton}) {
    const uint32_t p = Readable(g, 4) ? Load32(base, g) : 0;
    if (p && Readable(p, 160)) return p;
  }
  return 0;
}

// The current movie's name data pointer when it is one of the StartupMovies (index in 'which'), else 0.
uint32_t StartupMovieName(const uint8_t* base, uint32_t player, int& which, int& count, std::string& name) {
  which = -1;
  count = 0;
  const uint32_t data = Load32(base, player + 28), num = Load32(base, player + 32);
  if (!data || !num || num > 256) return 0;
  name = Wide(base, data);
  const uint32_t list = Load32(base, player + 140), n = Load32(base, player + 144);
  if (!list || n > 16 || !Readable(list, 12 * n)) return 0;
  count = int(n);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t s = Load32(base, list + 12 * i);
    if (s && Wide(base, s) == name) {
      which = int(i);
      return data;
    }
  }
  return 0;
}

// GameThreadIsMoviePlaying(player, name), with the caller's argument registers kept (the hooks call this at entry).
bool IsPlaying(PPCContext& ctx, uint8_t* base, uint32_t player, uint32_t name) {
  // PPCContext's fields are not in register order (r3 comes first): each by name.
  PPCRegister* const regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7,  &ctx.r8,  &ctx.r9,  &ctx.r10,
                               &ctx.r11, &ctx.r12, &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6,
                               &ctx.f7, &ctx.f8, &ctx.ctr};
  PPCRegister saved[sizeof(regs) / sizeof(regs[0])];
  for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) saved[i] = *regs[i];
  const uint64_t saved_lr = ctx.lr;
  const auto saved_cr0 = ctx.cr0, saved_cr1 = ctx.cr1, saved_cr5 = ctx.cr5, saved_cr6 = ctx.cr6, saved_cr7 = ctx.cr7;
  const auto saved_xer = ctx.xer;
  ctx.r3.u64 = player;
  ctx.r4.u64 = name;
  __imp__sub_822F7E50(ctx, base);
  const bool playing = ctx.r3.u32 != 0;
  for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) *regs[i] = saved[i];
  ctx.lr = saved_lr;
  ctx.cr0 = saved_cr0, ctx.cr1 = saved_cr1, ctx.cr5 = saved_cr5, ctx.cr6 = saved_cr6, ctx.cr7 = saved_cr7;
  ctx.xer = saved_xer;
  return playing;
}
}  // namespace

extern "C" void Me2AutoInputMark(int index);  // me2_menu.cpp (test harness input marks)

namespace me2::startup_movies {

void WaitForStartupMovies(PPCContext& ctx, uint8_t* base) {
  if (g_done.load(std::memory_order_acquire) || g_done.exchange(true)) return;
  // me2-nx-deko (2026-11-06): mark 1 of the test harness's input script (the Press Start screen), once this returns.
  struct Mark {
    ~Mark() {
      Me2AutoInputMark(1);
    }
  } mark;
  if (!REXCVAR_GET(me2_startup_movies_first)) return;
  const uint32_t player = Player(base);
  if (!player) {
    REXLOG_INFO("[movies] no movie player: the game goes on");
    return;
  }
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  const auto deadline = start + std::chrono::seconds(REXCVAR_GET(me2_startup_movies_max_s));
  const auto grace = std::chrono::milliseconds(500);
  auto quiet_since = clock::time_point{};
  std::string seen, last;
  bool waited = false;
  while (clock::now() < deadline) {
    int which = -1, count = 0;
    std::string name;
    const uint32_t data = StartupMovieName(base, player, which, count, name);
    const bool playing = data && IsPlaying(ctx, base, player, data);
    if (data && name != last) {
      seen += (seen.empty() ? "" : ", ") + name;
      last = name;
    }
    if (playing) {
      waited = true;
      quiet_since = {};
    } else {
      // Not playing: the last start-up movie (or none of them) ends the wait at once; an earlier one may be followed
      // by the next, so a short grace first.
      if (!waited && quiet_since == clock::time_point{} && (which < 0 || which + 1 >= count)) break;
      if (quiet_since == clock::time_point{}) quiet_since = clock::now();
      else if (clock::now() - quiet_since >= grace) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const double s = std::chrono::duration<double>(clock::now() - start).count();
  if (waited)
    REXLOG_INFO("[movies] the menu music waited {:.2f} s for the start-up movies ({}){}", s, seen,
                clock::now() >= deadline ? ": stopped at me2_startup_movies_max_s" : "");
  else
    REXLOG_INFO("[movies] no start-up movie playing at the menu music (last movie '{}')", last);
}

// True while a start-up movie plays, before the menu music's wait (then never again): the sound is to be dropped.
bool DropStartupSound(PPCContext& ctx, uint8_t* base) {
  if (g_done.load(std::memory_order_acquire) || !REXCVAR_GET(me2_startup_movies_first)) return false;
  using clock = std::chrono::steady_clock;
  static const clock::time_point first = clock::now();
  if (clock::now() - first > std::chrono::seconds(REXCVAR_GET(me2_startup_movies_max_s))) return false;
  const uint32_t player = Player(base);
  if (!player) return false;
  int which = -1, count = 0;
  std::string name;
  const uint32_t data = StartupMovieName(base, player, which, count, name);
  if (!data || !IsPlaying(ctx, base, player, data)) return false;
  static std::atomic<int> dropped{0};
  REXLOG_INFO("[movies] a sound started under the start-up movie '{}' was dropped ({})", name, ++dropped);
  return true;
}

}  // namespace me2::startup_movies

// UWwiseAudioComponent::Play(this r3) -> int (0 = not played).
REX_EXTERN(__imp__sub_8280BA28);
REX_HOOK_RAW(sub_8280BA28) {
  if (me2::startup_movies::DropStartupSound(ctx, base)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8280BA28(ctx, base);
}
