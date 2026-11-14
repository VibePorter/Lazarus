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
};

// LE1 save file -> the ME1 Xbox files. False with 'why' when it is not one, damaged, or a case not supported yet
// (female / custom face).
bool Convert(const std::vector<uint8_t>& file, InflateFn inflate, const Options& options, Converted& out,
             std::string& why);

// Only the display name (for the list), without converting. False when it does not parse.
bool DisplayName(const std::vector<uint8_t>& file, InflateFn inflate, const Options& options, std::string& display);

}  // namespace me2::me1_import::le1
