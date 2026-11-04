// Mass Effect 2 - interaction prompt ("Door (open)" + the blue selection square) blinking behind walls.
//
// WHAT THE GAME DOES (from the disassembly, 2026-10-28)
//   BioPlayerSelection.UpdateSelection (exec sub_82A44250 -> sub_82A73BB8) calls FindCurrentSelectionTarget
//   (exec sub_82A441E0 -> sub_82A73208, this = BioPlayerSelection). It walks the selectable actors and writes the best
//   one to m_oCurrentSelectionTarget (+0x4C; None when there is none). Most candidates (word +0x98 bit 0x40000000
//   clear) pass its visibility test only when they were RENDERED recently:
//       WorldInfo.TimeSeconds (Controller +0xBC -> +0x308) - Actor.LastRenderTime (+0xC8) <= 0.2 s (const 0x82022518)
//   The others (bit set) use a line trace instead. BioHUD.DrawSelection then draws the prompt for that target.
//
// WHY IT BLINKS ON THE PORT
//   One frame in which the renderer treats an occluded door as visible (any 'visible' fallback answer of the Umbra /
//   occlusion path: no result, no count, a hole in the depth grid...) draws it once, UE3 stamps its LastRenderTime and
//   the game offers it for the next 0.2 s: the prompt flashes. On the Xbox the occlusion answers are exact, so this
//   does not happen. Any remaining single-frame false 'visible' is enough, which is why tuning the occlusion answers
//   never removed the blink.
//
// WHAT THIS DOES
//   me2_selection_render_frames N (1 = the game's own rule): a target chosen by the LastRenderTime rule is only
//   accepted once it has been rendered in N frames in a row (its LastRenderTime advanced N times without a gap); a
//   target already accepted keeps the game's 0.2 s grace. A one-off render no longer makes a prompt; a door that is
//   really in view gets its prompt N-1 frames later. Rendering itself is not touched. Trace-tested targets are left
//   alone. me2_selection_trace logs the target changes and the decisions ([selection] lines).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>

#include "me2_gui_axis_release.h"

REXCVAR_DEFINE_INT32(me2_selection_render_frames, 1, "ME2",
                     "Interaction prompts: an object the game selects because it was rendered recently is accepted "
                     "only after it was rendered in this many frames in a row (1 = the game's own rule: one frame "
                     "within 0.2 s). Stops prompts of objects behind walls flashing after a single false-visible frame")
    .range(1, 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_selection_use_trace, false, "ME2",
                    "Interaction prompts: every candidate is tested with the game's own line trace (the path it uses "
                    "for actors with the trace flag) instead of 'rendered within 0.2 s'. The occlusion path has to "
                    "draw objects it knows nothing about (new on screen while turning), which made prompts of doors "
                    "behind walls flash")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_selection_trace, false, "ME2",
                    "Diagnostic: log the interaction target changes, their render age and the "
                    "me2_selection_render_frames decisions ([selection] lines)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_sa2_game_frames;  // src/sa2_capture.cpp: counted at each Swap
extern "C" void me2_occlusion_watch(int32_t x, int32_t y, int32_t z, uint64_t frame);  // me2_occlusion.cpp

namespace {

uint32_t Load32(const uint8_t* base, uint32_t va) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va));
}
float LoadF(const uint8_t* base, uint32_t va) {
  const uint32_t v = Load32(base, va);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void Store32(uint8_t* base, uint32_t va, uint32_t v) {
  *reinterpret_cast<uint32_t*>(base + va) = __builtin_bswap32(v);
}

constexpr uint32_t kTarget = 0x4C;           // BioPlayerSelection.m_oCurrentSelectionTarget
constexpr uint32_t kOuter = 0x28;            // the BioPlayerController
constexpr uint32_t kWorldInfo = 0xBC;        // Controller.WorldInfo
constexpr uint32_t kTimeSeconds = 0x308;     // WorldInfo.TimeSeconds
constexpr uint32_t kLastRenderTime = 0xC8;   // Actor.LastRenderTime
constexpr uint32_t kLocation = 0x110;       // Actor.Location (FindCurrentSelectionTarget's fallback point)
constexpr uint32_t kFlags = 0x98;            // Actor flag word; 0x40000000 = trace-tested selection
constexpr uint32_t kTraceTested = 0x40000000;
constexpr float kGapSeconds = 0.075f;        // LastRenderTime older than this = the run of rendered frames broke

// Game thread only (UpdateSelection).
struct State {
  uint32_t actor = 0;      // the candidate followed
  float last_lrt = -1.0f;  // its LastRenderTime at the previous call
  int run = 0;             // frames in a row with a new LastRenderTime
  bool accepted = false;
};
State g_s;
uint32_t g_last_reported = 0;
uint64_t g_suppressed = 0, g_accepted = 0, g_passed = 0;
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();

}  // namespace

REX_EXTERN(__imp__sub_82A73208);
REX_HOOK_RAW(sub_82A73208) {
  const uint32_t self = ctx.r3.u32;
  static const bool use_trace = REXCVAR_GET(me2_selection_use_trace);
  // me2_selection_use_trace: the trace flag on every candidate (WorldInfo +0x580 data / +0x584 count, the list the
  // function walks) for the duration of the call, then the flags as they were.
  static uint32_t flagged[1024];
  uint32_t n_flagged = 0;
  if (use_trace && self) {
    const uint32_t ctl = Load32(base, self + kOuter);
    const uint32_t wi = ctl ? Load32(base, ctl + kWorldInfo) : 0;
    const uint32_t data = wi ? Load32(base, wi + 0x580) : 0;
    const int32_t count = wi ? int32_t(Load32(base, wi + 0x584)) : 0;
    for (int32_t i = 0; data && i < count && n_flagged < 1024; ++i) {
      const uint32_t a = Load32(base, data + uint32_t(i) * 4);
      if (!a)
        continue;
      const uint32_t f = Load32(base, a + kFlags);
      if (!(f & kTraceTested)) {
        Store32(base, a + kFlags, f | kTraceTested);
        flagged[n_flagged++] = a;
      }
    }
  }
  __imp__sub_82A73208(ctx, base);
  for (uint32_t i = 0; i < n_flagged; ++i)
    Store32(base, flagged[i] + kFlags, Load32(base, flagged[i] + kFlags) & ~kTraceTested);
  static const int need_cvar = REXCVAR_GET(me2_selection_render_frames);
  const int need = use_trace ? 1 : need_cvar;
  static const bool trace = REXCVAR_GET(me2_selection_trace);
  if (!self || (need <= 1 && !trace))
    return;
  const uint32_t target = Load32(base, self + kTarget);
  const uint32_t controller = Load32(base, self + kOuter);
  const uint32_t world = controller ? Load32(base, controller + kWorldInfo) : 0;
  if (!world)
    return;
  const float now = LoadF(base, world + kTimeSeconds);
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);

  if (!target) {
    if (trace && g_last_reported)
      REXLOG_INFO("[selection] frame {} t={:.3f}: no target", frame, now);
    g_last_reported = 0;
    g_s = State{};
    return;
  }
  const bool trace_tested = use_trace || (Load32(base, target + kFlags) & kTraceTested) != 0;
  const float loc[3] = {LoadF(base, target + kLocation), LoadF(base, target + kLocation + 4),
                        LoadF(base, target + kLocation + 8)};
  if (trace)
    me2_occlusion_watch(int32_t(loc[0]), int32_t(loc[1]), int32_t(loc[2]), frame);
  const float lrt = LoadF(base, target + kLastRenderTime);
  if (target != g_s.actor) {
    g_s = State{};
    g_s.actor = target;
  }
  if (lrt != g_s.last_lrt) {
    ++g_s.run;  // rendered in a new frame
  } else if (now - lrt > kGapSeconds) {
    g_s.run = 0;
  }
  g_s.last_lrt = lrt;

  bool suppress = false;
  if (trace_tested || need <= 1) {
    ++g_passed;
  } else if (g_s.accepted || g_s.run >= need) {
    if (!g_s.accepted)
      ++g_accepted;
    g_s.accepted = true;
  } else {
    suppress = true;
    ++g_suppressed;
  }
  if (suppress)
    Store32(base, self + kTarget, 0);

  if (trace) {
    const uint32_t shown = suppress ? 0 : target;
    if (shown != g_last_reported || suppress)
      REXLOG_INFO("[selection] frame {} t={:.3f}: target {:08X} at ({:.0f}, {:.0f}, {:.0f}) ({}) render age {:.3f} s, "
                  "rendered frames in a row {} -> {}",
                  frame, now, target, loc[0], loc[1], loc[2], trace_tested ? "trace-tested" : "render-tested",
                  now - lrt, g_s.run,
                  suppress ? "suppressed" : (g_s.accepted ? "shown (accepted)" : "shown"));
    g_last_reported = shown;
    const auto t = std::chrono::steady_clock::now();
    if (t - g_last_report > std::chrono::seconds(10)) {
      g_last_report = t;
      REXLOG_INFO("[selection] 10 s: {} render-tested targets accepted after {} frames, {} calls suppressed, {} "
                  "passed as the game decided",
                  g_accepted, need, g_suppressed, g_passed);
      g_accepted = g_suppressed = g_passed = 0;
    }
  }
}

// ---- 2026-10-29: hacking mini-game (Bypass) cursor drifting on after the stick is let go (Switch only, user). ----
// The handlers' script HandleInputEvent(event, value) is called through sub_8273A5E0 (r3 = the handler, r4 = the
// event byte, f1 = the value); events 2 / 3 are the left stick X / Y (BioSkillGame_Bypass_Handler keeps the last one
// as m_vLeftInput and moves the cursor by it; its own dead zone is 0.1). me2_input_axis_trace logs every change of
// those two values ([input-axis] lines): whether a near-zero value arrives when the stick is released.
REXCVAR_DEFINE_BOOL(me2_input_axis_trace, false, "ME2",
                    "Diagnostic: log the left-stick axis values the GUI handlers get (HandleInputEvent events 2 / 3) "
                    "when they change ([input-axis] lines), and Bypass cursor arguments at the UE3/GFx boundary")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Legacy diagnostic workaround: BioSkillGame_Bypass_Handler.
// Update calls the movie's SetCursorPositions(x * m, y * m, 0, 0, dt) only while the dead-zone modifier m > 0
// (bytecode +0x44: JumpIfNot(native 177 '>' (local fDeadzoneModifier, 0.0f))), so a stick that is let go sends nothing
// inside the dead zone. The constant 0.0 becomes -1.0 so the call also occurs at zero input. The later movie inspection
// shows SetCursorPositions applies immediate displacement, not a retained speed: this is NOT a confirmed drift fix.
// Keep the zero-input calls observable in the targeted [bypass-movie] diagnostic. Patched in Update the first time one of
// its handlers gets a stick event (UObject +0x34 Class; UStruct +0x4C Children, UField +0x40 Next; UFunction +0x54 /
// +0x58 Script data / count).
REXCVAR_DEFINE_BOOL(me2_bypass_cursor_stop, false, "ME2",
                    "Bypass legacy workaround/diagnostic: Update also calls SetCursorPositions at zero input; "
                    "does not by itself prove that cursor drift is fixed")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern "C" uint32_t me2_object_function(const uint8_t* base, uint32_t obj, const char* class_name, const char* func);
extern "C" bool me2_object_lookup_ready();
namespace {
bool Readable(uint32_t va, uint32_t len) {
  if (va < 0x00010000u || va >= 0xA0000000u || va + len < va) return false;
  rex::memory::BaseHeap* heap = REX_KERNEL_MEMORY()->LookupHeap(va);
  return heap && (int(heap->QueryRangeAccess(va, va + len - 1)) & int(rex::memory::PageAccess::kReadOnly)) != 0;
}

void PatchBypassUpdate(uint8_t* base, uint32_t handler) {
  // A miss before InitCerberus has discovered the class layout is transient, not a non-Bypass handler.
  if (!me2_object_lookup_ready())
    return;
  static uint32_t done_fn = 0;
  static uint32_t tried[64];
  static uint32_t n_tried = 0;
  if (!Readable(handler, 0x38))
    return;
  const uint32_t cls = Load32(base, handler + 0x34);
  if (!Readable(cls, 0x50))
    return;
  // UObject addresses can be reused for another screen. Cache the persistent class, not a handler allocation.
  for (uint32_t i = 0; i < n_tried; ++i)
    if (tried[i] == cls)
      return;
  const auto remember = [&] {
    if (n_tried < 64)
      tried[n_tried++] = cls;
  };
  const uint32_t fn = me2_object_function(base, handler, "BioSkillGame_Bypass_Handler", "Update");
  if (!fn || fn == done_fn) {
    remember();
    return;
  }
  const uint32_t data = Load32(base, fn + 0x54), num = Load32(base, fn + 0x58);
  if (num < 0x20 || num > 0x2000 || !Readable(data, num)) {
    static unsigned warned = 0;
    if (warned++ < 4)
      REXLOG_WARN("[bypass] Update {:08X}: no readable script (data {:08X}, {} bytes)", fn, data, num);
    return;
  }
  remember();
  const uint8_t* c = base + data;
  // JumpIfNot ?? ?? | native 177 '>' | local ?? ?? ?? ?? | float 0.0 (or our -1.0) | end of parameters
  for (uint32_t k = 0; k + 15 <= num; ++k) {
    if (c[k] == 0x07 && c[k + 3] == 0xB1 && c[k + 4] == 0x00 && c[k + 9] == 0x1E && c[k + 14] == 0x16) {
      const uint32_t f = Load32(base, data + k + 10);
      if (f != 0 && f != 0xBF800000u)
        continue;
      static constexpr uint8_t kMinusOne[4] = {0xBF, 0x80, 0x00, 0x00};
      std::memcpy(base + data + k + 10, kMinusOne, 4);
      done_fn = fn;
      REXLOG_INFO("[bypass] zero-input movie calls enabled: Update (UFunction {:08X}, {} bytes of script) patched at +0x{:X}",
                  fn, num, k + 10);
      return;
    }
  }
  std::string head;
  for (uint32_t k = 0; k < std::min<uint32_t>(num, 0x60); ++k)
    head += fmt::format("{:02X} ", c[k]);
  REXLOG_WARN("[bypass] Update {:08X} ({} bytes): the dead-zone test was not found; script starts {}", fn, num, head);
  done_fn = fn;
}
}  // namespace

REX_EXTERN(__imp__sub_8273A5E0);
REX_HOOK_RAW(sub_8273A5E0) {
  static const bool cursor_stop = REXCVAR_GET(me2_bypass_cursor_stop);
  if (cursor_stop && ((ctx.r4.u32 & 0xFF) == 2 || (ctx.r4.u32 & 0xFF) == 3))
    PatchBypassUpdate(base, ctx.r3.u32);
  static const bool trace = REXCVAR_GET(me2_input_axis_trace);
  if (trace) {
    const uint32_t ev = ctx.r4.u32 & 0xFF;
    if (ev == 2 || ev == 3) {
      static float last[2] = {0.0f, 0.0f};
      static uint32_t lines = 0;
      const float v = float(ctx.f1.f64);
      float& l = last[ev - 2];
      const bool crossed = (std::fabs(v) < 0.1f) != (std::fabs(l) < 0.1f);
      if ((std::fabs(v - l) > 0.05f || crossed || (v == 0.0f) != (l == 0.0f)) && lines < 20000) {
        ++lines;
        REXLOG_INFO("[input-axis] frame {} handler {:08X} {} = {:.4f} (was {:.4f})",
                    g_sa2_game_frames.load(std::memory_order_relaxed), ctx.r3.u32, ev == 2 ? "X" : "Y", v, l);
        l = v;
      }
    }
  }
  __imp__sub_8273A5E0(ctx, base);
}

// Bypass follow-up: the movie moves the cursor immediately in SetCursorPositions;
// it does not retain a velocity. Trace the actual arguments crossing the UE3/GFx
// boundary to distinguish a stale script value from a conversion/movie problem.
// sub_8273C7E0's caller frame has the method FString at +96; the input array
// contains 28-byte BioSFParams (type byte +0, float +8), converted to 12-byte
// GFx values (number type 3, big-endian double +4). Both layouts are from PPC.
REX_EXTERN(__imp__sub_8273BE00);
REX_HOOK_RAW(sub_8273BE00) {
  static const bool trace = REXCVAR_GET(me2_input_axis_trace);
  if (!trace) {
    __imp__sub_8273BE00(ctx, base);
    return;
  }
  const uint32_t result = ctx.r3.u32;
  float raw[5] = {};
  uint32_t types[5] = {};
  bool bypass = false;
  if (trace) {
    const uint32_t sp = ctx.r1.u32, args = ctx.r4.u32;
    if (Readable(sp + 96, 12) && Readable(args, 12)) {
      const uint32_t method = Load32(base, sp + 96), length = Load32(base, sp + 100);
      static constexpr char kMethod[] = "SetCursorPositions";
      if (length == sizeof(kMethod) && Readable(method, 2 * sizeof(kMethod)) && Load32(base, args + 4) == 5) {
        bypass = true;
        for (uint32_t i = 0; i < sizeof(kMethod); ++i)
          if (base[method + 2 * i] != 0 || base[method + 2 * i + 1] != uint8_t(kMethod[i]))
            bypass = false;
        const uint32_t data = Load32(base, args);
        bypass = bypass && Readable(data, 5 * 28);
        if (bypass) {
          for (uint32_t i = 0; i < 5; ++i) {
            types[i] = base[data + 28 * i];
            raw[i] = LoadF(base, data + 28 * i + 8);
          }
        }
      }
    }
  }
  __imp__sub_8273BE00(ctx, base);
  if (!bypass || !Readable(result, 12)) return;
  const uint32_t data = Load32(base, result), count = Load32(base, result + 4);
  if (count != 5 || !Readable(data, 5 * 12)) return;
  double values[5];
  for (uint32_t i = 0; i < 5; ++i) {
    if (Load32(base, data + i * 12) != 3) {
      static uint32_t warnings = 0;
      if (warnings++ < 8)
        REXLOG_WARN("[bypass-movie] argument {}: raw type {}, unexpected GFx type {}",
                    i, types[i], Load32(base, data + i * 12));
      return;
    }
    const uint64_t bits = uint64_t(Load32(base, data + i * 12 + 4)) << 32 | Load32(base, data + i * 12 + 8);
    std::memcpy(&values[i], &bits, sizeof(bits));
  }
  static float last[4] = {};
  static bool seen = false;
  static uint32_t lines = 0;
  static uint64_t last_frame = 0;
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  bool changed = !seen || frame - last_frame >= 30;
  for (uint32_t i = 0; i < 4; ++i) {
    changed = changed || types[i] != 1;
    changed = changed || std::fabs(raw[i] - last[i]) > 0.05f || (raw[i] == 0.0f) != (last[i] == 0.0f);
    changed = changed || values[i] != double(raw[i]);
  }
  if (changed && lines < 2000) {
    ++lines;
    REXLOG_INFO("[bypass-movie] frame {} SetCursorPositions types ({}, {}, {}, {}, {}), "
                "raw ({:.5f}, {:.5f}, {:.5f}, {:.5f}) dt {:.5f}; "
                "GFx ({:.5f}, {:.5f}, {:.5f}, {:.5f}) dt {:.5f}",
                frame, types[0], types[1], types[2], types[3], types[4], raw[0], raw[1], raw[2], raw[3], raw[4],
                values[0], values[1], values[2], values[3], values[4]);
    std::copy(raw, raw + 4, last);
    last_frame = frame;
    seen = true;
  }
}

// Fix the release before the script's zero-pair gate. BioPlayerInput's native
// Axis command (sub_829B7CB0) first accumulates aGui* floats, THEN copies them
// into AxisBuffer. Observe the completed buffer at PreProcessInput VM entry, not the
// ordinary float accumulator, which never targets that array.
REXCVAR_DEFINE_BOOL(me2_gui_axis_release, true, "ME2",
                    "Deliver one neutral GUI stick pair when the mapped axes return to exact zero, "
                    "using the game's existing GUI dead zone; prevents retained menu/minigame input")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
extern "C" uint32_t me2_object_float_property(const uint8_t*, uint32_t, const char*, const char*, uint32_t);

namespace {
std::atomic<uint32_t> g_gui_preprocess_fn{0};
struct GuiAxisInput {
  uint32_t object = 0, cls = 0, axes = 0, deadzone = 0;
  uint64_t retry_frame = 0;
  uint64_t bind_retry_frame = 0;
  uint64_t trace_frame = 0;
  bool trace_seen = false;
  float trace_values[4] = {};
  Me2GuiAxisReleasePair pairs[2];
};
GuiAxisInput* GuiAxes(uint8_t* base, uint32_t object) {
  static GuiAxisInput inputs[8];
  static unsigned replacement = 0;
  if (!me2_object_lookup_ready()) return nullptr;
  const uint32_t cls = Load32(base, object + 0x34); // original callee requires this live UObject
  GuiAxisInput* state = nullptr;
  for (auto& item : inputs)
    if (item.object == object && item.cls == cls) { state = &item; break; }
  if (!state) {
    state = &inputs[replacement++ % 8];
    *state = {};
    state->object = object;
    state->cls = cls;
  }
  if (state->axes) return state;
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  if (frame < state->retry_frame) return nullptr;
  state->retry_frame = frame + 30;
  const uint32_t axes = me2_object_float_property(base, object, "BioPlayerInput", "AxisBuffer", 6);
  const uint32_t deadzone = me2_object_float_property(base, object, "BioPlayerInput", "GuiDeadzone", 1);
  if (!axes || !deadzone || !Readable(object + axes, 24) || !Readable(object + deadzone, 4)) return nullptr;
  state->axes = axes;
  state->deadzone = deadzone;
  REXLOG_INFO("[gui-axis-release] input {:08X}: AxisBuffer +0x{:X}, GuiDeadzone +0x{:X}", object, axes, deadzone);
  return state;
}
void StoreF(uint8_t* base, uint32_t va, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Store32(base, va, bits);
}
void BindGuiPreProcess(uint8_t* base, uint32_t object) {
  GuiAxisInput* state = GuiAxes(base, object);
  if (!state) return;
  const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
  if (frame < state->bind_retry_frame) return;
  state->bind_retry_frame = frame + 30;
  const uint32_t fn = me2_object_function(base, object, "BioPlayerInput", "PreProcessInput");
  if (!fn || !Readable(fn, 0xB0)) return;
  if (Load32(base, fn + 0xAC) != 0x82781F70u) {
    static unsigned reports = 0;
    if (reports++ < 2)
      REXLOG_WARN("[gui-axis-release] PreProcessInput {:08X}: unexpected script entry {:08X}; binding deferred",
                  fn, Load32(base, fn + 0xAC));
    return;
  }
  g_gui_preprocess_fn.store(fn, std::memory_order_release);
  REXLOG_INFO("[gui-axis-release] bound PreProcessInput {:08X} at VM entry (ProcessInternal)", fn);
}
void PrepareGuiAxes(uint8_t* base, uint32_t object) {
  static bool reported = false;
  if (!reported) {
    reported = true;
    REXLOG_INFO("[gui-axis-release] PreProcessInput VM hook reached");
  }
  GuiAxisInput* state = GuiAxes(base, object);
  if (state) {
    const float deadzone = LoadF(base, object + state->deadzone);
    float values[4];
    for (unsigned i = 0; i < 4; ++i) values[i] = LoadF(base, object + state->axes + i * 4);
    static const bool trace = REXCVAR_GET(me2_input_axis_trace);
    if (trace) {
      const uint64_t frame = g_sa2_game_frames.load(std::memory_order_relaxed);
      bool changed = !state->trace_seen, active = false;
      for (unsigned i = 0; i < 4; ++i) {
        active = active || values[i] != 0.0f;
        changed = changed || std::fabs(values[i] - state->trace_values[i]) > 0.05f ||
                  (values[i] == 0.0f) != (state->trace_values[i] == 0.0f);
      }
      static unsigned reports = 0;
      if ((changed || (active && frame - state->trace_frame >= 60)) && reports < 1024) {
        ++reports;
        REXLOG_INFO("[gui-axis-buffer] frame {} input {:08X}: ({:.5f}, {:.5f}, {:.5f}, {:.5f}), deadzone {:.5f}",
                    frame, object, values[0], values[1], values[2], values[3], deadzone);
        std::copy(values, values + 4, state->trace_values);
        state->trace_seen = true;
        state->trace_frame = frame;
      }
    }
    for (unsigned i = 0; i < 2; ++i) {
      const uint32_t address = object + state->axes + i * 8;
      float x = values[i * 2];
      const float y = values[i * 2 + 1];
      if (!state->pairs[i].Prepare(x, y, deadzone)) continue;
      StoreF(base, address, x);
      static uint32_t reports = 0;
      if (trace && reports++ < 256)
        REXLOG_INFO("[gui-axis-release] frame {} input {:08X}: {} stick neutral pair queued before PreProcessInput",
                    g_sa2_game_frames.load(std::memory_order_relaxed), object, i == 0 ? "left" : "right");
    }
  }
}
} // namespace

// sub_82781F70 is UObject::ProcessInternal. UFunction::Bind installs it for
// script functions, and CallFunction invokes it directly for non-native calls.
// The previous native PlayerInput dispatch is not used by the minigame path.
REX_EXTERN(__imp__sub_82781F70);
REX_HOOK_RAW(sub_82781F70) {
  const uint32_t fn = g_gui_preprocess_fn.load(std::memory_order_acquire);
  if (fn && Load32(base, ctx.r4.u32 + 4) == fn)
    PrepareGuiAxes(base, ctx.r3.u32);
  __imp__sub_82781F70(ctx, base);
}

REX_EXTERN(__imp__sub_825E9DA8);
REX_HOOK_RAW(sub_825E9DA8) {
  static const bool enabled = REXCVAR_GET(me2_gui_axis_release);
  // The known-active native accumulator is used only to discover the input
  // class once. It does not inspect or modify individual axis accumulations.
  if (enabled && !g_gui_preprocess_fn.load(std::memory_order_relaxed))
    BindGuiPreProcess(base, ctx.r3.u32);
  __imp__sub_825E9DA8(ctx, base);
}
