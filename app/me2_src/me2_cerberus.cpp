// Mass Effect 2 - the Cerberus Network (EA's online service) on the port.
//
// WHAT THE ENGINE DOES (ME2, from the image, 2026-10-27)
//   The main menu's "Connecting to Cerberus Network" is SFXGame's USFXOnlineComponentBlazeLogin: EA's BlazeSDK /
//   DirtySDK reach gosredirector.online.ea.com through XLSP, log in (Nucleus account, persona), then read entitlements
//   (ME2_DLC, CERBERUS_OFFER_ID) and the news (newsletter.php). The port has no Xbox LIVE and no network, so it fails.
//   UnrealScript sees the state through natives of USFXOnlineComponentBlazeLogin (IsConnected, GetConnectionState,
//   GetLoginStatus, IsCerberusMember, ...). The class's native table (12-byte entries: name "int<Class>exec<Func>",
//   function, 0) is at 0x83376Dxx-0x83376Fxx; its function words are 0 in the image and written at start-up.
//
// The natives, from the table dump (me2_diag_cerberus_natives, PC 2026-10-27): each is an exec function (r3 = this,
// r4 = FFrame, r5 = result) that finishes its (empty) parameter list and asks the component's native interface at
// this + 0x64: IsConnected sub_82840FE0 (slot 8, int; the same code serves BlazeNotification's
// GetDaysSinceCerberusRegistration), GetLoginStatus sub_82840E18 (slot 5, byte: ELoginStatus 0 not logged in,
// 1 local profile, 2 logged in), IsSignedIn sub_82843608 (slot 9, int), IsCerberusMember sub_82842408 (slot 32, int),
// GetConnectionState sub_82843070 (slot 44, byte). IsLive (sub_82841070) is not touched: the same code is BlazeHub's
// ConnectDirtySock. (The main menu's GetLocalizedNewsItemCount, sub_82A350C8, is the terminal's "News Item (1 / 1)"
// counter, not the news text: left alone.)
//
// me2_cerberus_spoof (default on): the natives answer "connected, logged in, Cerberus member" (the network is not
// touched: what the game then asks the server for still fails). me2_cerberus_news: the terminal's text ("" = the
// game's own). me2_diag_cerberus_natives (diagnostic): at the 300th Swap, logs every online native's function.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(me2_cerberus_spoof, true, "ME2",
                    "The Cerberus Network login natives answer connected / logged in / Cerberus member (no network)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(me2_cerberus_news, "Welcome to Lazarus", "ME2",
                      "Title of the news item the main menu's Cerberus terminal shows while spoofing (\"\" = none)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The main menu's text by answer (PC, 2026-10-27): 0, 1, 3 "Connecting to Cerberus Network", 2 "Activate Cerberus
// Network", 4 "Connect to Cerberus Network", 5 "Connected to Cerberus Network" (Y then opens the Cerberus terminal).
REXCVAR_DEFINE_INT32(me2_cerberus_connection_state, 5, "ME2",
                     "BlazeLogin GetConnectionState answer while me2_cerberus_spoof is on (-1 = the game's own; 5 = "
                     "connected, 4 = disconnected, 2 = activate Cerberus, 0/1/3 = connecting)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_diag_cerberus_natives, false, "ME2/Diagnostics",
                    "Log the online natives' functions (SFXOnline*, OnlineSubsystemLive, main menu) at the 300th Swap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
uint32_t Load32(const uint8_t* base, uint32_t va) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + va)); }

void Store32(uint8_t* base, uint32_t va, uint32_t v) { *reinterpret_cast<uint32_t*>(base + va) = __builtin_bswap32(v); }

void WriteTchar(uint8_t* base, uint32_t va, const std::string& s) {
  for (size_t i = 0; i <= s.size(); ++i) {
    const uint16_t c = i < s.size() ? uint16_t(uint8_t(s[i])) : 0;
    base[va + 2 * i] = uint8_t(c >> 8);
    base[va + 2 * i + 1] = uint8_t(c);
  }
}

std::string WideAt(const uint8_t* base, uint32_t va) {
  std::string s;
  for (int i = 0; i < 120; ++i, va += 2) {
    const uint16_t c = uint16_t(base[va] << 8 | base[va + 1]);
    if (!c) break;
    s += c < 128 ? char(c) : '?';
  }
  return s;
}
}  // namespace

extern "C" void Me2CerberusOnSwap(const uint8_t* base) {
  static std::atomic<uint32_t> swaps{0};
  if (swaps.fetch_add(1, std::memory_order_relaxed) + 1 != 300 || !REXCVAR_GET(me2_diag_cerberus_natives)) return;
  // The native tables live in .data, 0x83370000-0x83390000: entries name (a wide string in 0x82100000-0x822FF790), func.
  uint32_t logged = 0;
  for (uint32_t a = 0x83370000; a < 0x83390000; a += 4) {
    const uint32_t name = Load32(base, a);
    if (name < 0x82100000 || name >= 0x822FF790) continue;
    const std::string s = WideAt(base, name);
    if (s.rfind("int", 0) != 0 || s.find("exec") == std::string::npos) continue;
    if (s.find("SFXOnline") == std::string::npos && s.find("OnlineSubsystem") == std::string::npos &&
        s.find("MainMenu") == std::string::npos && s.find("EANetworking") == std::string::npos)
      continue;
    REXLOG_INFO("[cerberus] native {} at {:08X}: {:08X}", s, a, Load32(base, a + 4));
    ++logged;
  }
  REXLOG_INFO("[cerberus] {} online natives logged", logged);
}

// ---- the spoof
REX_EXTERN(__imp__sub_822B1050);  // FString::FString(const TCHAR*)
REX_EXTERN(__imp__sub_8288FB18);  // FString::~FString()

namespace {
std::atomic<int> g_spoof{-1};
bool Spoof() {
  int v = g_spoof.load(std::memory_order_relaxed);
  if (v < 0) {
    v = REXCVAR_GET(me2_cerberus_spoof) ? 1 : 0;
    g_spoof.store(v, std::memory_order_relaxed);
    REXLOG_INFO("[cerberus] login natives: {}", v ? "spoofed (connected, logged in, Cerberus member)" : "the game's own");
  }
  return v != 0;
}

// The first few answers of each native, the game's and ours, for the log.
void Note(const char* what, uint32_t game, uint32_t ours) {
  struct Seen { const char* what; uint32_t n; };
  static Seen seen[16] = {};
  for (Seen& s : seen) {
    if (!s.what) s.what = what;
    if (s.what != what) continue;
    if (s.n++ < 3) REXLOG_INFO("[cerberus] {}: the game's answer {}, given {}", what, game, ours);
    return;
  }
}
}  // namespace

#define ME2_CERBERUS_SPOOF(addr, what, bytes, value)                                            \
  REX_EXTERN(__imp__sub_##addr);                                                                \
  REX_HOOK_RAW(sub_##addr) {                                                                    \
    const uint32_t result = ctx.r5.u32;                                                         \
    __imp__sub_##addr(ctx, base);                                                               \
    if (!result || !Spoof()) return;                                                            \
    const uint32_t game = bytes == 1 ? base[result] : Load32(base, result);                     \
    if (bytes == 1) base[result] = uint8_t(value); else Store32(base, result, uint32_t(value)); \
    Note(what, game, uint32_t(value));                                                          \
  }
ME2_CERBERUS_SPOOF(82840FE0, "IsConnected", 4, 1)
ME2_CERBERUS_SPOOF(82840E18, "GetLoginStatus", 1, 2)
ME2_CERBERUS_SPOOF(82843608, "IsSignedIn", 4, 1)
ME2_CERBERUS_SPOOF(82842408, "IsCerberusMember", 4, 1)
// UE3's OnlineSubsystemLive (the Xbox LIVE profile), asked by SFXOnlineComponentUI.Connect when Y is pressed on the
// main menu's Cerberus tile ("Cerberus Network Activation requires an XBOX Live enabled game profile" otherwise):
// GetLoginStatus(LocalUserNum) sub_8285BE18 (byte, 2 = logged in), CanPlayOnline(LocalUserNum) sub_8285C0B8 (byte,
// EFeaturePrivilegeLevel 2 = enabled). Script-level answers only: XAM still reports a local profile.
ME2_CERBERUS_SPOOF(8285BE18, "OnlineSubsystemLive.GetLoginStatus", 1, 2)
ME2_CERBERUS_SPOOF(8285C0B8, "OnlineSubsystemLive.CanPlayOnline", 1, 2)

// GetConnectionState: the main menu asks it once after Press START (the game's answer on the port: 0, the menu then
// shows "Connecting to Cerberus Network"); me2_cerberus_connection_state sets the answer (5 = connected).
REX_EXTERN(__imp__sub_82843070);
REX_HOOK_RAW(sub_82843070) {
  const uint32_t result = ctx.r5.u32;
  __imp__sub_82843070(ctx, base);
  if (!result) return;
  static const int32_t state = REXCVAR_GET(me2_cerberus_connection_state);
  const uint32_t game = base[result];
  if (state >= 0 && Spoof()) base[result] = uint8_t(state);
  Note("GetConnectionState", game, base[result]);
}


// ---- diagnostic: which online natives the scripts call (me2_diag_cerberus_trace; the list: me2_cerberus_trace.inc)
REXCVAR_DEFINE_BOOL(me2_diag_cerberus_trace, false, "ME2/Diagnostics",
                    "Log the first calls of every online native (SFXOnline*, EANetworking, main menu, Live login)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
std::atomic<int> g_trace{-1};
void Trace(const char* name, std::atomic<uint32_t>& calls) {
  int t = g_trace.load(std::memory_order_relaxed);
  if (t < 0) {
    t = REXCVAR_GET(me2_diag_cerberus_trace) ? 1 : 0;
    g_trace.store(t, std::memory_order_relaxed);
  }
  if (!t) return;
  const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 3 || n == 10 || n == 100 || n == 1000) REXLOG_INFO("[cerberus-trace] {} (call {})", name, n);
}
}  // namespace

#define ME2_CERBERUS_TRACE(addr, name)          \
  REX_EXTERN(__imp__sub_##addr);                \
  REX_HOOK_RAW(sub_##addr) {                    \
    static std::atomic<uint32_t> calls{0};      \
    Trace(name, calls);                         \
    __imp__sub_##addr(ctx, base);               \
  }
#include "me2_cerberus_trace.inc"

// ---- the main menu's Cerberus terminal: "connected" and our own news item, in UnrealScript.
//
// BioSFHandler_MainMenu (SFXGame, tools/me2_ue3_disasm.py on the package tools/me2_package.py decompresses):
//   InitCerberus: m_BlazeUI = oOnline.GetComponentUserInterface(); CerberusConnect(1)   (on opening the menu)
//   CerberusConnect(mode): sets m_BlazeUI's callbacks (SetState -> SetOnlineState, OnDisplayNotification, ...), then
//     m_BlazeUI.Connect(mode): the real login (no LIVE: "requires an XBOX Live enabled game profile"; with
//     xam_signed_in_live: "Welcome to Cerberus Network", then an error).
//   RefreshCerberusState: SetOnlineState(oLogin.GetConnectionState()); only called by OnInputFocusRecieved.
//   SetOnlineState(5): "Connected to Cerberus Network" and CauseEvent('OpenRightComputer') (the terminal).
//   OnDisplayNotification -> AddCerberusItem(i_sTitle, i_sInfo, i_sImage): an item of the terminal's news.
// With me2_cerberus_spoof, when InitCerberus calls GetComponentUserInterface (hook below, the caller's FFrame in r4:
// +4 Node = the running UFunction, +8 Object, +0xC Code), InitCerberus gets new bytecode: its own up to the
// CerberusConnect(1) call (a call by name: 1B, FName (index, number), 24 01 16), then RefreshCerberusState() and
// AddCerberusItem(me2_cerberus_news, me2_cerberus_news_text, ""), return. The buffer comes from the game's allocator
// (an FString of spaces, never freed); the UFunction's Script (TArray at +0x54) and the running frame's Code move to
// it. SFXOnlineComponentUI.Connect (sub_82A2C4A8) is skipped (its one parameter taken off the code) while spoofing.
// UFunction (in memory): +0x2C Name index, +0x54 Script data / num / max. Runtime names: GNames, a TArray of
// FNameEntry* in .data, found by the entry of InitCerberus's own name holding "InitCerberus".
REXCVAR_DEFINE_STRING(me2_cerberus_news_text, "The Unofficial Mass Effect 2 Recompilation Project", "ME2",
                      "Body of the Cerberus terminal's news item (me2_cerberus_news is its title)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2026-10-27 (user): the main menu's "Connected to Cerberus Network" (SetOnlineState, case 5: sMessage =
// srCerb_ConnectedToCerb) says this instead, and the terminal's news navigation (the main menu movie's
// _root.menuCerberusMC.CerberusAction: the "News Items (n/m)" counter, Previous Item / Next Item and their buttons)
// is hidden.
REXCVAR_DEFINE_STRING(me2_cerberus_connected_text, "Cerberus Network Bypassed", "ME2",
                      "The main menu's Cerberus state text while connected (spoofed); \"\" = the game's own "
                      "\"Connected to Cerberus Network\"")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_cerberus_news_navigation, false, "ME2",
                    "Show the Cerberus terminal's news navigation (News Items (n/m), Previous Item, Next Item)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
struct Names {
  uint32_t data = 0, num = 0;
  uint32_t str_off = 0;  // where an entry's ANSI string starts
  uint32_t array = 0;   // stable GNames TArray header; its backing allocation can move on package loads
  bool ok = false;
};
Names g_names;

// A guest address whose bytes [va, va + len) are committed (the guest heaps' own page table; UE3's objects are at
// 0x2xxxxxxx on the console and the PC).
bool Readable(uint32_t va, uint32_t len) {
  if (va < 0x00010000u || va >= 0xA0000000u || va + len < va) return false;
  rex::memory::BaseHeap* heap = REX_KERNEL_MEMORY()->LookupHeap(va);
  return heap && (int(heap->QueryRangeAccess(va, va + len - 1)) & int(rex::memory::PageAccess::kReadOnly)) != 0;
}
bool HeapPointer(uint32_t va) { return Readable(va, 32); }

int FindInEntry(const uint8_t* base, uint32_t entry, const char* s) {
  const size_t n = std::strlen(s);
  for (uint32_t k = 0; k < 24; ++k)
    if (std::memcmp(base + entry + k, s, n + 1) == 0) return int(k);
  return -1;
}

bool RefreshNames(const uint8_t* base) {
  if (!g_names.ok || !Readable(g_names.array, 12)) return false;
  const uint32_t data = Load32(base, g_names.array), num = Load32(base, g_names.array + 4);
  const uint32_t max = Load32(base, g_names.array + 8);
  if (!num || num > 0x400000u || max < num || max > 0x400000u || (data & 3)) return false;
  if ((data != g_names.data || num != g_names.num) && !Readable(data, 4 * num)) return false;
  g_names.data = data;
  g_names.num = num;
  return true;
}

bool FindNames(const uint8_t* base, uint32_t known_index, const char* known) {
  if (g_names.ok) return RefreshNames(base);
  for (uint32_t a = 0x8300FD60u; a < 0x83530000u; a += 4) {
    const uint32_t data = Load32(base, a), num = Load32(base, a + 4), max = Load32(base, a + 8);
    if (num <= known_index || num > 0x400000u || max < num || max > 0x400000u || (data & 3) || !Readable(data, 4 * num))
      continue;
    const uint32_t entry = Load32(base, data + 4 * known_index);
    if (!HeapPointer(entry)) continue;
    const int off = FindInEntry(base, entry, known);
    if (off < 0) continue;
    g_names = {data, num, uint32_t(off), a, true};
    REXLOG_INFO("[cerberus] GNames at {:08X}: data {:08X}, {} names, strings at +{}", a, data, num, off);
    return true;
  }
  REXLOG_WARN("[cerberus] GNames not found");
  return false;
}

int32_t NameIndex(const uint8_t* base, const char* s) {
  if (!RefreshNames(base)) return -1;
  const size_t n = std::strlen(s);
  for (uint32_t i = 0; i < g_names.num; ++i) {
    const uint32_t entry = Load32(base, g_names.data + 4 * i);
    if (HeapPointer(entry) && std::memcmp(base + entry + g_names.str_off, s, n + 1) == 0) return int32_t(i);
  }
  return -1;
}

bool NameIs(const uint8_t* base, uint32_t index, const char* s) {
  if (!RefreshNames(base) || index >= g_names.num) return false;
  const uint32_t entry = Load32(base, g_names.data + 4 * index);
  return HeapPointer(entry) && std::memcmp(base + entry + g_names.str_off, s, std::strlen(s) + 1) == 0;
}

void PutName(std::vector<uint8_t>& c, int32_t index) {
  for (int k = 3; k >= 0; --k) c.push_back(uint8_t(uint32_t(index) >> (8 * k)));
  for (int k = 0; k < 4; ++k) c.push_back(0);
}

// UTF-8 to UTF-16 code units (the Basic Multilingual Plane; anything else becomes '?').
std::vector<uint16_t> Utf16(const std::string& s) {
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
      len = 4;  // outside the BMP
    }
    out.push_back(uint16_t(cp));
    i += len;
  }
  return out;
}

// A string constant: ASCII as EX_StringConst (0x1F, one byte a character); anything else (2026-11-01: the Russian
// edition's texts) as EX_UnicodeStringConst (0x34), TCHARs the interpreter reads as they are in memory: big-endian.
void PutString(std::vector<uint8_t>& c, const std::string& s) {
  const bool ascii = std::all_of(s.begin(), s.end(), [](char ch) { return uint8_t(ch) < 0x80; });
  if (ascii) {
    c.push_back(0x1F);  // EX_StringConst
    for (char ch : s) c.push_back(uint8_t(ch));
    c.push_back(0);
    return;
  }
  c.push_back(0x34);  // EX_UnicodeStringConst
  for (const uint16_t u : Utf16(s)) {
    c.push_back(uint8_t(u >> 8));
    c.push_back(uint8_t(u));
  }
  c.push_back(0);
  c.push_back(0);
}

// A buffer for new bytecode: an FString of spaces made by the game (FString(const TCHAR*), sub_822B1050), kept for
// good; 0 if it failed.
uint32_t AllocScript(PPCContext& ctx, uint8_t* base, size_t bytes) {
  const uint32_t chars = uint32_t(bytes) / 2 + 2;
  const uint64_t r1 = ctx.r1.u64;
  const uint32_t sp = (ctx.r1.u32 - 0x200u - chars * 2 - 0x40u) & ~15u;
  const uint32_t fstring = sp + 0x80, text_va = sp + 0x90;
  Store32(base, sp, ctx.r1.u32);
  WriteTchar(base, text_va, std::string(chars - 1, ' '));
  ctx.r1.u64 = sp;
  ctx.r3.u64 = fstring;
  ctx.r4.u64 = text_va;
  __imp__sub_822B1050(ctx, base);
  ctx.r1.u64 = r1;
  const uint32_t buffer = Load32(base, fstring);
  return HeapPointer(buffer) ? buffer : 0;
}

// A field (function, property, ...) of a class or its super classes by name. The UObject / UStruct offsets (Outer,
// UField::Next, UStruct::SuperField / Children) are found once, from the class of a function: the layout where the
// class's own name reads back and its children list holds 'probe'.
struct Layout {
  uint32_t outer = 0, next = 0, super = 0, children = 0;
  bool ok = false;
};
Layout g_layout;

uint32_t FieldIn(const uint8_t* base, const Layout& l, uint32_t cls, const char* name) {
  for (uint32_t s = cls, depth = 0; HeapPointer(s) && depth < 16; s = Load32(base, s + l.super), ++depth) {
    uint32_t f = Load32(base, s + l.children);
    for (uint32_t n = 0; HeapPointer(f) && n < 2000; f = Load32(base, f + l.next), ++n)
      if (NameIs(base, Load32(base, f + 0x2C), name)) return f;
  }
  return 0;
}

uint32_t FindField(const uint8_t* base, uint32_t function, const char* class_name, const char* name) {
  if (!g_layout.ok) {
    for (uint32_t outer = 0x20; outer <= 0x30 && !g_layout.ok; outer += 4) {
      const uint32_t cls = Load32(base, function + outer);
      if (!HeapPointer(cls) || !NameIs(base, Load32(base, cls + 0x2C), class_name)) continue;
      for (uint32_t next = 0x30; next <= 0x44 && !g_layout.ok; next += 4)
        for (uint32_t children = next + 4; children <= 0x50 && !g_layout.ok; children += 4)
          for (uint32_t super = next + 4; super <= 0x50 && !g_layout.ok; super += 4) {
            if (super == children) continue;
            const Layout l{outer, next, super, children, true};
            if (FieldIn(base, l, cls, "SetOnlineState") && FieldIn(base, l, cls, "AddCerberusItem")) {
              g_layout = l;
              REXLOG_INFO("[cerberus] UObject layout: Outer +0x{:X}, Next +0x{:X}, SuperField +0x{:X}, Children +0x{:X}",
                          outer, next, super, children);
            }
          }
    }
    if (!g_layout.ok) {
      REXLOG_WARN("[cerberus] the class layout was not found; the menu text and news navigation stay the game's");
      g_layout.outer = 1;  // tried
      return 0;
    }
  }
  return FieldIn(base, g_layout, Load32(base, function + g_layout.outer), name);
}

// A statement of a function's code [at, back) becomes a jump to 'replacement' put at the end, which jumps back to
// 'back' (the function's own jump offsets stay valid). The function must not be running.
bool ReplaceStatement(PPCContext& ctx, uint8_t* base, uint32_t function, uint32_t at, uint32_t back,
                      const std::vector<uint8_t>& replacement) {
  const uint32_t script = Load32(base, function + 0x54), num = Load32(base, function + 0x58);
  if (!HeapPointer(script) || num <= back || num > 0x4000 || back - at < 3) return false;
  std::vector<uint8_t> c(base + script, base + script + num);
  const uint32_t tail = num;
  c[at] = 0x06;  // goto tail
  c[at + 1] = uint8_t(tail >> 8);
  c[at + 2] = uint8_t(tail);
  for (uint32_t i = at + 3; i < back; ++i) c[i] = 0x0B;
  c.insert(c.end(), replacement.begin(), replacement.end());
  c.push_back(0x06);  // goto back
  c.push_back(uint8_t(back >> 8));
  c.push_back(uint8_t(back));
  c.push_back(0x53);
  const uint32_t buffer = AllocScript(ctx, base, c.size());
  if (!buffer) return false;
  std::memcpy(base + buffer, c.data(), c.size());
  Store32(base, function + 0x54, buffer);
  Store32(base, function + 0x58, uint32_t(c.size()));
  Store32(base, function + 0x5C, uint32_t(c.size()));
  return true;
}

// The main menu's "Connected to Cerberus Network" (srCerb_ConnectedToCerb, through 38 5C: the primitive cast
// StringRef -> String of 01 the instance variable) is set in two places:
//   SetOnlineState case 5, +0x82: sMessage = cast(srCerb_ConnectedToCerb)   (0F, 00 local, 38 5C 01 property; 13 bytes)
//   CerberusScreenStateChange case 1 (the right terminal opened, its top screen), +0x72:
//     SetCerberusConnectState(false, cast(srCerb_ConnectedToCerb), true, true)   (1B name, 28, 38 5C 01 property,
//     27 27 16; 20 bytes)
// Both get our text as a string constant instead.
void PatchConnectedText(PPCContext& ctx, uint8_t* base, uint32_t node, const std::string& text) {
  if (const uint32_t f = FindField(base, node, "BioSFHandler_MainMenu", "SetOnlineState")) {
    const uint32_t s = Load32(base, f + 0x54), n = Load32(base, f + 0x58);
    constexpr uint32_t at = 0x82, back = 0x8F;
    std::vector<uint8_t> r;
    if (HeapPointer(s) && n > back && base[s + at] == 0x0F && base[s + at + 1] == 0x00 && base[s + at + 6] == 0x38 &&
        base[s + at + 7] == 0x5C && base[s + at + 8] == 0x01 && base[s + back] == 0x14) {
      r.assign(base + s + at, base + s + at + 6);  // sMessage =
      PutString(r, text);
    }
    if (!r.empty() && ReplaceStatement(ctx, base, f, at, back, r))
      REXLOG_INFO("[cerberus] SetOnlineState: \"connected\" text -> \"{}\"", text);
    else
      REXLOG_WARN("[cerberus] SetOnlineState: not the expected code; its text stays the game's");
  }
  if (const uint32_t f = FindField(base, node, "BioSFHandler_MainMenu", "CerberusScreenStateChange")) {
    const uint32_t s = Load32(base, f + 0x54), n = Load32(base, f + 0x58);
    constexpr uint32_t at = 0x72, back = 0x86;
    std::vector<uint8_t> r;
    if (HeapPointer(s) && n > back && base[s + at] == 0x1B && NameIs(base, Load32(base, s + at + 1), "SetCerberusConnectState") &&
        base[s + at + 9] == 0x28 && base[s + at + 10] == 0x38 && base[s + at + 11] == 0x5C && base[s + at + 12] == 0x01 &&
        base[s + at + 17] == 0x27 && base[s + at + 18] == 0x27 && base[s + at + 19] == 0x16 && base[s + back] == 0x06) {
      r.assign(base + s + at, base + s + at + 10);  // SetCerberusConnectState(false,
      PutString(r, text);
      r.insert(r.end(), {0x27, 0x27, 0x16});  // true, true)
    }
    if (!r.empty() && ReplaceStatement(ctx, base, f, at, back, r))
      REXLOG_INFO("[cerberus] CerberusScreenStateChange: \"connected\" text -> \"{}\"", text);
    else
      REXLOG_WARN("[cerberus] CerberusScreenStateChange: not the expected code; its text stays the game's");
  }
}

// self.oPanel.SetClipVisibility(path, false), in the form the game's own calls have (SFXSFHandler_HUD.SetOverheat):
// 19 (context) 01 oPanel, skip (2 bytes) and 00, then the call (1B name ... 16). The oPanel property's pointer is
// taken from AddCerberusItem's self.oPanel.InvokeMethodArgs (at +0x6C: 19 01 property).
bool PutHideClip(std::vector<uint8_t>& c, const uint8_t* base, uint32_t add_item, int32_t set_clip, const char* path) {
  const uint32_t script = Load32(base, add_item + 0x54), num = Load32(base, add_item + 0x58);
  if (!HeapPointer(script) || num < 0x80 || base[script + 0x6C] != 0x19 || base[script + 0x6D] != 0x01) return false;
  const uint32_t panel = Load32(base, script + 0x6E);
  if (!HeapPointer(panel)) return false;
  std::vector<uint8_t> call;
  call.push_back(0x1B);
  PutName(call, set_clip);
  PutString(call, path);
  call.push_back(0x28);  // false
  call.push_back(0x16);
  c.push_back(0x19);
  c.push_back(0x01);
  for (int k = 3; k >= 0; --k) c.push_back(uint8_t(panel >> (8 * k)));
  c.push_back(uint8_t(call.size() >> 8));
  c.push_back(uint8_t(call.size()));
  c.push_back(0x00);
  c.insert(c.end(), call.begin(), call.end());
  return true;
}

void PatchInitCerberus(PPCContext& ctx, uint8_t* base, uint32_t frame) {
  const uint32_t node = Load32(base, frame + 4), pc = Load32(base, frame + 0xC);
  if (!HeapPointer(node) || !HeapPointer(pc)) return;
  const uint32_t node_name = Load32(base, node + 0x2C);
  if (!FindNames(base, node_name, "InitCerberus") || !NameIs(base, node_name, "InitCerberus")) return;
  const uint32_t script = Load32(base, node + 0x54), num = Load32(base, node + 0x58);
  if (!HeapPointer(script) || pc < script || pc + 12 > script + num) return;
  // The CerberusConnect(1) call right here: 1B, FName, 24 01 16.
  if (base[pc] != 0x1B || !NameIs(base, Load32(base, pc + 1), "CerberusConnect") || base[pc + 9] != 0x24 ||
      base[pc + 10] != 0x01 || base[pc + 11] != 0x16) {
    REXLOG_WARN("[cerberus] InitCerberus: the CerberusConnect(1) call is not where expected; not changed");
    return;
  }
  const int32_t refresh = NameIndex(base, "RefreshCerberusState"), add = NameIndex(base, "AddCerberusItem");
  if (refresh < 0 || add < 0) {
    REXLOG_WARN("[cerberus] InitCerberus: names not found (RefreshCerberusState {}, AddCerberusItem {})", refresh, add);
    return;
  }
  const uint32_t prefix = pc - script;
  std::vector<uint8_t> c(base + script, base + pc);
  c.push_back(0x1B);  // RefreshCerberusState() in the call's 12 bytes (the jump past it lands right after)
  PutName(c, refresh);
  c.push_back(0x16);
  c.push_back(0x0B);
  c.push_back(0x0B);
  static const std::string title = REXCVAR_GET(me2_cerberus_news), text = REXCVAR_GET(me2_cerberus_news_text);
  if (!title.empty()) {
    c.push_back(0x1B);  // AddCerberusItem(title, text, "")
    PutName(c, add);
    PutString(c, title);
    PutString(c, text);
    PutString(c, "");
    c.push_back(0x16);
  }
  // Before RefreshCerberusState runs (below, in the new code): the "connected" text.
  static const std::string connected = REXCVAR_GET(me2_cerberus_connected_text);
  if (!connected.empty()) PatchConnectedText(ctx, base, node, connected);
  // The news navigation hidden (after AddCerberusItem, which sets the movie's news up).
  bool hidden = false;
  if (!REXCVAR_GET(me2_cerberus_news_navigation)) {
    const int32_t set_clip = NameIndex(base, "SetClipVisibility");
    const uint32_t add_item = FindField(base, node, "BioSFHandler_MainMenu", "AddCerberusItem");
    hidden = set_clip >= 0 && add_item && PutHideClip(c, base, add_item, set_clip, "_root.menuCerberusMC.CerberusAction");
    if (!hidden) REXLOG_WARN("[cerberus] InitCerberus: the news navigation could not be hidden");
  }
  c.push_back(0x04);  // return nothing
  c.push_back(0x0B);
  c.push_back(0x53);  // end of script
  const uint32_t buffer = AllocScript(ctx, base, c.size());
  if (!buffer) return;
  std::memcpy(base + buffer, c.data(), c.size());
  Store32(base, node + 0x54, buffer);
  Store32(base, node + 0x58, uint32_t(c.size()));
  Store32(base, node + 0x5C, uint32_t(c.size()));
  Store32(base, frame + 0xC, buffer + prefix);  // the running InitCerberus goes on in the new code
  REXLOG_INFO("[cerberus] InitCerberus: CerberusConnect(1) -> RefreshCerberusState(){}{}; {} bytes at {:08X}",
              title.empty() ? std::string() : " + AddCerberusItem(\"" + title + "\")",
              hidden ? " + news navigation hidden" : "", c.size(), buffer);
}
}  // namespace

extern "C" bool Me2SwapControllerLabels(uint8_t* base, uint32_t va, uint32_t size);  // me2_button_icons.cpp

namespace {
// The controller labels (me2_button_icons.cpp): the GFxMovieInfo GUI_SF_SharedAssets.mainController's raw data. Found
// through GObjects (a TArray of UObject* in .data, told apart by holding the running InitCerberus), then the object
// named "mainController" of class GFxMovieInfo, then its TArray<byte> that starts "GFX". Object memory only (a
// sweep of the guest's memory read pages Renut keeps unmapped for its GPU-write tracking, PC 2026-10-27).
void PatchControllerLabels(const uint8_t* cbase, uint8_t* base, uint32_t frame) {
  static bool done = false;
  if (done) return;
  const uint32_t node = Load32(cbase, frame + 4);
  if (!HeapPointer(node)) return;
  const uint32_t node_name = Load32(cbase, node + 0x2C);
  if (!FindNames(cbase, node_name, "InitCerberus") || !NameIs(cbase, node_name, "InitCerberus")) return;
  done = true;
  const int32_t movie_name = NameIndex(cbase, "mainController");
  if (movie_name < 0) {
    REXLOG_WARN("[icons] controller labels: no name mainController");
    return;
  }
  for (uint32_t a = 0x8300FD60u; a < 0x83530000u; a += 4) {
    const uint32_t data = Load32(cbase, a), num = Load32(cbase, a + 4), max = Load32(cbase, a + 8);
    if (num < 10000 || num > 0x400000u || max < num || max > 0x400000u || (data & 3) || !Readable(data, 4 * num))
      continue;
    bool holds = false;
    for (uint32_t i = 0; i < num && !holds; ++i) holds = Load32(cbase, data + 4 * i) == node;
    if (!holds) continue;
    for (uint32_t i = 0; i < num; ++i) {
      const uint32_t obj = Load32(cbase, data + 4 * i);
      if (!HeapPointer(obj) || Load32(cbase, obj + 0x2C) != uint32_t(movie_name)) continue;
      const uint32_t cls = Load32(cbase, obj + 0x34);
      if (!HeapPointer(cls) || !NameIs(cbase, Load32(cbase, cls + 0x2C), "GFxMovieInfo")) continue;
      for (uint32_t off = 0x3C; off < 0x100; off += 4) {
        const uint32_t d = Load32(cbase, obj + off), n = Load32(cbase, obj + off + 4), m = Load32(cbase, obj + off + 8);
        if (n < 6868 || n > 0x10000 || m < n || !Readable(d, n) || std::memcmp(cbase + d, "GFX", 3) != 0) continue;
        if (!Me2SwapControllerLabels(base, d, n))
          REXLOG_WARN("[icons] controller labels: GFxMovieInfo mainController at {:08X} is not the expected movie", obj);
        return;
      }
    }
    REXLOG_WARN("[icons] controller labels: GFxMovieInfo mainController not found (GObjects at {:08X})", a);
    return;
  }
  REXLOG_WARN("[icons] controller labels: GObjects not found");
}
}  // namespace

REX_EXTERN(__imp__sub_8283AA18);  // SFXOnlineSubsystem.GetComponentUserInterface (exec)
REX_HOOK_RAW(sub_8283AA18) {
  const uint32_t frame = ctx.r4.u32;
  __imp__sub_8283AA18(ctx, base);
  // InitCerberus (the main menu): also the moment for the controller labels.
  PatchControllerLabels(base, base, frame);
  if (Spoof()) PatchInitCerberus(ctx, base, frame);
}

REX_EXTERN(__imp__sub_82A2C4A8);  // SFXOnlineComponentUI.Connect(byte mode) (exec)
REX_HOOK_RAW(sub_82A2C4A8) {
  const uint32_t frame = ctx.r4.u32;
  const uint32_t code = Load32(base, frame + 0xC);
  // While spoofing: no real login. The parameter is a local variable (00, property) then 16 (end of parameters).
  if (Spoof() && base[code] == 0x00 && base[code + 5] == 0x16) {
    Store32(base, frame + 0xC, code + 6);
    static uint32_t n = 0;
    if (n++ < 3) REXLOG_INFO("[cerberus] SFXOnlineComponentUI.Connect skipped (the login is spoofed)");
    return;
  }
  __imp__sub_82A2C4A8(ctx, base);
}

// 2026-10-29: for other modules (me2_selection.cpp, the hacking mini-game): the function 'func' of the object's class
// (or its super classes) when the class is called 'class_name'; 0 otherwise or before the main menu found GNames and the
// class layout (InitCerberus). UE3's game thread.
extern "C" bool me2_object_lookup_ready() {
  return g_names.ok && g_layout.ok && g_layout.outer != 1;
}
// 2026-10-31: for me2_frame_diag.cpp: whether guest bytes [va, va + len) are committed (Readable above).
extern "C" bool me2_guest_readable(uint32_t va, uint32_t len) { return Readable(va, len); }
// 2026-10-31: for me2_frame_diag.cpp (who forces the garbage collection): the text of FName 'index' into 'out'; false
// before the main menu found GNames or for an index outside the table.
extern "C" bool me2_name_text(const uint8_t* base, uint32_t index, char* out, size_t size) {
  if (!size) return false;
  out[0] = 0;
  if (!g_names.ok || !RefreshNames(base) || index >= g_names.num) return false;
  const uint32_t entry = Load32(base, g_names.data + 4 * index);
  if (!Readable(entry + g_names.str_off, 1)) return false;
  size_t i = 0;
  for (; i + 1 < size; ++i) {
    const char c = char(base[entry + g_names.str_off + i]);
    if (!c) break;
    out[i] = c;
  }
  out[i] = 0;
  return true;
}
extern "C" uint32_t me2_object_function(const uint8_t* base, uint32_t obj, const char* class_name, const char* func) {
  if (!me2_object_lookup_ready() || !HeapPointer(obj)) return 0;
  for (uint32_t cls = Load32(base, obj + 0x34), depth = 0; HeapPointer(cls) && depth < 16;
       cls = Load32(base, cls + g_layout.super), ++depth) {
    if (NameIs(base, Load32(base, cls + 0x2C), class_name))
      return FieldIn(base, g_layout, cls, func);
  }
  return 0;
}

// Native UProperty layout is also used by UInput::ResetInput (sub_825E94A0):
// ArrayDim +0x44, ElementSize +0x48, Offset +0x68. Fail closed on unexpected metadata.
extern "C" uint32_t me2_object_float_property(const uint8_t* base, uint32_t obj, const char* class_name,
                                             const char* property, uint32_t count) {
  const uint32_t field = me2_object_function(base, obj, class_name, property);
  if (!Readable(field, 0x6C)) return 0;
  const uint32_t type = Load32(base, field + 0x34);
  if (!HeapPointer(type) || !NameIs(base, Load32(base, type + 0x2C), "FloatProperty") ||
      Load32(base, field + 0x44) != count || Load32(base, field + 0x48) != 4) return 0;
  const uint32_t offset = Load32(base, field + 0x68);
  return offset >= 0x3C && offset < 0x4000 && !(offset & 3) ? offset : 0;
}
