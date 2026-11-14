/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <rex/input/read_hook.h>

#include <rex/dbg.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#if REX_PLATFORM_SWITCH
#include <rex/input/switch/switch_input_driver.h>
#else
#include <rex/input/sdl/sdl_input_driver.h>
#endif
#include <rex/input/state_merge.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>
#include <rex/filesystem/vfs.h>
#include <cctype>
#include <string>
#include <vector>
#include <cstdlib>

// Dunwall (2026-11-12, unattended PC smoke tests): "seconds:buttons;..." (e.g. "30:start;36:a;40:a,start"):
// each step presses its buttons (a b x y start back up down left right lb rb, comma separated) for 200 ms at that many
// seconds after the first controller read, on top of the real controller (one is supplied if none is connected).
REXCVAR_DEFINE_STRING(input_script, "", "Input",
                      "Scripted button presses for unattended runs: \"seconds:buttons;...\" (a,b,x,y,start,back,"
                      "up,down,left,right,lb,rb), each held 200 ms");

REXCVAR_DEFINE_BOOL(input_hold_until_dlc_ready, false, "Input",
                    "Pad buttons and triggers read as released until the game has opened the downloadable content its "
                    "scan found (Dunwall's main menu said 'Required content unavailable' before that)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(input_hold_trigger_file, "", "Input",
                      "input_hold_until_dlc_ready: the hold begins when the game opens a file of this name (no folder; "
                      "Dunwall: its main menu's notice movie); empty = no hold")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(input_hold_max_s, 20, "Input", "input_hold_until_dlc_ready: release after this many seconds at most")
    .range(5, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if REX_PLATFORM_SWITCH
REXCVAR_DEFINE_STRING(input_backend, "switch", "Input", "Input backend: switch")
    .allowed({"switch"});
#else
REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});
#endif

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
REXCVAR_DEFINE_BOOL(input_rumble, false, "Input",
                    "Controller vibration (off by default: controllers only get "
                    "zero vibration). true = the game's vibration");
REXCVAR_DEFINE_BOOL(input_rumble_trace, false, "Input",
                    "Diagnostic: bounded logging of guest vibration requests and SDL delivery results")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace rex::input {

namespace {

// Synthetic devices are parked past every physical ordinal so they cannot push
// a real pad off guest user 0. SlotAssignment routes them by their synthetic
// flag and never reads this value.
constexpr uint32_t kSyntheticOrdinal = UINT32_MAX;

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  std::lock_guard lock(mutex_);
  // device_owners_ holds raw driver pointers.
  devices_.clear();
  device_owners_.clear();
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  std::lock_guard lock(mutex_);
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  std::lock_guard lock(mutex_);
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  std::lock_guard lock(mutex_);
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment) {
  std::lock_guard lock(mutex_);
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void InputSystem::RefreshDevices() {
  /*
   * At most one enumeration every 50 ms.
   *
   * Stack sampling found the thread that prepares each game frame spending 5.8 % of its time
   * inside SetState (vibration), and every game call to the controller went through here:
   * three new vectors and a query to each driver, several times per frame. A controller is not
   * connected or disconnected 30 times per second; with 50 ms a new controller shows up before
   * anyone notices. The first call always enumerates.
   */
  {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const int64_t last = last_refresh_ns_.load(std::memory_order_relaxed);
    if (last != 0 && !devices_.empty() && now - last < 50000000) {
      return;
    }
    last_refresh_ns_.store(now, std::memory_order_relaxed);
  }
  std::vector<DeviceInfo> seen;
  std::vector<InputDriver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  // Carry forward ordinals already handed out, so a device keeps its guest user
  // when another pad is unplugged.
  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Runs after the carry-forward pass so a new device cannot take an ordinal a
  // live one is still holding. Lowest free rather than a growing counter,
  // because a reconnected pad arrives as a new device and would otherwise walk
  // off the end of the guest users.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Only physical devices consume an ordinal, so the first pad to connect is
    // guest user 0 however many synthetic devices enumerated ahead of it.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](uint32_t candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    uint32_t ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable: synthetic devices share one ordinal, and their relative order
  // decides which answers GetCapabilities when no pad is attached.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
  // sa2-nx: drivers with per-player settings (the Switch's per-player button maps) learn each device's player.
  if (changed) {
    for (auto& driver : drivers_) {
      driver->OnDevicesAssigned(devices_);
    }
  }
}

InputDriver* InputSystem::DriverForDevice(DeviceId id) {
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* InputSystem::DeviceInfoFor(DeviceId id) const {
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard lock(mutex_);
  if (!out_caps || !assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  if (ids.empty()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Prefer the pad in hand, so button glyphs follow it rather than whichever
  // device enumerated first.
  DeviceId chosen = active_devices_.Active(user_index);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }

  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return driver->GetDeviceCapabilities(chosen, flags, out_caps);
}

namespace { std::atomic<ReadHook> g_read_hook{nullptr}; }
void SetReadHook(ReadHook hook) { g_read_hook.store(hook, std::memory_order_release); }

// ---------------------------------------------------------------------------------------------------------------------
// Dunwall (2026-11-09, input_hold_until_dlc_ready). Dunwall's front end reads its downloadable content and saves
// ~8 s after the main menu is up and usable (its script's own order: the XAM trace shows the game asking nothing
// meanwhile); a Continue in that time says "Required content unavailable. Please ensure all storage devices are
// properly connected or re-download content" and the menu is rebuilt once the DLC is mounted. With the option on, the
// pad's buttons and triggers read as released from the moment the game opens input_hold_trigger_file (Dunwall:
// the notice it opens as the main menu comes up, after the title screen's "Press Start") until it has scanned its DLC
// and opened every package the scan found, plus 2 s (the menu's rebuild); at once when the scan finds none; at the
// latest after input_hold_max_s. Sticks stay live. Once a run (the first time the file opens).
// ---------------------------------------------------------------------------------------------------------------------
namespace {
std::atomic<int32_t> g_dlc_found{-1};   // the last scan's DLC count (-1: no scan since the hold began)
std::atomic<int32_t> g_dlc_opened{0};   // DLC packages opened since that scan
std::atomic<int64_t> g_dlc_ready_ms{0}; // when the last one opened (steady clock, ms)
std::atomic<int64_t> g_hold_start_ms{0};  // 0: not holding (yet)
std::atomic<bool> g_hold_done{false};

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void HoldTriggerOpened(std::string_view file_name) {
  static const std::string trigger = REXCVAR_GET(input_hold_trigger_file);
  if (trigger.empty() || file_name.size() != trigger.size() || g_hold_done.load(std::memory_order_relaxed)) return;
  for (size_t i = 0; i < trigger.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(file_name[i])) != std::tolower(static_cast<unsigned char>(trigger[i])))
      return;
  int64_t expected = 0;
  if (g_hold_start_ms.compare_exchange_strong(expected, SteadyMs())) {
    g_dlc_found.store(-1, std::memory_order_relaxed);
    g_dlc_opened.store(0, std::memory_order_relaxed);
    REXLOG_INFO("[input] {} opened: buttons held until the game's DLC is in (input_hold_until_dlc_ready)", trigger);
  }
}

[[maybe_unused]] const bool g_hold_observer_set = [] {
  rex::filesystem::SetOpenObserver(HoldTriggerOpened);
  return true;
}();

bool HoldButtons() {
  const int64_t start = g_hold_start_ms.load(std::memory_order_relaxed);
  if (!start || g_hold_done.load(std::memory_order_relaxed)) return false;
  static const bool on = REXCVAR_GET(input_hold_until_dlc_ready);
  const int64_t now = SteadyMs();
  const int32_t found = g_dlc_found.load(std::memory_order_acquire);
  const char* why = nullptr;
  if (!on) {
    why = "off";
  } else if (now - start >= int64_t(REXCVAR_GET(input_hold_max_s)) * 1000) {
    why = "time limit";
  } else if (found >= 0 && g_dlc_opened.load(std::memory_order_acquire) >= found &&
             now - std::max(g_dlc_ready_ms.load(std::memory_order_relaxed), int64_t(0)) >= 2000) {
    why = found ? "the game opened the DLC its scan found" : "the game's scan found no DLC";
  }
  if (!why) return true;
  if (!g_hold_done.exchange(true, std::memory_order_relaxed) && on)
    REXLOG_INFO("[input] buttons released after {:.1f} s: {} ({} found, {} opened; input_hold_until_dlc_ready)",
                double(now - start) / 1000.0, why, found, g_dlc_opened.load());
  return false;
}
}  // namespace

void NoteGameDlcEnumerated(uint32_t count) {
  g_dlc_opened.store(0, std::memory_order_relaxed);
  g_dlc_ready_ms.store(SteadyMs(), std::memory_order_relaxed);
  g_dlc_found.store(int32_t(count), std::memory_order_release);
}

void NoteGameDlcOpened() {
  g_dlc_ready_ms.store(SteadyMs(), std::memory_order_relaxed);
  g_dlc_opened.fetch_add(1, std::memory_order_acq_rel);
}

namespace {
// input_script: the buttons to press now (0 when none, or the script is empty).
uint16_t ScriptedButtons() {
  struct Step {
    uint64_t at_ms;
    uint16_t buttons;
  };
  static const std::vector<Step> steps = [] {
    std::vector<Step> out;
    const std::string script = REXCVAR_GET(input_script);
    size_t at = 0;
    while (at < script.size()) {
      size_t end = script.find(';', at);
      if (end == std::string::npos) end = script.size();
      const std::string item = script.substr(at, end - at);
      at = end + 1;
      const size_t colon = item.find(':');
      if (colon == std::string::npos) continue;
      Step step{uint64_t(std::atof(item.substr(0, colon).c_str()) * 1000.0), 0};
      std::string names = item.substr(colon + 1);
      size_t n = 0;
      while (n < names.size()) {
        size_t e = names.find(',', n);
        if (e == std::string::npos) e = names.size();
        const std::string name = names.substr(n, e - n);
        n = e + 1;
        static const std::pair<const char*, uint16_t> kNames[] = {
            {"a", X_INPUT_GAMEPAD_A}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}, {"start", X_INPUT_GAMEPAD_START},
            {"back", 0x0020}, {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
            {"lb", 0x0100}, {"rb", 0x0200}};
        for (const auto& [k, v] : kNames)
          if (name == k) step.buttons |= v;
      }
      if (step.buttons) out.push_back(step);
    }
    if (!out.empty()) REXLOG_INFO("[input-script] {} scripted presses", out.size());
    return out;
  }();
  if (steps.empty()) return 0;
  static const uint64_t start = SteadyMs();
  const uint64_t now = SteadyMs() - start;
  uint16_t buttons = 0;
  for (const Step& step : steps)
    if (now >= step.at_ms && now < step.at_ms + 200) buttons |= step.buttons;
  return buttons;
}
}  // namespace

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  std::lock_guard lock(mutex_);
  if (const ReadHook hook = g_read_hook.load(std::memory_order_acquire)) hook();
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_INPUT_STATE state = {};
    if (driver->GetDeviceState(id, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    active_devices_.Observe(user_index, id, state.gamepad);
    if (!any) {
      merged = state;
      any = true;
    } else {
      MergeInto(merged, state);
    }
  }

  // input_script: on top of the real controller (a pad of its own for user 0 when none is connected).
  const uint16_t scripted = user_index == 0 ? ScriptedButtons() : 0;
  if (!any && user_index == 0 && !REXCVAR_GET(input_script).empty()) {
    merged = {};
    any = true;
  }
  if (scripted) merged.gamepad.buttons = uint16_t(uint16_t(merged.gamepad.buttons) | scripted);
  if (!any) {
    if (user_index < kMaxGuestUsers)
      state_seen_[user_index] = false;
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // A single guest counter must cover ALL successful reads. Switching between a driver's packet number for one
  // device and a separate counter for multiple devices could hide the release if the two happened to be equal.
  // Keep it per InputSystem and per guest user; a reconnect also publishes a new packet.
  if (user_index < kMaxGuestUsers) {
    if (!state_seen_[user_index] ||
        std::memcmp(&last_gamepads_[user_index], &merged.gamepad, sizeof(merged.gamepad)) != 0) {
      last_gamepads_[user_index] = merged.gamepad;
      state_seen_[user_index] = true;
      ++state_packets_[user_index];
    }
    merged.packet_number = state_packets_[user_index];
  }
  if (HoldButtons()) {  // input_hold_until_dlc_ready (above): the game only sees the sticks yet
    merged.gamepad.buttons = 0;
    merged.gamepad.left_trigger = 0;
    merged.gamepad.right_trigger = 0;
  }
  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard lock(mutex_);
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  // Every pad on this user belongs to the same player, so all of them buzz.
  // Only pads decide the result: synthetic devices accept any vibration and
  // would otherwise report success for a pad that never rumbled.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  X_RESULT pad_error = X_ERROR_DEVICE_NOT_CONNECTED;
  // input_rumble = false: the game still sees its call serviced, but the controllers only
  // receive zero vibration.
  X_INPUT_VIBRATION no_rumble{};
#if !REX_PLATFORM_SWITCH
  static const bool trace_rumble = REXCVAR_GET(input_rumble_trace);
  if (trace_rumble && user_index < kMaxGuestUsers) {
    struct Trace { bool seen = false; uint16_t left = 0, right = 0; unsigned lines = 0; };
    static Trace traces[kMaxGuestUsers];
    auto& trace = traces[user_index];
    const uint16_t left = vibration->left_motor_speed, right = vibration->right_motor_speed;
    if ((!trace.seen || left != trace.left || right != trace.right) && trace.lines < 32) {
      ++trace.lines;
      trace.seen = true; trace.left = left; trace.right = right;
      REXLOG_INFO("[rumble] guest user {} requested ({}, {}), input_rumble={}, assigned devices={}",
                  user_index, left, right, REXCVAR_GET(input_rumble), ids.size());
    }
  }
#endif
  if (!REXCVAR_GET(input_rumble)) {
    vibration = &no_rumble;
  }
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    const DeviceInfo* info = DeviceInfoFor(id);
    if (!driver || !info) {
      continue;
    }
    X_RESULT result = driver->SetDeviceVibration(id, vibration);
    if (info->synthetic) {
      any_synthetic = true;
      continue;
    }
    any_pad = true;
    if (result == X_ERROR_SUCCESS) {
      pad_rumbled = true;
    } else {
      pad_error = result;
    }
  }
  if (any_pad) {
    return pad_rumbled ? X_ERROR_SUCCESS : pad_error;
  }
  return any_synthetic ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard lock(mutex_);
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_RESULT result = driver->GetDeviceKeystroke(id, flags, out_keystroke);
    if (result == X_ERROR_SUCCESS) {
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
      return result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

#if REX_PLATFORM_SWITCH
    if (REXCVAR_GET(input_backend) == "switch") {
      auto switch_driver = std::make_unique<nx::SwitchInputDriver>(nullptr, 0);
      if (switch_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(switch_driver));
      }
    }
#else
    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }
#endif

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace rex::input
