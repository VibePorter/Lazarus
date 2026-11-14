// Mass Effect 2 (TU2) - whole-mix audio dropouts: tell the Switch audio pump how many buffers the game's streaming
// XAudio2 voices have queued (2026-11-07, user: "completely mitigate sound hiccups"; v4 adds the Bink movie voice).
//
// The pump (SDK switch_audio_system.cpp PumpTick, through rex::audio::SetGuestVoiceProbe) holds new mixer passes while
// a streaming voice is empty and its own queue still has frames, and refills its queue (audio_switch_refill_frames)
// while every streaming voice is 2+ buffers ahead. Background: memory me2-wwise-sink.
//
// Voices: Wwise's output (2 x 1024-frame float buffers, the game's sound) and Bink's (start-up logos, cutscene
// movies; it starved during the BioWare logo = the start-up tearing). Both feed XAudio2 source voices, so instead of
// counting per library, XAudio2's own count is read: its source voice classes (vtables 0x820887D0, 0x82088948,
// 0x820889D8; constructors 82FD1C28 / 82FD2E00 / 82FD4AE8) keep BuffersQueued at voice+152 (SubmitSourceBuffer =
// slot 19: 82FD0FA0 / 82FD2460 / 82FD4290, interlocked +1, -1 again on failure; the engine decrements at buffer end;
// GetState slot 23 82FD12B0 / 82FD1F28 / 82FD3CF0 reports it). The submit hooks only note which voices stream: a voice
// counts while it submitted 3+ buffers and its last submit is under 250 ms old (a finished movie drops out by itself).
//
// History: v1 waited in the Audio Worker (the pump queue then held ~2-5 frames: tearing); v2/v3 counted Wwise's sink
// callbacks (v2 hooked OnVoiceProcessingPassStart as if it were OnBufferEnd: tearing). v3 worked for Wwise (Ryujinx:
// 1.9 buffers average) but the logo movie's own voice still starved.

#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

REXCVAR_DEFINE_BOOL(me2_audio_voice_gate, true, "ME2",
                    "Whole-mix audio dropouts: report the streaming XAudio2 voices' queues (Wwise, Bink movies) to the "
                    "Switch audio pump, which then holds new mixer passes while one is empty and keeps a refilled "
                    "cushion (audio_switch_refill_frames / audio_switch_voice_hold); false = the pump works as before");

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kQueuedOffset = 152;     // XAudio2 source voice: BuffersQueued (u32, interlocked)
constexpr int64_t kActiveNs = 250'000'000;  // a voice counts while its last submit is younger than this
constexpr uint32_t kMinSubmits = 3;         // ... and once it has submitted this many (a stream, not a one-shot)

struct Voice {
  std::atomic<uint32_t> va{0};
  std::atomic<int64_t> last_ns{0};
  std::atomic<uint32_t> submits{0};
};
std::array<Voice, 16> g_voices;
std::atomic<uint8_t*> g_base{nullptr};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// Guest thread, after a successful submit.
void NoteSubmit(uint32_t voice) {
  const int64_t now = NowNs();
  Voice* free_slot = nullptr;
  Voice* oldest = &g_voices[0];
  for (Voice& v : g_voices) {
    const uint32_t va = v.va.load(std::memory_order_relaxed);
    if (va == voice) {
      v.last_ns.store(now, std::memory_order_relaxed);
      v.submits.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (!va && !free_slot) free_slot = &v;
    if (v.last_ns.load(std::memory_order_relaxed) < oldest->last_ns.load(std::memory_order_relaxed)) oldest = &v;
  }
  Voice* slot = free_slot ? free_slot : oldest;  // a new voice (or reuse the stalest slot)
  slot->submits.store(1, std::memory_order_relaxed);
  slot->last_ns.store(now, std::memory_order_relaxed);
  slot->va.store(voice, std::memory_order_release);
}

// Pump thread, once a tick: the smallest BuffersQueued among the streaming voices, or -1 when none streams.
int VoiceProbe() {
  if (!REXCVAR_GET(me2_audio_voice_gate)) return -1;
  uint8_t* base = g_base.load(std::memory_order_acquire);
  if (!base) return -1;
  const int64_t now = NowNs();
  int lowest = -1;
  for (Voice& v : g_voices) {
    const uint32_t va = v.va.load(std::memory_order_acquire);
    if (!va || v.submits.load(std::memory_order_relaxed) < kMinSubmits ||
        now - v.last_ns.load(std::memory_order_relaxed) >= kActiveNs) {
      continue;
    }
    const uint32_t queued =
        __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + va + kQueuedOffset));
    const int q = queued > 64 ? 64 : int(queued);  // a freed voice could read anything
    if (lowest < 0 || q < lowest) lowest = q;
  }
  return lowest;
}

struct Install {
  Install() { rex::audio::SetGuestVoiceProbe(&VoiceProbe); }
} g_install;

}  // namespace

// XAudio2 SubmitSourceBuffer of the three source voice classes (this r3, XAUDIO2_BUFFER* r4) -> HRESULT r3.
#define ME2_SUBMIT_HOOK(addr)                                                   \
  REX_EXTERN(__imp__sub_##addr);                                                \
  REX_HOOK_RAW(sub_##addr) {                                                    \
    const uint32_t voice = ctx.r3.u32;                                          \
    __imp__sub_##addr(ctx, base);                                               \
    if (int32_t(ctx.r3.u32) >= 0) {                                             \
      if (!g_base.load(std::memory_order_relaxed)) g_base.store(base, std::memory_order_release); \
      NoteSubmit(voice);                                                        \
    }                                                                           \
  }

ME2_SUBMIT_HOOK(82FD0FA0)
ME2_SUBMIT_HOOK(82FD2460)
ME2_SUBMIT_HOOK(82FD4290)
