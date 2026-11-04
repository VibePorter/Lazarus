// Mass Effect 2 (TU2) - gyro aiming rows in Options -> Controller (2026-11-03, user: "custom controller menu option in
// settings for gyro controls", the gyro itself from masseffect-cinnamon: sdk/src/input/switch/switch_input_driver.cpp).
//
// The Options movie (GUI_SF_Options.Options, me2_menu.cpp) builds the console pages in initXBoxData: the Controller
// page is {label, story, type: 'BaseMenuItem', children: [5 rows]}, each row {label, story, type: 'TextSlider' |
// 'RadioGroup', optionId, control: {defaultValue, children: [{label, story}, ...]}}. A changed row calls the movie's
// SetOption(optionId, value) -> FSCommand -> UBioSFHandler_Options::execSetOption -> the C++ SetOption(this, id, value)
// (vtable 0x821EB068 slot 0x190 = sub_8293E3B0), a switch over ids 0..34 (others ignored).
//
// Four rows are added (ids 90..93): Gyro Aiming (Off / While Aiming / Always = input_gyro), Gyro Sensitivity (0.5 ..
// 6.0 = input_gyro_sensitivity), Invert Horizontal / Vertical (input_gyro_invert_x / _y).
//   - Space: initPCData (+0x1662A) runs only on the PC (Platform == PC); its 2,687-byte body becomes our row builder,
//     and its flags 0x0129 -> 0x0125 so `arguments` is preloaded (r2).
//   - Current values: Scaleform parses the movie once per session (console log 2026-11-03: the second opening showed
//     the first one's values), so they cannot live in its bytes. SetOptionsOnGUI's Invoke of SetOptions carries them
//     instead, in four PC-only arguments (Me2GyroSetOptionsArgs, a [[midasm_hook]]); the rows read this.windowMode,
//     this.qualityBloom, this.dynamicShadows and this.lightEnvironmentShadows.
//   - Call: after the Controller page's rows array is made (+0x176C0 InitArray), "Push 4" (+0x176C1) jumps to a cave
//     at the end of the Online entry's dead bytes (me2_menu.cpp): PushDuplicate; this.initPCData(rows); Pop; Push 4;
//     jump back. The builder pushes its rows into that array.
//   - Values: sub_8293E3B0 is hooked; ids 90..93 set the cvars and are saved to <user data>/me2_gyro.cfg (loaded at
//     start, over the toml's values).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(me2_gyro_menu, true, "ME2",
                    "Options -> Controller: gyro aiming rows (mode, sensitivity, invert); needs me2_options_hide_online")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(
    me2_gyro_menu_text,
    "Gyro Aiming|Aim by moving the console or controller: only while ZL is held, or all the time.|Off|While Aiming "
    "(ZL)|Always|Gyro Sensitivity|How fast the camera turns when you move the console or controller (L+R+X / L+R+Y "
    "also change it while playing).|Invert Gyro Horizontal|Swaps left and right for gyro aiming.|Invert Gyro "
    "Vertical|Swaps up and down for gyro aiming.",
    "ME2",
    "Gyro rows' texts, '|' separated: mode label, mode story, Off, While Aiming, Always, sensitivity label, "
    "sensitivity story, invert X label, invert X story, invert Y label, invert Y story (UTF-8)");

// 2026-11-04: Options -> Gameplay -> Subtitle Size (me2_subtitles.cpp), built by the same builder (see BuildBody).
REXCVAR_DECLARE(bool, me2_subtitle_resize);  // me2_subtitles.cpp
REXCVAR_DEFINE_BOOL(me2_subtitle_menu, true, "ME2",
                    "Options -> Gameplay: Subtitle Size row (me2_subtitle_size); needs me2_options_hide_online")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(me2_subtitle_menu_text,
                      "Subtitle Size|Makes the subtitles bigger. Takes effect from the next line.", "ME2",
                      "Subtitle Size row's texts, '|' separated: label, story (UTF-8)");

namespace me2::gyro_menu {
namespace {

constexpr int kIdMode = 90, kIdSens = 91, kIdInvX = 92, kIdInvY = 93, kIdSubSize = 94;
constexpr int kSensSteps = 12;  // 0.5, 1.0, ..., 6.0
constexpr int kSubSizes[] = {100, 120, 140, 160, 180, 200};  // me2_subtitle_size choices, percent

// The movie's constant pool (initXBoxData's DoInitAction): names used by the game's own rows.
constexpr uint8_t cLabel = 97, cStory = 99, cType = 101, cRadioGroup = 102, cOptionId = 103, cControl = 104,
                  cDefaultValue = 105, cChildren = 107, cRadioNo = 108, cRadioYes = 109, cTextSlider = 117;
// The PC-only members SetOptions stores (never read on the console) that carry our values in (Me2GyroSetOptionsArgs):
// SetOptions(...) arguments 18..21 = windowMode, qualityBloom, dynamicShadows, lightEnvironmentShadows.
constexpr uint8_t cWindowMode = 181, cQualityBloom = 177, cDynamicShadows = 174, cLightEnvShadows = 171;
// ... and argument 25, screenResolution (read only by initPCData, which the builder replaces): the subtitle size index.
constexpr uint8_t cScreenResolution = 189, cLength = 138;

std::mutex g_mutex;

int Mode() { return std::clamp(rex::cvar::Query<int32_t>("input_gyro"), 0, 2); }
double Sens() {
  const double s = rex::cvar::Query<double>("input_gyro_sensitivity");
  return s > 0 ? s : 3.0;
}
bool InvX() { return rex::cvar::Query<bool>("input_gyro_invert_x"); }
bool InvY() { return rex::cvar::Query<bool>("input_gyro_invert_y"); }
int SubSize() { return rex::cvar::Query<int32_t>("me2_subtitle_size"); }
int SubSizeIndex() {
  const int s = SubSize();
  int best = 0;
  for (int i = 0; i < int(std::size(kSubSizes)); ++i)
    if (std::abs(kSubSizes[i] - s) < std::abs(kSubSizes[best] - s)) best = i;
  return best;
}

std::filesystem::path CfgPath() {
  rex::Runtime* rt = rex::Runtime::instance();
  return rt ? rt->user_data_root() / "me2_gyro.cfg" : std::filesystem::path("me2_gyro.cfg");
}

void Save() {
  const std::filesystem::path p = CfgPath();
  if (std::FILE* f = std::fopen(p.string().c_str(), "wb")) {
    // The file keeps its first name (me2_gyro.cfg); the subtitle size is saved in it too.
    std::fprintf(f,
                 "input_gyro=%d\ninput_gyro_sensitivity=%.1f\ninput_gyro_invert_x=%s\ninput_gyro_invert_y=%s\n"
                 "me2_subtitle_size=%d\n",
                 Mode(), Sens(), InvX() ? "true" : "false", InvY() ? "true" : "false", SubSize());
    std::fflush(f);
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    REXLOG_INFO("[gyro-menu] saved to {}{}", p.string(), ok ? "" : " (write error)");
  } else {
    REXLOG_WARN("[gyro-menu] cannot write {}", p.string());
  }
}

int SensIndex() { return std::clamp(int(std::lround(Sens() / 0.5)) - 1, 0, kSensSteps - 1); }

std::vector<std::string> Texts() {
  std::vector<std::string> t;
  const std::string s = REXCVAR_GET(me2_gyro_menu_text);
  size_t a = 0;
  for (;;) {
    const size_t e = s.find('|', a);
    t.push_back(s.substr(a, e == std::string::npos ? std::string::npos : e - a));
    if (e == std::string::npos) break;
    a = e + 1;
  }
  static const char* const kDefault[] = {"Gyro Aiming", "", "Off", "While Aiming (ZL)", "Always", "Gyro Sensitivity",
                                         "", "Invert Gyro Horizontal", "", "Invert Gyro Vertical", ""};
  while (t.size() < std::size(kDefault)) t.push_back(kDefault[t.size()]);
  for (auto& x : t) std::erase(x, '\0');
  return t;
}

std::vector<std::string> SubtitleTexts() {
  const std::string s = REXCVAR_GET(me2_subtitle_menu_text);
  const size_t e = s.find('|');
  std::vector<std::string> t = {s.substr(0, e), e == std::string::npos ? std::string() : s.substr(e + 1)};
  if (t[0].empty()) t[0] = "Subtitle Size";
  for (auto& x : t) std::erase(x, '\0');
  return t;
}

// A tiny AVM1 emitter.
struct Avm {
  std::vector<uint8_t> code, push;
  void Flush() {
    if (push.empty()) return;
    code.push_back(0x96);
    code.push_back(uint8_t(push.size()));
    code.push_back(uint8_t(push.size() >> 8));
    code.insert(code.end(), push.begin(), push.end());
    push.clear();
  }
  Avm& C(uint8_t index) { push.insert(push.end(), {0x08, index}); return *this; }
  Avm& I(int32_t v) {
    push.push_back(0x07);
    for (int i = 0; i < 4; ++i) push.push_back(uint8_t(uint32_t(v) >> (8 * i)));
    return *this;
  }
  Avm& R(uint8_t r) { push.insert(push.end(), {0x04, r}); return *this; }
  Avm& S(const std::string& s) {
    push.push_back(0x00);
    push.insert(push.end(), s.begin(), s.end());
    push.push_back(0x00);
    return *this;
  }
  Avm& U() { push.push_back(0x03); return *this; }
  void Op(uint8_t op) {
    Flush();
    code.push_back(op);
  }
  // StoreRegister r (the value stays on the stack: Pop after it).
  void Store(uint8_t r) {
    Flush();
    code.insert(code.end(), {0x87, 0x01, 0x00, r});
  }
  // If (pops the condition): skips 'skip' bytes when it is true.
  void If(int16_t skip) {
    Flush();
    code.insert(code.end(), {0x9D, 0x02, 0x00, uint8_t(skip), uint8_t(uint16_t(skip) >> 8)});
  }
  void Append(Avm& other) {
    Flush();
    other.Flush();
    code.insert(code.end(), other.code.begin(), other.code.end());
  }
};
constexpr uint8_t kInitArray = 0x42, kInitObject = 0x43, kGetMember = 0x4E, kCallMethod = 0x52, kPop = 0x17,
                  kReturn = 0x3E, kEquals2 = 0x49;

// arguments[0].push(<the object on the stack>), or with splice_at >= 0 (the Gameplay call, rows in arguments[1]):
// arguments[1].splice(splice_at, 0, <the object>).
void PushToArg(Avm& a, int splice_at = -1) {
  if (splice_at < 0) {
    a.I(1).R(2).I(0);
    a.Op(kGetMember);
    a.S("push");
  } else {
    a.I(0).I(splice_at).I(3).R(2).I(1);
    a.Op(kGetMember);
    a.S("splice");
  }
  a.Op(kCallMethod);
  a.Op(kPop);
}

// The row's story is in register `story` (stored once: each slider choice shows it too). The row's value is
// this.<member> (`value`, a constant-pool index), set by SetOptions just before the pages are built: the movie is parsed
// once per session (Scaleform keeps it), so a value written into its bytes would stay the first opening's.
void SliderRow(Avm& a, const std::string& label, uint8_t story, int id, uint8_t value,
               const std::vector<std::string>& children, int splice_at = -1) {
  a.C(cLabel).S(label).C(cStory).R(story).C(cType).C(cTextSlider).C(cOptionId).I(id).C(cControl).C(cDefaultValue)
      .R(1).C(value);
  a.Op(kGetMember);
  a.C(cChildren);
  // InitArray takes the last pushed element as [0] (the game's own sliders push High, Medium, Low for
  // [Low, Medium, High]), so the choices go in last first: index i must show children[i].
  for (auto c = children.rbegin(); c != children.rend(); ++c) {
    a.C(cLabel).S(*c).C(cStory).R(story).I(2);
    a.Op(kInitObject);
  }
  a.I(int32_t(children.size()));
  a.Op(kInitArray);
  a.I(2);
  a.Op(kInitObject);  // control
  a.I(5);
  a.Op(kInitObject);  // the row
  PushToArg(a, splice_at);
}

void RadioRow(Avm& a, const std::string& label, uint8_t story, int id, uint8_t value) {
  a.C(cLabel).S(label).C(cStory).R(story).C(cType).C(cRadioGroup).C(cOptionId).I(id).C(cControl).C(cDefaultValue)
      .R(1).C(value);
  a.Op(kGetMember);
  a.C(cChildren);
  a.C(cLabel).R(1).C(cRadioNo);
  a.Op(kGetMember);
  a.I(1);
  a.Op(kInitObject);
  a.C(cLabel).R(1).C(cRadioYes);
  a.Op(kGetMember);
  a.I(1);
  a.Op(kInitObject);
  a.I(2);
  a.Op(kInitArray);
  a.I(2);
  a.Op(kInitObject);
  a.I(5);
  a.Op(kInitObject);
  PushToArg(a);
}

}  // namespace

bool Enabled() { return REXCVAR_GET(me2_gyro_menu); }
bool SubtitleRowEnabled() { return REXCVAR_GET(me2_subtitle_menu) && REXCVAR_GET(me2_subtitle_resize); }

// The builder body for initPCData (me2_menu.cpp writes it into the movie): 'size' bytes, the rest Pop.
// Called as initPCData(rows) from the Controller page (gyro rows, pushed at the end) and as initPCData(1, rows) from
// the Gameplay page (Subtitle Size, spliced in after Subtitles: rows = [Difficulty, Auto Level Up, Subtitles, ...]).
bool BuildBody(uint8_t* out, size_t size) {
  const std::vector<std::string> t = Texts();
  std::vector<std::string> steps;
  for (int i = 1; i <= kSensSteps; ++i) {
    char b[16];
    std::snprintf(b, sizeof(b), "%.1f", i * 0.5);
    steps.push_back(b);
  }
  // The gyro rows (Controller page).
  Avm g;
  if (Enabled()) {
    // The four stories in r4..r7 (initPCData has 17 registers; r1 this, r2 arguments, r3 _global are preloaded).
    const std::pair<uint8_t, const std::string*> stories[] = {{4, &t[1]}, {5, &t[6]}, {6, &t[8]}, {7, &t[10]}};
    for (const auto& [reg, story] : stories) {
      g.S(*story);
      g.Store(reg);
      g.Op(kPop);
    }
    SliderRow(g, t[0], 4, kIdMode, cWindowMode, {t[2], t[3], t[4]});
    SliderRow(g, t[5], 5, kIdSens, cQualityBloom, steps);
    RadioRow(g, t[7], 6, kIdInvX, cDynamicShadows);
    RadioRow(g, t[9], 7, kIdInvY, cLightEnvShadows);
  }
  g.U();
  g.Op(kReturn);
  // The Subtitle Size row (Gameplay page).
  Avm s;
  if (SubtitleRowEnabled()) {
    const std::vector<std::string> st = SubtitleTexts();
    std::vector<std::string> sizes;
    for (int pct : kSubSizes) sizes.push_back(std::to_string(pct) + " %");
    s.S(st[1]);
    s.Store(4);
    s.Op(kPop);
    SliderRow(s, st[0], 4, kIdSubSize, cScreenResolution, sizes, /*splice_at=*/3);
  }
  s.U();
  s.Op(kReturn);
  // if (arguments.length == 2) goto subtitles
  Avm a;
  a.R(2).C(cLength);
  a.Op(kGetMember);
  a.I(2);
  a.Op(kEquals2);
  a.If(int16_t(g.code.size()));  // g ends with Return: its bytes are final
  a.Append(g);
  a.Append(s);
  if (a.code.size() > size) {
    REXLOG_WARN("[gyro-menu] rows need {} bytes, {} available: shorten me2_gyro_menu_text", a.code.size(), size);
    return false;
  }
  std::memcpy(out, a.code.data(), a.code.size());
  std::memset(out + a.code.size(), 0x17, size - a.code.size());
  return true;
}

// At start: the menu's saved choices over the toml's values.
void Load() {
  if (!Enabled() && !SubtitleRowEnabled()) return;
  const std::filesystem::path p = CfgPath();
  std::FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) {
    REXLOG_INFO("[gyro-menu] no saved choices yet ({}): the toml's values", p.string());
    return;
  }
  char line[128];
  int n = 0;
  while (std::fgets(line, sizeof(line), f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    const size_t eq = s.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = s.substr(0, eq), value = s.substr(eq + 1);
    if ((key.rfind("input_gyro", 0) == 0 || key == "me2_subtitle_size") && rex::cvar::SetFlagByName(key, value)) ++n;
  }
  std::fclose(f);
  REXLOG_INFO("[gyro-menu] {} setting(s) from {}: mode {}, sensitivity {:.1f}, invert x {} y {}, subtitles {} %", n,
              p.string(), Mode(), Sens(), InvX(), InvY(), SubSize());
}

}  // namespace me2::gyro_menu

// The rows' current values into the movie: UBioSFHandler_Options::SetOptionsOnGUI (sub_8293DBC8) sends every option
// with movie->Invoke(method, &result, args r6, count r7 = 27) at 0x8293E370 ([[midasm_hook]] in overrides.toml, called
// before that bctrl). The arguments are GFxValues of 12 bytes (type, then the double) in SetOptions' order; 18..21
// (windowMode, qualityBloomFlag, dynamicShadowsFlag, lightEnvironmentShadowsFlag) are PC-only settings the console page
// never shows, so they carry the gyro mode, sensitivity index and inverts, which the rows read back; 25
// (screenResolution, PC-only too) carries the Subtitle Size index.
void Me2GyroSetOptionsArgs(PPCRegister& r6, PPCRegister& r7) {
  using namespace me2::gyro_menu;
  if (r7.u32 != 27 || (!Enabled() && !SubtitleRowEnabled())) return;
  auto* memory = REX_KERNEL_MEMORY();
  const double values[5] = {double(Mode()), double(SensIndex()), InvX() ? 1.0 : 0.0, InvY() ? 1.0 : 0.0,
                            double(SubSizeIndex())};
  const uint32_t index[5] = {18, 19, 20, 21, 25};
  for (int i = Enabled() ? 0 : 4; i < (SubtitleRowEnabled() ? 5 : 4); ++i) {
    uint8_t* arg = memory->TranslateVirtual<uint8_t*>(r6.u32 + 12u * index[i]);
    const uint32_t type = __builtin_bswap32(3u);  // GFxValue::VT_Number
    uint64_t bits;
    std::memcpy(&bits, &values[i], 8);
    bits = __builtin_bswap64(bits);
    std::memcpy(arg, &type, 4);
    std::memcpy(arg + 4, &bits, 8);
  }
}

// UBioSFHandler_Options::SetOption(this r3, int id r4, int value r5): the Options movie's changes. Ids 90..94 are ours
// (the game's switch ignores ids above 34; they never reach it).
REX_EXTERN(__imp__sub_8293E3B0);
REX_HOOK_RAW(sub_8293E3B0) {
  const int32_t id = int32_t(ctx.r4.u32), value = int32_t(ctx.r5.u32);
  if (id < 90 || id > 94 || (id < 94 ? !me2::gyro_menu::Enabled() : !me2::gyro_menu::SubtitleRowEnabled())) {
    __imp__sub_8293E3B0(ctx, base);
    return;
  }
  using namespace me2::gyro_menu;
  std::lock_guard<std::mutex> lock(g_mutex);
  switch (id) {
    case kIdMode:
      rex::cvar::SetFlagByName("input_gyro", std::to_string(std::clamp(value, 0, 2)));
      break;
    case kIdSens: {
      char b[16];
      std::snprintf(b, sizeof(b), "%.1f", (std::clamp(value, 0, kSensSteps - 1) + 1) * 0.5);
      rex::cvar::SetFlagByName("input_gyro_sensitivity", b);
      break;
    }
    case kIdInvX:
      rex::cvar::SetFlagByName("input_gyro_invert_x", value ? "true" : "false");
      break;
    case kIdInvY:
      rex::cvar::SetFlagByName("input_gyro_invert_y", value ? "true" : "false");
      break;
    case kIdSubSize:
      rex::cvar::SetFlagByName("me2_subtitle_size",
                               std::to_string(kSubSizes[std::clamp(value, 0, int(std::size(kSubSizes)) - 1)]));
      break;
  }
  Save();
  REXLOG_INFO("[gyro-menu] option {} = {}: mode {}, sensitivity {:.1f}, invert x {} y {}, subtitles {} %", id, value,
              Mode(), Sens(), InvX(), InvY(), SubSize());
}
