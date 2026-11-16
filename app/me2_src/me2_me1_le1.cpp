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
// First pass (checked against a Python prototype byte for byte): male characters with the default face. 2026-11-15:
// female characters and LE1 head morphs (-> the template's BioMorphFaceSaveObject), see FillFace.
#include "me2_me1_le1.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
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

using Vec3 = std::array<float, 3>;
// LE1's head morph (trilogy-save-editor HeadMorph): object paths as text ("BIOG_HMF_HIR_PRO.Sexy.HMF_HIR_Sxy_Diff").
struct HeadMorph {
  Text hair;
  std::vector<Text> accessories;
  std::vector<std::pair<Text, float>> features;
  std::vector<std::pair<Text, Vec3>> bones;
  std::vector<Vec3> lod0;  // LE1 saves LOD0 only (LOD1-3 are empty)
  std::vector<std::pair<Text, float>> scalars;
  std::vector<std::pair<Text, std::array<float, 4>>> vectors;
  std::vector<std::pair<Text, Text>> textures;
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
  HeadMorph morph;
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
Vec3 V3(Reader& r) {
  Vec3 v;
  for (float& f : v) f = r.F32();
  return v;
}
void ReadHeadMorph(Reader& r, HeadMorph& m) {
  m.hair = r.Str();
  for (uint32_t k = r.Count(); k-- && r.ok();) m.accessories.push_back(r.Str());
  for (uint32_t k = r.Count(); k-- && r.ok();) {
    Text t = r.Str();
    m.features.emplace_back(std::move(t), r.F32());
  }
  for (uint32_t k = r.Count(); k-- && r.ok();) {
    Text t = r.Str();
    m.bones.emplace_back(std::move(t), V3(r));
  }
  for (uint32_t k = r.Count(); k-- && r.ok();) m.lod0.push_back(V3(r));
  for (int lod = 1; lod < 4; ++lod) r.Skip(size_t(r.Count()) * 12);
  for (uint32_t k = r.Count(); k-- && r.ok();) {
    Text t = r.Str();
    m.scalars.emplace_back(std::move(t), r.F32());
  }
  for (uint32_t k = r.Count(); k-- && r.ok();) {
    Text t = r.Str();
    std::array<float, 4> c;
    for (float& f : c) f = r.F32();
    m.vectors.emplace_back(std::move(t), c);
  }
  for (uint32_t k = r.Count(); k-- && r.ok();) {
    Text t = r.Str();
    m.textures.emplace_back(std::move(t), r.Str());
  }
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
  if (s.head_morph) ReadHeadMorph(r, s.morph);
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
    if (eo_ - io_ != 28 * ic_) return why = "template: import table", false;
    for (uint32_t k = 0; k < ic_; ++k) {
      Import im;
      for (int w = 0; w < 7; ++w) im.w[w] = int32_t(Be32(d, io_ + 28 * k + 4 * w));
      imports_.push_back(im);
    }
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
    if (c >= 0 || size_t(-c) > imports_.size()) return {};
    return NameText(imports_[size_t(-c - 1)].w[5]);
  }

  // ME2 (2026-11-15): the import for an object path ("Pkg.Group.Object", class cls_pkg.cls; the package and groups as
  // Core.Package, as the template's own rows): the existing row, else new rows at the end (existing references stay
  // valid). Its reference (negative).
  int32_t ImportRef(const std::string& path, const char* cls_pkg, const char* cls) {
    int32_t outer = 0;
    size_t at = 0;
    while (at <= path.size()) {
      size_t dot = path.find('.', at);
      if (dot == std::string::npos) dot = path.size();
      const bool last = dot == path.size();
      const int32_t class_pkg = NameIndex(last ? cls_pkg : "Core");  // (this order: the names it may add)
      const int32_t class_name = NameIndex(last ? cls : "Package");
      const FName name = Value(path.substr(at, dot - at));
      int32_t found = 0;
      for (size_t k = 0; k < imports_.size() && !found; ++k) {
        const Import& im = imports_[k];
        if (im.w[4] == outer && im.w[5] == name.index && im.w[6] == name.number && im.w[2] == class_name)
          found = -int32_t(k + 1);
      }
      if (!found) {
        Import im;
        const int32_t w[7] = {class_pkg, 0, class_name, 0, outer, name.index, name.number};
        std::memcpy(im.w, w, sizeof(w));
        imports_.push_back(im);
        found = -int32_t(imports_.size());
      }
      outer = found;
      at = dot + 1;
    }
    return outer;
  }

  // An FName as UE3 makes it from text: "Name_12" = "Name" number 13, but no split for a leading zero ("Name_01").
  FName Value(const std::string& text) {
    const size_t us = text.rfind('_');
    if (us != std::string::npos && us > 0 && us + 1 < text.size() && text.size() - us - 1 < 10) {
      const std::string digits = text.substr(us + 1);
      const bool all_digits = std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
      if (all_digits && (digits.size() == 1 || digits[0] != '0'))
        return FName{NameIndex(text.substr(0, us)), int32_t(std::stoul(digits)) + 1};
    }
    return FName{NameIndex(text), 0};
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
    const uint32_t eo = io + 28 * uint32_t(imports_.size());
    uint32_t total = eo;
    for (const Export& e : exports_) total += uint32_t(e.entry_bytes.size());
    std::vector<uint8_t> out(d_.begin(), d_.begin() + long(no_));
    SetBe32(out, 8, total);
    SetBe32(out, so_, uint32_t(names_.size()));
    SetBe32(out, so_ + 4, no);
    SetBe32(out, so_ + 12, eo);
    SetBe32(out, so_ + 16, uint32_t(imports_.size()));
    SetBe32(out, so_ + 20, io);
    const uint32_t gens = Be32(out, gen_);
    for (uint32_t g = 0; g < gens && gen_ + 4 + 12 * g + 8 <= out.size(); ++g)
      SetBe32(out, gen_ + 4 + 12 * g + 4, uint32_t(names_.size()));
    out.insert(out.end(), names.begin(), names.end());
    for (const Import& im : imports_)
      for (int32_t w : im.w) Put32(out, uint32_t(w));
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
  struct Import {
    int32_t w[7];  // class package FName, class FName, outer, object FName
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
  std::vector<Import> imports_;
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
  void Set(const char* name, const char* type, std::vector<uint8_t> value, const char* struct_name = nullptr) {
    if (Prop* pr = Get(name)) {
      pr->value = std::move(value);
      return;
    }
    Prop pr;
    pr.name = pkg_.Name_(name);
    pr.type = pkg_.Name_(type);
    if (struct_name) pr.struct_name = pkg_.Name_(struct_name);
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
    e.Set("m_oMorphFace", "ObjectProperty", Int(0));  // the default face: no morph object (FillFace may set one)
    return true;
  });
}

// ---------------------------------------------------------------------------------------------------------------------
// ME2 (2026-11-15): gender and the LE1 head morph.
//
// ME2 (SFXGame SFXLegacyData): InitializeSpawnableImportedPlayer sets NewPlayer.bIsFemale = (m_Gender == 1), m_Gender
// copied from the player's BioPawnBehaviorSaveObject (ME1's EGender: the template's female henchman has 1);
// ApplyImportedMorphFaceFromSaveObject loads its own FemaleBaseFace / MaleBaseFace (BIOG_MORPH_FACE.Player_Base_*),
// runs BioMorphFaceSaveObject.LoadObject on it (hair mesh, other meshes, features, BioMaterialOverrideSaveObject,
// final skeleton; LODn vertex positions while the save's m_vPosition has an entry for that vertex) and sets
// m_oBaseHead = None. The template's own BioMorphFaceSaveObject / BioMaterialOverrideSaveObject are refilled:
//   m_oHairMesh / m_oOtherMeshes: imports of LE1's paths (all of them exist in ME2 Xbox's BIOG_HMF_HIR_PRO,
//   BIOG_HMF_HED_PROMorph_R, BIOG_HMM_HED_PROMorph); textures LE1 leaves None are left out (the material's own);
//   LOD0 = LE1's; LOD1 = LE1's LOD0 through me2_me1_le1_faces.inc (ME2's base LOD1 vertex -> nearest LOD0 vertex +
//   offset; generated by tools/me2_me1_face_tables.py from the game's BIOG_MORPH_FACE; on an ME1 Xbox save it lands a
//   mean 0.027 from ME1's own LOD1, the base LOD1 0.23); LOD2 empty = ME2's base face,
//   as ME1 itself saves it (the template's LOD2 equals ME2's base male LOD2 exactly). ME2's base faces: female 2232
//   LOD0 vertices (= LE1's female head), male 2294.
#if __has_include("me2_me1_le1_faces.inc")
#include "me2_me1_le1_faces.inc"
#define ME2_LE1_FACE_TABLES 1
#endif

struct LodTable {
  uint32_t lod0 = 0, lod1 = 0;
  const uint16_t* nearest = nullptr;
  const uint32_t* offset = nullptr;
};
LodTable FaceTable(bool female) {
  LodTable t;
#ifdef ME2_LE1_FACE_TABLES
  if (female) {
    t = {kFaceFemaleLod0, kFaceFemaleLod1, kFaceFemaleNearest, kFaceFemaleOffset};
  } else {
    t = {kFaceMaleLod0, kFaceMaleLod1, kFaceMaleNearest, kFaceMaleOffset};
  }
#else
  (void)female;
#endif
  return t;
}

std::vector<uint8_t> NameValue(Package& pkg, const std::string& text) {
  const FName n = pkg.Value(text);
  std::vector<uint8_t> b;
  Put32(b, uint32_t(n.index)), Put32(b, uint32_t(n.number));
  return b;
}
Prop MakeProp(Package& pkg, const char* name, const char* type, std::vector<uint8_t> value,
              const char* struct_name = nullptr) {
  Prop p;
  p.name = pkg.Name_(name);
  p.type = pkg.Name_(type);
  if (struct_name) p.struct_name = pkg.Name_(struct_name);
  p.value = std::move(value);
  return p;
}
std::vector<uint8_t> Floats(const float* f, size_t n) {
  std::vector<uint8_t> b;
  for (size_t k = 0; k < n; ++k) PutF(b, f[k]);
  return b;
}
// A VertexBuffer struct: { m_vPosition: array of FVector (raw) }.
std::vector<uint8_t> VertexBuffer(Package& pkg, const std::vector<Vec3>& v) {
  std::vector<uint8_t> body = Int(int32_t(v.size()));
  for (const Vec3& p : v) PutF(body, p[0]), PutF(body, p[1]), PutF(body, p[2]);
  std::vector<uint8_t> b;
  pkg.EmitList(b, {MakeProp(pkg, "m_vPosition", "ArrayProperty", std::move(body))});
  return b;
}

// ME2 (2026-11-15): LE1's skin tones are not ME1's. ME1 / ME2's SkinTone slider (BioP_Char's
// SFXMorphFaceFrontEndDataSource_0 male / _1 female, BioMorphFaceFESliderColour m_acColours, FColor sRGB) runs along one
// warm curve, stored linear (UE3 pow 2.2: the female palette's 4th entry is ME2's default female SkinTone 0.759 0.349
// 0.208 exactly); LE1's creator (its own face textures) gives desaturated tones off that curve, e.g. 0.730 0.542 0.459
// = sRGB 221 193 179, which on ME2's near-white base diffuse rendered pale grey. Mapped by luminance onto the palette:
// LE1's lightness, ME2's hue.
std::array<float, 4> MapSkinTone(const std::array<float, 4>& c, bool female) {
  static const uint8_t kFemale[6][3] = {{126, 82, 59}, {163, 104, 73}, {192, 128, 95}, {225, 158, 125}, {239, 177, 150}, {252, 203, 175}};
  static const uint8_t kMale[6][3] = {{126, 82, 59}, {163, 104, 73}, {192, 128, 95}, {225, 158, 125}, {242, 179, 153}, {252, 189, 162}};
  const auto& pal = female ? kFemale : kMale;
  auto lin = [](uint8_t v) { return std::pow(v / 255.0, 2.2); };
  auto lum = [](double r, double g, double b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; };
  double e[6][3], l[6];
  for (int k = 0; k < 6; ++k) {
    for (int ch = 0; ch < 3; ++ch) e[k][ch] = lin(pal[k][ch]);
    l[k] = lum(e[k][0], e[k][1], e[k][2]);
  }
  const double want = lum(c[0], c[1], c[2]);
  int k = 0;
  double t = 0;
  if (want <= l[0]) {
    k = 0, t = 0;
  } else if (want >= l[5]) {
    k = 4, t = 1;
  } else {
    while (k < 4 && want > l[k + 1]) ++k;
    t = (want - l[k]) / (l[k + 1] - l[k]);
  }
  std::array<float, 4> out;
  for (int ch = 0; ch < 3; ++ch) out[ch] = float(e[k][ch] + t * (e[k + 1][ch] - e[k][ch]));
  out[3] = c[3];
  return out;
}

// ME2 (2026-11-15): makeup strengths. LE1 keeps ME1 / ME2's makeup colour presets (Emily's blush, lips, eye shadow and
// brow colours are exact entries of BioP_Char's female SliderColour lists) but stores stronger tints than ME2's creator
// ever sets (blush 1.14, brow 2.0; ME2's at most 1.0): heavy red cheeks. Each preset index pairs a colour with a strength
// (the SliderScalar list of the same slider); a colour found among the presets takes ME2's strength for it, any other
// is clamped to ME2's largest.
struct MakeupSlider {
  const char* colour;
  const char* scalar;
  std::vector<std::array<uint8_t, 3>> presets;  // sRGB
  std::vector<float> strengths;
};
const std::vector<MakeupSlider>& MakeupSliders() {
  static const std::vector<MakeupSlider> k = {
      {"HED_Blush_Vector", "HED_Blush_Scalar",
       {{0, 0, 0}, {217, 166, 123}, {228, 176, 175}, {245, 168, 156}, {219, 88, 88}, {187, 94, 79}, {166, 75, 44}, {183, 115, 112}, {212, 104, 83}},
       {0.0f, 0.75f, 0.75f, 0.9f, 0.65f, 0.75f, 0.85f, 1.0f, 1.0f}},
      {"HED_Lips_Tint_Vector", "HED_Lips_Tint_Scalar",
       {{0, 0, 0}, {159, 107, 100}, {165, 71, 84}, {133, 91, 62}, {150, 47, 52}, {43, 22, 33}, {0, 0, 0}},
       {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f}},
      {"HED_EyeShadow_Tint_Vector", "HED_EyeShadow_Tint_Scalar",
       {{0, 0, 0}, {64, 54, 53}, {88, 69, 62}, {108, 67, 28}, {54, 23, 22}, {43, 22, 33}, {20, 20, 30}, {60, 50, 76}, {80, 47, 58}, {109, 57, 38}, {55, 60, 28}},
       {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.55f, 0.8f, 0.8f, 1.0f, 0.8f}},
      {"HED_Brow_Tint_Vector", "HED_Brow_Tint_Scalar",
       {{0, 0, 0}, {150, 120, 112}, {184, 143, 137}, {199, 145, 91}, {150, 67, 50}, {30, 28, 30}, {0, 0, 0}, {192, 85, 130}, {205, 65, 112}, {161, 102, 34}, {121, 200, 60}},
       {0.0f, 1.0f, 0.8f, 0.1f, 0.9f, 0.55f, 0.0f, 0.35f, 0.5f, 0.9f, 0.3f}},
  };
  return k;
}
// The strength ME2 would use for this colour (preset match within 1 per channel), or -1 for "clamp only".
float MakeupStrength(const MakeupSlider& sl, const std::array<float, 4>& c) {
  int rgb[3];
  for (int ch = 0; ch < 3; ++ch) rgb[ch] = int(std::lround(255.0 * std::pow(std::max(0.0f, c[ch]), 1.0 / 2.2)));
  for (size_t k = 0; k < sl.presets.size(); ++k) {
    bool same = true;
    for (int ch = 0; ch < 3; ++ch) same = same && std::abs(rgb[ch] - int(sl.presets[k][ch])) <= 1;
    if (same) return sl.strengths[k];
  }
  return -1.0f;
}

// ME2 (2026-11-15): hair colour. ME2's HED_Hair_Colour_Vector presets (BioP_Char, sRGB): blonde 250 211 154, 159 118 80,
// 96 52 43, 127 83 62, 96 69 52, 64 52 48, 57 55 55: no red. LE1 offers vivid ones (Emily: 230 73 56, saturation 0.76,
// brightness 0.90), neon on ME2's hair textures. Kept as a hue, toned into ME2's range (the user's choice, 2026-11-15):
// saturation at most 0.55 (96 52 43, ME2's most saturated), and a saturated colour (above 0.45) at most as bright as
// ME2's brightest saturated one (159 118 80: 0.62); only the pale blonde is brighter. Emily -> 159 80 71 (copper red).
std::array<float, 4> MapHairColour(const std::array<float, 4>& c, bool& changed) {
  constexpr double kMaxSat = (96.0 - 43.0) / 96.0, kSatBright = 0.45, kMaxBright = 159.0 / 255.0;
  double x[3];
  for (int ch = 0; ch < 3; ++ch) x[ch] = std::pow(std::max(0.0, double(c[ch])), 1.0 / 2.2);
  const double mx = std::max({x[0], x[1], x[2]}), mn = std::min({x[0], x[1], x[2]});
  changed = false;
  if (mx <= 0) return c;
  const double sat = (mx - mn) / mx;
  constexpr double kSlack = 0.005;  // ME2's own presets sit on the limits: left as they are
  const bool too_sat = sat > kMaxSat + kSlack, too_bright = sat > kSatBright && mx > kMaxBright + kSlack;
  if (!too_sat && !too_bright) return c;
  const double new_sat = too_sat ? kMaxSat : sat, new_v = too_bright ? kMaxBright : mx;
  changed = true;
  const double new_mn = new_v * (1.0 - new_sat);
  std::array<float, 4> out;
  for (int ch = 0; ch < 3; ++ch) {
    const double f = mx > mn ? (x[ch] - mn) / (mx - mn) : 0.0;  // the hue: each channel's place between min and max
    out[ch] = float(std::pow(new_mn + (new_v - new_mn) * f, 2.2));
  }
  out[3] = c[3];
  return out;
}

// ME2 (2026-11-15): LE1 hair and accessory paths ME2 does not have. The Legendary Edition renamed some of ME1's styles
// ("PROCustom" + the old group: LE1 BIOG_HMF_HIR_PRO.PROCustomShort02.HMF_HIR_PROCustomShort02_MDL is ME1 / ME2's
// Short02.HMF_HIR_PROShort_MDL; a save with it imported without hair). ME2's hair packages as the game ships them
// (BIOG_HMF_HIR_PRO.xxx and BIOG_HMM_HIR_PRO_R.xxx, decompressed with tools/me2_package.py): every SkeletalMesh and
// every top-level group. A mesh ME2 has is kept; otherwise the group without "PROCustom" (any case), the mesh of that
// group whose name is closest; LE1's male package BIOG_HMM_HIR_PRO is ME2's BIOG_HMM_HIR_PRO_R. A texture in a group
// ME2 does not have is left out (the mesh's own material has its textures). Anything else stays as LE1 wrote it.
constexpr const char* kMe2HairMeshes[] = {
    "BIOG_HMF_HIR_PRO.Afro.HMF_HIR_Afr_MDL", "BIOG_HMF_HIR_PRO.Ashley.HMF_HIR_PROAshley_MDL",
    "BIOG_HMF_HIR_PRO.Beanie.HMF_HAT_Bn1_MDL", "BIOG_HMF_HIR_PRO.Beanie02.HMF_HAT_Bn2_MDL",
    "BIOG_HMF_HIR_PRO.Beret.HMF_HAT_Brt_MDL", "BIOG_HMF_HIR_PRO.Cap.HMF_HAT_Cap_MDL",
    "BIOG_HMF_HIR_PRO.Classy.HMF_HIR_Cls_MDL", "BIOG_HMF_HIR_PRO.Cosmo.HMF_HAT_Cos_MDL",
    "BIOG_HMF_HIR_PRO.Cuban.HMF_HAT_Cub_MDL", "BIOG_HMF_HIR_PRO.Cute.HMF_HIR_Cte_MDL",
    "BIOG_HMF_HIR_PRO.Cyberbabe.HMF_HIR_Cyb_MDL", "BIOG_HMF_HIR_PRO.Cyclops.HMF_HAT_CYC_MDL",
    "BIOG_HMF_HIR_PRO.Earrings.HMF_HAT_Er1_MDL", "BIOG_HMF_HIR_PRO.Earrings.HMF_HAT_Er2_MDL",
    "BIOG_HMF_HIR_PRO.Earrings.HMF_HAT_Er3_MDL", "BIOG_HMF_HIR_PRO.Earrings.HMF_HAT_Er4_MDL",
    "BIOG_HMF_HIR_PRO.Earrings.HMF_HAT_Er5_MDL", "BIOG_HMF_HIR_PRO.Earrings.HMF_HAT_Er6_MDL",
    "BIOG_HMF_HIR_PRO.Earrings.HMF_HAT_Er7_MDL", "BIOG_HMF_HIR_PRO.GlassesFull.HMF_HAT_GLA_MDL",
    "BIOG_HMF_HIR_PRO.GlassesHalf.HMF_HAT_GLH_MDL", "BIOG_HMF_HIR_PRO.HairClip.HMF_HAT_Hcp_MDL",
    "BIOG_HMF_HIR_PRO.HighBun.HMF_HIR_Hbn_MDL", "BIOG_HMF_HIR_PRO.Iconbun.HMF_HIR_IBun_MDL",
    "BIOG_HMF_HIR_PRO.LowBun.HMF_HIR_Lbn_MDL", "BIOG_HMF_HIR_PRO.LowBun.HMF_HIR_Mbu_MDL",
    "BIOG_HMF_HIR_PRO.MidBun.HMF_HIR_Mbn_MDL", "BIOG_HMF_HIR_PRO.Mohawk.HMF_HIR_Mhk_MDL",
    "BIOG_HMF_HIR_PRO.Mom.HMF_HIR_Mom_MDL", "BIOG_HMF_HIR_PRO.Moto.HMF_HAT_MTR_MDL",
    "BIOG_HMF_HIR_PRO.MotoGlasses.HMF_HAT_MTG_MDL", "BIOG_HMF_HIR_PRO.Officer.HMF_HAT_Off_MDL",
    "BIOG_HMF_HIR_PRO.PROCustomAfro.HMF_HIR_PROCustom_Afr_MDL", "BIOG_HMF_HIR_PRO.PROCustomCute.HMF_HIR_PROCustom_Cute_MDL",
    "BIOG_HMF_HIR_PRO.PROCustomSexy.HMF_HIR_PROCustom_Sexy_MDL", "BIOG_HMF_HIR_PRO.PonyTail.HMF_HIR_Ptl_MDL",
    "BIOG_HMF_HIR_PRO.Sexy.HMF_HIR_Sxy_MDL", "BIOG_HMF_HIR_PRO.Short02.HMF_HIR_PROShort_MDL",
    "BIOG_HMM_HIR_PRO_R.Beanie01.HMM_HAT_Bn1_MDL", "BIOG_HMM_HIR_PRO_R.Beanie02.HMM_HAT_Bn2_MDL",
    "BIOG_HMM_HIR_PRO_R.Beret.HMM_HAT_Brt_MDL", "BIOG_HMM_HIR_PRO_R.Cap.HMM_HAT_Cap_MDL",
    "BIOG_HMM_HIR_PRO_R.Cosmo.HMM_HAT_Cos_MDL", "BIOG_HMM_HIR_PRO_R.Cuban.HMM_HAT_Cub_MDL",
    "BIOG_HMM_HIR_PRO_R.Cyclops.HMM_HAT_CYC_MDL", "BIOG_HMM_HIR_PRO_R.FormalSpikes.HMM_HIR_Fsk_MDL",
    "BIOG_HMM_HIR_PRO_R.GlassesFull.HMM_HAT_Glf_MDL", "BIOG_HMM_HIR_PRO_R.GlassesHalf.HMM_HAT_GLH_MDL",
    "BIOG_HMM_HIR_PRO_R.Kaiden.HMM_HIR_Kai_MDL", "BIOG_HMM_HIR_PRO_R.Mohawk.HMM_HIR_Mhk_MDL",
    "BIOG_HMM_HIR_PRO_R.Moto.HMM_HAT_MTR_MDL", "BIOG_HMM_HIR_PRO_R.MotoGlasses.HMM_HAT_MTG_MDL",
    "BIOG_HMM_HIR_PRO_R.Officer.HMM_HAT_Off_MDL", "BIOG_HMM_HIR_PRO_R.RollinsSpike.HMM_HIR_Rsk_MDL",
    "BIOG_HMM_HIR_PRO_R.SargeSpike.HMM_HIR_Ssk_01_MDL", "BIOG_HMM_HIR_PRO_R.SargeSpike.HMM_HIR_Ssk_02_MDL",
    "BIOG_HMM_HIR_PRO_R.Short01.HMM_HIR_PROShort01_MDL", "BIOG_HMM_HIR_PRO_R.Short02.HMM_HIR_PROShort02_MDL",
    "BIOG_HMM_HIR_PRO_R.Short03.HMM_HIR_PROShort03_MDL", "BIOG_HMM_HIR_PRO_R.Visor.HMM_HAT_VSR_MDL",
};
constexpr const char* kMe2HairGroups[] = {
    "BIOG_HMF_HIR_PRO.Afro", "BIOG_HMF_HIR_PRO.Ashley", "BIOG_HMF_HIR_PRO.Beanie", "BIOG_HMF_HIR_PRO.Beanie02",
    "BIOG_HMF_HIR_PRO.Beret", "BIOG_HMF_HIR_PRO.Cap", "BIOG_HMF_HIR_PRO.Classy", "BIOG_HMF_HIR_PRO.Cosmo",
    "BIOG_HMF_HIR_PRO.CrewCut", "BIOG_HMF_HIR_PRO.Cuban", "BIOG_HMF_HIR_PRO.Cute", "BIOG_HMF_HIR_PRO.Cyberbabe",
    "BIOG_HMF_HIR_PRO.Cyclops", "BIOG_HMF_HIR_PRO.Earrings", "BIOG_HMF_HIR_PRO.GlassesFull",
    "BIOG_HMF_HIR_PRO.GlassesHalf", "BIOG_HMF_HIR_PRO.HairClip", "BIOG_HMF_HIR_PRO.HighBun", "BIOG_HMF_HIR_PRO.Human",
    "BIOG_HMF_HIR_PRO.Iconbun", "BIOG_HMF_HIR_PRO.LowBun", "BIOG_HMF_HIR_PRO.MidBun", "BIOG_HMF_HIR_PRO.Mira",
    "BIOG_HMF_HIR_PRO.Mohawk", "BIOG_HMF_HIR_PRO.Mom", "BIOG_HMF_HIR_PRO.Moto", "BIOG_HMF_HIR_PRO.MotoGlasses",
    "BIOG_HMF_HIR_PRO.Officer", "BIOG_HMF_HIR_PRO.PROCustomAfro", "BIOG_HMF_HIR_PRO.PROCustomCute",
    "BIOG_HMF_HIR_PRO.PROCustomSexy", "BIOG_HMF_HIR_PRO.PonyTail", "BIOG_HMF_HIR_PRO.Pulled",
    "BIOG_HMF_HIR_PRO.Pulled02", "BIOG_HMF_HIR_PRO.Sarge", "BIOG_HMF_HIR_PRO.Sexy", "BIOG_HMF_HIR_PRO.Short02",
    "BIOG_HMF_HIR_PRO.shepard", "BIOG_HMM_HIR_PRO_R.BaldWillis", "BIOG_HMM_HIR_PRO_R.Beanie01",
    "BIOG_HMM_HIR_PRO_R.Beanie02", "BIOG_HMM_HIR_PRO_R.Beret", "BIOG_HMM_HIR_PRO_R.Cap", "BIOG_HMM_HIR_PRO_R.Cosmo",
    "BIOG_HMM_HIR_PRO_R.CrewCut", "BIOG_HMM_HIR_PRO_R.Cuban", "BIOG_HMM_HIR_PRO_R.Cyclops", "BIOG_HMM_HIR_PRO_R.Formal",
    "BIOG_HMM_HIR_PRO_R.FormalSpikes", "BIOG_HMM_HIR_PRO_R.Geezer", "BIOG_HMM_HIR_PRO_R.GlassesFull",
    "BIOG_HMM_HIR_PRO_R.GlassesHalf", "BIOG_HMM_HIR_PRO_R.Human", "BIOG_HMM_HIR_PRO_R.Kaiden",
    "BIOG_HMM_HIR_PRO_R.Mohawk", "BIOG_HMM_HIR_PRO_R.Moto", "BIOG_HMM_HIR_PRO_R.MotoGlasses",
    "BIOG_HMM_HIR_PRO_R.Officer", "BIOG_HMM_HIR_PRO_R.RollinsSpike", "BIOG_HMM_HIR_PRO_R.Sarge",
    "BIOG_HMM_HIR_PRO_R.SargeSpike", "BIOG_HMM_HIR_PRO_R.Short01", "BIOG_HMM_HIR_PRO_R.Short02",
    "BIOG_HMM_HIR_PRO_R.Short03", "BIOG_HMM_HIR_PRO_R.Spalding", "BIOG_HMM_HIR_PRO_R.TheAfro",
    "BIOG_HMM_HIR_PRO_R.TheRollins", "BIOG_HMM_HIR_PRO_R.TheSlick", "BIOG_HMM_HIR_PRO_R.TheWillis",
    "BIOG_HMM_HIR_PRO_R.Visor",
};

std::string LowerAscii(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The path ME2 should get for an LE1 hair-package path: itself, a mapped one, or "" (a texture to leave out).
// 'changed' says when it is not LE1's own.
std::string MapHairAsset(const std::string& path, bool mesh, bool& changed) {
  changed = false;
  const size_t a = path.find('.');
  const size_t b = a == std::string::npos ? std::string::npos : path.find('.', a + 1);
  if (b == std::string::npos) return path;
  std::string package = path.substr(0, a);
  if (package == "BIOG_HMM_HIR_PRO") package = "BIOG_HMM_HIR_PRO_R";
  if (package != "BIOG_HMF_HIR_PRO" && package != "BIOG_HMM_HIR_PRO_R") return path;  // not a hair package
  const std::string group = path.substr(a + 1, b - a - 1), rest = path.substr(b + 1);
  const std::string same = package + "." + group + "." + rest;
  auto has = [&](const char* const* list, size_t n, const std::string& want) {
    for (size_t i = 0; i < n; ++i)
      if (LowerAscii(list[i]) == LowerAscii(want)) return std::string(list[i]);
    return std::string();
  };
  if (mesh) {
    if (std::string hit = has(kMe2HairMeshes, std::size(kMe2HairMeshes), same); !hit.empty()) {
      changed = hit != path;
      return hit;
    }
    std::string base = group;
    if (LowerAscii(base).rfind("procustom", 0) == 0) base = base.substr(9);
    const std::string prefix = LowerAscii(package + "." + base + ".");
    // The closest mesh of that group: most characters in common with LE1's mesh name without "Custom".
    std::string want = LowerAscii(rest), pick;
    if (size_t at = want.find("custom"); at != std::string::npos) want.erase(at, 6);
    size_t best = 0;
    for (const char* m : kMe2HairMeshes) {
      const std::string lm = LowerAscii(m);
      if (lm.rfind(prefix, 0) != 0) continue;
      const std::string name = lm.substr(prefix.size());
      size_t common = 0;
      while (common < name.size() && common < want.size() && name[common] == want[common]) ++common;
      if (pick.empty() || common > best) pick = m, best = common;
    }
    if (!pick.empty()) changed = true;
    return pick.empty() ? path : pick;  // path: no counterpart (Me2HasHairMesh says so)
  }
  if (!has(kMe2HairGroups, std::size(kMe2HairGroups), package + "." + group).empty()) {
    changed = same != path;
    return same;
  }
  changed = true;
  return std::string();  // a texture of a group ME2 lacks: the mesh's own material's
}

// ME2 (2026-11-15, the user's request): an LE1 hair mesh with no counterpart in ME2 takes the default style:
// ME2's base and iconic female faces (BIOG_MORPH_FACE.Player_Base_Female / Player_Iconic_Female m_oHairMesh) wear
// PROCustomSexy; the male base face has no hair mesh (the default male hair is the scalp texture), so None.
constexpr const char* kDefaultFemaleHair = "BIOG_HMF_HIR_PRO.PROCustomSexy.HMF_HIR_PROCustom_Sexy_MDL";
bool Me2HasHairMesh(const std::string& path) {
  for (const char* m : kMe2HairMeshes)
    if (LowerAscii(m) == LowerAscii(path)) return true;
  return false;
}

bool FillFace(Package& pkg, const Le1Save& s, bool keep_face, bool map_skin, bool map_makeup, bool map_hair,
              std::string& note, std::string& why) {
  const bool morph = s.head_morph && keep_face;
  auto edit = [&](int k, const char* what, auto&& fn) -> bool {
    if (k < 0 || size_t(k) >= pkg.Exports()) return why = std::string("template: no ") + what, false;
    std::vector<Prop> props;
    if (!pkg.Props(size_t(k), props)) return why = std::string("template: the properties of ") + what, false;
    Editor e(pkg, props);
    if (!fn(e)) return false;
    pkg.SetProps(size_t(k), props);
    return true;
  };
  int32_t player = 0, appearance = 0;
  const int game = pkg.Find("BioSaveGame", "CurrentGame");
  if (!edit(game, "CurrentGame", [&](Editor& e) {
        player = e.Object("m_Player");
        appearance = e.Object("m_PlayerAppearance");
        return true;
      }))
    return false;
  if (s.female && !edit(player - 1, "the player", [&](Editor& e) {
        e.Set("m_Gender", "ByteProperty", {1});
        return true;
      }))
    return false;
  const int face = pkg.Find("BioMorphFaceSaveObject");
  const std::string base_face = s.female ? "Player_Base_Female" : "Player_Base_Male";
  if (!edit(appearance - 1, "the appearance", [&](Editor& e) {
        e.Set("m_oMorphFace", "ObjectProperty", Int(morph ? face + 1 : 0));
        e.Set("m_pFaceAsset", "ObjectProperty", Int(pkg.ImportRef("BIOG_MORPH_FACE." + base_face, "BIOC_Base", "BioMorphFace")));
        return true;
      }))
    return false;
  note = s.female ? "female" : "male";
  if (!morph) {
    note += s.head_morph ? ", default face (me2_me1_import_le1_face off)" : ", default face";
    return true;
  }
  const HeadMorph& m = s.morph;
  auto object = [&](const Text& path, const char* cls) -> int32_t {
    const std::string p = path.Ascii();
    return p.empty() || p == "None" ? 0 : pkg.ImportRef(p, "Engine", cls);
  };
  int32_t material = 0;
  const LodTable table = FaceTable(s.female);
  bool lod1 = false;
  uint32_t features_written = 0, textures_written = 0;
  std::string skin, makeup, hair;
  auto fmt3 = [](const std::array<float, 4>& c) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.3f %.3f %.3f", c[0], c[1], c[2]);
    return std::string(b);
  };
  std::string assets;  // hair-package paths changed for ME2 (MapHairAsset)
  auto mapped = [&](const Text& t, bool mesh) {
    bool changed = false;
    const std::string p = t.Ascii(), to = MapHairAsset(p, mesh, changed);
    if (changed) assets += (assets.empty() ? "" : ", ") + p + " -> " + (to.empty() ? std::string("left out") : to);
    Text out;
    for (char c : to) out.chars.push_back(char16_t(static_cast<unsigned char>(c)));
    return out;
  };
  if (!edit(face, "the morph face", [&](Editor& e) {
        material = e.Object("m_oMaterialOverrides");
        e.Set("m_oBaseHead", "ObjectProperty", Int(0));  // ME2 sets None after loading anyway
        Text hair_mesh = mapped(m.hair, true);
        if (const std::string h = hair_mesh.Ascii();
            (h.rfind("BIOG_HMF_HIR_PRO.", 0) == 0 || h.rfind("BIOG_HMM_HIR_PRO", 0) == 0) && !Me2HasHairMesh(h)) {
          const std::string fallback = s.female ? kDefaultFemaleHair : "";
          assets += (assets.empty() ? "" : ", ") + h + " not in ME2 -> the default " +
                    (s.female ? fallback : std::string("(no hair mesh, as ME2's male base face)"));
          hair_mesh = {};
          for (char c : fallback) hair_mesh.chars.push_back(char16_t(static_cast<unsigned char>(c)));
        }
        e.Set("m_oHairMesh", "ObjectProperty", Int(object(hair_mesh, "SkeletalMesh")));
        if (!m.accessories.empty()) {
          std::vector<uint8_t> refs = Int(int32_t(m.accessories.size()));
          for (const Text& a : m.accessories) Put32(refs, uint32_t(object(mapped(a, true), "SkeletalMesh")));
          e.Set("m_oOtherMeshes", "ArrayProperty", std::move(refs));
        }
        std::vector<uint8_t> features = Int(0);
        uint32_t n = 0;
        for (const auto& [name, offset] : m.features) {
          if (name.Ascii() == "None") continue;  // LE1 keeps a "None" feature ME1 Xbox saves do not have
          std::vector<uint8_t> value = NameValue(pkg, name.Ascii());
          pkg.EmitList(features, {MakeProp(pkg, "sFeatureName", "NameProperty", std::move(value)),
                                  MakeProp(pkg, "Offset", "FloatProperty", Flt(offset))});
          ++n;
        }
        SetBe32(features, 0, n);
        features_written = n;
        e.Set("m_aMorphFeatures", "ArrayProperty", std::move(features));
        std::vector<uint8_t> bones = Int(int32_t(m.bones.size()));
        for (const auto& [name, pos] : m.bones) {
          std::vector<uint8_t> value = NameValue(pkg, name.Ascii());
          pkg.EmitList(bones, {MakeProp(pkg, "nName", "NameProperty", std::move(value)),
                               MakeProp(pkg, "vPos", "StructProperty", Floats(pos.data(), 3), "Vector")});
        }
        e.Set("m_aFinalSkeleton", "ArrayProperty", std::move(bones));
        std::vector<Vec3> l1;
        if (table.nearest && m.lod0.size() == table.lod0) {
          l1.resize(table.lod1);
          for (uint32_t i = 0; i < table.lod1; ++i)
            for (int c = 0; c < 3; ++c) {
              float off;
              std::memcpy(&off, &table.offset[3 * i + c], 4);
              l1[i][c] = m.lod0[table.nearest[i]][c] + off;
            }
          lod1 = true;
        }
        e.Set("m_aVertexBuffersLOD0", "StructProperty", VertexBuffer(pkg, m.lod0), "VertexBuffer");
        e.Set("m_aVertexBuffersLOD1", "StructProperty", VertexBuffer(pkg, l1), "VertexBuffer");
        e.Set("m_aVertexBuffersLOD2", "StructProperty", VertexBuffer(pkg, {}), "VertexBuffer");
        e.Set("OwnerName", "StrProperty", Str(U16("BIOG_MORPH_FACE." + base_face)));
        return true;
      }))
    return false;
  if (material <= 0) return why = "template: the morph face has no material overrides", false;
  if (!edit(material - 1, "the face's material overrides", [&](Editor& e) {
        std::vector<uint8_t> tex = Int(0);
        uint32_t n = 0;
        for (const auto& [name, path] : m.textures) {
          if (path.chars.empty() || path.Ascii() == "None") continue;
          const Text me2_path = mapped(path, false);
          if (me2_path.chars.empty()) continue;  // a hair texture ME2 does not have: the mesh's own
          std::vector<uint8_t> value = NameValue(pkg, name.Ascii());
          const int32_t ref = object(me2_path, "Texture2D");
          pkg.EmitList(tex, {MakeProp(pkg, "nName", "NameProperty", std::move(value)),
                             MakeProp(pkg, "m_pTexture", "ObjectProperty", Int(ref))});
          ++n;
        }
        SetBe32(tex, 0, n);
        textures_written = n;
        std::vector<uint8_t> colors = Int(int32_t(m.vectors.size()));
        for (const auto& [name, original] : m.vectors) {
          std::array<float, 4> c = original;
          if (map_skin && name.Ascii() == "SkinTone") {
            c = MapSkinTone(original, s.female);
            skin = fmt3(original) + " -> " + fmt3(c);
          }
          if (map_hair && name.Ascii() == "HED_Hair_Colour_Vector") {
            bool changed = false;
            c = MapHairColour(original, changed);
            if (changed) hair = fmt3(original) + " -> " + fmt3(c);
          }
          std::vector<uint8_t> value = NameValue(pkg, name.Ascii());
          pkg.EmitList(colors, {MakeProp(pkg, "nName", "NameProperty", std::move(value)),
                                MakeProp(pkg, "cValue", "StructProperty", Floats(c.data(), 4), "LinearColor")});
        }
        std::vector<uint8_t> scalars = Int(int32_t(m.scalars.size()));
        for (const auto& [name, original] : m.scalars) {
          float v = original;
          if (map_makeup)
            for (const MakeupSlider& sl : MakeupSliders()) {
              if (name.Ascii() != sl.scalar) continue;
              float strength = -1.0f;
              for (const auto& [cname, c] : m.vectors)
                if (cname.Ascii() == sl.colour) strength = MakeupStrength(sl, c);
              const float top = *std::max_element(sl.strengths.begin(), sl.strengths.end());
              v = strength >= 0 ? strength : std::min(original, top);
              if (v != original) {
                char b[96];
                std::snprintf(b, sizeof(b), "%s%s %.2f -> %.2f", makeup.empty() ? "" : ", ", sl.scalar, original, v);
                makeup += b;
              }
            }
          std::vector<uint8_t> value = NameValue(pkg, name.Ascii());
          pkg.EmitList(scalars, {MakeProp(pkg, "nName", "NameProperty", std::move(value)),
                                 MakeProp(pkg, "sValue", "FloatProperty", Flt(v))});
        }
        e.Set("m_aTextureOverrides", "ArrayProperty", std::move(tex));
        e.Set("m_aColorOverrides", "ArrayProperty", std::move(colors));
        e.Set("m_aScalarOverrides", "ArrayProperty", std::move(scalars));
        e.Set("OwnerName", "StrProperty",
              Str(U16("BIOG_MORPH_FACE." + base_face + (s.female ? ".BioMaterialOverride_2" : ".BioMaterialOverride_5"))));
        return true;
      }))
    return false;
  note += ", custom face (" + std::to_string(m.lod0.size()) + " vertices, " + std::to_string(features_written) +
          " features, " + std::to_string(textures_written) + " textures, hair " + m.hair.Ascii() +
          (assets.empty() ? std::string() : "; for ME2: " + assets) +
          (skin.empty() ? std::string() : "; skin tone " + skin) +
          (makeup.empty() ? std::string() : "; makeup " + makeup) +
          (hair.empty() ? std::string() : "; hair colour " + hair) + "; LOD1 " +
          (lod1 ? std::string("rebuilt") : std::string("left as ME2's base face: ") +
                                              (table.nearest ? "vertex count differs" : "no me2_me1_le1_faces.inc")) +
          ")";
  return true;
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

bool ContentImports(const std::vector<uint8_t>& d, std::vector<std::string>& paths) {
  paths.clear();
  if (d.size() < 64 || Be32(d, 0) != 0x9E2A83C1u) return false;
  const int32_t fn = int32_t(Be32(d, 12));
  const size_t so = 16 + (fn >= 0 ? size_t(fn) : size_t(-int64_t(fn)) * 2) + 4;
  if (so + 24 > d.size()) return false;
  const uint32_t nc = Be32(d, so), no = Be32(d, so + 4), ic = Be32(d, so + 16), io = Be32(d, so + 20);
  if (nc > 100000 || ic > 100000 || no >= d.size() || io + 28ull * ic > d.size()) return false;
  std::vector<std::string> names;
  size_t p = no;
  for (uint32_t k = 0; k < nc; ++k) {
    if (p + 4 > d.size()) return false;
    const int32_t n = int32_t(Be32(d, p));
    if (n <= 0 || p + 4 + size_t(n) + 8 > d.size()) return false;  // ASCII names (as the template's)
    names.emplace_back(reinterpret_cast<const char*>(&d[p + 4]), size_t(n) - 1);
    p += 4 + size_t(n) + 8;
  }
  auto name = [&](size_t at) -> std::string {
    const uint32_t i = Be32(d, at), num = Be32(d, at + 4);
    if (i >= names.size()) return {};
    return num ? names[i] + "_" + std::to_string(num - 1) : names[i];
  };
  // Only imports some object refers to (the template keeps rows the LE1 face no longer uses: its male hair / face):
  // the objects' bytes (after the header) searched for the reference, -(index + 1) as a big-endian word.
  const size_t objects = std::min<size_t>(Be32(d, 8), d.size());
  std::vector<uint8_t> used(ic, 0);
  for (size_t o = objects; o + 4 <= d.size(); ++o) {
    const int32_t v = int32_t(Be32(d, o));
    if (v < 0 && size_t(-int64_t(v)) <= ic) used[size_t(-int64_t(v)) - 1] = 1;
  }
  std::vector<std::string> first, rest;
  for (uint32_t k = 0; k < ic; ++k) {
    const size_t at = io + 28 * size_t(k);
    const std::string cls = name(at + 8);
    if (!used[k] || cls.empty() || cls == "Class" || cls == "Package") continue;
    std::string path = name(at + 20);
    int32_t outer = int32_t(Be32(d, at + 16));
    for (int depth = 0; outer < 0 && depth < 16; ++depth) {
      if (size_t(-int64_t(outer)) > ic) return false;
      const size_t o = io + 28 * size_t(-outer - 1);
      path = name(o + 20) + "." + path;
      outer = int32_t(Be32(d, o + 16));
    }
    if (outer != 0) continue;  // inside an export: not a content package's object
    (path.rfind("BIOG_MORPH_FACE.", 0) == 0 ? first : rest).push_back(path);
  }
  paths = std::move(first);
  for (std::string& s : rest)
    if (std::find(paths.begin(), paths.end(), s) == paths.end()) paths.push_back(std::move(s));
  return true;
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
  if (kTemplateSize == 0)
    return why = "this build has no ME1 template save (app/me2_src/me2_me1_le1_template.inc, see me2_me1_le1.cpp)", false;
  std::vector<uint8_t> tpl(kTemplateSize);
  if (!inflate(kTemplateZ, sizeof(kTemplateZ), tpl.data(), tpl.size())) return why = "template does not inflate", false;
  Package pkg;
  std::string face;
  if (!pkg.Load(tpl, why) || !Fill(pkg, s, why) || !FillFace(pkg, s, options.keep_face, options.map_skin_tone, options.map_makeup, options.map_hair_colour,
                face, why))
    return false;
  out.player = pkg.Write();
  out.state = StateSav(s);
  out.display = Display(s, options);
  out.summary = s.first_name.Utf8() + (original_name != s.first_name.chars ? " (was " + Utf16ToUtf8(original_name) + ")" : "") + ", " + face + ", " + ClassName(s.player_class) + ", level " + std::to_string(s.level) +
                ", origin " + std::to_string(s.origin) + ", notoriety " + std::to_string(s.notoriety) + ", " +
                std::to_string(s.quests.size()) + " quests, " + std::to_string(s.ints.size()) + " plot ints";
  return true;
}

}  // namespace me2::me1_import::le1
