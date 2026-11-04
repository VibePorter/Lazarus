/**
 * @file        audio/switch/switch_audio_system.cpp
 * @brief       Audio output for Nintendo Switch through libnx audout
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/audio/switch/switch_audio_system.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <switch.h>

#include "../../core/threading_switch.h"

// Passive counters: the audio thread neither writes reports nor pauses the game.
extern "C" void RexSwitchPerfAdd(unsigned id, u64 value);
extern "C" void RexSwitchPerfMax(unsigned id, u64 value);

REXCVAR_DECLARE(uint32_t, switch_audio_core_mask);  // system/xthread.cpp

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_BOOL(audio_switch_pump, true, "Audio",
                    "Ask the game for one frame every 5.333 ms (187.5 Hz pump, like "
                    "MarathonRecomp-NX) instead of 4 in a row when each audout buffer is consumed, which "
                    "left the game's audio server voice without a packet (robotic audio); "
                    "read when the audio is opened");
// ME2 (2026-10-26): 16 (85 ms) instead of 10 (53 ms): ME2's audio server falls behind in the heavy scenes (XMA lock
// waits of 10-20 ms, CPU-bound frames); a longer cushion keeps the output going, at 32 ms more latency.
REXCVAR_DEFINE_BOOL(audio_switch_pump_inflight, true, "Audio",
                    "Switch, with audio_switch_pump: requests the game has not answered yet count toward "
                    "audio_switch_frames_in_queue, so a late game is not sent a burst of requests (ME2 cut-outs). "
                    "false = only the delivered frames count, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(audio_switch_frames_in_queue, 16, "Audio",
                     "With audio_switch_pump: game frames kept queued before audout "
                     "(cushion; 10 = 53 ms, 6 up to build 105). A longer queue lets the game wait without cutting the "
                     "output, at the cost of latency: sa2_audio_wait_server_ms waits up to 30 ms; read "
                     "when the audio is opened");

namespace rex::audio::nx {

namespace {

// Guest frames per audout buffer, and buffers kept queued in the device.
// 4 frames are 1024 samples (21.3 ms), the block size MarathonRecomp-NX found
// to be safe on the shared cores; three buffers cap the latency at ~64 ms.
constexpr uint32_t kFramesPerBuffer = 4;
constexpr uint32_t kBufferCount = 3;
constexpr uint32_t kBufferSamples = kFramesPerBuffer * SwitchAudioDriver::kChannelSamples;
constexpr uint32_t kBufferBytes = kBufferSamples * 2 * sizeof(s16);
// audout wants page-aligned sample memory.
constexpr uint32_t kBufferAllocBytes = (kBufferBytes + 0xFFF) & ~0xFFFu;

// Above guest threads (0x3B) so the device is never starved, like the audio
// pump of MarathonRecomp-NX (0x2B).
constexpr int kOutputThreadPriority = 0x2B;
constexpr size_t kOutputThreadStack = 0x10000;
// Bounded so the thread notices shutdown even if the device stops releasing.
constexpr u64 kWaitTimeoutNs = 100'000'000;
// audio_switch_pump: one frame (256 samples) every 5.333 ms with absolute deadlines. With this many
// frames queued or fewer, another one is requested on each tick, and below the minimum one more is
// requested to catch up (the system clock and the audio clock do not match exactly).
constexpr u64 kPumpIntervalNs = 1'000'000'000ull * SwitchAudioDriver::kChannelSamples / 48000;
constexpr size_t kTargetQueuedFrames = 6;
constexpr size_t kMinQueuedFrames = 2;

}  // namespace

/* --- driver ------------------------------------------------------------------ */

SwitchAudioDriver::SwitchAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SwitchAudioDriver::~SwitchAudioDriver() {
  std::lock_guard<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SwitchAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  // Runs inside the guest callback while it holds the global critical region:
  // copy and return, never block on the device.
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[kFrameSamples];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }
  std::memcpy(output_frame, input_frame, kFrameSamples * sizeof(float));
  std::lock_guard<std::mutex> guard(frames_mutex_);
  frames_queued_.push(output_frame);
  // One request answered (never below 0: frames released by MixFrameInto without the pump are not counted).
  answered_.fetch_add(1, std::memory_order_relaxed);
  int32_t n = outstanding_.load(std::memory_order_relaxed);
  while (n > 0 && !outstanding_.compare_exchange_weak(n, n - 1, std::memory_order_relaxed)) {
  }
}

bool SwitchAudioDriver::MixFrameInto(float* stereo_out, const StereoFold& fold, float gain,
                                     bool release) {
  float* frame;
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    if (frames_queued_.empty()) {
      return false;
    }
    frame = frames_queued_.front();
    frames_queued_.pop();
  }
  std::array<float, kChannelSamples * 2> folded;
  conversion::sequential_6_BE_to_interleaved_2_LE(folded.data(), frame, kChannelSamples, fold,
                                                  gain);
  for (size_t i = 0; i < folded.size(); ++i) {
    stereo_out[i] += folded[i];
  }
  {
    std::lock_guard<std::mutex> guard(frames_mutex_);
    frames_unused_.push(frame);
  }
  // Exactly one release per frame really consumed: that is what lets the guest
  // render the next one. With audio_switch_pump, PumpTick requests them at a fixed rate.
  if (release) {
    auto released = semaphore_->Release(1, nullptr);
    assert_true(released);
  }
  return true;
}

size_t SwitchAudioDriver::QueuedFrames() {
  std::lock_guard<std::mutex> guard(frames_mutex_);
  return frames_queued_.size();
}

bool SwitchAudioDriver::RequestFrame() {
  // The semaphore has a maximum: if the game does not serve the requests, the excess ones are lost.
  if (!semaphore_->Release(1, nullptr)) return false;
  outstanding_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

/* --- system ------------------------------------------------------------------ */

struct SwitchAudioSystem::Output {
  Thread thread{};
  bool thread_created = false;
  std::atomic<bool> running{false};
  bool pump = false;  // audio_switch_pump, read when audio is opened
  size_t frames_in_queue = kTargetQueuedFrames;  // audio_switch_frames_in_queue, read when audio is opened
  bool audout_initialized = false;
  bool audout_started = false;
  std::array<AudioOutBuffer, kBufferCount> buffers{};
  std::array<s16*, kBufferCount> samples{};
  std::array<float, kBufferSamples * 2> mix{};
};

std::unique_ptr<AudioSystem> SwitchAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<SwitchAudioSystem>(function_dispatcher);
}

SwitchAudioSystem::SwitchAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

SwitchAudioSystem::~SwitchAudioSystem() {
  StopOutput();
}

X_STATUS SwitchAudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                         rex::thread::Semaphore* semaphore,
                                         AudioDriver** out_driver) {
  assert_not_null(out_driver);
  // The device opens with the first client and then stays open, playing
  // silence while nobody renders.
  if (!output_ && !StartOutput()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  auto driver = new SwitchAudioDriver(memory_, semaphore);
  {
    std::lock_guard<std::mutex> guard(drivers_mutex_);
    drivers_.push_back(driver);
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void SwitchAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  auto switch_driver = static_cast<SwitchAudioDriver*>(driver);
  {
    std::lock_guard<std::mutex> guard(drivers_mutex_);
    drivers_.erase(std::remove(drivers_.begin(), drivers_.end(), switch_driver), drivers_.end());
  }
  delete switch_driver;
}

bool SwitchAudioSystem::StartOutput() {
  auto output = std::make_unique<Output>();
  output->pump = REXCVAR_GET(audio_switch_pump);
  output->frames_in_queue = static_cast<size_t>(std::clamp(REXCVAR_GET(audio_switch_frames_in_queue), 2, 32));
  REXLOG_INFO("SwitchAudioSystem: 187.5 Hz pump {}, {} frames queued", output->pump ? "active" : "off",
              output->frames_in_queue);

  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: audoutInitialize failed: 0x{:08X}", rc);
    return false;
  }
  output->audout_initialized = true;
  REXLOG_INFO("SwitchAudioSystem: audout {} Hz, {} channels, PCM format {}",
              audoutGetSampleRate(), audoutGetChannelCount(), int(audoutGetPcmFormat()));
  if (audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2 ||
      audoutGetPcmFormat() != PcmFormat_Int16) {
    REXLOG_ERROR("SwitchAudioSystem: unexpected audout format");
    audoutExit();
    return false;
  }

  for (size_t i = 0; i < kBufferCount; ++i) {
    output->samples[i] = static_cast<s16*>(std::aligned_alloc(0x1000, kBufferAllocBytes));
    if (!output->samples[i]) {
      REXLOG_ERROR("SwitchAudioSystem: out of memory for audio buffers");
      output_ = std::move(output);
      StopOutput();
      return false;
    }
    std::memset(output->samples[i], 0, kBufferAllocBytes);
    AudioOutBuffer& buffer = output->buffers[i];
    buffer.next = nullptr;
    buffer.buffer = output->samples[i];
    buffer.buffer_size = kBufferAllocBytes;
    buffer.data_size = kBufferBytes;
    buffer.data_offset = 0;
  }

  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: audoutStartAudioOut failed: 0x{:08X}", rc);
    output_ = std::move(output);
    StopOutput();
    return false;
  }
  output->audout_started = true;

  output_ = std::move(output);
  output_->running = true;
  rc = threadCreate(
      &output_->thread,
      [](void* arg) { static_cast<SwitchAudioSystem*>(arg)->OutputThreadMain(); }, this, nullptr,
      kOutputThreadStack, kOutputThreadPriority, -2);
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: threadCreate failed: 0x{:08X}", rc);
    output_->running = false;
    StopOutput();
    return false;
  }
  output_->thread_created = true;
  rc = threadStart(&output_->thread);
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: threadStart failed: 0x{:08X}", rc);
    output_->running = false;
    StopOutput();
    return false;
  }
  return true;
}

void SwitchAudioSystem::StopOutput() {
  if (!output_) {
    return;
  }
  output_->running = false;
  if (output_->thread_created) {
    threadWaitForExit(&output_->thread);
    threadClose(&output_->thread);
  }
  if (output_->audout_started) {
    audoutStopAudioOut();
  }
  if (output_->audout_initialized) {
    audoutExit();
  }
  for (s16* samples : output_->samples) {
    std::free(samples);
  }
  output_.reset();
}

void SwitchAudioSystem::FillAndAppend(size_t buffer_index) {
  Output& output = *output_;
  output.mix.fill(0.0f);
  // Snapshot once, so a change mid-buffer cannot split it across two mixes.
  const StereoFold fold = GetStereoFold();
  const float gain = GetOutputGain();
  uint32_t frames_mixed = 0;
  uint32_t frames_missing = 0;
  {
    std::lock_guard<std::mutex> guard(drivers_mutex_);
    for (uint32_t frame = 0; frame < kFramesPerBuffer; ++frame) {
      float* out = output.mix.data() + frame * SwitchAudioDriver::kChannelSamples * 2;
      for (SwitchAudioDriver* driver : drivers_) {
        // A client with nothing queued contributes silence and gets no
        // release, exactly as the SDL driver does on underrun.
        if (driver->MixFrameInto(out, fold, gain, !output.pump)) {
          ++frames_mixed;
        } else {
          ++frames_missing;
        }
      }
    }
  }
  s16* samples = output.samples[buffer_index];
  bool pcm_nonzero = false;
  // How much the mix saturates before it is clipped (the crackling in handheld mode). Counting only.
  float pico = 0.0f;
  uint32_t saturated = 0;
  if (REXCVAR_GET(audio_mute)) {
    std::memset(samples, 0, kBufferBytes);
  } else {
    for (size_t i = 0; i < output.mix.size(); ++i) {
      const float value = std::isfinite(output.mix[i]) ? output.mix[i] : 0.0f;
      const float magnitude = std::fabs(value);
      pico = std::max(pico, magnitude);
      saturated += magnitude > 1.0f ? 1 : 0;
      samples[i] = static_cast<s16>(std::lrint(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
      pcm_nonzero |= samples[i] != 0;
    }
  }
  // ME2 (2026-10-26): dropouts of the whole mix. Per 5.33 ms frame: audible (peak >= 0.002), or digital silence
  // (peak < 0.00005); a run of 1-24 silent frames (up to 128 ms) between audible frames is a dropout
  // (60 = dropouts, 61 = their frames). Longer runs are pauses. Only this thread runs it.
  {
    static bool was_audible = false;
    static uint32_t silent_run = 0;
    const size_t per_frame = SwitchAudioDriver::kChannelSamples * 2;
    for (uint32_t frame = 0; frame < kFramesPerBuffer; ++frame) {
      float frame_peak = 0.0f;
      const float* m = output.mix.data() + frame * per_frame;
      for (size_t i = 0; i < per_frame; ++i)
        frame_peak = std::max(frame_peak, std::isfinite(m[i]) ? std::fabs(m[i]) : 0.0f);
      if (frame_peak >= 0.002f) {
        if (was_audible && silent_run > 0 && silent_run <= 24) {
          RexSwitchPerfAdd(60, 1);
          RexSwitchPerfAdd(61, silent_run);
        }
        // ME2 (2026-10-26): every silence of up to ~2 s between audible frames, with its time (the log line's), to
        // line them up with IO and loads; the title-screen cut may be longer than the 128 ms counted above.
        if (was_audible && silent_run > 0 && silent_run <= 400) {
          static uint32_t logged = 0;
          if (logged < 600) {
            ++logged;
            REXLOG_INFO("[audio] {}: the whole mix silent for {:.1f} ms (ended now)",
                        silent_run <= 24 ? "dropout" : "silence", silent_run * 5.333f);
          }
        }
        was_audible = true;
        silent_run = 0;
      } else if (frame_peak < 0.00005f) {
        if (was_audible) ++silent_run;
        if (silent_run > 400) was_audible = false;  // a pause, not a cut
      } else {
        was_audible = false;  // a quiet tail, not a cut
        silent_run = 0;
      }
    }
  }
  RexSwitchPerfAdd(21, saturated);
  RexSwitchPerfAdd(22, pico >= 0.98f ? 1 : 0);
  RexSwitchPerfMax(23, static_cast<u64>(std::min(pico, 100.0f) * 10000.0f));
  output.buffers[buffer_index].data_size = kBufferBytes;
  output.buffers[buffer_index].data_offset = 0;
  Result rc = audoutAppendAudioOutBuffer(&output.buffers[buffer_index]);
  RexSwitchPerfAdd(24, frames_mixed);
  RexSwitchPerfAdd(25, frames_missing);
  if (R_SUCCEEDED(rc)) {
    RexSwitchPerfAdd(26, pcm_nonzero ? 1 : 0);
    RexSwitchPerfAdd(27, 1);
  }
  if (R_FAILED(rc)) {
    REXLOG_ERROR("SwitchAudioSystem: audoutAppendAudioOutBuffer failed: 0x{:08X}", rc);
  }
}

void SwitchAudioSystem::PumpTick() {
  static const bool inflight = REXCVAR_GET(audio_switch_pump_inflight);
  std::lock_guard<std::mutex> guard(drivers_mutex_);
  for (SwitchAudioDriver* driver : drivers_) {
    const size_t queued = driver->QueuedFrames();
    // ME2 (2026-10-26): requests the game has not answered yet count as frames on their way. Without that, a game
    // late by N ticks got N more requests, then served them back to back: its mixer (XAudio2) ran N passes in a
    // row, faster than Wwise renders in real time, its voice starved and the whole mix went silent (digital zero)
    // while every output buffer was full - the cut-outs, worst during loads and fights.
    const int32_t outstanding = inflight ? std::max(driver->Outstanding(), 0) : 0;
    const size_t on_the_way = queued + size_t(outstanding);
    if (on_the_way <= output_->frames_in_queue) {
      driver->RequestFrame();
    }
    if (queued < kMinQueuedFrames) {
      // The queue has (almost) emptied: one more to catch up, whatever is on its way, so a request the game never
      // answers cannot stop the audio.
      driver->RequestFrame();
    }
    // Safety: a count stuck for 64 ticks (~340 ms) without an answer is dropped.
    if (inflight) driver->AgeOutstanding(64);
    if (inflight && outstanding >= 4) {
      static uint32_t logged = 0;
      if (logged < 200) {
        ++logged;
        REXLOG_INFO("[audio] the game is {} frame requests behind ({} frames queued): no new request this tick",
                    outstanding, queued);
      }
    }
  }
}

void SwitchAudioSystem::OutputThreadMain() {
  // ME2 (2026-10-26): with the NSP forwarder (core 3 allowed) the output thread joins the other audio threads there.
  if (const unsigned long long use = (REXCVAR_GET(switch_audio_core_mask) & 0xFu) & RexSwitchProcessCoreMask();
      use && (use & 0x8))
    RexSwitchSetCurrentThreadMask(__builtin_ctzll(use), use);
  Output& output = *output_;
  for (size_t i = 0; i < kBufferCount; ++i) {
    FillAndAppend(i);
  }
  const u64 interval = armNsToTicks(kPumpIntervalNs);
  u64 deadline = armGetSystemTick() + interval;
  while (output.running.load(std::memory_order_relaxed)) {
    u64 timeout_ns = kWaitTimeoutNs;
    if (output.pump) {
      const u64 now = armGetSystemTick();
      if (now >= deadline) {
        PumpTick();
        deadline += interval;
        if (now >= deadline) {
          deadline = now + interval;  // more than one tick late: no catch-up burst
        }
        continue;
      }
      timeout_ns = armTicksToNs(deadline - now);  // at most until the next deadline
    }
    AudioOutBuffer* released = nullptr;
    u32 released_count = 0;
    Result rc = audoutWaitPlayFinish(&released, &released_count, timeout_ns);
    // libnx hands back one released buffer per call; drain the rest so none is
    // left out of the rotation.
    while (R_SUCCEEDED(rc) && released && released_count) {
      const ptrdiff_t index = released - output.buffers.data();
      if (index >= 0 && index < ptrdiff_t(kBufferCount)) {
        FillAndAppend(size_t(index));
      }
      released = nullptr;
      released_count = 0;
      rc = audoutGetReleasedAudioOutBuffer(&released, &released_count);
    }
  }
}

}  // namespace rex::audio::nx
