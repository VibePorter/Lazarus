// Mass Effect 2 - New Game -> Import ME1 Character without the Xbox's importer program.
//
// 2026-11-02 (user: make the ME1 import work). How the Xbox 360 does it (static analysis, see memory me2-me1-import):
//   1. The menu entry runs the native LaunchUnreaper: ME2 sets launch data and starts game:\Unreap.xex through
//      XLaunchNewImage (sub_82C4BB80 -> XamLoaderLaunchTitle).
//   2. Unreap.xex (title 4D5308C5, "Mass Effect 2 Importer") lists the ME1 "Char_*" saved games, copies every file of
//      the chosen one into its own saved game "Char_Prep" (type 1, title 4D5308C5) and starts Default.xex with the
//      launch data "<CommandLine of game:\media\Unreap.ini> -autodevice<device> -expectuser<user>", where the ini's
//      CommandLine is "BioP_ProNor -importstartupmovies -legacydataready" (the xex's own fallback, "BioP_ProNor
//      -nostartupmovies", lacks -legacydataready: 2026-11-02 Ryujinx run, the prologue started without importing).
//   3. ME2 takes that launch data as its command line (sub_82865C98): the prologue map, and LegacyDataCallback runs
//      "defer importcharacter Char_Prep <n>" -> sub_8297C628 opens Char_Prep under 4D5308C5 (XamContentCreateInternal,
//      root XCONTENT) and reads player.sav (a UE3 package, sub_8297CC78) and state.sav (sub_8297CA38).
//
// The port's routes (2026-11-02):
//   - me2_me1_import_load_screen (default): the PC version's route. The main menu's Import ME1 Character opens the
//     game's own load screen in its ME1 mode; this file fills its list with the saves of me2_me1_import_dir (each
//     made a saved game of 4D5308C5) and picking one runs "defer importcharacter <save> 1 forcenewgame": imported,
//     new game, no restart.
//   - me2_me1_import_no_restart: no list; Char_Prep from one save and the same command.
//   - neither: the Xbox's way with a restart (Char_Prep, launch data in a file, the next start imports).
//
// Saves taken from the folder:
//   - <name>.MassEffectSave: the PC version's save, 0x2028-byte header + ZIP (player.sav, state.sav, ...). Its
//     player.sav has the same header in front of a little-endian package whose offsets count from the start of the
//     file: the header goes and the offsets move with it (RebasePcPackage), then every value is put in the Xbox's
//     byte order (ToXbox: ME2's Xbox code reads these files raw), state.sav too.
//   - a folder holding player.sav (an Xbox 360 save's files, e.g. extracted from its STFS package): copied as it is.
//
// me2_me1_import (default on), me2_me1_import_dir ("" = <NRO folder>/ME1), me2_me1_import_save. "[me1-import]" lines.
#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/hook.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <ctime>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

REXCVAR_DEFINE_BOOL(me2_me1_import, true, "ME2",
                    "New Game -> Import ME1 Character: the port prepares the character itself (no Unreap.xex)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(me2_me1_import_dir, "", "ME2",
                      "Folder with the ME1 saves to import (PC .MassEffectSave files or Xbox save folders); \"\" = "
                      "the folder ME1 next to the executable")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(me2_me1_import_save, "", "ME2",
                      "The ME1 save to import (its file or folder name in me2_me1_import_dir); \"\" = the newest, "
                      "Char_* saves first")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// 2026-11-02 PC test: the load screen opened empty. The Xbox build keeps the PC version's ME1 list display but not the
// code that filled it (from ME1's Documents folder): InitializeLoadList (sub_8294D838) leaves in mode 1 before its
// display. This file fills the list (see "the ME1 list" below) and walks the function to the display.
REXCVAR_DEFINE_BOOL(me2_me1_import_load_screen, true, "ME2",
                    "Import ME1 Character opens the game's own load screen with the ME1 saves of me2_me1_import_dir "
                    "(the PC version's route: pick one, the new game starts, no restart)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_me1_import_no_restart, true, "ME2",
                    "Import ME1 Character imports in place: Char_Prep is prepared and \"defer importcharacter Char_Prep "
                    "1 forcenewgame\" runs (the PC version's command) instead of Unreap.xex and a restart")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" bool me2_name_text(const uint8_t* base, uint32_t index, char* out, size_t size);  // me2_cerberus.cpp

namespace me2::me1_import {
namespace fs = std::filesystem;
using rex::X_STATUS;  // X_STATUS_SUCCESS needs it in scope
using rex::system::xam::XCONTENT_AGGREGATE_DATA;
using rex::system::XContentType;

namespace {
constexpr uint32_t kImporterTitle = 0x4D5308C5;
constexpr const char* kPrepName = "Char_Prep";
constexpr const char* kPcMarker = "me2_pc_byte_order";  // in a prepared saved game: state.sav is little-endian
constexpr const char* kSourceMarker = "me2_me1_source";   // in a prepared saved game: the ME1 save it was made from
constexpr const char* kLaunchFile = "me1_import_launch.txt";
constexpr uint32_t kDevice = 1;  // the SDK's hard drive (DummyDeviceId::HDD)

rex::system::xam::ContentManager* Content() {
  auto* kernel = REX_KERNEL_STATE();
  return kernel ? kernel->content_manager() : nullptr;
}

const fs::path& SavedGameRoot(rex::system::xam::ContentManager* c) {
  return c->saved_game_root().empty() ? c->root_path() : c->saved_game_root();
}

uint64_t Xuid() {
  auto* kernel = REX_KERNEL_STATE();
  return kernel && kernel->user_profile() ? kernel->user_profile()->xuid() : 0;
}

std::string Lower(std::string s) {
  for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

bool ReadAll(const fs::path& path, std::vector<uint8_t>& out) {
  FILE* f = rex::filesystem::OpenFile(path, "rb");
  if (!f) return false;
  out.clear();
  uint8_t buf[64 * 1024];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  const bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

bool WriteAll(const fs::path& path, const void* data, size_t size) {
  FILE* f = rex::filesystem::OpenFile(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, size, f) == size;
  return std::fclose(f) == 0 && ok;
}

uint16_t Le16(const std::vector<uint8_t>& d, size_t o) { return uint16_t(d[o] | d[o + 1] << 8); }
uint32_t Le32(const std::vector<uint8_t>& d, size_t o) {
  return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24;
}
void PutLe32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
  for (int i = 0; i < 4; ++i) d[o + i] = uint8_t(v >> (8 * i));
}

struct File {
  std::string name;
  std::vector<uint8_t> data;
};

// The files of a ZIP (stored or deflated), from its central directory.
bool Unzip(const std::vector<uint8_t>& d, size_t zip_start, std::vector<File>& out, std::string& why) {
  size_t eocd = std::string::npos;
  if (d.size() >= zip_start + 22) {
    for (size_t o = d.size() - 22;; --o) {
      if (Le32(d, o) == 0x06054B50u) {
        eocd = o;
        break;
      }
      if (o == zip_start) break;
    }
  }
  if (eocd == std::string::npos) return why = "no ZIP end record", false;
  const uint32_t count = Le16(d, eocd + 10);
  // Offsets in the directory count from the ZIP's first byte (the header in front of it is not part of it).
  size_t cd = zip_start + Le32(d, eocd + 16);
  for (uint32_t i = 0; i < count; ++i) {
    if (cd + 46 > d.size() || Le32(d, cd) != 0x02014B50u) return why = "broken ZIP directory", false;
    const uint16_t method = Le16(d, cd + 10);
    const uint32_t csize = Le32(d, cd + 20), usize = Le32(d, cd + 24);
    const uint16_t nlen = Le16(d, cd + 28), xlen = Le16(d, cd + 30), clen = Le16(d, cd + 32);
    const size_t local = zip_start + Le32(d, cd + 42);
    File f;
    f.name.assign(reinterpret_cast<const char*>(&d[cd + 46]), nlen);
    cd += 46 + nlen + xlen + clen;
    if (local + 30 > d.size() || Le32(d, local) != 0x04034B50u) return why = "broken ZIP entry " + f.name, false;
    const size_t data = local + 30 + Le16(d, local + 26) + Le16(d, local + 28);
    if (data + csize > d.size() || usize > 64u * 1024 * 1024) return why = "ZIP entry out of range " + f.name, false;
    f.data.resize(usize);
    if (method == 0) {
      if (csize != usize) return why = "stored ZIP entry with two sizes " + f.name, false;
      std::memcpy(f.data.data(), &d[data], usize);
    } else if (method == 8) {
      const int got = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(f.data.data()), int(usize),
                                                       reinterpret_cast<const char*>(&d[data]), int(csize));
      if (got != int(usize)) return why = "inflate failed for " + f.name, false;
    } else {
      return why = fmt::format("ZIP method {} for {}", method, f.name), false;
    }
    const size_t slash = f.name.find_last_of("/\\");
    if (slash != std::string::npos) f.name = f.name.substr(slash + 1);
    if (!f.name.empty()) out.push_back(std::move(f));
  }
  return true;
}

// A PC package with a header in front: the header goes, every offset into the file moves with it. Layout of ME1's
// version 491 / licensee 1008 summary and export entries (checked on two PC saves: the exports' data then tiles the
// file to its end). Leaves 'd' alone and returns false when it is not that.
bool RebasePcPackage(std::vector<uint8_t>& d, std::string& why) {
  if (d.size() >= 4 && Le32(d, 0) == 0x9E2A83C1u) return true;  // already starts with the package
  size_t b = std::string::npos;
  for (size_t o = 0; o + 4 <= std::min<size_t>(d.size(), 0x10000); ++o)
    if (Le32(d, o) == 0x9E2A83C1u) {
      b = o;
      break;
    }
  if (b == std::string::npos) return why = "no little-endian package tag", false;
  auto need = [&](size_t o, size_t n) { return o + n <= d.size(); };
  size_t o = b + 12;  // tag, versions, total header size
  if (!need(o, 4)) return why = "short summary", false;
  const int32_t folder = int32_t(Le32(d, o));
  o += 4 + (folder < 0 ? size_t(-folder) * 2 : size_t(folder));
  o += 4;  // package flags
  if (!need(o, 28)) return why = "short summary", false;
  const size_t f_names = o + 4, f_exports = o + 12, f_imports = o + 20, f_depends = o + 24;
  const uint32_t export_count = Le32(d, o + 8);
  const uint32_t base = uint32_t(b);
  for (size_t f : {size_t(b + 8), f_names, f_exports, f_imports, f_depends})
    if (Le32(d, f) < base || Le32(d, f) > d.size()) return why = "summary offset outside the file", false;
  // Export entries: class, super, outer, name, number, archetype (6 x 4), flags (8), size, offset, component map
  // (count + 12 each), export flags, generation net object counts (count + 4 each), GUID (16), one more int (ME1).
  size_t e = Le32(d, f_exports);
  std::vector<size_t> offsets;
  for (uint32_t i = 0; i < export_count; ++i) {
    if (!need(e, 48)) return why = "export table runs past the file", false;
    const size_t f_off = e + 36;
    e += 40;
    e += 4 + size_t(Le32(d, e)) * 12;
    if (!need(e, 8)) return why = "export table runs past the file", false;
    e += 4;
    e += 4 + size_t(Le32(d, e)) * 4;
    e += 20;
    if (Le32(d, f_off) < base || Le32(d, f_off) > d.size()) return why = "export data outside the file", false;
    offsets.push_back(f_off);
  }
  if (e != Le32(d, f_depends)) return why = fmt::format("export table ends at {:X}, not at the depends table", e), false;
  for (size_t f : {size_t(b + 8), f_names, f_exports, f_imports, f_depends}) PutLe32(d, f, Le32(d, f) - base);
  for (size_t f : offsets) PutLe32(d, f, Le32(d, f) - base);
  d.erase(d.begin(), d.begin() + b);
  return true;
}

// ME1 PC -> Xbox 360 byte order (2026-11-02). ME2's Xbox code reads the ME1 files with plain raw reads (Serialize(&x,
// 4) through archive vtable +4; the archive's byte-swap flag is honoured only by the package summary parser), so a
// little-endian PC file has to arrive big-endian, value by value. The layouts (checked on two PC saves: every byte of
// every object accounted for, and the result walked again as big-endian converts back to the original):
//   player.sav (after RebasePcPackage), ME1's package version 491 / 1008: summary (tag, versions, header size, folder
//   FString, flags, then words up to the name table), names (FString + 8-byte flags), imports (7 words), exports (6
//   words, 8-byte object flags, size, offset, component map count + 3 words each, export flags, generation count +
//   words, GUID + one more word), depends (words up to the header's end), then each object: NetIndex and tagged
//   properties (name, type, size, index; struct name; BoolProperty's value in the tag) up to "None". Values: words for
//   int / float / object / string ref / name ..., one byte for ByteProperty, FString, structs tagged or binary words
//   (Vector, Rotator), arrays of words, bytes, tagged structs or FStrings.
//   state.sav: 0, version, word (the PC header; the Xbox form is -1, version, word), the map name FString, then words
//   only (5716 bytes in both test saves: location, rotation, plot arrays and maps of ints).
class ToXbox {
 public:
  explicit ToXbox(std::vector<uint8_t>& data) : out_(data), src_(data) {}

  bool Package(std::string& why) {
    if (src_.size() < 32 || I(0) != int32_t(0x9E2A83C1u)) return Fail(why, "no little-endian package tag");
    W(0, 3);
    size_t o = Str(12);
    if (!o) return Fail(why, "bad folder name");
    W(o, 1);
    o += 4;
    if (o + 28 > src_.size()) return Fail(why, "short summary");
    const int32_t nc = I(o), no = I(o + 4), ec = I(o + 8), eo = I(o + 12), ic = I(o + 16), io = I(o + 20),
                  dep = I(o + 24), total = I(8);
    if (nc < 0 || ec < 0 || ic < 0 || no < int32_t(o) || (no - int32_t(o)) % 4 || total > int32_t(src_.size()) ||
        dep > total || dep < 0)
      return Fail(why, "summary offsets");
    W(o, (size_t(no) - o) / 4);
    size_t p = size_t(no);
    for (int32_t k = 0; k < nc; ++k) {
      const int32_t n = I(p);
      std::string text;
      if (n > 0 && p + 4 + size_t(n) <= src_.size())
        text.assign(reinterpret_cast<const char*>(&src_[p + 4]), size_t(n) - 1);
      names_.push_back(text);
      p = Str(p);
      if (!p || p + 8 > src_.size()) return Fail(why, "name table");
      Q(p);
      p += 8;
    }
    if (p != size_t(io)) return Fail(why, "name table end");
    W(p, size_t(7) * size_t(ic));
    p += size_t(28) * size_t(ic);
    if (p != size_t(eo)) return Fail(why, "import table end");
    std::vector<std::pair<size_t, size_t>> objects;
    for (int32_t k = 0; k < ec; ++k) {
      if (p + 48 > src_.size()) return Fail(why, "export table");
      W(p, 6);
      Q(p + 24);
      W(p + 32, 2);
      objects.emplace_back(size_t(uint32_t(I(p + 36))), size_t(uint32_t(I(p + 32))));
      p += 40;
      const int32_t cm = I(p);
      if (cm < 0 || cm > 100000) return Fail(why, "export component map");
      W(p, 1 + size_t(3) * size_t(cm));
      p += 4 + size_t(12) * size_t(cm);
      W(p, 1);
      p += 4;
      const int32_t gn = I(p);
      if (gn < 0 || gn > 100000) return Fail(why, "export generations");
      W(p, 1 + size_t(gn));
      p += 4 + size_t(4) * size_t(gn);
      W(p, 5);
      p += 20;
    }
    if (p != size_t(dep) || (total - dep) % 4) return Fail(why, "export table end / depends");
    W(p, size_t(total - dep) / 4);
    for (size_t k = 0; k < names_.size(); ++k)
      if (names_[k] == "None") none_ = int32_t(k);
    if (none_ < 0) return Fail(why, "no name None");
    for (const auto& [off, size] : objects) {
      if (off + size > src_.size() || size < 4) return Fail(why, "object outside the file");
      W(off, 1);  // NetIndex
      if (Props(off + 4, off + size, true) != off + size)
        return Fail(why, fmt::format("object at {:X} ({} bytes): its properties do not parse", off, size));
    }
    return true;
  }

  bool State(std::string& why) {
    if (src_.size() < 16 || I(0) != 0 || I(4) < 0 || I(4) > 35) return Fail(why, "not the PC state.sav header");
    out_[0] = out_[1] = out_[2] = out_[3] = 0xFF;  // -1
    W(4, 2);
    const size_t o = Str(12);
    if (!o || (src_.size() - o) % 4) return Fail(why, "state.sav after the map name is not words");
    W(o, (src_.size() - o) / 4);
    return true;
  }

 private:
  std::vector<uint8_t>& out_;
  const std::vector<uint8_t> src_;  // the little-endian original (copied before out_ changes)
  std::vector<std::string> names_;
  int32_t none_ = -1;

  static bool Fail(std::string& why, const std::string& text) {
    why = text;
    return false;
  }
  int32_t I(size_t o) const { return o + 4 <= src_.size() ? int32_t(Le32(src_, o)) : 0; }
  void W(size_t o, size_t n) {
    for (size_t k = 0; k < n && o + 4 * k + 4 <= src_.size(); ++k)
      for (int b = 0; b < 4; ++b) out_[o + 4 * k + b] = src_[o + 4 * k + 3 - b];
  }
  void Q(size_t o) {
    if (o + 8 > src_.size()) return;
    for (int b = 0; b < 8; ++b) out_[o + b] = src_[o + 7 - b];
  }
  // An FString at o (length word, then 1-byte or, for a negative length, 2-byte characters); its end, 0 if broken.
  size_t Str(size_t o) {
    if (o + 4 > src_.size()) return 0;
    const int32_t n = I(o);
    W(o, 1);
    o += 4;
    if (n < 0) {
      if (n < -0x100000 || o + size_t(-n) * 2 > src_.size()) return 0;
      for (int32_t k = 0; k < -n; ++k) {
        out_[o + 2 * k] = src_[o + 2 * k + 1];
        out_[o + 2 * k + 1] = src_[o + 2 * k];
      }
      return o + size_t(-n) * 2;
    }
    if (o + size_t(n) > src_.size()) return 0;
    return o + size_t(n);
  }
  bool NameAt(size_t p, size_t end) const {
    if (p + 8 > end) return false;
    const int32_t a = I(p), b = I(p + 4);
    return a >= 0 && size_t(a) < names_.size() && b >= 0 && b < 100000;
  }
  // Tagged properties from p to "None"; converted when 'write'. The end, or 0 when they do not parse.
  size_t Props(size_t p, size_t end, bool write) {
    for (;;) {
      if (!NameAt(p, end)) return 0;
      const int32_t name = I(p);
      if (write) W(p, 2);
      p += 8;
      if (name == none_) return p;
      if (!NameAt(p, end) || p + 16 > end) return 0;
      const std::string& type = names_[size_t(I(p))];
      const int32_t size = I(p + 8);
      if (write) W(p, 4);
      p += 16;
      if (type == "StructProperty") {
        if (!NameAt(p, end)) return 0;
        if (write) W(p, 2);
        p += 8;
      }
      if (type == "BoolProperty") {
        if (p + 4 > end) return 0;
        if (write) W(p, 1);
        p += 4;
        continue;
      }
      if (size < 0 || p + size_t(size) > end) return 0;
      if (!Value(type, p, size_t(size), write)) return 0;
      p += size_t(size);
    }
  }
  bool Value(const std::string& t, size_t p, size_t size, bool write) {
    if (t == "IntProperty" || t == "FloatProperty" || t == "ObjectProperty" || t == "StringRefProperty" ||
        t == "ClassProperty" || t == "ComponentProperty" || t == "InterfaceProperty" || t == "DelegateProperty" ||
        t == "NameProperty") {
      if (size % 4) return false;
      if (write) W(p, size / 4);
      return true;
    }
    if (t == "ByteProperty") {
      if (size == 8 && write) W(p, 2);
      return size == 1 || size == 8;
    }
    if (t == "StrProperty") return StrCheck(p, p + size) && (!write || Str(p) == p + size);
    if (t == "StructProperty") {
      if (Props(p, p + size, false) == p + size) return !write || Props(p, p + size, true) == p + size;
      if (size % 4) return false;
      if (write) W(p, size / 4);  // a binary struct of words (Vector, Rotator)
      return true;
    }
    if (t == "ArrayProperty") {
      if (size < 4) return false;
      const int32_t n = I(p);
      const size_t body = size - 4, q = p + 4, end = p + size;
      if (n < 0) return false;
      if (write) W(p, 1);
      if (n == 0) return body == 0;
      if (body == size_t(n)) return true;  // bytes
      size_t r = q;
      for (int32_t k = 0; k < n && r; ++k) r = Props(r, end, false);
      if (r == end) {
        if (!write) return true;
        r = q;
        for (int32_t k = 0; k < n && r; ++k) r = Props(r, end, true);
        return r == end;
      }
      if (body != size_t(n) * 4) {  // FStrings (when the elements are not plainly words)
        r = q;
        bool ok = true;
        for (int32_t k = 0; k < n && ok; ++k) {
          if (r + 4 > end) {
            ok = false;
            break;
          }
          const int32_t len = I(r);
          r += 4 + (len >= 0 ? size_t(len) : size_t(-int64_t(len)) * 2);
          ok = r <= end;
        }
        if (ok && r == end) {
          if (!write) return true;
          r = q;
          for (int32_t k = 0; k < n && r; ++k) r = Str(r);
          return r == end;
        }
      }
      if (body % 4 == 0 && (body == size_t(n) * 4 || body == size_t(n) * 8 || body == size_t(n) * 12 ||
                            body == size_t(n) * 16)) {
        if (write) W(q, body / 4);
        return true;
      }
      return false;
    }
    return false;
  }
  // Whether an FString at o ends exactly at 'end' (no writing).
  bool StrCheck(size_t o, size_t end) const {
    if (o + 4 > end) return false;
    const int32_t n = I(o);
    const size_t len = n >= 0 ? size_t(n) : size_t(-int64_t(n)) * 2;
    return o + 4 + len == end;
  }
};

// Xbox ME1 export entries (ME2's reader sub_8297D838: class, super, outer, name, archetype, 8-byte flags, size,
// offset, component map, export flags, generation counts, GUID) end at the GUID; ME1 PC's have one more word (package
// flags), which shifted every later entry for ME2 (2026-11-02 PC run: a read far outside the file). On a package
// already in the Xbox's byte order: that word goes from each entry, everything after the export table moves forward,
// and the header size, the depends offset and every object's offset move with it.
bool DropExportPackageFlags(std::vector<uint8_t>& d, std::string& why) {
  auto be = [&](size_t o) -> int32_t {
    return o + 4 <= d.size() ? int32_t(uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3])
                             : 0;
  };
  auto put = [&](std::vector<uint8_t>& v, size_t o, int32_t x) {
    for (int b = 0; b < 4; ++b) v[o + b] = uint8_t(uint32_t(x) >> (24 - 8 * b));
  };
  const int32_t n = be(12);
  if (n < 0 || n > 1024) return why = "folder name", false;
  const size_t o = 16 + size_t(n) + 4;
  const int32_t ec = be(o + 8), eo = be(o + 12);
  const size_t f_dep = o + 24;
  if (ec < 0 || eo < 0 || size_t(eo) > d.size()) return why = "export table", false;
  std::vector<uint8_t> out(d.begin(), d.begin() + eo);
  std::vector<size_t> entries;
  size_t p = size_t(eo);
  for (int32_t k = 0; k < ec; ++k) {
    const size_t start = p;
    p += 40;
    const int32_t cm = be(p);
    if (cm < 0 || cm > 100000) return why = "export component map", false;
    p += 4 + size_t(12) * size_t(cm) + 4;
    const int32_t gn = be(p);
    if (gn < 0 || gn > 100000) return why = "export generations", false;
    p += 4 + size_t(4) * size_t(gn) + 16;
    if (p + 4 > d.size()) return why = "export table runs past the file", false;
    entries.push_back(out.size());
    out.insert(out.end(), d.begin() + start, d.begin() + p);
    p += 4;  // package flags, dropped
  }
  out.insert(out.end(), d.begin() + p, d.end());
  const int32_t shift = 4 * ec;
  put(out, 8, be(8) - shift);
  put(out, f_dep, be(f_dep) - shift);
  for (size_t e : entries) {
    const int32_t off = int32_t(uint32_t(out[e + 36]) << 24 | uint32_t(out[e + 37]) << 16 | uint32_t(out[e + 38]) << 8 |
                                out[e + 39]);
    put(out, e + 36, off - shift);
  }
  d.swap(out);
  return true;
}

// Enum values. ME1 PC (package version 491) stores a ByteProperty with an enum as the value's FName (8 bytes); the
// Xbox ME1 stores the byte, and ME2's property code reads one byte for a ByteProperty its class knows (2026-11-02 PC
// trace: after m_SpectreRank the reader was 7 bytes behind; reading at version 491 did not change that). So on the
// big-endian package (Xbox export layout) each such value becomes its index: the name looked up in ME2's own enums
// (me2_me1_enums.inc, from SFXGame), "GlobalEnum" with number N = byte N - 1 (a byte without an enum). Values whose
// name is not there stay names (8 bytes): their properties are ones ME2's classes do not have, which it skips by size.
// Values shrink, so every enclosing struct / array size, every object's size and offset are rebuilt.
struct EnumEntry {
  const char* name;
  int value;
};
constexpr EnumEntry kEnumValues[] = {
#include "me2_me1_enums.inc"
};

int EnumIndex(const std::string& name) {
  size_t lo = 0, hi = sizeof(kEnumValues) / sizeof(kEnumValues[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = std::strcmp(kEnumValues[mid].name, name.c_str());
    if (c == 0) return kEnumValues[mid].value;
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return -1;
}

class EnumBytes {
 public:
  explicit EnumBytes(std::vector<uint8_t>& d) : d_(d) {}

  bool Run(std::string& why) {
    const int32_t n = I(12);
    if (n < 0 || n > 1024) return why = "folder name", false;
    const size_t o = 16 + size_t(n) + 4;
    const int32_t nc = I(o), no = I(o + 4), ec = I(o + 8), eo = I(o + 12);
    if (nc < 0 || ec < 0 || no < 0 || eo < 0) return why = "summary", false;
    size_t q = size_t(no);
    for (int32_t k = 0; k < nc; ++k) {
      const int32_t len = I(q);
      if (len < 0 || q + 4 + size_t(len) + 8 > d_.size()) return why = "name table", false;
      names_.emplace_back(reinterpret_cast<const char*>(&d_[q + 4]), len ? size_t(len) - 1 : 0);
      q += 4 + size_t(len) + 8;
    }
    for (size_t k = 0; k < names_.size(); ++k)
      if (names_[k] == "None") none_ = int32_t(k);
    if (none_ < 0) return why = "no name None", false;
    struct Obj {
      size_t off, size, entry;
    };
    std::vector<Obj> objs;
    size_t p = size_t(eo);
    for (int32_t k = 0; k < ec; ++k) {
      if (p + 60 > d_.size()) return why = "export table", false;
      objs.push_back({size_t(uint32_t(I(p + 36))), size_t(uint32_t(I(p + 32))), p});
      p += 40;
      const int32_t cm = I(p);
      if (cm < 0 || cm > 100000) return why = "export component map", false;
      p += 4 + size_t(12) * size_t(cm) + 4;
      const int32_t gn = I(p);
      if (gn < 0 || gn > 100000) return why = "export generations", false;
      p += 4 + size_t(4) * size_t(gn) + 16;
    }
    if (objs.empty()) return true;
    std::sort(objs.begin(), objs.end(), [](const Obj& a, const Obj& b) { return a.off < b.off; });
    std::vector<uint8_t> out(d_.begin(), d_.begin() + objs.front().off);
    for (const Obj& ob : objs) {
      if (ob.off + ob.size > d_.size() || ob.size < 4) return why = "object outside the file", false;
      std::vector<uint8_t> body(d_.begin() + ob.off, d_.begin() + ob.off + 4);  // NetIndex
      if (Props(ob.off + 4, ob.off + ob.size, body) != ob.off + ob.size)
        return why = fmt::format("object at {:X}: its properties do not parse", ob.off), false;
      Put(out, ob.entry + 32, uint32_t(body.size()));
      Put(out, ob.entry + 36, uint32_t(out.size()));
      out.insert(out.end(), body.begin(), body.end());
    }
    d_.swap(out);
    return true;
  }

  int fixed = 0, kept = 0;

 private:
  std::vector<uint8_t>& d_;
  std::vector<std::string> names_;
  int32_t none_ = -1;

  int32_t I(size_t o) const {
    return o + 4 <= d_.size() ? int32_t(uint32_t(d_[o]) << 24 | uint32_t(d_[o + 1]) << 16 | uint32_t(d_[o + 2]) << 8 | d_[o + 3])
                              : 0;
  }
  static void Put(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    for (int b = 0; b < 4; ++b) v[o + b] = uint8_t(x >> (24 - 8 * b));
  }
  bool NameAt(size_t p, size_t end) const {
    if (p + 8 > end) return false;
    const int32_t a = I(p), b = I(p + 4);
    return a >= 0 && size_t(a) < names_.size() && b >= 0 && b < 100000;
  }
  void Copy(std::vector<uint8_t>& out, size_t from, size_t to) const {
    out.insert(out.end(), d_.begin() + from, d_.begin() + to);
  }
  // Tagged properties at p (to "None") appended to 'out', rewritten; their end in d_, 0 if they do not parse.
  size_t Props(size_t p, size_t end, std::vector<uint8_t>& out) {
    for (;;) {
      if (!NameAt(p, end)) return 0;
      if (I(p) == none_) {
        Copy(out, p, p + 8);
        return p + 8;
      }
      if (!NameAt(p + 8, end) || p + 24 > end) return 0;
      const std::string& type = names_[size_t(I(p + 8))];
      const int32_t size = I(p + 16);
      size_t q = p + 24;
      if (type == "StructProperty") {
        if (!NameAt(q, end)) return 0;
        q += 8;
      }
      const size_t tag = out.size();
      Copy(out, p, q);
      if (type == "BoolProperty") {
        if (q + 4 > end) return 0;
        Copy(out, q, q + 4);
        p = q + 4;
        continue;
      }
      if (size < 0 || q + size_t(size) > end) return 0;
      const size_t vend = q + size_t(size);
      bool done = false;
      if (type == "ByteProperty" && size == 8 && NameAt(q, vend)) {
        const std::string& v = names_[size_t(I(q))];
        const int32_t num = I(q + 4);
        const int value = v == "GlobalEnum" && num > 0 ? num - 1 : (num == 0 ? EnumIndex(v) : -1);
        if (value >= 0 && value < 256) {
          out.push_back(uint8_t(value));
          ++fixed;
          done = true;
        } else {
          ++kept;
        }
      } else if (type == "StructProperty") {
        std::vector<uint8_t> inner;
        if (Props(q, vend, inner) == vend) {
          out.insert(out.end(), inner.begin(), inner.end());
          done = true;
        }
      } else if (type == "ArrayProperty" && size >= 4) {
        const int32_t cnt = I(q);
        if (cnt > 0) {
          std::vector<uint8_t> inner(d_.begin() + q, d_.begin() + q + 4);
          size_t r = q + 4;
          for (int32_t k = 0; k < cnt && r; ++k) r = Props(r, vend, inner);
          if (r == vend) {
            out.insert(out.end(), inner.begin(), inner.end());
            done = true;
          }
        }
      }
      if (!done) Copy(out, q, vend);
      Put(out, tag + 16, uint32_t(out.size() - (tag + (q - p))));  // the value's new size
      p = vend;
    }
  }
};

bool EnumsToBytes(std::vector<uint8_t>& d, std::string& why) {
  EnumBytes e(d);
  if (!e.Run(why)) return false;
  REXLOG_INFO("[me1-import] player.sav: {} enum value(s) made bytes, {} left as names", e.fixed, e.kept);
  return true;
}

struct Candidate {
  fs::path path;
  std::string name;
  bool folder = false;
  uint64_t time = 0;
  bool package = false;  // an Xbox 360 save package (STFS "CON " file), read through the SDK's STFS device
  std::string display;   // its display name (the Xbox's), for the list
};

constexpr uint32_t kMe1Title = 0x4D5307E8;  // Mass Effect (Xbox 360)

// An Xbox 360 saved-game package (2026-11-02, user: Xbox ME1 saves too): its header via the SDK's STFS reader
// (StfsContainerDevice::ReadPackageHeader checks the magic: CON / LIVE / PIRS). Saved games only (content type 1);
// another title than ME1's is taken with a warning (the contents decide).
bool XboxPackage(const fs::path& path, std::string& display) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size < sizeof(rex::filesystem::StfsHeader)) return false;
  FILE* f = rex::filesystem::OpenFile(path, "rb");
  if (!f) return false;
  char magic[4] = {};
  const bool read = std::fread(magic, 1, 4, f) == 4;
  std::fclose(f);
  if (!read || (std::memcmp(magic, "CON ", 4) && std::memcmp(magic, "LIVE", 4) && std::memcmp(magic, "PIRS", 4)))
    return false;
  const auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(path);
  if (!header) return false;
  const uint32_t type = uint32_t(header->metadata.content_type.get());
  const uint32_t title = header->metadata.execution_info.title_id;
  if (type != 1) {
    REXLOG_WARN("[me1-import] {}: an Xbox package, but content type {:08X}, not a saved game", rex::path_to_utf8(path), type);
    return false;
  }
  if (title != kMe1Title)
    REXLOG_WARN("[me1-import] {}: a saved game of title {:08X}, not ME1's {:08X}; tried anyway", rex::path_to_utf8(path),
                title, kMe1Title);
  display = rex::string::to_utf8(header->metadata.display_name(rex::system::XLanguage::kEnglish));
  return true;
}

// The files of an Xbox save package (all of them, any folder level, by their names).
bool ReadPackage(const fs::path& path, std::vector<File>& files, std::string& why) {
  rex::filesystem::StfsContainerDevice device("ME1Import:", path);  // not registered: read here only
  if (!device.Initialize()) return why = "the package does not open (damaged?)", false;
  std::vector<rex::filesystem::Entry*> todo;
  if (auto* root = device.ResolvePath("")) todo.push_back(root);
  while (!todo.empty()) {
    rex::filesystem::Entry* e = todo.back();
    todo.pop_back();
    for (const auto& child : e->children()) todo.push_back(child.get());
    if (e->attributes() & rex::filesystem::kFileAttributeDirectory) continue;
    rex::filesystem::File* in = nullptr;
    if (e->Open(rex::filesystem::FileAccess::kFileReadData, &in) != X_STATUS_SUCCESS || !in)
      return why = "cannot read " + e->name(), false;
    File out{e->name(), std::vector<uint8_t>(e->size())};
    size_t got = 0;
    const bool ok = out.data.empty() || (in->ReadSync(std::span<uint8_t>(out.data.data(), out.data.size()), 0, &got) ==
                                             X_STATUS_SUCCESS && got == out.data.size());
    in->Destroy();
    if (!ok) return why = "cannot read " + e->name(), false;
    files.push_back(std::move(out));
  }
  return true;
}

std::vector<Candidate> FindSaves(const fs::path& dir) {
  std::vector<Candidate> list;
  for (const auto& info : rex::filesystem::ListFiles(dir)) {
    Candidate c{dir / info.name, rex::path_to_utf8(info.name), false, info.write_timestamp};
    if (info.type == rex::filesystem::FileInfo::Type::kDirectory) {
      std::error_code ec;
      if (!fs::exists(c.path / "player.sav", ec)) continue;
      c.folder = true;
    } else if (Lower(c.name).size() < 15 || Lower(c.name).rfind(".masseffectsave") != Lower(c.name).size() - 15) {
      if (!XboxPackage(c.path, c.display)) continue;
      c.package = true;
    }
    list.push_back(std::move(c));
  }
  std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
    const bool ca = Lower(a.name).rfind("char_", 0) == 0, cb = Lower(b.name).rfind("char_", 0) == 0;
    if (ca != cb) return ca;
    if (a.time != b.time) return a.time > b.time;
    return a.name < b.name;
  });
  return list;
}

// The chosen save's files as Char_Prep wants them; 'pc' = state.sav is little-endian.
bool LoadSave(const Candidate& c, std::vector<File>& files, bool& pc, std::string& why) {
  pc = false;
  if (c.folder) {
    for (const auto& info : rex::filesystem::ListFiles(c.path)) {
      if (info.type != rex::filesystem::FileInfo::Type::kFile) continue;
      File f{rex::path_to_utf8(info.name), {}};
      if (!ReadAll(c.path / info.name, f.data)) return why = "cannot read " + f.name, false;
      files.push_back(std::move(f));
    }
  } else if (c.package) {
    if (!ReadPackage(c.path, files, why)) return false;
  } else {
    std::vector<uint8_t> d;
    if (!ReadAll(c.path, d)) return why = "cannot read the file", false;
    size_t zip = std::string::npos;
    for (size_t o = 0; o + 4 <= d.size(); ++o)
      if (Le32(d, o) == 0x04034B50u) {
        zip = o;
        break;
      }
    if (zip == std::string::npos) return why = "no ZIP inside (not a PC .MassEffectSave?)", false;
    if (!Unzip(d, zip, files, why)) return false;
    pc = true;
  }
  bool player = false, state = false;
  for (File& f : files) {
    const std::string n = Lower(f.name);
    player |= n == "player.sav";
    state |= n == "state.sav";
    if (!pc) continue;
    std::string w;
    if (n == "state.sav") {
      if (!ToXbox(f.data).State(w)) return why = "state.sav: " + w, false;
    } else if (n.size() > 4 && n.substr(n.size() - 4) == ".sav") {
      // Packages with the PC header (player.sav, WorldSavePackage.sav): the game reads them from their first byte, in
      // the Xbox's byte order.
      const bool ok = RebasePcPackage(f.data, w) && ToXbox(f.data).Package(w) && DropExportPackageFlags(f.data, w) &&
                      EnumsToBytes(f.data, w);
      if (!ok) {
        if (n == "player.sav") return why = "player.sav: " + w, false;
        REXLOG_WARN("[me1-import] {}: {}; left out", f.name, w);
        f.data.clear();
      }
    }
  }
  files.erase(std::remove_if(files.begin(), files.end(), [](const File& f) { return f.data.empty(); }), files.end());
  if (!player || !state) return why = "player.sav or state.sav missing", false;
  return true;
}

fs::path SavesFolder(rex::system::xam::ContentManager* c, uint64_t xuid) {
  return SavedGameRoot(c) / fmt::format("{:016X}", xuid) / fmt::format("{:08X}", kImporterTitle) / "00000001";
}

fs::path HeaderFile(rex::system::xam::ContentManager* c, uint64_t xuid, const std::string& name) {
  return SavedGameRoot(c) / fmt::format("{:016X}", xuid) / fmt::format("{:08X}", kImporterTitle) / "Headers" /
         "00000001" / (name + ".header");
}

fs::path ImportDir() {
  const std::string dir_text = REXCVAR_GET(me2_me1_import_dir);
  return dir_text.empty() ? rex::filesystem::GetExecutableFolder() / "ME1" : fs::path(dir_text);
}

// One saved game of the importer's title (4D5308C5) holding an ME1 save's files, as Unreap.xex leaves Char_Prep.
// 'source' (the save's name in the import folder) goes into a marker file: the folders this code made are found again.
bool WriteContent(rex::system::xam::ContentManager* content, uint64_t xuid, const std::string& name,
                  const std::string& display, const std::vector<File>& files, bool pc, const std::string& source) {
  const fs::path folder = SavesFolder(content, xuid) / name;
  std::error_code ec;
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  for (const File& f : files)
    if (!WriteAll(folder / f.name, f.data.data(), f.data.size())) {
      REXLOG_ERROR("[me1-import] cannot write {}", rex::path_to_utf8(folder / f.name));
      return false;
    }
  if (pc && !WriteAll(folder / kPcMarker, "1", 1)) return false;
  if (!WriteAll(folder / kSourceMarker, source.data(), source.size())) return false;
  XCONTENT_AGGREGATE_DATA data;
  data.device_id = kDevice;
  data.content_type = XContentType::kSavedGame;
  data.set_display_name(rex::string::to_utf16(display));
  data.set_file_name(name);
  data.xuid = xuid;
  data.title_id = kImporterTitle;
  content->WriteContentHeaderFile(xuid, data);
  return true;
}

bool PrepareCharacter(std::string& command_line) {
  auto* content = Content();
  if (!content) return false;
  const fs::path dir = ImportDir();
  auto list = FindSaves(dir);
  REXLOG_INFO("[me1-import] {} ME1 save(s) in {}", list.size(), rex::path_to_utf8(dir));
  for (const auto& c : list) REXLOG_INFO("[me1-import]   {}{}", c.name, c.folder ? " (folder)" : "");
  const std::string wanted = REXCVAR_GET(me2_me1_import_save);
  const Candidate* pick = nullptr;
  for (const auto& c : list)
    if (wanted.empty() || Lower(c.name) == Lower(wanted)) {
      pick = &c;
      break;
    }
  if (!pick) {
    REXLOG_ERROR("[me1-import] no ME1 save to import{} (put .MassEffectSave files or Xbox save folders in {})",
                 wanted.empty() ? "" : " named '" + wanted + "'", rex::path_to_utf8(dir));
    return false;
  }
  std::vector<File> files;
  bool pc = false;
  std::string why;
  if (!LoadSave(*pick, files, pc, why)) {
    REXLOG_ERROR("[me1-import] {}: {}", pick->name, why);
    return false;
  }
  const uint64_t xuid = Xuid();
  if (!WriteContent(content, xuid, kPrepName, "ME1 character", files, pc, pick->name)) return false;
  content->NotifySavedGameWritten("me1 import");
  // The prologue's LegacyDataCallback (execDetermineParams sub_82A83918) reads -expectuser, -autodevice and
  // -legacydataready from GCmdLine; only with the last one does its TryImport run "defer importcharacter Char_Prep
  // <autodevice>".
  command_line = fmt::format("BioP_ProNor -importstartupmovies -legacydataready -autodevice{} -expectuser{}", kDevice, 0);
  REXLOG_INFO("[me1-import] {} ({} file(s){}) -> {}", pick->name, files.size(), pc ? ", PC byte order" : "",
              rex::path_to_utf8(SavesFolder(content, xuid) / kPrepName));
  return true;
}

// A saved-game name for an ME1 save: its file or folder name without the extension, letters / digits / _ / - only,
// at most 40 characters (XCONTENT file names hold 42), made unique.
std::string ContentName(const Candidate& c, const std::vector<std::string>& taken) {
  std::string stem = c.folder ? c.name : rex::path_to_utf8(fs::path(c.name).stem());
  std::string n;
  for (char ch : stem)
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') n.push_back(ch);
  if (n.empty()) n = "ME1_Save";
  if (n.size() > 40) n.resize(40);
  if (Lower(n) == Lower(kPrepName)) n = "ME1_" + n;
  std::string u = n;
  for (int i = 2; std::find_if(taken.begin(), taken.end(), [&](const std::string& t) { return Lower(t) == Lower(u); }) !=
                  taken.end();
       ++i) {
    const std::string suffix = fmt::format("_{}", i);
    u = n.substr(0, std::min<size_t>(n.size(), 40 - suffix.size())) + suffix;
  }
  return u;
}

// The load-screen route (me2_me1_import_load_screen): every ME1 save of the import folder becomes a saved game of the
// importer's title, the way the Xbox's import list finds them; the ones made before for saves no longer there go.
struct ListEntry {
  std::string name;     // the saved game of 4D5308C5 (m_filename)
  std::string display;  // the text the list shows
  std::tm when{};       // the ME1 save's file time
};
std::vector<ListEntry> g_list;

std::tm FileTime(const fs::path& path) {
  std::tm t{};
  struct stat st {};
  if (::stat(rex::path_to_utf8(path).c_str(), &st) == 0) {
    const std::time_t secs = st.st_mtime;
#if defined(_WIN32)
    localtime_s(&t, &secs);
#else
    localtime_r(&secs, &t);
#endif
  }
  return t;
}

void PrepareLegacySaves() {
  g_list.clear();
  auto* content = Content();
  if (!content) return;
  const uint64_t xuid = Xuid();
  const fs::path dir = ImportDir();
  const auto list = FindSaves(dir);
  std::vector<std::string> made;
  for (const Candidate& c : list) {
    std::vector<File> files;
    bool pc = false;
    std::string why;
    if (!LoadSave(c, files, pc, why)) {
      REXLOG_WARN("[me1-import] {}: {}; left out of the list", c.name, why);
      continue;
    }
    const std::string name = ContentName(c, made);
    const std::string display = !c.display.empty() ? c.display
                                : c.folder         ? c.name
                                                   : rex::path_to_utf8(fs::path(c.name).stem());
    if (!WriteContent(content, xuid, name, display, files, pc, c.name)) continue;
    made.push_back(name);
    g_list.push_back({name, display, FileTime(c.path)});
    REXLOG_INFO("[me1-import] '{}' -> saved game '{}' of {:08X} ({} file(s){})", c.name, name, kImporterTitle,
                files.size(), pc ? ", PC byte order" : "");
  }
  // Folders made earlier (by this route or the restart route's Char_Prep) that no save stands for any more.
  std::error_code ec;
  for (const auto& info : rex::filesystem::ListFiles(SavesFolder(content, xuid))) {
    if (info.type != rex::filesystem::FileInfo::Type::kDirectory) continue;
    const std::string name = rex::path_to_utf8(info.name);
    const fs::path folder = SavesFolder(content, xuid) / info.name;
    if (!fs::exists(folder / kSourceMarker, ec) && Lower(name) != Lower(kPrepName)) continue;  // not ours
    if (std::find(made.begin(), made.end(), name) != made.end()) continue;
    fs::remove_all(folder, ec);
    fs::remove(HeaderFile(content, xuid, name), ec);
    REXLOG_INFO("[me1-import] removed the old saved game '{}'", name);
  }
  content->NotifySavedGameWritten("me1 import list");
  REXLOG_INFO("[me1-import] {} ME1 save(s) from {} offered to the load screen", made.size(), rex::path_to_utf8(dir));
}


std::string NameOf(const uint8_t* base, uint32_t object) {
  char text[128];
  if (!object || !me2_name_text(base, __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + object + 0x2C)), text,
                                sizeof(text)))
    return {};
  return text;
}

// GCmdLine (TCHAR[], big-endian UTF-16; Parse(GCmdLine, ...) in sub_82A83918): " <arg>" appended when missing.
constexpr uint32_t kGCmdLine = 0x833E7150;
constexpr uint32_t kGCmdLineChars = 16384;
void AppendCommandLine(uint8_t* base, const std::string& arg) {
  std::string now;
  uint32_t n = 0;
  for (; n < kGCmdLineChars; ++n) {
    const uint16_t c = uint16_t(base[kGCmdLine + 2 * n] << 8 | base[kGCmdLine + 2 * n + 1]);
    if (!c) break;
    now.push_back(c < 128 ? char(c) : '?');
  }
  if (Lower(now).find(Lower(arg)) != std::string::npos) return;
  const std::string add = " " + arg;
  if (n + add.size() + 1 >= kGCmdLineChars) return;
  for (size_t i = 0; i <= add.size(); ++i) {
    const uint16_t c = i < add.size() ? uint16_t(uint8_t(add[i])) : 0;
    base[kGCmdLine + 2 * (n + i)] = uint8_t(c >> 8);
    base[kGCmdLine + 2 * (n + i) + 1] = uint8_t(c);
  }
  REXLOG_INFO("[me1-import] command line now '{}{}'", now, add);
}

// The opposite: every " <arg>" (whole word, any case) taken out of GCmdLine.
void RemoveCommandLine(uint8_t* base, const std::string& arg) {
  std::u16string now;
  for (uint32_t n = 0; n < kGCmdLineChars; ++n) {
    const uint16_t c = uint16_t(base[kGCmdLine + 2 * n] << 8 | base[kGCmdLine + 2 * n + 1]);
    if (!c) break;
    now.push_back(char16_t(c));
  }
  auto lower = [](char16_t c) { return c < 128 ? char16_t(std::tolower(int(c))) : c; };
  const std::u16string want = [&] {
    std::u16string w;
    for (char ch : arg) w.push_back(lower(char16_t(uint8_t(ch))));
    return w;
  }();
  bool removed = false;
  for (size_t i = 0; i + want.size() <= now.size();) {
    bool match = true;
    for (size_t k = 0; k < want.size() && match; ++k) match = lower(now[i + k]) == want[k];
    const bool starts = i == 0 || now[i - 1] == u' ';
    const bool ends = i + want.size() == now.size() || now[i + want.size()] == u' ';
    if (match && starts && ends) {
      const size_t from = (i > 0) ? i - 1 : i;  // with the space in front of it
      now.erase(from, want.size() + (i - from));
      removed = true;
      i = from;
    } else {
      ++i;
    }
  }
  if (!removed) return;
  for (size_t i = 0; i <= now.size(); ++i) {
    const uint16_t c = i < now.size() ? uint16_t(now[i]) : 0;
    base[kGCmdLine + 2 * i] = uint8_t(c >> 8);
    base[kGCmdLine + 2 * i + 1] = uint8_t(c);
  }
  REXLOG_INFO("[me1-import] '{}' taken off the command line", arg);
}
}  // namespace

void Startup(rex::Runtime* runtime) {
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  auto* content = kernel ? kernel->content_manager() : nullptr;
  if (!content) return;
  const fs::path file = content->root_path() / kLaunchFile;
  std::vector<uint8_t> text;
  if (!ReadAll(file, text)) return;
  std::error_code ec;
  fs::remove(file, ec);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == 0)) text.pop_back();
  if (text.empty()) return;
  auto xam = kernel->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  if (!xam) return;
  auto& loader = xam->loader_data();
  loader.launch_data.assign(text.begin(), text.end());
  loader.launch_data.push_back(0);
  loader.launch_data_present = true;
  REXLOG_INFO("[me1-import] continuing the import: launch data '{}'",
              std::string(text.begin(), text.end()));
}

}  // namespace me2::me1_import

using namespace me2::me1_import;

// XLaunchNewImage(path r3, flags r4): Unreap.xex is prepared here; anything else (and a failed preparation) goes on.
// A console command as the local player types it: the way ME2's own LegacyDataCallback.TryImport (sub_82A621F8)
// runs "defer importcharacter ...": the player controller [[[GEngine 0x833D60B0] + 84] + 60] + 0, its ConsoleCommand
// (vtable +320: r3 = the returned FString, r4 = this, r5 = FString command, r6 = bWriteToLog). FString(const TCHAR*)
// sub_822B1050, ~FString sub_8288FB18. The FStrings and the text live on the guest stack below the caller's frame.
REX_EXTERN(__imp__sub_822B1050);
REX_EXTERN(__imp__sub_8288FB18);
namespace {
bool RunConsoleCommand(PPCContext& ctx, uint8_t* base, const std::string& command) {
  auto load = [&](uint32_t va) { return va ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va)) : 0u; };
  const uint32_t engine = load(0x833D60B0u);  // GEngine ([0x833D60B0] in TryImport sub_82A621F8)
  const uint32_t players = engine ? load(engine + 84) : 0;
  const uint32_t player = players ? load(players + 60) : 0;
  const uint32_t pc = player ? load(player) : 0;
  const uint32_t vtable = pc ? load(pc) : 0;
  const uint32_t fn = vtable ? load(vtable + 320) : 0;
  if (!fn || command.size() > 200) {
    REXLOG_ERROR("[me1-import] no player controller for the console command (engine {:08X} pc {:08X})", engine, pc);
    return false;
  }
  const PPCContext saved = ctx;
  const uint32_t sp = (ctx.r1.u32 - 0x400u) & ~0xFu;
  const uint32_t f_command = sp + 0x80, f_result = sp + 0x90, text = sp + 0x100;
  auto store = [&](uint32_t va, uint32_t v) { *reinterpret_cast<uint32_t*>(base + va) = __builtin_bswap32(v); };
  store(sp, ctx.r1.u32);  // back chain
  for (uint32_t i = 0; i < 3; ++i) store(f_command + 4 * i, 0), store(f_result + 4 * i, 0);
  for (size_t i = 0; i <= command.size(); ++i) {
    const uint16_t c = i < command.size() ? uint16_t(uint8_t(command[i])) : 0;
    base[text + 2 * i] = uint8_t(c >> 8);
    base[text + 2 * i + 1] = uint8_t(c);
  }
  ctx.r1.u64 = sp;
  ctx.r3.u64 = f_command;
  ctx.r4.u64 = text;
  __imp__sub_822B1050(ctx, base);
  ctx.r3.u64 = f_result;
  ctx.r4.u64 = pc;
  ctx.r5.u64 = f_command;
  ctx.r6.u64 = 1;
  rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
  ctx.r3.u64 = f_result;
  __imp__sub_8288FB18(ctx, base);
  ctx.r3.u64 = f_command;
  __imp__sub_8288FB18(ctx, base);
  ctx = saved;
  REXLOG_INFO("[me1-import] console: {}", command);
  return true;
}
}  // namespace

// The importer's launch (sub_829F5E40, called by SFXEngine.LaunchUnreaper / LaunchUnreaperWithDiscCheck after the
// main menu's "Import ME1 Character?" box): sets Unreap.xex's launch data and ends in XLaunchNewImage, which does not
// return (the function has no epilogue after it). With me2_me1_import_no_restart it is replaced: Char_Prep from the
// chosen save, then the PC version's "defer importcharacter <name> <device> forcenewgame" (SFXSFHandler_Load
// .Callback_ConfirmLoadGame): IMPORTCHARACTER (sub_8287A818) imports and, with a third word, starts the new game.
REX_EXTERN(__imp__sub_829F5E40);
REX_HOOK_RAW(sub_829F5E40) {
  static const bool in_place = REXCVAR_GET(me2_me1_import) && REXCVAR_GET(me2_me1_import_no_restart);
  if (!in_place) {
    __imp__sub_829F5E40(ctx, base);
    return;
  }
  std::string unused;
  if (!PrepareCharacter(unused)) {
    REXLOG_ERROR("[me1-import] nothing to import; back to the menu");
    return;
  }
  RunConsoleCommand(ctx, base, fmt::format("defer importcharacter {} {} forcenewgame", kPrepName, kDevice));
}

REX_EXTERN(__imp__sub_82C4BB80);
REX_HOOK_RAW(sub_82C4BB80) {
  const char* path = ctx.r3.u32 ? reinterpret_cast<const char*>(base + ctx.r3.u32) : "";
  std::string lower(path, strnlen(path, 260));
  for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  if (REXCVAR_GET(me2_me1_import) && lower.find("unreap.xex") != std::string::npos) {
    std::string command_line;
    if (!PrepareCharacter(command_line)) {
      REXLOG_ERROR("[me1-import] the import stops here; ME2 is told that Unreap.xex could not start");
      ctx.r3.u64 = 2;  // ERROR_FILE_NOT_FOUND
      return;
    }
    auto* content = Content();
    const auto file = content->root_path() / kLaunchFile;
    if (!WriteAll(file, command_line.data(), command_line.size())) {
      REXLOG_ERROR("[me1-import] cannot write {}", rex::path_to_utf8(file));
      ctx.r3.u64 = 2;
      return;
    }
    REXLOG_INFO("[me1-import] character prepared; the game closes now as on the Xbox. START IT AGAIN: the next start "
                "imports the character ('{}')", command_line);
    rex::FlushLogging();
  }
  __imp__sub_82C4BB80(ctx, base);
}

// The console commands' Exec (sub_8287A818: this r3, command TCHAR* r4; parses IMPORTCHARACTER among others). An
// "importcharacter ..." (a save picked in the load screen's ME1 list, or the restart route's TryImport) puts
// -legacydataready on the command line first: the prologue's LegacyDataCallback (DetermineParams) reads it when the
// new game's map starts, and without it UpdateOp takes the canon (no import) branch.
// The hook itself is me2_frame_diag.cpp's ME2_GC_CALLER(8287A818), which calls this before the original.
extern "C" void me2_me1_import_console_exec(PPCContext& ctx, uint8_t* base) {
  static const bool on = REXCVAR_GET(me2_me1_import);
  if (on && ctx.r4.u32) {
    constexpr const char kWord[] = "importcharacter";
    uint32_t at = ctx.r4.u32;
    auto ch = [&](uint32_t i) { return uint16_t(base[at + 2 * i] << 8 | base[at + 2 * i + 1]); };
    while (ch(0) == u' ' || ch(0) == u'\t') at += 2;
    bool match = true;
    for (uint32_t i = 0; i < sizeof(kWord) - 1 && match; ++i) {
      const uint16_t c = ch(i);
      match = c < 128 && std::tolower(int(c)) == kWord[i];
    }
    const uint16_t next = ch(sizeof(kWord) - 1);
    if (match && (next == 0 || next == u' ' || next == u'\t')) AppendCommandLine(base, "-legacydataready");
  }
}

// SFXEngine.ReLaunchExecutable (sub_829F5C88; BioPlayerController.Callback_PlayerLoggedOut): the Xbox's restart
// (Splash.bmp, render teardown, XLaunchNewImage default.xex). The port cannot start itself again, and the teardown
// crashed freeing a texture (2026-11-04 Ryujinx), so it is logged and skipped.
REXCVAR_DEFINE_BOOL(me2_skip_relaunch, true, "ME2",
                    "SFXEngine.ReLaunchExecutable (the Xbox's restart of the game) does nothing; logged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REX_EXTERN(__imp__sub_829F5C88);
REX_HOOK_RAW(sub_829F5C88) {
  if (REXCVAR_GET(me2_skip_relaunch)) {
    REXLOG_WARN("[me1-import] SFXEngine.ReLaunchExecutable called (the Xbox restarts the game here): skipped");
    rex::FlushLogging();
    return;
  }
  __imp__sub_829F5C88(ctx, base);
}

// state.sav (sub_8297CA38: this r3, folder FString r4): which save, and the result.
REX_EXTERN(__imp__sub_8297CA38);
REX_HOOK_RAW(sub_8297CA38) {
  std::string where;
  bool pc = false;
  if (auto* content = Content()) {
    const fs::path open = content->GetOpenPackagePath("XCONTENT");
    std::error_code ec;
    pc = !open.empty() && fs::exists(open / kPcMarker, ec);
    where = rex::path_to_utf8(open);
  }
  __imp__sub_8297CA38(ctx, base);
  REXLOG_INFO("[me1-import] state.sav of {}{} -> {}", where.empty() ? "(no XCONTENT open)" : where,
              pc ? " (converted from a PC save)" : "", ctx.r3.u32 ? "read" : "FAILED");
}

// The package version player.sav is read with. sub_8297CC78 makes its memory archive (ArVer at +4, ArLicenseeVer at
// +12) version 391 / 92, the Xbox ME1's, before reading anything, and never takes the file's. A PC save (version 491 /
// 1008) has the newer layouts of that engine: enum values as names (8 bytes, not 1), Vector / Rotator in binary, ...
// ME2's engine reads those itself at version 491 (2026-11-02 PC run: at 391 every enum value put the reader 7 bytes
// behind). So for a save converted from the PC version the archive gets these versions when its export table is read
// (sub_8297D838, archive r3), before any object. ArLicenseeVer stays the Xbox's 92 by default: BioWare's own checks
// are on that scale (the PC's 1008 is not), and the files come from the same game.
REXCVAR_DEFINE_INT32(me2_me1_import_pc_version, 491, "ME2",
                     "Package version ME2 reads an ME1 PC save's player.sav with (0 = the Xbox's 391)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_me1_import_pc_licensee, 92, "ME2",
                     "Licensee version ME2 reads an ME1 PC save's player.sav with (the Xbox's 92; the PC files say 1008)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8297D838);
REX_HOOK_RAW(sub_8297D838) {
  const uint32_t ar = ctx.r3.u32;
  bool pc = false;
  if (auto* content = Content()) {
    const fs::path open = content->GetOpenPackagePath("XCONTENT");
    std::error_code ec;
    pc = !open.empty() && fs::exists(open / kPcMarker, ec);
  }
  const int32_t version = REXCVAR_GET(me2_me1_import_pc_version);
  if (pc && version > 0 && ar) {
    auto get = [&](uint32_t o) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ar + o)); };
    auto put = [&](uint32_t o, uint32_t v) { *reinterpret_cast<uint32_t*>(base + ar + o) = __builtin_bswap32(v); };
    REXLOG_INFO("[me1-import] player.sav: read as package version {} / {} (was {} / {})", version,
                REXCVAR_GET(me2_me1_import_pc_licensee), get(4), get(12));
    put(4, uint32_t(version));
    put(12, uint32_t(REXCVAR_GET(me2_me1_import_pc_licensee)));
  }
  __imp__sub_8297D838(ctx, base);
}

// Diagnostic: the property tags ME2 reads from player.sav (FPropertyTag reader sub_8275E9F0: archive r3, tag r4 =
// Type +0, BoolVal +8, Name +12, StructName +20, Size +28, ArrayIndex +32; the memory archive's position at +128),
// logged while the file position is inside me2_me1_import_trace ("from-to", hex; "" = off).
REXCVAR_DEFINE_STRING(me2_me1_import_trace, "", "ME2",
                      "Diagnostic: log the property tags ME2 reads from player.sav between these file positions "
                      "(\"22AE0-23600\", hex)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace {
thread_local bool t_reading_player = false;
}

REX_EXTERN(__imp__sub_8275E9F0);
REX_HOOK_RAW(sub_8275E9F0) {
  static uint32_t lo = 0, hi = 0;
  static const bool on = [] {
    const std::string r = REXCVAR_GET(me2_me1_import_trace);
    const size_t dash = r.find('-');
    if (dash == std::string::npos) return false;
    lo = uint32_t(std::strtoul(r.substr(0, dash).c_str(), nullptr, 16));
    hi = uint32_t(std::strtoul(r.substr(dash + 1).c_str(), nullptr, 16));
    return hi > lo;
  }();
  if (!on || !t_reading_player) {
    __imp__sub_8275E9F0(ctx, base);
    return;
  }
  const uint32_t ar = ctx.r3.u32, tag = ctx.r4.u32;
  auto get = [&](uint32_t va) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va)); };
  const uint32_t before = get(ar + 128);
  __imp__sub_8275E9F0(ctx, base);
  const uint32_t after = get(ar + 128);
  if (before < lo || before > hi) return;
  auto name = [&](uint32_t at) {
    char text[96];
    return me2_name_text(base, get(at), text, sizeof(text)) ? std::string(text) : fmt::format("#{}", get(at));
  };
  REXLOG_INFO("[me1-trace] tag at {:X}-{:X}: {} {} size {} index {} struct {} bool {}", before, after, name(tag + 12),
              name(tag + 0), get(tag + 28), get(tag + 32), name(tag + 20), get(tag + 8));
}

// player.sav's reader (sub_8297CC78; the whole file into memory, its package summary parsed by hand): the result.
REX_EXTERN(__imp__sub_8297CC78);
REX_HOOK_RAW(sub_8297CC78) {
  t_reading_player = true;
  __imp__sub_8297CC78(ctx, base);
  t_reading_player = false;
  REXLOG_INFO("[me1-import] player.sav reader -> {}", ctx.r3.u32 ? "read" : "FAILED");
}

// The main menu's Import ME1 Character (BioSFHandler_MainMenu.HandleEvent, case 13) asks WorldInfo.IsConsoleBuild(1):
// true -> the Xbox's message box and LaunchUnreaperWithDiscCheck (Unreap.xex, a restart); false -> the PC's route,
// ImportedDataManager.m_bImportAllowed = true and MoveToLoadGui(1): the load screen lists the ME1 saves
// (LegacyRecords, BioSaveGame.SaveGameRecord: m_filename / m_DeviceId) and picking one runs
// "defer importcharacter <m_filename> <m_DeviceId> forcenewgame" (no restart). That function calls IsConsoleBuild
// nowhere else. execIsConsoleBuild sub_82429A88 (this r3, FFrame r4: +4 Node = the calling UFunction, result r5).
REX_EXTERN(__imp__sub_82429A88);
REX_HOOK_RAW(sub_82429A88) {
  const uint32_t frame = ctx.r4.u32, result = ctx.r5.u32;
  __imp__sub_82429A88(ctx, base);
  static const bool on = REXCVAR_GET(me2_me1_import) && REXCVAR_GET(me2_me1_import_load_screen);
  if (!on || !frame || !result) return;
  const uint32_t node = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + frame + 4));
  if (!node || NameOf(base, node) != "HandleEvent") return;
  const uint32_t outer = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + node + 0x28));
  if (NameOf(base, outer) != "BioSFHandler_MainMenu") return;
  PrepareLegacySaves();
  // -legacydataready goes on only when a save is picked (the importcharacter hook below): left on after the list was
  // closed, New Game's prologue looked for an import that never came (Char_Prep missing) and ended in
  // ReLaunchExecutable (2026-11-04 Ryujinx crash). An earlier visit's flag goes here.
  RemoveCommandLine(base, "-legacydataready");
  *reinterpret_cast<uint32_t*>(base + result) = 0;
  REXLOG_INFO("[me1-import] Import ME1 Character: the load screen's ME1 list (the PC version's route)");
}

// ---------------------------------------------------------------------------------------------------------------------
// The ME1 list. SFXSFHandler_Load (vtable 0x821EC848) native InitializeLoadList = slot 99, sub_8294D838 (this r3,
// CareerIdx r4). Fields: SaveList +132 (TArray<SaveGUIRecord>, 40 bytes: SaveGame +0, FriendlyName +4, AreaImage +16,
// ImagePath +20, SaveIndex +32, Type +36 (4 = legacy)), LegacyRecords +256 (TArray<BioSaveGame.SaveGameRecord>,
// 108 bytes: m_filename +0, m_saveName +12, m_saveNameShort +24, m_sec/m_Min/m_hr +36/+40/+44, m_day/m_mo/m_year
// +48/+52/+56, m_secPlayed/m_minPlayed/m_hrPlayed +60/+64/+68, m_charId +72, m_ClassId +84, m_CharcterLevel +88,
// m_bCanDelete +92, m_DeviceId +96, m_SaveGameNumber +100, flags +104; array destructor sub_82952480), LoadMode +308.
// In mode 1 the function: empties SaveList (sub_8294FFA8), asks sub_82935270 and sub_829FAEC8 (the current career's
// save folder, root me2careers), then leaves unless LoadMode is 0 or 2; with 0 it lists the career's *.xbsav, closes
// me2careers (XContentClose wrapper sub_82C4B090) and, if LoadMode is 1 then, shows SaveList + LegacyRecords (the
// display the PC version filled from ME1's Documents). Picking a type-4 entry runs (Callback_ConfirmLoadGame)
// "defer importcharacter <m_filename> <m_DeviceId> forcenewgame".
// So while this list is built: the two checks answer yes without opening the career (nothing to list), LoadMode is 0
// until the close, where the ME1 saves go into both arrays and LoadMode is 1 again for the display.
REX_EXTERN(__imp__sub_822B4368);  // FArray::Add(array, count, element size, alignment) -> first new index
REX_EXTERN(__imp__sub_82952480);  // TArray<SaveGameRecord> destructor
REX_EXTERN(__imp__sub_8294FFA8);  // TArray<SaveGUIRecord>::Empty (InitializeLoadList's first step on SaveList)
namespace {
thread_local uint32_t t_legacy_list = 0;  // the SFXSFHandler_Load building its ME1 list

uint32_t Get32(const uint8_t* base, uint32_t va) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va)); }
void Put32(uint8_t* base, uint32_t va, uint32_t v) { *reinterpret_cast<uint32_t*>(base + va) = __builtin_bswap32(v); }

// Guest calls from host code: a frame below the caller's, its registers given back afterwards.
struct GuestCalls {
  PPCContext& ctx;
  uint8_t* base;
  PPCContext saved;
  uint32_t text;  // scratch for one TCHAR string
  GuestCalls(PPCContext& c, uint8_t* b) : ctx(c), base(b), saved(c) {
    const uint32_t sp = (ctx.r1.u32 - 0x800u) & ~0xFu;
    Put32(base, sp, ctx.r1.u32);
    text = sp + 0x100;
    ctx.r1.u64 = sp;
  }
  ~GuestCalls() { ctx = saved; }
  // FString(const TCHAR*) into the (zeroed) FString at 'at'.
  void String(uint32_t at, const std::string& value) {
    const std::string v = value.substr(0, 300);
    for (size_t i = 0; i <= v.size(); ++i) {
      const uint16_t c = i < v.size() ? uint16_t(uint8_t(v[i])) : 0;
      base[text + 2 * i] = uint8_t(c >> 8);
      base[text + 2 * i + 1] = uint8_t(c);
    }
    ctx.r3.u64 = at;
    ctx.r4.u64 = text;
    __imp__sub_822B1050(ctx, base);
  }
  // A new zeroed element of 'size' bytes at the end of the TArray at 'array'; its address.
  uint32_t Add(uint32_t array, uint32_t size) {
    ctx.r3.u64 = array;
    ctx.r4.u64 = 1;
    ctx.r5.u64 = size;
    ctx.r6.u64 = 8;
    __imp__sub_822B4368(ctx, base);
    const uint32_t at = Get32(base, array) + ctx.r3.u32 * size;
    std::memset(base + at, 0, size);
    return at;
  }
};

void FillLegacyList(PPCContext& ctx, uint8_t* base, uint32_t self) {
  GuestCalls g(ctx, base);
  // LegacyRecords from an earlier opening: destroyed, then empty.
  if (Get32(base, self + 256) || Get32(base, self + 260)) {
    ctx.r3.u64 = self + 256;
    __imp__sub_82952480(ctx, base);
    for (uint32_t i = 0; i < 3; ++i) Put32(base, self + 256 + 4 * i, 0);
  }
  // SaveList: entries the career listing might have added (it lists nothing while this list is built) go, so that
  // SaveList[i] and LegacyRecords[i] stay pairs (the display walks both with one index).
  const uint32_t first = Get32(base, self + 136);
  if (first) {
    ctx.r3.u64 = self + 132;
    __imp__sub_8294FFA8(ctx, base);
  }
  for (size_t i = 0; i < g_list.size(); ++i) {
    const ListEntry& e = g_list[i];
    const uint32_t r = g.Add(self + 256, 108);
    g.String(r + 0, e.name);
    g.String(r + 12, e.display);
    g.String(r + 24, e.display);
    Put32(base, r + 36, uint32_t(e.when.tm_sec));
    Put32(base, r + 40, uint32_t(e.when.tm_min));
    Put32(base, r + 44, uint32_t(e.when.tm_hour));
    Put32(base, r + 48, uint32_t(e.when.tm_mday));
    Put32(base, r + 52, uint32_t(e.when.tm_mon + 1));
    Put32(base, r + 56, uint32_t(e.when.tm_year + 1900));
    Put32(base, r + 96, kDevice);
    Put32(base, r + 100, uint32_t(i));
    const uint32_t s = g.Add(self + 132, 40);
    g.String(s + 4, e.display);
    Put32(base, s + 32, uint32_t(i));
    base[s + 36] = 4;  // SaveRecordType_Legacy
  }
  REXLOG_INFO("[me1-import] load screen: {} ME1 save(s) listed (SaveList had {})", g_list.size(), first);
}
}  // namespace

REX_EXTERN(__imp__sub_8294D838);
REX_HOOK_RAW(sub_8294D838) {
  const uint32_t self = ctx.r3.u32;
  const int32_t career = ctx.r4.s32;
  static const bool on = REXCVAR_GET(me2_me1_import) && REXCVAR_GET(me2_me1_import_load_screen);
  const bool legacy = on && base[self + 308] == 1 && !g_list.empty();
  t_legacy_list = legacy ? self : 0;
  __imp__sub_8294D838(ctx, base);
  if (legacy && t_legacy_list) {
    REXLOG_WARN("[me1-import] load screen: the list builder left before the display (LoadMode now {})", base[self + 308]);
    base[self + 308] = 1;
  }
  t_legacy_list = 0;
  REXLOG_INFO("[me1-import] load screen list: LoadMode {} career {} -> SaveList {} / LegacyRecords {}", base[self + 308],
              career, Get32(base, self + 136), Get32(base, self + 260));
}

// The two checks before the career's folder is listed (sub_82935270: a script event, true / false; sub_829FAEC8:
// opens the current career's saved game as me2careers): while the ME1 list is built, yes without doing it, and
// LoadMode 0 so that the function goes on (to an empty career listing).
REX_EXTERN(__imp__sub_82935270);
REX_HOOK_RAW(sub_82935270) {
  if (t_legacy_list) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82935270(ctx, base);
}

REX_EXTERN(__imp__sub_829FAEC8);
REX_HOOK_RAW(sub_829FAEC8) {
  if (t_legacy_list) {
    base[t_legacy_list + 308] = 0;
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_829FAEC8(ctx, base);
}

// XContentClose(root, overlapped) wrapper: in the list builder the close of me2careers right before "LoadMode == 1:
// show the ME1 list". Our entries go in there and LoadMode is 1 again.
REX_EXTERN(__imp__sub_82C4B090);
REX_HOOK_RAW(sub_82C4B090) {
  if (const uint32_t self = t_legacy_list) {
    t_legacy_list = 0;
    FillLegacyList(ctx, base, self);
    base[self + 308] = 1;
  }
  __imp__sub_82C4B090(ctx, base);
}
