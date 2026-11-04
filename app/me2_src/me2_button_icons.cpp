// Mass Effect 2 - the button prompts with the Switch's buttons.
//
// The game draws its button prompts from its own textures (Startup_INT: GUI_GlobalIcons.Buttons.XBox_Btn_*, 64x64, and
// the glowing face buttons GUI_SF_SharedAssets.mainController_I*, 32x32; A8R8G8B8, tiled). Renut's ReadLevel
// (render/textures.cpp, g_texture_replace) hands every small 32-bit 2D level to Replace below untiled, in the guest's
// byte order; one whose FNV-1a 64 hash is a known icon's gets its Switch replacement in the same layout. The table and
// the icons: tools/me2_button_icons.py (the user's Switch glyphs through tools/me2_icon_style.py), see that script for
// the mapping (A and B by letter, X and Y by position, Nintendo's colours by letter).
//
// me2_switch_button_icons (default on): the Switch icons. Off: the game's.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

REXCVAR_DEFINE_BOOL(me2_switch_button_icons, true, "ME2",
                    "Button prompts with the Switch's buttons (the game's Xbox 360 icons replaced as they are loaded)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// 2026-10-27 (user): the diagram's Y icon 1 px higher. In its clip (sprite 23, ControlY, used by two of the three
// layouts) the icon (sprite 22) is placed by a PlaceObject2 at +0x657, translation 12 bits a value from bit 13031:
// (-2013, 25) twips; A, B and X sit at y 38-57 twips in theirs.
REXCVAR_DEFINE_DOUBLE(me2_diagram_y_raise, 1.0, "ME2",
                      "Controller diagram: how far up (pixels of the movie) the Y icon moves; 0 = the game's place")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_diagram_buttons_raise, 2.0, "ME2",
                      "Action diamond: additional upward offset for all four icons in movie pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(me2_diagram_switch_y_right, 2.0, "ME2",
                      "Action diamond: rightward offset for the Switch Y icon in movie pixels")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(me2_diag_icon_dump, "", "ME2/Diagnostics",
                      "Folder to write every distinct small 32-bit texture the renderer loads to (<hash>_<w>x<h>.bin, "
                      "the untiled guest bytes); \"\" = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {
extern bool (*g_texture_replace)(uint32_t width, uint32_t height, uint8_t* guest_texels);
}

namespace {

struct IconReplacement {
  uint64_t hash;
  uint32_t width, height;
  const uint8_t* texels;
  const char* name;
};

#include "me2_button_icons_data.inc"
#include "me2_interrupt_icons_data.inc"

uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

void Dump(uint32_t width, uint32_t height, const uint8_t* texels, uint64_t hash) {
  static const std::string dir = REXCVAR_GET(me2_diag_icon_dump);
  if (dir.empty()) return;
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard<std::mutex> lock(mutex);
  if (!seen.insert(hash).second || seen.size() > 4000) return;
  char name[64];
  std::snprintf(name, sizeof(name), "/%016llX_%ux%u.bin", static_cast<unsigned long long>(hash), width, height);
  if (std::FILE* f = std::fopen((dir + name).c_str(), "wb")) {
    std::fwrite(texels, 1, size_t(width) * height * 4, f);
    std::fclose(f);
  }
}

bool Replace(uint32_t width, uint32_t height, uint8_t* texels) {
  const size_t bytes = size_t(width) * height * 4;
  const uint64_t hash = Fnv1a64(texels, bytes);
  Dump(width, height, texels, hash);
  // Only the sizes the icons have.
  if (!((width == 64 && height == 64) || (width == 32 && height == 32))) return false;
  for (const IconReplacement& r : kIcons) {
    if (r.hash != hash || r.width != width || r.height != height) continue;
    std::memcpy(texels, r.texels, bytes);
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 40) REXLOG_INFO("[icons] {} -> Switch icon", r.name);
    return true;
  }
  return false;
}

// 2026-10-27 (user): the controller diagram (com.XController, 13 screens) by the Switch's positions. Its icons are the
// glowing ones above, only used by the shared movie GUI_SF_SharedAssets.mainController (Startup_INT; imported as
// mainController.swf), and go by position (the bottom one, the game's A, shows the Switch's B; the right one, the
// game's B, the Switch's A). 2026-10-27: the HUD's and the power wheel's face buttons (GUI_SF_HUD.ME2_HUD_I89,
// ME2_PowerWheel_I101/I105/IF6/IF9, the in-game prompts) are the same textures byte for byte, and a replacement only
// sees the texels, so the textures are all replaced by letter (A shows the Switch's A) and the diagram takes its two
// pictures the other way round instead: its image tags (DefineExternalImage, 1001) name the texture files, id 6
// "mainController_I6.tga" (A, drawn by the bottom button's shape) and id 16 "mainController_I10.tga" (B, the right
// one's); the names swap (the tags' lengths differ by one: the bytes between them move by one, the movie's length
// stays), so the bottom button draws B and the right one A. The ActionScript finds a button's clip by name
// (".Control" + letter), so the clips ControlA (bottom) and ControlB (right) swap names in their PlaceObject2 tags
// (all three layouts): A's label and highlight (confirm) go to the right clip with the Switch's A, B's to the bottom
// one. The clips themselves stay (each is drawn for its own place; swapping their characters moved their insides,
// and re-pointing the icon shapes' bitmaps lost the bottom one: GFx resolves the ids in file order). Patched in the
// movie's bytes in guest memory (the GFxMovieInfo's raw data) before any screen imports it. ReplayCharacterSelect has
// its own controller and icons and is left alone.
constexpr uint8_t kMainControllerHeader[8] = {'G', 'F', 'X', 8, 0xD4, 0x1A, 0, 0};  // version 8, 6868 bytes
constexpr uint32_t kNameA[3] = {5884, 6298, 6511}, kNameB[3] = {5934, 6352, 6564};
constexpr uint32_t kImageA = 0x2CA, kImageB = 0x4E1;  // the two image tags (header included)
constexpr uint8_t kImageTagA[33] = {0x5F, 0xFA, 0x06, 0x00, 0x0D, 0x00, 0x1F, 0x00, 0x20, 0x00, 0x00, 0x15, 'm', 'a', 'i',
                                    'n',  'C',  'o',  'n',  't',  'r',  'o',  'l',  'l',  'e',  'r',  '_',  'I', '6', '.',
                                    't',  'g',  'a'};
constexpr uint8_t kImageTagB[34] = {0x60, 0xFA, 0x10, 0x00, 0x0D, 0x00, 0x1F, 0x00, 0x20, 0x00, 0x00, 0x16, 'm', 'a',
                                    'i',  'n',  'C',  'o',  'n',  't',  'r',  'o',  'l',  'l',  'e',  'r',  '_', 'I',
                                    '1',  '0',  '.',  't',  'g',  'a'};

// The Y icon's placement in its clip: 06 (character, matrix) 05 00 (depth 5) 16 00 (sprite 22), the matrix.
constexpr uint32_t kYPlace = 0x657;
constexpr uint8_t kYPlaceBytes[9] = {0x06, 0x05, 0x00, 0x16, 0x00, 0x19, 0x04, 0x60, 0x32};

bool PositionDiamondIcons(uint8_t* m) {
  // Inner icon placements only: text, arrows and actions keep their positions. The
  // bottom icon has a second matrix in its highlighted frame; both must move.
  // Game X -> Switch Y (left), game Y -> Switch X (top), A/B pictures swap below.
  struct Placement { uint32_t at, bit; int32_t x, y; uint8_t bytes[9]; bool switch_y, game_y; };
  static constexpr Placement places[] = {
      {0x372, 7103, 2005, 38, {0x06,0x05,0,0x08,0,0x18,0xFA,0xA0,0x4C}, false, false},
      {0x38B, 7287, 2009, 42, {0x0D,0x05,0,0x18,0xFB,0x20,0x54,0xE8,0}, false, false},
      {0x4B6, 9695, 2005, 57, {0x06,0x05,0,0x0E,0,0x18,0xFA,0xA0,0x72}, true, false},
      {0x588, 11375, -2008, 39, {0x06,0x05,0,0x12,0,0x19,0x05,0,0x4E}, false, false},
      {kYPlace, 13031, -2013, 25, {0x06,0x05,0,0x16,0,0x19,0x04,0x60,0x32}, false, true},
  };
  static const double up = REXCVAR_GET(me2_diagram_buttons_raise);
  static const double right = REXCVAR_GET(me2_diagram_switch_y_right);
  static const double top_up = REXCVAR_GET(me2_diagram_y_raise);
  auto twips = [](double px) { return int32_t(px * 20.0 + (px < 0 ? -0.5 : 0.5)); };
  // Validate every placement and range before changing any icon.
  for (const auto& p : places) {
    const double x = p.x + (p.switch_y ? right * 20.0 : 0.0);
    const double y = p.y - (up + (p.game_y ? top_up : 0.0)) * 20.0;
    if (!(x >= -2048 && x <= 2047 && y >= -2048 && y <= 2047) ||
        std::memcmp(m + p.at, p.bytes, sizeof(p.bytes)) != 0) return false;
  }
  auto put = [m](uint32_t bit, int32_t value) {
    for (uint32_t i = 0; i < 12; ++i, ++bit) {
      const uint8_t mask = uint8_t(1 << (7 - (bit & 7)));
      m[bit >> 3] = uint8_t(value >> (11 - i) & 1 ? m[bit >> 3] | mask : m[bit >> 3] & ~mask);
    }
  };
  for (const auto& p : places) {
    put(p.bit, p.x + (p.switch_y ? twips(right) : 0));
    put(p.bit + 12, p.y - twips(up + (p.game_y ? top_up : 0.0)));
  }
  REXLOG_INFO("[icons] action diamond: icons {:.2f} px up, Switch Y {:.2f} px right", up, right);
  return true;
}

// The image tags' texture names swapped: id 6 gets B's, id 16 A's.
void SwapImages(uint8_t* m) {
  const uint32_t mid_from = kImageA + sizeof(kImageTagA), mid_len = kImageB - mid_from;
  std::memmove(m + kImageA + sizeof(kImageTagB), m + mid_from, mid_len);  // the bytes between: one later
  uint8_t* a = m + kImageA;  // id 6, 34 bytes, B's name
  std::memcpy(a, kImageTagB, sizeof(kImageTagB));
  a[2] = 0x06;
  uint8_t* b = m + kImageA + sizeof(kImageTagB) + mid_len;  // id 16, 33 bytes, A's name
  std::memcpy(b, kImageTagA, sizeof(kImageTagA));
  b[2] = 0x10;
}

bool IsName(const uint8_t* p, char letter) { return std::memcmp(p, "Control", 7) == 0 && p[7] == letter && p[8] == 0; }
}  // namespace

// Conversation's interrupts use vector LT/RT lettering, not the replaceable icon textures.
// Patch both complete shape payloads before GFx parses them. IDs, tag lengths, timing-ring
// geometry and ActionScript stay in place. The generated contours read ZL (Paragon) / ZR (Renegade).
extern "C" bool Me2PatchConversationTriggers(uint8_t* m, uint32_t size) {
  if (!REXCVAR_GET(me2_switch_button_icons)) return true;
  constexpr uint8_t header[] = {'G', 'F', 'X', 8, 0xB2, 0xF2, 0, 0};
  if (size != 62130 || std::memcmp(m, header, sizeof(header)) != 0) return false;
  const bool old0 = std::memcmp(m + kInterruptOffset0, kInterruptOld0, sizeof(kInterruptOld0)) == 0;
  const bool new0 = std::memcmp(m + kInterruptOffset0, kInterruptNew0, sizeof(kInterruptNew0)) == 0;
  const bool old1 = std::memcmp(m + kInterruptOffset1, kInterruptOld1, sizeof(kInterruptOld1)) == 0;
  const bool new1 = std::memcmp(m + kInterruptOffset1, kInterruptNew1, sizeof(kInterruptNew1)) == 0;
  // Unknown data leaves BOTH shapes untouched; a loader's second visit is a no-op.
  if ((!old0 && !new0) || (!old1 && !new1)) return false;
  if (new0 && new1) return true;
  std::memcpy(m + kInterruptOffset0, kInterruptNew0, sizeof(kInterruptNew0));
  std::memcpy(m + kInterruptOffset1, kInterruptNew1, sizeof(kInterruptNew1));
  REXLOG_INFO("[icons] conversation interrupts: Paragon ZL / Renegade ZR");
  return true;
}

// The movie's bytes at 'va' (me2_cerberus.cpp finds the GFxMovieInfo's raw data through GObjects on the main menu:
// Startup_INT loaded, no screen has imported the movie yet). False when they are not the expected movie.
extern "C" bool Me2SwapControllerLabels(uint8_t* base, uint32_t va, uint32_t size) {
  if (!REXCVAR_GET(me2_switch_button_icons)) return true;
  uint8_t* m = base + va;
  if (size < 6868 || std::memcmp(m, kMainControllerHeader, 8) != 0) return false;
  for (uint32_t i = 0; i < 3; ++i)
    if (!IsName(m + kNameA[i], 'A') || !IsName(m + kNameB[i], 'B')) return false;
  if (std::memcmp(m + kImageA, kImageTagA, sizeof(kImageTagA)) != 0 ||
      std::memcmp(m + kImageB, kImageTagB, sizeof(kImageTagB)) != 0 ||
      std::memcmp(m + kYPlace, kYPlaceBytes, sizeof(kYPlaceBytes)) != 0)
    return false;
  if (!PositionDiamondIcons(m)) return false;
  SwapImages(m);
  for (uint32_t i = 0; i < 3; ++i) {
    m[kNameA[i] + 7] = 'B';
    m[kNameB[i] + 7] = 'A';
  }
  REXLOG_INFO("[icons] controller diagram: mainController.swf ControlA <-> ControlB and their pictures at {:08X}", va);
  return true;
}

namespace {
struct Install {
  Install() {
    // Read at the first texture upload would need a guard on that path; the cvar is read here once the table is set.
    renut::gpu::g_texture_replace = [](uint32_t w, uint32_t h, uint8_t* t) {
      static const bool on = REXCVAR_GET(me2_switch_button_icons);
      return on && Replace(w, h, t);
    };
  }
} g_install;

}  // namespace
