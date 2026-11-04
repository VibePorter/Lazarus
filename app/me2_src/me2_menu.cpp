// Mass Effect 2 - main menu and Options entries the port has no use for.
//
// 2026-10-27 (user): New Game -> Import ME1 Character, Extras -> Downloadable Content and Options -> Online are
// gone, and Options -> "XBOX 360 Controller" reads "Controller".
//
// All four live in the ActionScript (AVM1) of two GFx movies, not in UnrealScript:
//   GUI_SF_MainMenu.MainMenu (EntryMenu), frame 1, setDisplayMenuEntries builds each list with displaymenu.push(...):
//     New Game: [Female, Male] then Import ME1 when m_bCanImport (SetupMainMenu passes IsReadyForLegacyImport()).
//     Extras (console): new Array(EMPTY, Achievements, Downloadable Content, Options, Credits).
//     The list code takes the length of displaymenu (slots past it are hidden, the selector stops at the last entry,
//     the selected entry is found by search), so dropping entries is enough.
//   GUI_SF_Options.Options (Startup_INT), class Options, initXBoxData: optionsData = [Gameplay, Controller, Graphics,
//     Sound, Online] as one array literal (Online is pushed first, so it is the last entry: no index of another entry
//     moves; removeOption/addOption are not called by the scripts). The Controller entry's label is
//     this.srController = "$163473" (the TLK string "XBOX 360 Controller").
// The edits (offsets in the movie, checked against the game's bytes; tools: scratch avm1.py disassembler):
//   MainMenu +0x25DC8  "if (m_bCanImport == true) push(IMPORT_ME1)" starts with a jump past it.
//   MainMenu +0x25D7E  push ENTRY_LIVECONTENT; GetVariable -> push; Pop, and +0x25D94 the Array's count 5 -> 4.
//   Options  +0x17168  the Online entry's code becomes "push this, 'optionsData'; jump past it" (268 bytes, the rest
//                      is dead code), and +0x179AC the top array's count 5 -> 4.
//   Options  +0x1743D  "push 'label', this.srController" jumps to new code in the dead Online bytes:
//                      push 'label', "<me2_options_controller_label>"; jump back.
// Applied where the GFx loader opens a file (sub_82AD9540 and sub_82AC5560, see the hooks) right after the opener
// returns the GFile, before the loader parses it: the
// GFile holds a pointer to the movie's bytes (UE3's opener: a memory file over the GFxMovieInfo's RawData), found in
// its first words as the pointer whose bytes are a GFX header with the movie's length next to it. Parsed movies are
// kept by GFx, so each movie is patched on its first load (and again whenever its package is loaded again).
//
// me2_menu_hide_import_me1, me2_menu_hide_dlc, me2_options_hide_online (default on); me2_options_controller_label
// ("Controller"; "" = the game's own text; needs me2_options_hide_online, its code lives in the Online entry's bytes).
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" bool Me2PatchConversationTriggers(uint8_t* movie, uint32_t size);
bool Me2PatchSubtitleHtml(uint8_t* movie, uint32_t size);  // me2_subtitles.cpp

REXCVAR_DEFINE_BOOL(me2_menu_hide_import_me1, true, "ME2", "Main menu: no New Game -> Import ME1 Character")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-11-02 (ME1 import, me2_me1_import.cpp): the entry also without the game's m_bCanImport (IsReadyForLegacyImport,
// UnrealScript, not looked into yet). Only when me2_menu_hide_import_me1 is off.
REXCVAR_DEFINE_BOOL(me2_menu_force_import_me1, false, "ME2",
                    "Main menu: New Game -> Import ME1 Character always shown (needs me2_menu_hide_import_me1 = false)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_menu_hide_dlc, true, "ME2", "Main menu: no Extras -> Downloadable Content")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_options_hide_online, true, "ME2", "Options: no Online entry (auto login, telemetry)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-27 (user): the HUD's action button (the glowing A next to the target's action text, "ButtonA" in
// GUI_SF_HUD.ME2_HUD) sits too low. Its clip is placed on the movie's root at (589.85, 217.05) px (PlaceObject2 at
// +0xBE76; its matrix's translation: 15 bits a value from bit 390159); the script only sets its _x (SetTargetStatus)
// and keeps _y = initY - offset, so the placement's y is where it stays. The text box TargetStatus spans y 204.4-227.6
// and the icon (shape 138, 19.2 px high) 208.1-227.3: centred on the box, but the letters sit in its upper part.
REXCVAR_DEFINE_DOUBLE(me2_hud_button_a_raise, 2.0, "ME2",
                      "HUD: how far up (pixels of the 1280x720 HUD) the action button A next to the target text "
                      "moves; 0 = the game's place")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Its x: SetTargetStatus, ButtonA._x = -5 + TargetStatus._x + TargetStatus._width / 2 - textWidth / 2 -
// ButtonA._width / 2 (the -5: an integer push at +0x1861).
REXCVAR_DEFINE_INT32(me2_hud_button_a_right, 2, "ME2",
                     "HUD: how far right (whole pixels of the 1280x720 HUD) the action button A moves; 0 = the game's "
                     "place")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_pause_button_b_raise, 1.0, "ME2",
                      "Pause menu: upward offset for the B icon beside Return in movie pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-29 (user): the death screen's face buttons follow the Switch: A on the right, B at the bottom. The console
// layout of GUI_SF_GameOver.GameOver (Startup_INT; consoleButtons, sprite 12) has the A entry (clip loadLastSaved,
// "Load Last Save") at the bottom and the B entry (openMainMenu, "Main Menu") on the right, their icons drawn by one
// shape (GameOver_I8 = A at the bottom, GameOver_IA = B on the right). The two clips swap their texts here (frame 1:
// consoleButtons.loadLastSaved.text = srLoadLast at +0x574 and openMainMenu.text = srMainMenu at +0x59E, constants 24
// and 32) and the two pictures swap their external image names (GameOver_I8.tga at +0xBB2 <-> GameOver_IA.tga at
// +0xBE8, same length: one character each), so the bottom shows the B picture and the right one the A picture. The
// buttons keep their actions (A resumes, B goes to the main menu): Resume is now on the right next to A, Main Menu at
// the bottom next to B. false = the game's layout.
// 2026-10-29 (user: the Bypass hack showed B at Start Bypass and A at Exit): the pictures used to be swapped in the
// replacement table (tools/me2_button_icons.py), but GameOver_I8 / _IA are byte for byte the A / B icons of the hacking
// mini-games (MiniGameBypass_I185 / _I180, MiniGameDecryption_IA5 / _IA0) and the table only sees texels, so the
// swap reached them too. The table is by letter again; the swap lives here, in this movie only.
REXCVAR_DEFINE_BOOL(me2_gameover_swap_ab, true, "ME2",
                    "Death screen: Resume next to the A button on the right, Main Menu next to B at the bottom (the "
                    "Switch's layout)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(me2_options_controller_label, "Controller", "ME2",
                      "Options: the label of the controller entry (\"\" = the game's \"XBOX 360 Controller\"; needs "
                      "me2_options_hide_online)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me2::gyro_menu {  // me2_gyro_menu.cpp
bool Enabled();
bool SubtitleRowEnabled();
bool BuildBody(uint8_t* out, size_t size);
}  // namespace me2::gyro_menu

namespace {
uint32_t Load32(const uint8_t* base, uint32_t va) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va)); }

bool Readable(uint32_t va, uint32_t len) {
  if (va < 0x00010000u || va >= 0xA0000000u || va + len < va) return false;
  rex::memory::BaseHeap* heap = REX_KERNEL_MEMORY()->LookupHeap(va);
  return heap && (int(heap->QueryRangeAccess(va, va + len - 1)) & int(rex::memory::PageAccess::kReadOnly)) != 0;
}

// A replacement of the movie's bytes at 'at': 'was' (the game's) becomes 'now' (same place, 'now' may be shorter).
struct Edit {
  uint32_t at;
  std::vector<uint8_t> was, now;
};

std::vector<uint8_t> Jump(uint32_t at, uint32_t to) {  // ActionJump (5 bytes), offset from the next action
  const int32_t off = int32_t(to) - int32_t(at + 5);
  return {0x99, 0x02, 0x00, uint8_t(off), uint8_t(off >> 8)};
}

// Applies all of 'edits' or none: each place must hold the game's bytes or ours ('checks': the game's bytes that must
// be there, untouched). Nothing is written when every edit is already in place.
bool Apply(uint8_t* m, const std::vector<Edit>& edits, const char* what, const std::vector<Edit>& checks = {}) {
  bool done = true;
  for (const Edit& e : edits) {
    const bool was = std::memcmp(m + e.at, e.was.data(), e.was.size()) == 0;
    const bool now = std::memcmp(m + e.at, e.now.data(), e.now.size()) == 0;
    if (!was && !now) {
      REXLOG_WARN("[menu] {}: not the expected bytes at +{:X}; left as the game's", what, e.at);
      return false;
    }
    done = done && now;
  }
  if (done) return true;
  for (const Edit& c : checks)
    if (std::memcmp(m + c.at, c.was.data(), c.was.size()) != 0) {
      REXLOG_WARN("[menu] {}: not the expected bytes at +{:X}; left as the game's", what, c.at);
      return false;
    }
  for (const Edit& e : edits) std::memcpy(m + e.at, e.now.data(), e.now.size());
  REXLOG_INFO("[menu] {}", what);
  return true;
}

constexpr uint8_t kGfx[4] = {'G', 'F', 'X', 8};
constexpr uint32_t kMainMenuSize = 228966, kOptionsSize = 110364, kHudSize = 61172;  // the GFX headers' lengths
constexpr uint32_t kGameOverSize = 28867;

void PatchGameOver(uint8_t* m) {
  if (!REXCVAR_GET(me2_gameover_swap_ab))
    return;
  Apply(m,
        {{0x574, {0x96, 0x04, 0x00, 0x08, 0x01, 0x08, 0x18, 0x1C, 0x4F}, {0x96, 0x04, 0x00, 0x08, 0x01, 0x08, 0x20, 0x1C, 0x4F}},
         {0x59E, {0x96, 0x04, 0x00, 0x08, 0x01, 0x08, 0x20, 0x1C, 0x4F}, {0x96, 0x04, 0x00, 0x08, 0x01, 0x08, 0x18, 0x1C, 0x4F}},
         // DefineExternalImage ids 8 and 10: "GameOver_I8.tga" <-> "GameOver_IA.tga" (the '8' / 'A' after "GameOver_I")
         {0xBB2, {'G', 'a', 'm', 'e', 'O', 'v', 'e', 'r', '_', 'I', '8', '.', 't', 'g', 'a'},
          {'G', 'a', 'm', 'e', 'O', 'v', 'e', 'r', '_', 'I', 'A', '.', 't', 'g', 'a'}},
         {0xBE8, {'G', 'a', 'm', 'e', 'O', 'v', 'e', 'r', '_', 'I', 'A', '.', 't', 'g', 'a'},
          {'G', 'a', 'm', 'e', 'O', 'v', 'e', 'r', '_', 'I', '8', '.', 't', 'g', 'a'}}},
        "death screen: Resume next to A on the right, Main Menu next to B at the bottom");
}

void PatchMainMenu(uint8_t* m) {
  if (REXCVAR_GET(me2_menu_hide_import_me1))
    Apply(m,
          {{0x25DC8,
            {0x96, 0x02, 0x00, 0x08, 0x36, 0x1C, 0x96, 0x02, 0x00, 0x05, 0x01, 0x49, 0x12, 0x9D, 0x02, 0x00, 0x18, 0x00},
            Jump(0x25DC8, 0x25DF2)}},
          "main menu: New Game -> Import ME1 Character removed");
  else if (REXCVAR_GET(me2_menu_force_import_me1))
    // The condition (push m_bCanImport; push true; Equals2; Not; If +0x18) jumps to the push itself.
    Apply(m,
          {{0x25DC8,
            {0x96, 0x02, 0x00, 0x08, 0x36, 0x1C, 0x96, 0x02, 0x00, 0x05, 0x01, 0x49, 0x12, 0x9D, 0x02, 0x00, 0x18, 0x00},
            Jump(0x25DC8, 0x25DDA)}},
          "main menu: New Game -> Import ME1 Character always shown");
  if (REXCVAR_GET(me2_menu_hide_dlc))
    Apply(m,
          {{0x25D7E, {0x96, 0x02, 0x00, 0x08, 0x5F, 0x1C}, {0x96, 0x02, 0x00, 0x08, 0x5F, 0x17}},
           {0x25D90, {0x96, 0x07, 0x00, 0x07, 0x05, 0x00, 0x00, 0x00, 0x08, 0x4C, 0x40},
            {0x96, 0x07, 0x00, 0x07, 0x04, 0x00, 0x00, 0x00, 0x08, 0x4C, 0x40}}},
          "main menu: Extras -> Downloadable Content removed");
}

void PatchOptions(uint8_t* m) {
  if (!REXCVAR_GET(me2_options_hide_online)) {
    if (!REXCVAR_GET(me2_options_controller_label).empty())
      REXLOG_WARN("[menu] options: me2_options_controller_label needs me2_options_hide_online; label left as the game's");
    return;
  }
  constexpr uint32_t kOnline = 0x17168, kOnlineEnd = 0x17274, kCave = 0x17174, kLabel = 0x1743D, kLabelBack = 0x17447;
  // The Online entry: the game's code starts and ends with these (and the Sound entry follows).
  const std::vector<uint8_t> online_start = {0x96, 0x0A, 0x00, 0x04, 0x01, 0x08, 0x53, 0x08,
                                             0x61, 0x04, 0x01, 0x08, 0x8E, 0x4E, 0x96, 0x06};
  const std::vector<uint8_t> online_end = {0x96, 0x05, 0x00, 0x07, 0x02, 0x00, 0x00, 0x00, 0x42,
                                           0x96, 0x05, 0x00, 0x07, 0x04, 0x00, 0x00, 0x00, 0x43};
  std::vector<uint8_t> skip = {0x96, 0x04, 0x00, 0x04, 0x01, 0x08, 0x53};  // push this (r1), 'optionsData'
  const std::vector<uint8_t> jump = Jump(kOnline + 7, kOnlineEnd);
  skip.insert(skip.end(), jump.begin(), jump.end());
  std::vector<Edit> edits = {
      {kOnline, online_start, skip},
      {0x179A8, {0x96, 0x05, 0x00, 0x07, 0x05, 0x00, 0x00, 0x00, 0x42}, {0x96, 0x05, 0x00, 0x07, 0x04, 0x00, 0x00, 0x00, 0x42}},
  };
  // Checked only on the first pass: the end of the Online entry (its bytes become dead code below).
  const std::vector<Edit> checks = {{kOnlineEnd - uint32_t(online_end.size()), online_end, {}}};
  std::string what = "options: Online removed";
  uint32_t dead = kCave;  // from here to kOnlineEnd: never run, filled with Pop (keeps the bytes well formed)
  static const std::string label = REXCVAR_GET(me2_options_controller_label);
  if (!label.empty() && label.find('\0') == std::string::npos && label.size() <= 200) {
    // push 'label' (constant 97), "<label>"; jump back after the game's push and GetMember.
    std::vector<uint8_t> cave = {0x96, uint8_t(label.size() + 4), 0x00, 0x08, 0x61, 0x00};
    cave.insert(cave.end(), label.begin(), label.end());
    cave.push_back(0x00);
    const std::vector<uint8_t> back = Jump(kCave + uint32_t(cave.size()), kLabelBack);
    cave.insert(cave.end(), back.begin(), back.end());
    std::vector<uint8_t> site = Jump(kLabel, kCave);
    site.insert(site.end(), 5, 0x17);  // the rest of the old push and its GetMember: Pop, never run
    edits.push_back({kLabel, {0x96, 0x06, 0x00, 0x08, 0x61, 0x04, 0x01, 0x08, 0xD2, 0x4E}, site});
    edits.push_back({kCave, {}, cave});
    dead = kCave + uint32_t(cave.size());
    what += ", \"XBOX 360 Controller\" -> \"" + label + "\"";
  }
  // me2-nx-deko-tu2 (2026-11-03): gyro aiming rows on the Controller page (me2_gyro_menu.cpp). The last 28 dead bytes
  // call this.initPCData(rows) right after the page's rows array is made; initPCData's body becomes the row builder.
  constexpr uint32_t kGyroCave = kOnlineEnd - 28, kRowsSite = 0x176C1, kRowsBack = 0x176C9;
  // 2026-11-04: Subtitle Size on the Gameplay page (me2_subtitles.cpp): the 33 dead bytes before the gyro snippet call
  // this.initPCData(1, rows) after the Gameplay page's rows array is made (+0x1799E InitArray, "Push 4" at +0x1799F).
  constexpr uint32_t kSubCave = kGyroCave - 33, kGameplaySite = 0x1799F, kGameplayBack = 0x179A7;
  constexpr uint32_t kPcHeader = 0x1662A, kPcFlags = 0x16631, kPcBody = 0x16635, kPcBodyEnd = 0x170B4;
  const bool gyro = me2::gyro_menu::Enabled() && dead <= kSubCave;
  const bool subs = me2::gyro_menu::SubtitleRowEnabled() && dead <= kSubCave;
  std::vector<Edit> all_checks = checks;
  const std::vector<uint8_t> push4 = {0x96, 0x05, 0x00, 0x07, 0x04, 0x00, 0x00, 0x00};
  if (gyro || subs) {
    edits.push_back({dead, {}, std::vector<uint8_t>(kSubCave - dead, 0x17)});
    if (subs) {
      // PushDuplicate; push 1, 2, this (r1), 'initPCData' (constant 95); CallMethod; Pop; push 4; jump back.
      std::vector<uint8_t> snippet = {0x4C, 0x96, 0x0E, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00, 0x07, 0x02, 0x00, 0x00,
                                      0x00, 0x04, 0x01, 0x08, 0x5F, 0x52, 0x17};
      snippet.insert(snippet.end(), push4.begin(), push4.end());
      const std::vector<uint8_t> back = Jump(kSubCave + uint32_t(snippet.size()), kGameplayBack);
      snippet.insert(snippet.end(), back.begin(), back.end());
      std::vector<uint8_t> site = Jump(kGameplaySite, kSubCave);
      site.insert(site.end(), 3, 0x17);
      edits.push_back({kSubCave, {}, snippet});
      edits.push_back({kGameplaySite, push4, site});
      what += ", Subtitle Size on the Gameplay page";
    } else {
      edits.push_back({kSubCave, {}, std::vector<uint8_t>(kGyroCave - kSubCave, 0x17)});
    }
    if (gyro) {
      // PushDuplicate; push 1, this (r1), 'initPCData' (constant 95); CallMethod; Pop; push 4; jump back to InitObject.
      std::vector<uint8_t> snippet = {0x4C, 0x96, 0x09, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00, 0x04, 0x01, 0x08, 0x5F,
                                      0x52, 0x17, 0x96, 0x05, 0x00, 0x07, 0x04, 0x00, 0x00, 0x00};
      const std::vector<uint8_t> back = Jump(kGyroCave + uint32_t(snippet.size()), kRowsBack);
      snippet.insert(snippet.end(), back.begin(), back.end());
      std::vector<uint8_t> site = Jump(kRowsSite, kGyroCave);
      site.insert(site.end(), 3, 0x17);
      edits.push_back({kGyroCave, {}, snippet});
      edits.push_back({kRowsSite, push4, site});
      what += ", gyro rows on the Controller page";
    } else {
      edits.push_back({kGyroCave, {}, std::vector<uint8_t>(kOnlineEnd - kGyroCave, 0x17)});
    }
    // initPCData: DefineFunction2 "" () regs 17, flags 0x0129 -> 0x0125 (arguments preloaded into r2, not suppressed).
    edits.push_back({kPcFlags, {0x29, 0x01}, {0x25, 0x01}});
    all_checks.push_back({kPcHeader, {0x8E, 0x08, 0x00, 0x00, 0x00, 0x00, 0x11}, {}});
    all_checks.push_back({kPcFlags + 2, {0x7F, 0x0A}, {}});  // body size 0xA7F: ends at kPcBodyEnd
  } else {
    edits.push_back({dead, {}, std::vector<uint8_t>(kOnlineEnd - dead, 0x17)});
  }
  const bool ok = Apply(m, edits, what.c_str(), all_checks);
  // The rows' values change with the player's choices: the builder is rewritten at every opening.
  if ((gyro || subs) && ok && m[kPcFlags] == 0x25 &&
      ((gyro && m[kRowsSite] == 0x99) || (subs && m[kGameplaySite] == 0x99)))
    me2::gyro_menu::BuildBody(m + kPcBody, kPcBodyEnd - kPcBody);
}

// SWF bit fields (most significant bit first).
int32_t GetBits(const uint8_t* m, uint32_t bit, uint32_t n) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < n; ++i, ++bit) v = v << 1 | (m[bit >> 3] >> (7 - (bit & 7)) & 1);
  return v >> (n - 1) ? int32_t(v) - (1 << n) : int32_t(v);
}

void PutBits(uint8_t* m, uint32_t bit, uint32_t n, int32_t value) {
  for (uint32_t i = 0; i < n; ++i, ++bit) {
    const uint8_t mask = uint8_t(1 << (7 - (bit & 7)));
    m[bit >> 3] = uint8_t(value >> (n - 1 - i) & 1 ? m[bit >> 3] | mask : m[bit >> 3] & ~mask);
  }
}

void PatchPauseMenu(uint8_t* m) {
  static const double raise = REXCVAR_GET(me2_pause_button_b_raise);
  if (raise == 0.0) return;
  // MainWheel: Return (sprite 194), ButtonB (sprite 193), PlaceObject2
  // at +0x40A1A. Move the icon alone, leaving textBoxB and ReturnBG in place.
  // Original translation (-3932,43) twips, 13 bits per axis.
  const double y = 43.0 - raise * 20.0;
  if (!(y >= -4096 && y <= 4095)) return;
  const int32_t ty = 43 - int32_t(raise * 20.0 + (raise < 0 ? -0.5 : 0.5));
  const std::vector<uint8_t> was = {0x26, 0x02, 0x00, 0xC1, 0x00, 0x1B, 0x0A, 0x40, 0x15, 0x80,
                                    'B', 'u', 't', 't', 'o', 'n', 'B', 0};
  auto now = was;
  PutBits(now.data(), 60, 13, ty);
  Apply(m, {{0x40A20, was, now}}, "pause menu: B icon raised");
}

void PatchHudX(uint8_t* m) {
  static const int32_t right = REXCVAR_GET(me2_hud_button_a_right);
  // Push '_x' (constant 66), int -5, ButtonA's text box (constant 36).
  constexpr uint32_t kPush = 0x185B, kValue = kPush + 6;
  static const uint8_t kHead[6] = {0x96, 0x09, 0x00, 0x08, 0x42, 0x07};
  if (right == 0 || std::memcmp(m + kPush, kHead, sizeof(kHead)) != 0 || m[kValue + 4] != 0x08 || m[kValue + 5] != 0x24)
    return;
  const int32_t value = -5 + right;
  const int32_t now = int32_t(uint32_t(m[kValue]) | uint32_t(m[kValue + 1]) << 8 | uint32_t(m[kValue + 2]) << 16 |
                              uint32_t(m[kValue + 3]) << 24);
  if (now == value) return;
  if (now != -5) {
    REXLOG_WARN("[menu] HUD: ButtonA's x offset is {}, not the game's -5; left alone", now);
    return;
  }
  for (int k = 0; k < 4; ++k) m[kValue + k] = uint8_t(uint32_t(value) >> (8 * k));
  REXLOG_INFO("[menu] HUD: action button A {} px right", right);
}

void PatchHud(uint8_t* m) {
  PatchHudX(m);
  static const double raise = REXCVAR_GET(me2_hud_button_a_raise);
  if (raise == 0.0) return;
  // PlaceObject2 (long header), flags 0x26 (name, matrix, character), depth 149, character 140, the matrix (no scale
  // or rotation, 15-bit translation 11797, 4341 twips), the name "ButtonA".
  constexpr uint32_t kPlace = 0xBE76, kTy = 390159 + 15, kGameTy = 4341;
  static const uint8_t kHead[13] = {0xBF, 0x06, 0x12, 0x00, 0x00, 0x00, 0x26, 0x95, 0x00, 0x8C, 0x00, 0x1E, 0xB8};
  if (std::memcmp(m + kPlace, kHead, sizeof(kHead)) != 0 || std::memcmp(m + kPlace + 16, "ButtonA", 8) != 0 ||
      GetBits(m, kTy - 15, 15) != 11797) {
    REXLOG_WARN("[menu] HUD: ButtonA's placement is not the expected bytes; left as the game's");
    return;
  }
  const int32_t ty = int32_t(kGameTy) - int32_t(raise * 20.0 + (raise < 0 ? -0.5 : 0.5));
  if (ty < -16384 || ty > 16383) return;
  const int32_t now = GetBits(m, kTy, 15);
  if (now == ty) return;
  if (now != int32_t(kGameTy)) {
    REXLOG_WARN("[menu] HUD: ButtonA's y is {} twips, not the game's; left alone", now);
    return;
  }
  PutBits(m, kTy, 15, ty);
  REXLOG_INFO("[menu] HUD: action button A {:.2f} px up (y {:.2f} -> {:.2f})", raise, kGameTy / 20.0, ty / 20.0);
}

// The movie a GFile (the opener's result) reads: a pointer in its first words to a GFX header whose length is the
// movie's. Returns what it found, for the log.
const char* PatchFile(const uint8_t* cbase, uint8_t* base, uint32_t file) {
  if (!Readable(file, 0x40)) return "no file";
  for (uint32_t off = 4; off < 0x40; off += 4) {
    const uint32_t data = Load32(cbase, file + off);
    if (!Readable(data, 16) || std::memcmp(cbase + data, kGfx, 4) != 0) continue;
    const uint32_t size = uint32_t(cbase[data + 4]) | uint32_t(cbase[data + 5]) << 8 | uint32_t(cbase[data + 6]) << 16 |
                          uint32_t(cbase[data + 7]) << 24;
    if (size == kHudSize && Readable(data, size)) {
      PatchHud(base + data);
      return "HUD";
    }
    if (size == 293999 && Readable(data, size)) {
      PatchPauseMenu(base + data);
      return "pause menu";
    }
    if (size == kGameOverSize && Readable(data, size)) {
      PatchGameOver(base + data);
      return "death screen";
    }
    if (size == 62130 && Readable(data, size)) {
      if (!Me2PatchConversationTriggers(base + data, size))
        REXLOG_WARN("[icons] conversation interrupts: unexpected vector shapes; left as the game's");
      Me2PatchSubtitleHtml(base + data, size);
      return "conversation";
    }
    if (size != kMainMenuSize && size != kOptionsSize) return "another movie";
    if (!Readable(data, size)) return "movie not readable";
    if (size == kMainMenuSize) PatchMainMenu(base + data);
    else PatchOptions(base + data);
    return size == kMainMenuSize ? "main menu" : "options";
  }
  return "no GFX data in the file";
}

std::string Url(const uint8_t* base, uint32_t va) {
  std::string s;
  if (!Readable(va, 1)) return s;
  for (uint32_t i = 0; i < 160 && base[va + i]; ++i) s += char(base[va + i]);
  return s;
}

void Opened(const char* where, const uint8_t* cbase, uint8_t* base, uint32_t url, uint32_t file) {
  const std::string u = Url(cbase, url);
  const char* found = file ? PatchFile(cbase, base, file) : "not opened";
  // Only the two movies (the loader opens each file through both functions).
  if (u.find("MainMenu.MainMenu") != std::string::npos || u.find("Options.Options") != std::string::npos ||
      u.find("HUD.ME2_HUD") != std::string::npos || u.find("GameOver.GameOver") != std::string::npos ||
      u.find("InGameGui.MainWheel") != std::string::npos ||
      u.find("Conversation.Conversation") != std::string::npos)
    REXLOG_INFO("[menu] {} '{}': {}", where, u, found);
}
}  // namespace

// The loader's two ways to a file: GFxLoaderImpl::OpenFile (sub_82AD9540: this, url, flags) and the load states' own
// (sub_82AC5560: this, url, log; "GFxLoader failed to open '%s'"), which asks the file opener itself. Both return the
// GFile (0 on failure); patching is the same whichever runs (a patched movie is recognised).
REX_EXTERN(__imp__sub_82AD9540);
REX_HOOK_RAW(sub_82AD9540) {
  const uint32_t url = ctx.r4.u32;
  __imp__sub_82AD9540(ctx, base);
  Opened("GFxLoaderImpl::OpenFile", base, base, url, ctx.r3.u32);
}

REX_EXTERN(__imp__sub_82AC5560);
REX_HOOK_RAW(sub_82AC5560) {
  const uint32_t url = ctx.r4.u32;
  __imp__sub_82AC5560(ctx, base);
  Opened("GFxLoadStates::OpenFile", base, base, url, ctx.r3.u32);
}

// ---------------------------------------------------------------------------------------------------------------------
// me2-nx-deko (2026-11-01, user: the title screen's "Press START" should say "Press +"): chosen strings of the talk
// table replaced by id. FBioTlkFile's lookup sub_827CEE50(this r3, string id r4, FString* out r5, female r6) binary
// searches the id (sub_827CEDD0) in the male (+12 entries, +40 count) or female (+8, +36) table, decodes the Huffman
// bits into the FString (sub_822CD030, FString += TCHAR*) and returns found. Its callers are the talk table's getter
// (sub_827D21B0), FBioTlkFile::LoadFromDisk (sub_827CEFC8) and three script / GUI paths, so GUI "$id" texts and
// script lookups both come through here. me2_tlk_overrides: "ID=text|ID=text" (UTF-8); a listed id answers that text in
// every language (the Russian profiles set their own). 168205 = "Press START" ("Нажмите кнопку START" in Russian),
// 174829 = "Start Button" ("Кнопка Старт").
REXCVAR_DEFINE_STRING(me2_tlk_overrides, "168205=Press +|174829=+ Button", "ME2",
                      "Talk-table strings replaced by id: \"ID=text|ID=text\" (e.g. 168205 = the title screen's "
                      "\"Press START\"); empty = the game's own")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
struct TlkOverride {
  uint32_t id;
  std::vector<uint16_t> text;  // UTF-16
};

std::vector<uint16_t> TlkUtf16(const std::string& s) {
  std::vector<uint16_t> out;
  for (size_t i = 0; i < s.size();) {
    const uint8_t b = uint8_t(s[i]);
    uint32_t cp = '?';
    size_t len = 1;
    if (b < 0x80) {
      cp = b;
    } else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
      cp = (b & 0x1Fu) << 6 | (uint8_t(s[i + 1]) & 0x3Fu);
      len = 2;
    } else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
      cp = (b & 0x0Fu) << 12 | (uint8_t(s[i + 1]) & 0x3Fu) << 6 | (uint8_t(s[i + 2]) & 0x3Fu);
      len = 3;
    } else if ((b & 0xF8) == 0xF0) {
      len = 4;
    }
    out.push_back(uint16_t(cp));
    i += len;
  }
  return out;
}

const std::vector<TlkOverride>& TlkOverrides() {
  static const std::vector<TlkOverride> list = [] {
    std::vector<TlkOverride> out;
    const std::string text = REXCVAR_GET(me2_tlk_overrides);
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find('|', at);
      if (end == std::string::npos) end = text.size();
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      const size_t eq = item.find('=');
      if (eq == std::string::npos) continue;
      const uint32_t id = uint32_t(std::strtoul(item.substr(0, eq).c_str(), nullptr, 10));
      if (!id) continue;
      std::vector<uint16_t> u = TlkUtf16(item.substr(eq + 1));
      if (u.size() > 1000) continue;
      out.push_back({id, std::move(u)});
      REXLOG_INFO("[tlk] string {} -> \"{}\" (me2_tlk_overrides)", id, item.substr(eq + 1));
    }
    return out;
  }();
  return list;
}
}  // namespace

REX_EXTERN(__imp__sub_827CEE50);
REX_EXTERN(__imp__sub_822CD030);
REX_HOOK_RAW(sub_827CEE50) {
  const auto& list = TlkOverrides();
  const uint32_t id = ctx.r4.u32 & 0x0FFFFFFFu, out = ctx.r5.u32;
  const TlkOverride* hit = nullptr;
  for (const TlkOverride& o : list)
    if (o.id == id) hit = &o;
  if (!hit || !Readable(out, 12)) {
    __imp__sub_827CEE50(ctx, base);
    return;
  }
  // The text (big-endian TCHARs) on the guest stack below the caller's frame, then FString::Empty-like (Num = 0,
  // the allocation kept) and FString += text, as the original does with each decoded run.
  const uint32_t sp = ctx.r1.u32;
  const uint32_t chars = uint32_t(hit->text.size()) + 1;
  const uint32_t frame = (sp - 0x100u - chars * 2u) & ~15u;
  const uint32_t text_va = frame + 0x80;
  uint8_t* const p = base + text_va;
  for (uint32_t i = 0; i < chars; ++i) {
    const uint16_t c = i + 1 < chars ? hit->text[i] : 0;
    p[2 * i] = uint8_t(c >> 8);
    p[2 * i + 1] = uint8_t(c);
  }
  *reinterpret_cast<uint32_t*>(base + frame) = __builtin_bswap32(sp);  // the back chain, as stwu
  *reinterpret_cast<uint32_t*>(base + out + 4) = 0;                      // FString num = 0
  ctx.r1.u64 = frame;
  ctx.r3.u64 = out;
  ctx.r4.u64 = text_va;
  __imp__sub_822CD030(ctx, base);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = 1;  // found
}
