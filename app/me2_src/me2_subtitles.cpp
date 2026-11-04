// Mass Effect 2 - subtitle size (2026-11-04, user: "subtitles size adjustment option", live, in the Options menu).
//
// Subtitles are drawn by the Conversation movie (GUI_SF_Conversation.Conversation, GFX length 62130): its
// setSubtitle(sSubtitle, nPosition, nColor, nAlpha, bBackground) picks a clip (SubtitleBottom / SubtitleTop /
// SubtitleConversation / loadingText for position 5) and sets SubtitleText.text = sSubtitle. The text fields are HTML
// fields with a layout (alignment) of their own; the font is 22 px (16 px for loadingText).
// The movie is parsed once per session, so the size cannot live in its bytes. Instead:
//   - The movie (patched where the loader opens it, me2_menu.cpp PatchFile): setSubtitle's three "SubtitleText.text ="
//     become "SubtitleText.htmlText =". The constant pool has no "htmlText": entry 123 (a 128-character test line
//     used only by RunTests) becomes "htmlText", the entries after it move down and a Jump over Pop filler takes the
//     freed bytes, so no code moves.
//   - Each line: a native (sub_8273EB20) calls movie->Invoke("setSubtitle", result, args, 5) at 0x8273EF4C, and the
//     conversation's script does it through UBioSFPanel.InvokeMethodArgs (sub_8273C7E0, at 0x8273C934; a Ryujinx run
//     showed the native one never runs for conversations). Both are [[midasm_hook]]s on the same function: for a
//     method named "...ubtitle...", args[0] (a GFxValue: type 5 = wide string, then the pointer) is replaced by
//     '<font size="N">line</font>' (the line escaped) in a guest buffer, N = the field's size * me2_subtitle_size %.
//     GFx copies the string during Invoke. The game's colour, alpha and shadow are set on the whole field after it.
// The size is the Options -> Gameplay -> Subtitle Size row (me2_gyro_menu.cpp) or the toml.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

REXCVAR_DEFINE_BOOL(me2_subtitle_resize, true, "ME2",
                    "Subtitles can be resized (me2_subtitle_size; Options -> Gameplay -> Subtitle Size)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(me2_subtitle_size, 100, "ME2",
                     "Subtitle text size in percent of the game's (22 px): 100 .. 200; Options -> Gameplay -> "
                     "Subtitle Size saves its choice over this");

namespace {

std::atomic<bool> g_html{false};  // the movie's setSubtitle sets htmlText (the patch is in)
std::mutex g_mutex;
uint32_t g_buffer = 0, g_capacity = 0;  // guest buffer for the wrapped line (UTF-16 BE)

constexpr uint32_t kPool = 0x270, kPoolCount = 140, kEntry = 123;
constexpr uint32_t kSites[3] = {0x197C, 0x1A01, 0x1AA6};  // Push c71 'text', r6 (before SetMember)
constexpr char kTestLine[] = "Nihlus: Obviously this goes beyond mere human interests";
constexpr char kHtmlText[] = "htmlText";

uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
void Put16(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}

}  // namespace

// Called by me2_menu.cpp for the Conversation movie (m = its bytes, size = its GFX length).
bool Me2PatchSubtitleHtml(uint8_t* m, uint32_t size) {
  if (!REXCVAR_GET(me2_subtitle_resize)) return false;
  const uint8_t site_game[7] = {0x96, 0x04, 0x00, 0x08, 71, 0x04, 0x06};
  const uint8_t site_ours[7] = {0x96, 0x04, 0x00, 0x08, kEntry, 0x04, 0x06};
  if (size < 0x2000 || m[kPool] != 0x88 || Get16(m + kPool + 3) != kPoolCount) {
    REXLOG_WARN("[subtitles] Conversation movie: no constant pool where expected; subtitle size off");
    return false;
  }
  const uint32_t pool_end = kPool + 3 + Get16(m + kPool + 1);
  // Entry 123's place.
  uint32_t p = kPool + 5;
  for (uint32_t i = 0; i < kEntry; ++i) {
    while (p < pool_end && m[p]) ++p;
    ++p;
  }
  if (p >= pool_end) return false;
  const bool done = std::strcmp(reinterpret_cast<const char*>(m + p), kHtmlText) == 0;
  bool sites_game = true, sites_ours = true;
  for (uint32_t s : kSites) {
    sites_game = sites_game && std::memcmp(m + s, site_game, 7) == 0;
    sites_ours = sites_ours && std::memcmp(m + s, site_ours, 7) == 0;
  }
  if (done && sites_ours) {
    g_html = true;
    return true;
  }
  if (done || !sites_game || std::memcmp(m + p, kTestLine, sizeof(kTestLine) - 1) != 0) {
    REXLOG_WARN("[subtitles] Conversation movie: not the expected bytes; subtitle size off");
    return false;
  }
  const uint32_t old_len = uint32_t(std::strlen(reinterpret_cast<const char*>(m + p))) + 1;
  const uint32_t new_len = sizeof(kHtmlText);
  const uint32_t gap = old_len - new_len;  // 120
  // Entry 123 becomes "htmlText", the rest of the pool moves down by 'gap'.
  std::memmove(m + p + new_len, m + p + old_len, pool_end - (p + old_len));
  std::memcpy(m + p, kHtmlText, new_len);
  Put16(m + kPool + 1, Get16(m + kPool + 1) - gap);
  // The freed bytes: Jump to the first action after them, then Pop (never run; keeps the bytes well formed).
  const uint32_t hole = pool_end - gap;
  const int32_t off = int32_t(gap) - 5;
  const uint8_t jump[5] = {0x99, 0x02, 0x00, uint8_t(off), uint8_t(off >> 8)};
  std::memcpy(m + hole, jump, 5);
  std::memset(m + hole + 5, 0x17, gap - 5);
  for (uint32_t s : kSites) m[s + 4] = kEntry;
  g_html = true;
  REXLOG_INFO("[subtitles] Conversation movie: setSubtitle sets htmlText (size {} %)", REXCVAR_GET(me2_subtitle_size));
  return true;
}

// movie->Invoke(r4 method, r5 result, r6 args, r7 count) in sub_8273EB20, before the bctrl at 0x8273EF4C.
void Me2SubtitleInvokeArgs(PPCRegister& r4, PPCRegister& r6, PPCRegister& r7) {
  // Only methods with "ubtitle" in the name (UBioSFPanel.InvokeMethodArgs carries every scripted call).
  auto* memory = REX_KERNEL_MEMORY();
  const char* name = r4.u32 ? memory->TranslateVirtual<const char*>(r4.u32) : nullptr;
  if (!name || !std::strstr(name, "ubtitle")) return;
  // Diagnostic (2026-11-04, user: the size does not change): the first calls are logged, then every 200th.
  static std::atomic<uint32_t> calls{0};
  const uint32_t call = calls.fetch_add(1) + 1;
  const bool say = call <= 24 || call % 200 == 0;
  std::string shown;
  for (int i = 0; i < 32 && name[i]; ++i) shown += (name[i] >= 32 && name[i] < 127) ? name[i] : '?';
  if (r7.u32 != 5 || !g_html.load(std::memory_order_relaxed)) {
    if (say)
      REXLOG_INFO("[subtitles] call {}: '{}' with {} arguments: left alone ({})", call, shown, r7.u32,
                  g_html ? "not 5 arguments" : "the movie is not patched");
    return;
  }
  if (!name || std::strncmp(name, "setSubtitle", 12) != 0) {
    if (say) REXLOG_INFO("[subtitles] call {}: '{}': not setSubtitle, left alone", call, shown);
    return;
  }
  uint8_t* args = memory->TranslateVirtual<uint8_t*>(r6.u32);
  const uint32_t raw_type = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(args));
  const uint32_t type = raw_type & 0x3F;
  uint32_t text_va;
  std::memcpy(&text_va, args + 4, 4);
  text_va = __builtin_bswap32(text_va);
  if ((type != 4 && type != 5) || text_va == 0) {
    if (say)
      REXLOG_INFO("[subtitles] call {}: setSubtitle's text is type 0x{:X} at {:08X}: left alone", call, raw_type,
                  text_va);
    return;
  }
  // The line, as UTF-16 code units.
  std::u16string line;
  if (type == 5) {
    const uint8_t* s = memory->TranslateVirtual<const uint8_t*>(text_va);
    for (uint32_t i = 0; i < 8192; ++i) {
      const char16_t c = char16_t(s[2 * i] << 8 | s[2 * i + 1]);
      if (!c) break;
      line.push_back(c);
    }
  } else {
    const uint8_t* s = memory->TranslateVirtual<const uint8_t*>(text_va);
    for (uint32_t i = 0; i < 8192 && s[i]; ++i) line.push_back(char16_t(s[i]));  // ASCII / Latin-1
  }
  if (line.empty()) {  // "" hides the subtitle: left as it is
    if (say) REXLOG_INFO("[subtitles] call {}: setSubtitle(\"\") (hide)", call);
    return;
  }
  // Position 5 = loadingText (16 px), the others 22 px.
  uint64_t bits;
  std::memcpy(&bits, args + 12 + 4, 8);
  bits = __builtin_bswap64(bits);
  double position;
  std::memcpy(&position, &bits, 8);
  const int base = position == 5.0 ? 16 : 22;
  const int pct = std::clamp(REXCVAR_GET(me2_subtitle_size), 50, 300);
  const int px = std::max(8, int(std::lround(base * pct / 100.0)));

  std::u16string html;
  const std::string head = "<font size=\"" + std::to_string(px) + "\">";
  html.append(head.begin(), head.end());
  for (char16_t c : line) {
    const char* esc = c == u'<' ? "&lt;" : c == u'>' ? "&gt;" : c == u'&' ? "&amp;" : c == u'\n' ? "<br>" : nullptr;
    if (esc)
      while (*esc) html.push_back(char16_t(*esc++));
    else if (c != u'\r')
      html.push_back(c);
  }
  for (const char* t = "</font>"; *t; ++t) html.push_back(char16_t(*t));

  std::lock_guard<std::mutex> lock(g_mutex);
  const uint32_t need = uint32_t(html.size() + 1) * (type == 5 ? 2u : 1u);
  if (need > g_capacity) {
    if (g_buffer) memory->SystemHeapFree(g_buffer);
    g_capacity = std::max<uint32_t>(4096, (need + 4095) & ~4095u);
    g_buffer = memory->SystemHeapAlloc(g_capacity);
    if (!g_buffer) {
      g_capacity = 0;
      return;
    }
  }
  uint8_t* out = memory->TranslateVirtual<uint8_t*>(g_buffer);
  if (type == 5) {
    for (size_t i = 0; i < html.size(); ++i) {
      out[2 * i] = uint8_t(html[i] >> 8);
      out[2 * i + 1] = uint8_t(html[i]);
    }
    out[2 * html.size()] = out[2 * html.size() + 1] = 0;
  } else {
    for (size_t i = 0; i < html.size(); ++i) out[i] = uint8_t(html[i]);
    out[html.size()] = 0;
  }
  const uint32_t be = __builtin_bswap32(g_buffer);
  std::memcpy(args + 4, &be, 4);
  if (say) {
    std::string text;
    for (size_t i = 0; i < html.size() && text.size() < 90; ++i)
      text += (html[i] >= 32 && html[i] < 127) ? char(html[i]) : '?';
    REXLOG_INFO("[subtitles] call {}: setSubtitle position {} type {}: {} px ({} %): {}", call, position, type, px,
                pct, text);
  }
}
