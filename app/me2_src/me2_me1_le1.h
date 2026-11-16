// ME2 (2026-11-14): ME1 Legendary Edition saves (.pcsav, LE1 save version 50) for the ME1 import.
// me2_me1_le1.cpp turns one into the two files ME2's importer reads from Char_Prep: player.sav (an ME1 Xbox package,
// built from a template package) and state.sav (ME1 Xbox, big-endian). See the .cpp for the formats.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace me2::me1_import::le1 {

// Inflates a zlib stream (with its 2-byte header) into exactly 'out_size' bytes; true on success.
using InflateFn = bool (*)(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);

// Whether 'file' starts like an LE1 save (the compressed-chunk tag, little-endian).
bool LooksLikeLe1(const std::vector<uint8_t>& file);

struct Converted {
  std::vector<uint8_t> player;  // player.sav
  std::vector<uint8_t> state;   // state.sav
  std::string display;          // the save's display name (UTF-8), e.g. "John Shepard : Level - 60"
  std::string summary;          // a line for the log: name, class, level, ...
};

// Names in Latin letters (2026-11-14): ME2's English fonts have no Cyrillic (a Russian LE1's "Джон" showed as squares).
// The default first names (Джон / Джейн) become John / Jane, other Cyrillic names are transliterated, and a display
// name with letters outside Latin-1 becomes "<first name> Shepard - Level <n>".
struct Options {
  bool latin_names = true;
  // ME2 (2026-11-15): carry LE1's head morph over (hair, sliders, bones, vertices, materials); false = the default
  // face of the character's gender.
  bool keep_face = true;
  // ME2 (2026-11-15): LE1's skin tone moved onto ME1 / ME2's skin palette (its lightness kept); false = as LE1 stored
  // it (LE1's desaturated tones render pale on ME2's face texture).
  bool map_skin_tone = true;
  // ME2 (2026-11-15): makeup strengths (blush, lips, eye shadow, brow tint) as ME2's creator pairs them with the
  // colour preset LE1 stored, else clamped to ME2's largest (LE1's blush 1.14 / brow 2.0 went past ME2's 1.0).
  bool map_makeup = true;
  // ME2 (2026-11-15): a hair colour brighter / more saturated than ME2's presets reach (LE1's vivid red: neon) toned
  // into their range, its hue kept.
  bool map_hair_colour = true;
};

// LE1 save file -> the ME1 Xbox files. False with 'why' when it is not one or damaged.
bool Convert(const std::vector<uint8_t>& file, InflateFn inflate, const Options& options, Converted& out,
             std::string& why);

// ME2 (2026-11-15): the objects an ME1 Xbox player.sav imports from content packages (textures, meshes, the face
// asset): "Package.Group.Object" paths, BIOG_MORPH_FACE's first; classes and packages left out. False when the
// package's import table does not parse.
bool ContentImports(const std::vector<uint8_t>& package, std::vector<std::string>& paths);

// Only the display name (for the list), without converting. False when it does not parse.
bool DisplayName(const std::vector<uint8_t>& file, InflateFn inflate, const Options& options, std::string& display);

}  // namespace me2::me1_import::le1
