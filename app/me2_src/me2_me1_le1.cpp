// ME2 (2026-11-14): ME1 Legendary Edition (LE1) saves -> the ME1 Xbox files ME2's importer reads.
//
// LE1 .pcsav (little-endian; layout after KarlitosVII/trilogy-save-editor, src/save_data/mass_effect_1_le): the tag
// C1 83 2A 9E, block size, a total chunk (compressed, uncompressed), one (compressed, uncompressed) per block, the
// zlib blocks, then checksum, compression flag and size (12 bytes). The data: version 50, character id, created date,
// plot table (bools as u32 words, ints, floats), journal (progress counter, quests: counter, updated, history; then
// the quest ids), codex (entries: pages of (page, new); then the ids), timestamp, seconds played, the player record,
// base level name, map name, parent map name, location, rotation, squad, display name, file name. bool = 4 bytes,
// enums = 1 byte, Option = bool + value, Vec / maps = u32 count + elements, strings = i32 length (negative: UTF-16).
//
// state.sav (ME1 Xbox, big-endian), as ME2 TU2 reads it (sub_8297CA38; quests sub_828BA728 / sub_828BA660, codex
// sub_828B9F50 / sub_828B9E48 / sub_828B9D00 / sub_828B9C90): FFFFFFFF, 30, 2, map FString, location, rotation, plot
// bools / ints / floats, quest progress counter, quests (count; key, version 1, counter, history, updated), codex
// (count; key, version 1, pages count, (page, new) pairs). LE1 keeps the same plot data with the keys in separate
// arrays.
//
// player.sav: an ME1 Xbox package (v391 / 92, no depends offset; objects back to back in export order after the
// header) from me2_me1_le1_template.inc (an ME1 Xbox save), whose CurrentGame, player BioPawnBehaviorSaveObject,
// BioSquadSaveObject, BioInventorySaveObject and BioInterfaceAppearanceSaveObject get the LE1 save's values. Names
// the new properties need are appended to the name table; offsets and sizes are rebuilt.
//
// First pass (checked against a Python prototype byte for byte): male characters with the default face. Female
// characters (another face asset) and LE1 head morphs (-> ME1 BioMorphFaceSaveObject) are refused with a reason.
#include "me2_me1_le1.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace me2::me1_import::le1 {
namespace {

// The template is a real ME1 Xbox save and is not part of the public tree. With the file present (generate it from
// an ME1 Xbox 360 player.sav of your own: the header of the .inc says how) the LE1 import works; without it the build
// still links and a .pcsav is refused with a reason.
#if __has_include("me2_me1_le1_template.inc")
#include "me2_me1_le1_template.inc"
#else
constexpr size_t kTemplateSize = 0;
constexpr unsigned char kTemplateZ[] = {0};
#endif

// ---------------------------------------------------------------------------------------------------------------------
// UTF helpers
std::string Utf16ToUtf8(const std::u16string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
      c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | c >> 6);
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | c >> 12);
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | c >> 18);
      out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// An LE1 string: its characters as UTF-16 (1-byte strings are Windows-1252; taken as Latin-1).
struct Text {
  std::u16string chars;
  std::string Utf8() const { return Utf16ToUtf8(chars); }
  std::string Ascii() const {
    std::string s;
    for (char16_t c : chars) s += c < 0x80 ? char(c) : '?';
    return s;
  }
};

// ---------------------------------------------------------------------------------------------------------------------
// LE1 reader (little-endian)
class Reader {
 public:
  Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
  bool ok() const { return ok_; }
  size_t pos() const { return p_; }
  size_t left() const { return n_ - p_; }

  uint32_t U32() {
    if (!Need(4)) return 0;
    const uint32_t v = uint32_t(d_[p_]) | uint32_t(d_[p_ + 1]) << 8 | uint32_t(d_[p_ + 2]) << 16 | uint32_t(d_[p_ + 3]) << 24;
    p_ += 4;
    return v;
  }
  int32_t I32() { return int32_t(U32()); }
  float F32() {
    const uint32_t v = U32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  uint8_t U8() {
    if (!Need(1)) return 0;
    return d_[p_++];
  }
  bool Bool() {
    const uint32_t v = U32();
    if (v > 1) ok_ = false;
    return v == 1;
  }
  Text Str() {
    Text t;
    const int32_t n = I32();
    if (n > 0) {
      if (n > 0x100000 || !Need(size_t(n))) return t;
      for (int32_t k = 0; k + 1 < n; ++k) t.chars += char16_t(d_[p_ + k]);
      p_ += size_t(n);
    } else if (n < 0) {
      const size_t m = size_t(-int64_t(n));
      if (m > 0x100000 || !Need(m * 2)) return t;
      for (size_t k = 0; k + 1 < m; ++k) t.chars += char16_t(d_[p_ + 2 * k] | d_[p_ + 2 * k + 1] << 8);
      p_ += m * 2;
    }
    return t;
  }
  uint32_t Count() {
    const uint32_t n = U32();
    if (n > 1000000) ok_ = false;
    return ok_ ? n : 0;
  }
  void Skip(size_t n) {
    if (Need(n)) p_ += n;
  }

 private:
  bool Need(size_t k) {
    if (!ok_ || n_ - p_ < k) {
      ok_ = false;
      return false;
    }
    return true;
  }
  const uint8_t* d_;
  size_t n_, p_ = 0;
  bool ok_ = true;
};

struct SimpleTalent {
  int32_t id = 0, rank = 0;
};
struct ComplexTalent {
  int32_t id = 0, rank = 0, max_rank = 0, level_offset = 0, levels_per_rank = 0, visual_order = 0;
  std::vector<int32_t> prereq_ids, prereq_ranks;
};
struct Quest {
  int32_t counter = 0;
  bool updated = false;
  std::vector<int32_t> history;
};

struct Le1Save {
  Text character_id;
  std::vector<uint32_t> bools;
  std::vector<int32_t> ints;
  std::vector<float> floats;
  int32_t quest_counter = 0;
  std::vector<Quest> quests;
  std::vector<int32_t> quest_ids;
  std::vector<std::vector<std::pair<int32_t, bool>>> codex;
  std::vector<int32_t> codex_ids;
  int32_t seconds_played = 0;
  // player
  bool female = false, head_morph = false;
  int32_t class_name = 0, level = 0, last_name = 0, specialization = 0, talent_points = 0;
  uint8_t player_class = 0, origin = 0, notoriety = 0, spectre_rank = 0, attribute_primary = 0, attribute_secondary = 0;
  float xp = 0;
  Text first_name;
  std::vector<SimpleTalent> simple;
  std::vector<ComplexTalent> complex;
  int32_t credits = 0, medigel = 0, auto_levelup = 0, stamina = 0, focus = 0, precision = 0, coordination = 0;
  float grenades = 0, omnigel = 0, health_per_level = 0, stability = 0, health = 0, shield = 0;
  // after the player
  Text base_level;
  float location[3] = {};
  int32_t rotation[3] = {};
  Text display_name;
};

std::vector<int32_t> IntVec(Reader& r) {
  std::vector<int32_t> v(r.Count());
  for (auto& x : v) x = r.I32();
  return v;
}

void SkipItem(Reader& r) {
  r.I32(), r.U8(), r.I32(), r.I32(), r.Bool(), r.Bool();
  for (uint32_t k = r.Count(); k-- && r.ok();) r.I32(), r.U8(), r.I32(), r.I32();  // attached mods
}
void SkipItems(Reader& r) {
  for (uint32_t k = r.Count(); k-- && r.ok();) SkipItem(r);
}
std::vector<SimpleTalent> SimpleTalents(Reader& r) {
  std::vector<SimpleTalent> v(r.Count());
  for (auto& t : v) t.id = r.I32(), t.rank = r.I32();
  return v;
}
std::vector<ComplexTalent> ComplexTalents(Reader& r) {
  std::vector<ComplexTalent> v(r.Count());
  for (auto& t : v) {
    t.id = r.I32();
    t.rank = r.I32();
    t.max_rank = r.I32();
    t.level_offset = r.I32();
    t.levels_per_rank = r.I32();
    t.visual_order = r.I32();
    t.prereq_ids = IntVec(r);
    t.prereq_ranks = IntVec(r);
  }
  return v;
}
void SkipHeadMorph(Reader& r) {
  r.Str();                                                                              // hair mesh
  for (uint32_t k = r.Count(); k-- && r.ok();) r.Str();                                 // accessory meshes
  for (uint32_t k = r.Count(); k-- && r.ok();) r.Str(), r.F32();                        // morph features
  for (uint32_t k = r.Count(); k-- && r.ok();) r.Str(), r.Skip(12);                     // offset bones
  for (int lod = 0; lod < 4; ++lod) r.Skip(size_t(r.Count()) * 12);                     // LOD vertices
  for (uint32_t k = r.Count(); k-- && r.ok();) r.Str(), r.F32();                        // scalar parameters
  for (uint32_t k = r.Count(); k-- && r.ok();) r.Str(), r.Skip(16);                     // vector parameters
  for (uint32_t k = r.Count(); k-- && r.ok();) r.Str(), r.Str();                        // texture parameters
}
void SkipHenchman(Reader& r) {
  r.Str();
  SimpleTalents(r);
  ComplexTalents(r);
  SkipItems(r);  // equipment
  SkipItems(r);  // quick slots
  r.Skip(4 * 5);  // talent points, pool points, auto level-up template, last name, class name
  r.U8();         // class base
  r.Skip(8);      // health per level, stability
  r.U8(), r.U8();  // gender, race
  r.Skip(4 * 5);  // toxic, stamina, focus, precision, coordination
  r.U8(), r.U8();  // attributes
  r.Skip(4 * 3);  // health, shield, level
  r.Bool();       // helmet shown
  r.U8();         // current quick slot
  r.F32();        // health max
}

bool Unpack(const std::vector<uint8_t>& file, InflateFn inflate, std::vector<uint8_t>& out, std::string& why) {
  auto le = [&](size_t o) -> uint32_t {
    return uint32_t(file[o]) | uint32_t(file[o + 1]) << 8 | uint32_t(file[o + 2]) << 16 | uint32_t(file[o + 3]) << 24;
  };
  if (!LooksLikeLe1(file) || file.size() < 16) return why = "not an LE1 save (tag)", false;
  const uint32_t total = le(12);
  if (total == 0 || total > 64u * 1024 * 1024) return why = "LE1 save: bad size", false;
  std::vector<std::pair<uint32_t, uint32_t>> blocks;
  size_t p = 16;
  uint64_t got = 0;
  while (got < total) {
    if (p + 8 > file.size()) return why = "LE1 save: block table", false;
    blocks.emplace_back(le(p), le(p + 4));
    got += le(p + 4);
    p += 8;
  }
  out.assign(total, 0);
  size_t o = 0;
  for (const auto& [c, u] : blocks) {
    if (p + c > file.size() || o + u > out.size()) return why = "LE1 save: block outside the file", false;
    if (!inflate(&file[p], c, &out[o], u)) return why = "LE1 save: a block does not inflate", false;
    p += c;
    o += u;
  }
  return true;
}

bool Parse(const std::vector<uint8_t>& d, Le1Save& s, std::string& why) {
  Reader r(d.data(), d.size());
  if (r.I32() != 50) return why = "LE1 save: version is not 50", false;
  s.character_id = r.Str();
  r.Skip(16);  // created date
  s.bools.resize(r.Count());
  for (auto& w : s.bools) w = r.U32();
  s.ints = IntVec(r);
  s.floats.resize(r.Count());
  for (auto& f : s.floats) f = r.F32();
  s.quest_counter = r.I32();
  s.quests.resize(r.Count());
  for (auto& q : s.quests) {
    q.counter = r.I32();
    q.updated = r.Bool();
    q.history = IntVec(r);
  }
  s.quest_ids = IntVec(r);
  s.codex.resize(r.Count());
  for (auto& e : s.codex) {
    e.resize(r.Count());
    for (auto& [page, is_new] : e) page = r.I32(), is_new = r.Bool();
  }
  s.codex_ids = IntVec(r);
  r.Skip(16);  // timestamp
  s.seconds_played = r.I32();
  // the player
  s.female = r.Bool();
  s.class_name = r.I32();
  s.player_class = r.U8();
  s.level = r.I32();
  s.xp = r.F32();
  s.first_name = r.Str();
  s.last_name = r.I32();
  s.origin = r.U8();
  s.notoriety = r.U8();
  s.specialization = r.I32();
  s.spectre_rank = r.U8();
  s.talent_points = r.I32();
  r.I32();  // talent pool points
  r.Str();  // mapped talent
  s.head_morph = r.Bool();
  if (s.head_morph) SkipHeadMorph(r);
  s.simple = SimpleTalents(r);
  s.complex = ComplexTalents(r);
  for (int k = 0; k < 4 && r.ok(); ++k) SkipItems(r);  // equipment, quick slots, inventory, buy pack
  s.credits = r.I32();
  s.medigel = r.I32();
  s.grenades = r.F32();
  s.omnigel = r.F32();
  r.Str();   // face code
  r.Bool();  // armor overridden
  s.auto_levelup = r.I32();
  s.health_per_level = r.F32();
  s.stability = r.F32();
  r.U8();   // race
  r.F32();  // toxic
  s.stamina = r.I32();
  s.focus = r.I32();
  s.precision = r.I32();
  s.coordination = r.I32();
  s.attribute_primary = r.U8();
  s.attribute_secondary = r.U8();
  r.Skip(12);  // charm, intimidate, haggle
  s.health = r.F32();
  s.shield = r.F32();
  r.I32();   // xp level
  r.Bool();  // is driving
  IntVec(r);  // game options
  r.Bool();   // helmet shown
  r.Skip(5);
  r.Str();    // last power
  r.F32();    // health max
  r.Skip(size_t(r.Count()) * 8);  // hotkeys
  r.Str();
  r.Str();  // primary, secondary weapon
  // after the player
  s.base_level = r.Str();
  r.Str();
  r.Str();  // map name, parent map name
  for (float& f : s.location) f = r.F32();
  for (int32_t& v : s.rotation) v = r.I32();
  for (uint32_t k = r.Count(); k-- && r.ok();) SkipHenchman(r);
  s.display_name = r.Str();
  r.Str();  // file name
  if (!r.ok()) return why = "LE1 save: the data does not parse", false;
  if (s.quests.size() != s.quest_ids.size() || s.codex.size() != s.codex_ids.size())
    return why = "LE1 save: quest / codex ids do not match", false;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Big-endian writing
void Put32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
void PutF(std::vector<uint8_t>& b, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Put32(b, v);
}
// An FString: 1-byte characters when all are ASCII (as the Python prototype), else UTF-16 with each character
// LITTLE-endian: ME2's FString reader (TU2 sub_82791E68) byte-swaps every UTF-16 character it reads, also from these
// big-endian files (written big-endian, "Джон" came out as squares: U+0414 read as U+1404).
void PutStr(std::vector<uint8_t>& b, const std::u16string& t) {
  if (t.empty()) {
    Put32(b, 0);
    return;
  }
  const bool ascii = std::all_of(t.begin(), t.end(), [](char16_t c) { return c < 0x80; });
  if (ascii) {
    Put32(b, uint32_t(t.size() + 1));
    for (char16_t c : t) b.push_back(uint8_t(c));
    b.push_back(0);
  } else {
    Put32(b, uint32_t(-int32_t(t.size() + 1)));
    for (char16_t c : t) b.push_back(uint8_t(c)), b.push_back(uint8_t(c >> 8));
    b.push_back(0), b.push_back(0);
  }
}
std::u16string U16(const std::string& ascii) { return std::u16string(ascii.begin(), ascii.end()); }
uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}
void SetBe32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
  d[o] = uint8_t(v >> 24), d[o + 1] = uint8_t(v >> 16), d[o + 2] = uint8_t(v >> 8), d[o + 3] = uint8_t(v);
}
std::vector<uint8_t> Int(int32_t v) {
  std::vector<uint8_t> b;
  Put32(b, uint32_t(v));
  return b;
}
std::vector<uint8_t> Flt(float f) {
  std::vector<uint8_t> b;
  PutF(b, f);
  return b;
}
std::vector<uint8_t> Str(const std::u16string& t) {
  std::vector<uint8_t> b;
  PutStr(b, t);
  return b;
}

std::vector<uint8_t> StateSav(const Le1Save& s) {
  std::vector<uint8_t> b;
  Put32(b, 0xFFFFFFFFu);
  Put32(b, 30);
  Put32(b, 2);
  PutStr(b, s.base_level.chars);
  for (float f : s.location) PutF(b, f);
  for (int32_t v : s.rotation) Put32(b, uint32_t(v));
  Put32(b, uint32_t(s.bools.size()));
  for (uint32_t w : s.bools) Put32(b, w);
  Put32(b, uint32_t(s.ints.size()));
  for (int32_t v : s.ints) Put32(b, uint32_t(v));
  Put32(b, uint32_t(s.floats.size()));
  for (float f : s.floats) PutF(b, f);
  Put32(b, uint32_t(s.quest_counter));
  Put32(b, uint32_t(s.quests.size()));
  for (size_t k = 0; k < s.quests.size(); ++k) {
    const Quest& q = s.quests[k];
    Put32(b, uint32_t(s.quest_ids[k]));
    Put32(b, 1);  // version
    Put32(b, uint32_t(q.counter));
    Put32(b, uint32_t(q.history.size()));
    for (int32_t v : q.history) Put32(b, uint32_t(v));
    Put32(b, q.updated ? 1 : 0);
  }
  Put32(b, uint32_t(s.codex.size()));
  for (size_t k = 0; k < s.codex.size(); ++k) {
    Put32(b, uint32_t(s.codex_ids[k]));
    Put32(b, 1);  // version
    Put32(b, uint32_t(s.codex[k].size()));
    for (const auto& [page, is_new] : s.codex[k]) Put32(b, uint32_t(page)), Put32(b, is_new ? 1 : 0);
  }
  return b;
}

// ---------------------------------------------------------------------------------------------------------------------
// ME1 Xbox package (big-endian, v391): parse, edit tagged properties, write
struct FName {
  int32_t index = 0, number = 0;
};
struct Prop {
  FName name, type, struct_name;
  int32_t idx = 0, boolval = 0;
  std::vector<uint8_t> value;
};

class Package {
 public:
  bool Load(const std::vector<uint8_t>& d, std::string& why) {
    d_ = d;
    if (d.size() < 64 || Be32(d, 0) != 0x9E2A83C1u) return why = "template: no package tag", false;
    size_t p = 12;
    const int32_t fn = int32_t(Be32(d, p));
    p += 4 + (fn >= 0 ? size_t(fn) : size_t(-int64_t(fn)) * 2);
    so_ = p + 4;  // after the package flags
    nc_ = Be32(d, so_), no_ = Be32(d, so_ + 4), ec_ = Be32(d, so_ + 8), eo_ = Be32(d, so_ + 12);
    ic_ = Be32(d, so_ + 16), io_ = Be32(d, so_ + 20);
    gen_ = so_ + 24 + 16;
    total_ = Be32(d, 8);
    if (no_ > d.size() || io_ > d.size() || eo_ > d.size() || total_ > d.size() || !(no_ < io_ && io_ < eo_ && eo_ < total_))
      return why = "template: summary", false;
    p = no_;
    for (uint32_t k = 0; k < nc_; ++k) {
      if (p + 4 > d.size()) return why = "template: names", false;
      const int32_t n = int32_t(Be32(d, p));
      if (n <= 0 || p + 4 + size_t(n) + 8 > d.size()) return why = "template: name", false;
      Name nm;
      nm.text.assign(reinterpret_cast<const char*>(&d[p + 4]), size_t(n) - 1);
      std::memcpy(nm.flags, &d[p + 4 + size_t(n)], 8);
      names_.push_back(nm);
      p += 4 + size_t(n) + 8;
    }
    if (p != io_) return why = "template: name table end", false;
    p = eo_;
    for (uint32_t k = 0; k < ec_; ++k) {
      Export e;
      e.entry = p;
      if (p + 44 > total_) return why = "template: exports", false;
      e.cls = int32_t(Be32(d, p));
      e.name = int32_t(Be32(d, p + 12));
      e.size = Be32(d, p + 32);
      e.off = Be32(d, p + 36);
      size_t q = p + 40;
      const uint32_t cm = Be32(d, q);
      q += 4 + 12 * size_t(cm);
      q += 4;
      const uint32_t gn = Be32(d, q);
      q += 4 + 4 * size_t(gn) + 16;
      if (q > total_ || e.off + size_t(e.size) > d.size()) return why = "template: export entry", false;
      e.entry_bytes.assign(d.begin() + long(p), d.begin() + long(q));
      e.object.assign(d.begin() + long(e.off), d.begin() + long(e.off + e.size));
      exports_.push_back(std::move(e));
      p = q;
    }
    if (p != total_) return why = "template: export table end", false;
    return true;
  }

  // The class name of export k (its class is an import).
  std::string ClassOf(size_t k) const {
    const int32_t c = exports_[k].cls;
    if (c >= 0) return {};
    const size_t at = io_ + size_t(-c - 1) * 28;
    return NameText(int32_t(Be32(d_, at + 20)));
  }
  std::string NameOfExport(size_t k) const { return NameText(exports_[k].name); }
  // The export with this class (and name, when given); -1 if none.
  int Find(const std::string& cls, const std::string& name = {}) const {
    for (size_t k = 0; k < exports_.size(); ++k)
      if (ClassOf(k) == cls && (name.empty() || NameOfExport(k) == name)) return int(k);
    return -1;
  }
  size_t Exports() const { return exports_.size(); }

  int32_t NameIndex(const std::string& text) {
    for (size_t k = 0; k < names_.size(); ++k)
      if (names_[k].text == text) return int32_t(k);
    Name nm;
    nm.text = text;
    const uint8_t flags[8] = {0x00, 0x07, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(nm.flags, flags, 8);
    names_.push_back(nm);
    return int32_t(names_.size() - 1);
  }
  FName Name_(const std::string& text) { return FName{NameIndex(text), 0}; }
  std::string NameText(int32_t k) const { return k >= 0 && size_t(k) < names_.size() ? names_[size_t(k)].text : std::string(); }
  bool Is(const FName& n, const char* text) const { return n.number == 0 && NameText(n.index) == text; }

  // Object k's tagged properties (after its net index); false when they do not parse.
  bool Props(size_t k, std::vector<Prop>& out) const {
    const auto& b = exports_[k].object;
    size_t end;
    return ParseList(b, 4, b.size(), out, end) && end == b.size();
  }
  void SetProps(size_t k, const std::vector<Prop>& props) {
    auto& b = exports_[k].object;
    std::vector<uint8_t> o(b.begin(), b.begin() + 4);
    EmitList(o, props);
    b = std::move(o);
  }

  bool ParseList(const std::vector<uint8_t>& b, size_t p, size_t end, std::vector<Prop>& out, size_t& stop) const {
    const int32_t none = FindName("None");
    for (;;) {
      if (p + 8 > end) return false;
      Prop pr;
      pr.name = FName{int32_t(Be32(b, p)), int32_t(Be32(b, p + 4))};
      p += 8;
      if (pr.name.index == none && pr.name.number == 0) {
        stop = p;
        return true;
      }
      if (p + 16 > end) return false;
      pr.type = FName{int32_t(Be32(b, p)), int32_t(Be32(b, p + 4))};
      const uint32_t size = Be32(b, p + 8);
      pr.idx = int32_t(Be32(b, p + 12));
      p += 16;
      const std::string type = NameText(pr.type.index);
      if (type == "StructProperty") {
        if (p + 8 > end) return false;
        pr.struct_name = FName{int32_t(Be32(b, p)), int32_t(Be32(b, p + 4))};
        p += 8;
      }
      if (type == "BoolProperty") {
        if (p + 4 > end) return false;
        pr.boolval = int32_t(Be32(b, p));
        p += 4;
      } else {
        if (size > end - p) return false;
        pr.value.assign(b.begin() + long(p), b.begin() + long(p + size));
        p += size;
      }
      out.push_back(std::move(pr));
    }
  }
  void EmitList(std::vector<uint8_t>& o, const std::vector<Prop>& props) {
    for (const Prop& pr : props) {
      Put32(o, uint32_t(pr.name.index)), Put32(o, uint32_t(pr.name.number));
      Put32(o, uint32_t(pr.type.index)), Put32(o, uint32_t(pr.type.number));
      const bool is_bool = NameText(pr.type.index) == "BoolProperty";
      Put32(o, is_bool ? 0 : uint32_t(pr.value.size()));
      Put32(o, uint32_t(pr.idx));
      if (NameText(pr.type.index) == "StructProperty")
        Put32(o, uint32_t(pr.struct_name.index)), Put32(o, uint32_t(pr.struct_name.number));
      if (is_bool)
        Put32(o, uint32_t(pr.boolval));
      else
        o.insert(o.end(), pr.value.begin(), pr.value.end());
    }
    const FName none = Name_("None");
    Put32(o, uint32_t(none.index)), Put32(o, uint32_t(none.number));
  }

  std::vector<uint8_t> Write() const {
    std::vector<uint8_t> names;
    for (const Name& n : names_) {
      Put32(names, uint32_t(n.text.size() + 1));
      names.insert(names.end(), n.text.begin(), n.text.end());
      names.push_back(0);
      names.insert(names.end(), n.flags, n.flags + 8);
    }
    const uint32_t no = no_;
    const uint32_t io = no + uint32_t(names.size());
    const uint32_t eo = io + (eo_ - io_);
    uint32_t total = eo;
    for (const Export& e : exports_) total += uint32_t(e.entry_bytes.size());
    std::vector<uint8_t> out(d_.begin(), d_.begin() + long(no_));
    SetBe32(out, 8, total);
    SetBe32(out, so_, uint32_t(names_.size()));
    SetBe32(out, so_ + 4, no);
    SetBe32(out, so_ + 12, eo);
    SetBe32(out, so_ + 20, io);
    const uint32_t gens = Be32(out, gen_);
    for (uint32_t g = 0; g < gens && gen_ + 4 + 12 * g + 8 <= out.size(); ++g)
      SetBe32(out, gen_ + 4 + 12 * g + 4, uint32_t(names_.size()));
    out.insert(out.end(), names.begin(), names.end());
    out.insert(out.end(), d_.begin() + long(io_), d_.begin() + long(eo_));
    uint32_t off = total;
    for (const Export& e : exports_) {
      std::vector<uint8_t> entry = e.entry_bytes;
      SetBe32(entry, 32, uint32_t(e.object.size()));
      SetBe32(entry, 36, off);
      off += uint32_t(e.object.size());
      out.insert(out.end(), entry.begin(), entry.end());
    }
    for (const Export& e : exports_) out.insert(out.end(), e.object.begin(), e.object.end());
    return out;
  }

 private:
  struct Name {
    std::string text;
    uint8_t flags[8];
  };
  struct Export {
    size_t entry = 0;
    int32_t cls = 0, name = 0;
    uint32_t size = 0, off = 0;
    std::vector<uint8_t> entry_bytes, object;
  };
  int32_t FindName(const char* text) const {
    for (size_t k = 0; k < names_.size(); ++k)
      if (names_[k].text == text) return int32_t(k);
    return -1;
  }
  std::vector<uint8_t> d_;
  uint32_t nc_ = 0, no_ = 0, ec_ = 0, eo_ = 0, ic_ = 0, io_ = 0, total_ = 0;
  size_t so_ = 0, gen_ = 0;
  std::vector<Name> names_;
  std::vector<Export> exports_;
};

// Property list editing (as the prototype: replaced in place, or added before Owner / OwnerName / OwnerClass).
class Editor {
 public:
  Editor(Package& pkg, std::vector<Prop>& props) : pkg_(pkg), props_(props) {}
  Prop* Get(const char* name) {
    for (Prop& pr : props_)
      if (pkg_.Is(pr.name, name) && pr.idx == 0) return &pr;
    return nullptr;
  }
  void Set(const char* name, const char* type, std::vector<uint8_t> value) {
    if (Prop* pr = Get(name)) {
      pr->value = std::move(value);
      return;
    }
    Prop pr;
    pr.name = pkg_.Name_(name);
    pr.type = pkg_.Name_(type);
    pr.value = std::move(value);
    auto at = std::find_if(props_.begin(), props_.end(), [&](const Prop& p) {
      return pkg_.Is(p.name, "Owner") || pkg_.Is(p.name, "OwnerName") || pkg_.Is(p.name, "OwnerClass");
    });
    props_.insert(at, std::move(pr));
  }
  int32_t Object(const char* name) {
    Prop* pr = Get(name);
    return pr && pr->value.size() == 4 ? int32_t(Be32(pr->value, 0)) : 0;
  }

 private:
  Package& pkg_;
  std::vector<Prop>& props_;
};

std::vector<uint8_t> IntArray(const std::vector<int32_t>& v) {
  std::vector<uint8_t> b;
  Put32(b, uint32_t(v.size()));
  for (int32_t x : v) Put32(b, uint32_t(x));
  return b;
}
Prop IntProp(Package& pkg, const char* name, int32_t v) {
  Prop p;
  p.name = pkg.Name_(name);
  p.type = pkg.Name_("IntProperty");
  p.value = Int(v);
  return p;
}
Prop ArrProp(Package& pkg, const char* name, const std::vector<int32_t>& v) {
  Prop p;
  p.name = pkg.Name_(name);
  p.type = pkg.Name_("ArrayProperty");
  p.value = IntArray(v);
  return p;
}

bool Fill(Package& pkg, const Le1Save& s, std::string& why) {
  auto edit = [&](int k, const char* what, auto&& fn) -> bool {
    if (k < 0 || size_t(k) >= pkg.Exports()) return why = std::string("template: no ") + what, false;
    std::vector<Prop> props;
    if (!pkg.Props(size_t(k), props)) return why = std::string("template: the properties of ") + what, false;
    Editor e(pkg, props);
    if (!fn(e)) return false;
    pkg.SetProps(size_t(k), props);
    return true;
  };
  int32_t player = 0, appearance = 0, squad = 0, inventory = 0;
  bool ok = edit(pkg.Find("BioSaveGame", "CurrentGame"), "CurrentGame", [&](Editor& e) {
    player = e.Object("m_Player");
    appearance = e.Object("m_PlayerAppearance");
    std::string map = s.base_level.Ascii();
    for (char& c : map) c = char(std::tolower(static_cast<unsigned char>(c)));
    e.Set("m_sMapName", "StrProperty", Str(U16(map)));
    e.Set("m_sCharacterID", "StrProperty", Str(s.character_id.chars));
    e.Set("m_fPlaythroughTime", "FloatProperty", Flt(float(s.seconds_played)));
    e.Set("m_Henchmen", "ArrayProperty", Int(0));
    return true;
  });
  if (!ok) return false;
  if (player <= 0 || appearance <= 0) return why = "template: CurrentGame has no player / appearance", false;
  ok = edit(player - 1, "the player", [&](Editor& e) {
    squad = e.Object("m_Squad");
    e.Set("m_XPLevel", "IntProperty", Int(s.level));
    e.Set("m_FirstName", "StrProperty", Str(s.first_name.chars));
    e.Set("m_LastName", "StringRefProperty", Int(s.last_name));
    e.Set("m_LocalizedClassName", "StringRefProperty", Int(s.class_name));
    e.Set("m_AutoLevelUpTemplateID", "IntProperty", Int(s.auto_levelup));
    e.Set("m_SpecializationBonusID", "IntProperty", Int(s.specialization));
    e.Set("m_BackgroundOrigin", "ByteProperty", {s.origin});
    e.Set("m_BackgroundNotoriety", "ByteProperty", {s.notoriety});
    e.Set("m_SpectreRank", "ByteProperty", {s.spectre_rank});
    if (s.attribute_primary) e.Set("m_AttributePrimary", "ByteProperty", {s.attribute_primary});
    e.Set("m_AttributeSecondary", "ByteProperty", {s.attribute_secondary});
    e.Set("m_TalentPoints", "IntProperty", Int(s.talent_points));
    e.Set("m_Stamina", "IntProperty", Int(s.stamina));
    e.Set("m_Focus", "IntProperty", Int(s.focus));
    e.Set("m_Precision", "IntProperty", Int(s.precision));
    e.Set("m_Coordination", "IntProperty", Int(s.coordination));
    e.Set("m_HealthPerLevel", "FloatProperty", Flt(s.health_per_level));
    e.Set("m_StabilityCurrent", "FloatProperty", Flt(s.stability));
    e.Set("m_HealthCurrent", "FloatProperty", Flt(s.health));
    e.Set("m_ShieldCurrent", "FloatProperty", Flt(s.shield));
    std::vector<uint8_t> simple = Int(int32_t(s.simple.size()));
    for (const SimpleTalent& t : s.simple)
      pkg.EmitList(simple, {IntProp(pkg, "m_TalentID", t.id), IntProp(pkg, "m_Ranks", t.rank)});
    e.Set("m_aSimpleTalents", "ArrayProperty", std::move(simple));
    std::vector<uint8_t> complex = Int(int32_t(s.complex.size()));
    for (const ComplexTalent& t : s.complex)
      pkg.EmitList(complex, {IntProp(pkg, "m_TalentID", t.id), IntProp(pkg, "m_Ranks", t.rank),
                             IntProp(pkg, "m_MaxRank", t.max_rank), IntProp(pkg, "m_LevelOffset", t.level_offset),
                             IntProp(pkg, "m_LevelsPerRank", t.levels_per_rank),
                             IntProp(pkg, "m_VisualOrder", t.visual_order),
                             ArrProp(pkg, "m_PrereqTalentIDArray", t.prereq_ids),
                             ArrProp(pkg, "m_PrereqTalentRankArray", t.prereq_ranks)});
    e.Set("m_aComplexTalents", "ArrayProperty", std::move(complex));
    return true;
  });
  if (!ok) return false;
  if (squad <= 0) return why = "template: the player has no squad object", false;
  ok = edit(squad - 1, "the squad", [&](Editor& e) {
    inventory = e.Object("m_Inventory");
    e.Set("m_nSquadExperience", "IntProperty", Int(int32_t(s.xp)));
    e.Set("m_nSquadLevel", "IntProperty", Int(s.level));
    e.Set("m_nMaxLevel", "IntProperty", Int(std::max(60, s.level)));
    return true;
  });
  if (!ok) return false;
  if (inventory <= 0) return why = "template: the squad has no inventory object", false;
  ok = edit(inventory - 1, "the inventory", [&](Editor& e) {
    e.Set("m_nResourceCredits", "IntProperty", Int(std::min(s.credits, 9999999)));  // ME1's cap
    e.Set("m_nResourceGrenades", "IntProperty", Int(int32_t(s.grenades)));
    e.Set("m_fResourceMedigel", "FloatProperty", Flt(float(s.medigel)));
    e.Set("m_fResourceSalvage", "FloatProperty", Flt(s.omnigel));
    return true;
  });
  if (!ok) return false;
  return edit(appearance - 1, "the appearance", [&](Editor& e) {
    e.Set("m_oMorphFace", "ObjectProperty", Int(0));  // the default face: no morph object
    return true;
  });
}

// Cyrillic -> Latin (Russian / Ukrainian / Belarusian letters; others kept). Capitals stay capitals.
std::u16string Transliterate(const std::u16string& t) {
  static const char* const kLower[] = {"a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
                                       "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu",
                                       "ya"};  // U+0430..U+044F
  std::u16string out;
  for (char16_t c : t) {
    const bool upper = c >= 0x0410 && c <= 0x042F;
    const char* m = nullptr;
    if (upper || (c >= 0x0430 && c <= 0x044F)) m = kLower[upper ? c - 0x0410 : c - 0x0430];
    else if (c == 0x0401 || c == 0x0451) m = "yo";
    else if (c == 0x0404 || c == 0x0454) m = "ye";
    else if (c == 0x0406 || c == 0x0456 || c == 0x0407 || c == 0x0457) m = "i";
    else if (c == 0x040E || c == 0x045E) m = "u";
    else if (c == 0x0490 || c == 0x0491) m = "g";
    if (!m) {
      out += c == 0x2013 || c == 0x2014 ? u'-' : c;  // dashes the fonts may lack
      continue;
    }
    const bool cap = upper || c == 0x0401 || c == 0x0404 || c == 0x0406 || c == 0x0407 || c == 0x040E || c == 0x0490;
    for (size_t k = 0; m[k]; ++k) out += char16_t(cap && k == 0 ? std::toupper(static_cast<unsigned char>(m[k])) : m[k]);
  }
  return out;
}
bool OnlyLatin1(const std::u16string& t) {
  return std::all_of(t.begin(), t.end(), [](char16_t c) { return c < 0x100; });
}
// The first name as the English game can draw it.
std::u16string LatinFirstName(const std::u16string& name, bool female) {
  if (OnlyLatin1(name)) return name;
  if (name == u"Джон" || name == u"Джейн")  // Джон / Джейн: the default names
    return female ? u"Jane" : u"John";
  return Transliterate(name);
}
std::string Display(const Le1Save& s, const Options& o) {
  if (!o.latin_names || OnlyLatin1(s.display_name.chars)) return s.display_name.Utf8();
  return Utf16ToUtf8(LatinFirstName(s.first_name.chars, s.female)) + " Shepard - Level " + std::to_string(s.level);
}

const char* ClassName(uint8_t c) {
  static const char* const kNames[] = {"Soldier", "Engineer", "Adept", "Infiltrator", "Sentinel", "Vanguard"};
  return c < 6 ? kNames[c] : "?";
}

}  // namespace

bool LooksLikeLe1(const std::vector<uint8_t>& file) {
  return file.size() >= 4 && file[0] == 0xC1 && file[1] == 0x83 && file[2] == 0x2A && file[3] == 0x9E;
}

bool DisplayName(const std::vector<uint8_t>& file, InflateFn inflate, const Options& options, std::string& display) {
  std::vector<uint8_t> data;
  std::string why;
  Le1Save s;
  if (!Unpack(file, inflate, data, why) || !Parse(data, s, why)) return false;
  display = Display(s, options);
  return true;
}

bool Convert(const std::vector<uint8_t>& file, InflateFn inflate, const Options& options, Converted& out,
             std::string& why) {
  std::vector<uint8_t> data;
  Le1Save s;
  if (!Unpack(file, inflate, data, why) || !Parse(data, s, why)) return false;
  const std::u16string original_name = s.first_name.chars;
  if (options.latin_names) s.first_name.chars = LatinFirstName(s.first_name.chars, s.female);
  if (s.female) return why = "female characters are not supported yet", false;
  if (s.head_morph) return why = "custom faces are not supported yet (the default face is)", false;
  if (kTemplateSize == 0)
    return why = "this build has no ME1 template save (app/me2_src/me2_me1_le1_template.inc, see me2_me1_le1.cpp)", false;
  std::vector<uint8_t> tpl(kTemplateSize);
  if (!inflate(kTemplateZ, sizeof(kTemplateZ), tpl.data(), tpl.size())) return why = "template does not inflate", false;
  Package pkg;
  if (!pkg.Load(tpl, why) || !Fill(pkg, s, why)) return false;
  out.player = pkg.Write();
  out.state = StateSav(s);
  out.display = Display(s, options);
  out.summary = s.first_name.Utf8() + (original_name != s.first_name.chars ? " (was " + Utf16ToUtf8(original_name) + ")" : "") + ", " + ClassName(s.player_class) + ", level " + std::to_string(s.level) +
                ", origin " + std::to_string(s.origin) + ", notoriety " + std::to_string(s.notoriety) + ", " +
                std::to_string(s.quests.size()) + " quests, " + std::to_string(s.ints.size()) + " plot ints";
  return true;
}

}  // namespace me2::me1_import::le1
