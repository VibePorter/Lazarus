// Mass Effect 2 (TU2) - diagnostic: which sounds start, and whose (2026-11-03, user: a pistol sound plays during the
// studio logos at start-up, in every build).
//
// UAudioComponent::Play (sub_8238BF90, this r3; called by UAudioComponent.execPlay sub_8238D0D0 and by C++ directly)
// reads the component's SoundCue at +0x58. With me2_diag_sounds_s > 0, every Play during the first that many seconds
// after start is logged as a "[sound]" line: the cue's name, the component's, and its Outer (the owner) with classes.
// Also the UnrealScript natives that start music and Wwise sounds (BioSFManager/BioSFHandler.PlayGuiMusic with the
// track's name, WwiseAudioComponent.Play / PlayWwiseEvent / PostGlobalEvent / PlayInstance, WwiseMusicVolume.StartMusic;
// user, 2026-11-03: the main menu theme starts during the BioWare logo), with the calling script function. The
// PlayGuiMusic hooks first let me2_startup_movies.cpp hold the menu music until the start-up logos have ended
// (2026-11-04: only those; a Wwise sound under the logos is dropped there, so the load is not held).
// Names come from GNames (found by its entry 0, "None"); UObject: Outer +0x28, Name +0x2C (+0x30 number), Class +0x34.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

REXCVAR_DEFINE_INT32(me2_diag_sounds_s, 0, "ME2/Diagnostics",
                     "Diagnostic: log every sound started (UAudioComponent::Play: cue, component, owner) during the "
                     "first this many seconds after start ([sound] lines, at most 400); 0 = off")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me2::startup_movies {
void WaitForStartupMovies(PPCContext& ctx, uint8_t* base);  // me2_startup_movies.cpp
}

namespace {
const auto g_start = std::chrono::steady_clock::now();
std::atomic<uint32_t> g_lines{0};
std::mutex g_mutex;
uint32_t g_names_array = 0, g_str_off = 0;
bool g_names_tried = false;

uint32_t Load32(const uint8_t* base, uint32_t va) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va));
}
bool Readable(uint32_t va, uint32_t len) {
  if (va < 0x00010000u || va >= 0xA0000000u || va + len < va) return false;
  rex::memory::BaseHeap* heap = REX_KERNEL_MEMORY()->LookupHeap(va);
  return heap && (int(heap->QueryRangeAccess(va, va + len - 1)) & int(rex::memory::PageAccess::kReadOnly)) != 0;
}

// GNames: a TArray {data, num, max} in .data whose entry 0 holds "None".
bool FindNames(const uint8_t* base) {
  if (g_names_array) return true;
  if (g_names_tried) return false;
  g_names_tried = true;
  for (uint32_t a = 0x8300FD60u; a < 0x83530000u; a += 4) {
    const uint32_t data = Load32(base, a), num = Load32(base, a + 4), max = Load32(base, a + 8);
    if (num < 10000 || num > 0x400000u || max < num || max > 0x400000u || (data & 3) || !Readable(data, 16)) continue;
    const uint32_t e0 = Load32(base, data);
    if (!Readable(e0, 32)) continue;
    for (uint32_t k = 0; k < 24; ++k) {
      if (std::memcmp(base + e0 + k, "None", 5) == 0) {
        g_names_array = a;
        g_str_off = k;
        REXLOG_INFO("[sound] GNames at {:08X} ({} names)", a, num);
        return true;
      }
    }
  }
  REXLOG_WARN("[sound] GNames not found: names left out");
  return false;
}

// A name by its GNames index (and number: 0 = none, else _number-1).
std::string NameOf(const uint8_t* base, uint32_t index, uint32_t number) {
  std::string s = "#" + std::to_string(index);
  if (FindNames(base)) {
    const uint32_t data = Load32(base, g_names_array), num = Load32(base, g_names_array + 4);
    if (index < num && Readable(data + 4 * index, 4)) {
      const uint32_t e = Load32(base, data + 4 * index);
      if (Readable(e + g_str_off, 1)) {
        s.clear();
        for (uint32_t i = 0; i < 96 && Readable(e + g_str_off + i, 1) && base[e + g_str_off + i]; ++i)
          s += char(base[e + g_str_off + i]);
      }
    }
  }
  if (number) s += "_" + std::to_string(number - 1);
  return s;
}

std::string Name(const uint8_t* base, uint32_t obj) {
  if (!obj || !Readable(obj, 0x38)) return obj ? "?" : "None";
  return NameOf(base, Load32(base, obj + 0x2C), Load32(base, obj + 0x30));
}

std::string Described(const uint8_t* base, uint32_t obj) {
  if (!obj) return "None";
  const uint32_t cls = Readable(obj, 0x38) ? Load32(base, obj + 0x34) : 0;
  return Name(base, obj) + " (" + Name(base, cls) + ")";
}

// Seconds since start when inside the window and under the line cap (and counts the line), else a negative value.
// The first call says whether the window is on, so a log without "[sound]" lines tells "off" from "never called".
double Window() {
  static const int32_t window = REXCVAR_GET(me2_diag_sounds_s);
  static std::atomic<bool> said{false};
  if (window > 0 && !said.exchange(true)) REXLOG_INFO("[sound] diagnostic on for the first {} s", window);
  if (window <= 0) return -1.0;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  if (t >= window || g_lines.fetch_add(1, std::memory_order_relaxed) >= 400) return -1.0;
  return t;
}

// UnrealScript natives (this r3, FFrame* r4: Node +4, Object +8, Code +12): the caller's script function and its
// class, read before the original runs.
std::string Caller(const uint8_t* base, uint32_t stack) {
  if (!Readable(stack, 16)) return "?";
  const uint32_t node = Load32(base, stack + 4);
  const uint32_t outer = node && Readable(node, 0x38) ? Load32(base, node + 0x28) : 0;
  return Name(base, outer) + "." + Name(base, node);
}

// The Wwise objects (WwiseEvent / WwiseStream / WwiseBank...) a component points at in its first 0x200 bytes: what a
// WwiseAudioComponent.Play is about to play.
std::string WwiseObjects(const uint8_t* base, uint32_t self) {
  std::string out;
  int n = 0;
  if (!Readable(self, 0x200)) return out;
  for (uint32_t off = 0x38; off < 0x200 && n < 3; off += 4) {
    const uint32_t p = Load32(base, self + off);
    if (!p || p == self || (p & 3) || !Readable(p, 0x38)) continue;
    const uint32_t cls = Load32(base, p + 0x34);
    if (!cls || !Readable(cls, 0x38)) continue;
    const std::string cname = Name(base, cls);
    if (cname.rfind("Wwise", 0) != 0 || cname == "WwiseAudioComponent") continue;
    out += (out.empty() ? "" : ", ") + Name(base, p) + " (" + cname + " at +" + std::to_string(off) + ")";
    ++n;
  }
  return out;
}

// One "[sound]" line for an exec native: what was called, on which object (with its owner), from which script
// function; 'arg' is a description of the first parameter, read after the original (empty: none).
void LogExec(const uint8_t* base, double t, const char* what, uint32_t self, const std::string& caller,
             const std::string& arg) {
  const uint32_t outer = self && Readable(self, 0x38) ? Load32(base, self + 0x28) : 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  REXLOG_INFO("[sound] {:.2f} s: {}{}{} | on {} | owner {} | from {}", t, what, arg.empty() ? "" : " ",
              arg, Described(base, self), Described(base, outer), caller);
}
}  // namespace

REX_EXTERN(__imp__sub_8238BF90);
REX_HOOK_RAW(sub_8238BF90) {
  const double t = Window();
  if (t >= 0) {
    const uint32_t comp = ctx.r3.u32;
    if (Readable(comp, 0x60)) {
      const uint32_t cue = Load32(base, comp + 0x58), outer = Load32(base, comp + 0x28);
      const uint32_t outer2 = outer && Readable(outer, 0x38) ? Load32(base, outer + 0x28) : 0;
      std::lock_guard<std::mutex> lock(g_mutex);
      REXLOG_INFO("[sound] {:.2f} s: Play cue {} | component {} | owner {} | owner's outer {}", t,
                  Described(base, cue), Described(base, comp), Described(base, outer), Described(base, outer2));
    }
  }
  __imp__sub_8238BF90(ctx, base);
}

// The exec natives (native table: name string, function). Each frame size is the native's own stwu; a parameter the
// native evaluates into its frame is read there after it returns (the frame is below the entry r1, untouched since).
#define ME2_SOUND_EXEC(addr, what, frame, arg_kind, arg_off, wait)                 \
  REX_EXTERN(__imp__sub_##addr);                                                   \
  REX_HOOK_RAW(sub_##addr) {                                                       \
    if (wait) me2::startup_movies::WaitForStartupMovies(ctx, base);                \
    const double t = Window();                                                     \
    const uint32_t self = ctx.r3.u32, sp = ctx.r1.u32;                             \
    const std::string caller = t >= 0 ? Caller(base, ctx.r4.u32) : std::string();  \
    __imp__sub_##addr(ctx, base);                                                  \
    if (t < 0) return;                                                             \
    std::string arg;                                                               \
    const uint32_t at = sp - (frame) + (arg_off);                                  \
    if ((arg_kind) == 1 && Readable(at, 8))                                        \
      arg = "'" + NameOf(base, Load32(base, at), Load32(base, at + 4)) + "'";      \
    else if ((arg_kind) == 2 && Readable(at, 4))                                   \
      arg = Described(base, Load32(base, at));                                     \
    else if ((arg_kind) == 3)                                                      \
      arg = WwiseObjects(base, self);                                              \
    LogExec(base, t, what, self, caller, arg);                                     \
  }

// arg kinds: 0 none, 1 FName, 2 object, 3 the Wwise objects the component holds; wait: the menu music's wait
ME2_SOUND_EXEC(82406B08, "BioSFManager.PlayGuiMusic", 144, 1, 88, true)
ME2_SOUND_EXEC(82402FB0, "BioSFHandler.PlayGuiMusic", 144, 1, 88, true)
ME2_SOUND_EXEC(828343E0, "WwiseAudioComponent.Play", 144, 3, 0, false)
ME2_SOUND_EXEC(828342E8, "WwiseAudioComponent.PlayWwiseEvent", 144, 2, 88, false)
ME2_SOUND_EXEC(828348C0, "WwiseAudioComponent.PostGlobalEvent", 128, 1, 80, false)
ME2_SOUND_EXEC(82833FE8, "WwiseAudioComponent.PlayInstance", 144, 3, 0, false)
ME2_SOUND_EXEC(828353F8, "WwiseMusicVolume.StartMusic", 96, 0, 0, false)
