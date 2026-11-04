// Mass Effect 2 - saves in the Switch's own save data, per user.
//
// 2026-11-01 (user): the saves move from the SD card (<user_data_root>/<xuid>/454108CE/00000001/...) into the save
// data the console keeps for each user of the HOME-menu forwarder's title (tools/nx/build_me2_forwarder.sh: its NACP
// declares the save data and asks for a user at launch). They show up in System Settings -> Data Management and in
// JKSV / Checkpoint; a crash in the middle of saving leaves the previous save, because nothing reaches the console's
// storage before a commit.
//
// Only when the game runs under the forwarder's title ID (me2_switch_save_title_id): from hbmenu the process has no
// title of its own, and under title takeover it has another game's, whose save data must not be touched; the saves
// then stay on the SD card as before.
//
// What moves: saved games (content type 1, with their headers) and the per-game profile settings. DLC (type 2) and the
// rest of the user data (shader cache, achievements) stay on the SD card. The SDK's ContentManager routes the paths
// (SetSavedGameRoot) and calls back after each write (container closed, header, thumbnail, delete, profile setting):
// that is where the save data is committed. A commit fails while a file is still open for writing; it is then tried
// again every second for a minute (one retry thread for the session), at the next write, and when the game loses focus.
//
// First start: with me2_switch_save_copy_sd the saves already on the SD card are copied in (files that are not there
// yet; the SD copies stay as a backup), then a marker (.from_sd) keeps it from happening again.
#include "me2_switch_save.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

REXCVAR_DEFINE_BOOL(me2_switch_save, true, "ME2",
                    "Switch: saves in the console's save data of the user picked at launch (HOME-menu forwarder only); "
                    "false = on the SD card as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(me2_switch_save_title_id, "014D453244000000", "ME2",
                      "Switch: the forwarder's title ID, owner of the save data (other titles keep the SD saves)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(me2_switch_save_copy_sd, true, "ME2",
                    "Switch: on the first start, copy the saves from the SD card into the console's save data (the SD "
                    "copies stay)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

namespace me2::switch_save {

#if REX_PLATFORM_SWITCH
namespace {

constexpr char kDevice[] = "me2save";
constexpr char kRoot[] = "me2save:/";
constexpr char kMarker[] = "me2save:/.from_sd";
// The same sizes as the forwarder's NACP (tools/nx/build_me2_forwarder.sh), for a save data this code has to create.
// A save is ~30 KB; the journal bounds what can be written between two commits.
constexpr s64 kSaveSize = 64ll << 20;
constexpr s64 kJournalSize = 32ll << 20;

std::mutex g_mutex;
bool g_mounted = false;
bool g_pending = false;
std::atomic<bool> g_retrying{false};
std::atomic<bool> g_stop{false};
uint64_t g_failures = 0;  // commits after a write that failed (g_mutex)

bool CommitLocked(const char* what) {
  if (!g_mounted) return true;
  const Result rc = fsdevCommitDevice(kDevice);
  if (R_SUCCEEDED(rc)) {
    if (g_pending) REXLOG_INFO("[switch-save] committed ({}), after an earlier failure", what);
    g_pending = false;
    return true;
  }
  if (!g_pending)
    REXLOG_WARN("[switch-save] commit after '{}' failed (0x{:X}); tried again", what, rc);
  g_pending = true;
  if (std::strcmp(what, "retry") != 0) ++g_failures;  // a new write that did not commit
  return false;
}

std::condition_variable g_retry_cv;

// One retry thread for the session, started at the first failed commit (g_mutex held). Never detached and never
// destroyed (2026-11-01 crash: libnx's pthreads have no pthread_detach, so std::thread::detach() threw ENOSYS and the
// game terminated; a joinable std::thread destroyed at exit would terminate too): the object is left allocated.
void StartRetries() {
  g_retry_cv.notify_one();
  if (g_retrying.exchange(true)) return;
  new std::thread([] {
    std::unique_lock<std::mutex> lock(g_mutex);
    uint64_t seen = 0;
    while (!g_stop.load()) {
      // Until a commit fails that this thread has not tried to repair yet.
      g_retry_cv.wait(lock, [&] { return (g_pending && g_failures != seen) || g_stop.load(); });
      seen = g_failures;
      // Once a second for a minute; later writes commit on their own (and wake this again if they fail).
      for (int i = 0; i < 60 && g_pending && !g_stop.load(); ++i) {
        g_retry_cv.wait_for(lock, std::chrono::seconds(1), [] { return g_stop.load(); });
        if (g_pending) CommitLocked("retry");
      }
      seen = g_failures;
      if (g_pending && !g_stop.load())
        REXLOG_ERROR("[switch-save] still not committed after a minute; tried again at the next save");
    }
  });
}

void Written(const char* what) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!CommitLocked(what)) StartRetries();
}

std::string Trimmed(std::string s) {
  std::erase(s, '"');  // string cvars can come back quoted
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
  return s;
}

std::string Nickname(AccountUid uid) {
  AccountProfile profile;
  if (R_FAILED(accountGetProfile(&profile, uid))) return "?";
  AccountProfileBase base{};
  AccountUserData data{};
  std::string name = "?";
  if (R_SUCCEEDED(accountProfileGet(&profile, &data, &base))) name.assign(base.nickname, strnlen(base.nickname, sizeof(base.nickname)));
  accountProfileClose(&profile);
  return name;
}

// The user picked on the HOME menu before the launch (the forwarder's NACP asks for one); without one (an older
// forwarder), the user selection screen.
bool PickUser(AccountUid* uid, std::string* nickname) {
  *uid = {};
  if (R_FAILED(accountInitialize(AccountServiceType_Application))) {
    REXLOG_WARN("[switch-save] account service not available");
    return false;
  }
  if (R_FAILED(accountGetPreselectedUser(uid)) || !accountUidIsValid(uid)) {
    *uid = {};
    PselUserSelectionSettings settings{};
    const Result rc = pselShowUserSelector(uid, &settings);
    if (R_FAILED(rc) || !accountUidIsValid(uid)) {
      REXLOG_WARN("[switch-save] no user picked (0x{:X})", rc);
      accountExit();
      return false;
    }
  }
  *nickname = Nickname(*uid);
  accountExit();
  return true;
}

bool Mount(u64 title, AccountUid uid) {
  Result rc = fsdevMountSaveData(kDevice, title, uid);
  if (R_SUCCEEDED(rc)) return true;
  // Not created yet (the HOME menu does not create it): as JKSV / Checkpoint do.
  FsSaveDataAttribute attr{};
  attr.application_id = title;
  attr.uid = uid;
  attr.save_data_type = FsSaveDataType_Account;
  FsSaveDataCreationInfo info{};
  info.save_data_size = kSaveSize;
  info.journal_size = kJournalSize;
  info.available_size = 0x4000;
  info.owner_id = title;
  info.save_data_space_id = FsSaveDataSpaceId_User;
  FsSaveDataMetaInfo meta{};
  meta.size = 0x40060;
  meta.type = FsSaveDataMetaType_Thumbnail;
  const Result created = fsCreateSaveDataFileSystem(&attr, &info, &meta);
  rc = fsdevMountSaveData(kDevice, title, uid);
  if (R_FAILED(rc)) {
    REXLOG_WARN("[switch-save] save data of {:016X} not mounted (0x{:X}; create 0x{:X})", title, rc, created);
    return false;
  }
  REXLOG_INFO("[switch-save] save data of {:016X} created ({} MB, journal {} MB)", title, kSaveSize >> 20,
              kJournalSize >> 20);
  return true;
}

// A write, read back, and commit: the save data works before the game is pointed at it.
bool Probe() {
  const char* path = "me2save:/.probe";
  bool ok = false;
  if (std::FILE* f = std::fopen(path, "wb")) {
    ok = std::fwrite("me2", 1, 3, f) == 3;
    ok = std::fclose(f) == 0 && ok;
  }
  char back[4] = {};
  if (std::FILE* f = ok ? std::fopen(path, "rb") : nullptr) {
    ok = std::fread(back, 1, 3, f) == 3 && std::string(back, 3) == "me2";
    std::fclose(f);
  } else {
    ok = false;
  }
  std::remove(path);
  return ok && R_SUCCEEDED(fsdevCommitDevice(kDevice));
}

struct CopyCount {
  uint32_t files = 0, skipped = 0, failed = 0;
  uint64_t bytes = 0, since_commit = 0;
};

bool CopyFile(const std::filesystem::path& from, const std::filesystem::path& to, CopyCount& n) {
  std::FILE* in = rex::filesystem::OpenFile(from, "rb");
  if (!in) return false;
  std::FILE* out = rex::filesystem::OpenFile(to, "wb");
  if (!out) {
    std::fclose(in);
    return false;
  }
  std::vector<uint8_t> buffer(64 * 1024);
  bool ok = true;
  for (;;) {
    const size_t got = std::fread(buffer.data(), 1, buffer.size(), in);
    if (got == 0) break;
    if (std::fwrite(buffer.data(), 1, got, out) != got) {
      ok = false;
      break;
    }
    n.bytes += got;
    n.since_commit += got;
  }
  ok = !std::ferror(in) && ok;
  std::fclose(in);
  ok = std::fclose(out) == 0 && ok;
  return ok;
}

void CopyTree(const std::filesystem::path& from, const std::filesystem::path& to, CopyCount& n) {
  std::error_code ec;
  if (!std::filesystem::is_directory(from, ec)) return;
  std::filesystem::create_directories(to, ec);
  for (const auto& f : rex::filesystem::ListFiles(from)) {
    if (f.type == rex::filesystem::FileInfo::Type::kDirectory) {
      CopyTree(from / f.name, to / f.name, n);
      continue;
    }
    if (std::filesystem::exists(to / f.name, ec)) {
      ++n.skipped;
      continue;
    }
    if (CopyFile(from / f.name, to / f.name, n)) {
      ++n.files;
    } else {
      ++n.failed;
      REXLOG_WARN("[switch-save] could not copy {}", rex::path_to_utf8(from / f.name));
    }
    // The journal holds what is written between two commits.
    if (n.since_commit > (8u << 20)) {
      fsdevCommitDevice(kDevice);
      n.since_commit = 0;
    }
  }
}

bool IsHex(const std::string& s, size_t length) {
  if (s.size() != length) return false;
  for (const char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// The SD saves: <root>/<xuid>/<title>/00000001 and .../Headers/00000001, and <root>/<title>/profile.
void CopyFromSd(const std::filesystem::path& sd) {
  CopyCount n;
  const std::filesystem::path save(kRoot);
  for (const auto& top : rex::filesystem::ListFiles(sd)) {
    if (top.type != rex::filesystem::FileInfo::Type::kDirectory) continue;
    const std::string name = rex::path_to_utf8(top.name);
    if (IsHex(name, 16)) {
      for (const auto& title : rex::filesystem::ListFiles(sd / top.name)) {
        if (title.type != rex::filesystem::FileInfo::Type::kDirectory || !IsHex(rex::path_to_utf8(title.name), 8))
          continue;
        const auto from = sd / top.name / title.name;
        const auto to = save / top.name / title.name;
        CopyTree(from / "00000001", to / "00000001", n);
        CopyTree(from / "Headers" / "00000001", to / "Headers" / "00000001", n);
      }
    } else if (IsHex(name, 8)) {
      CopyTree(sd / top.name / "profile", save / top.name / "profile", n);
    }
  }
  if (std::FILE* f = std::fopen(kMarker, "wb")) {
    const std::string text = "copied from " + rex::path_to_utf8(sd) + "\n";
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }
  const Result rc = fsdevCommitDevice(kDevice);
  REXLOG_INFO("[switch-save] first start: {} file(s), {} KB copied from {} ({} already there, {} failed; commit 0x{:X}). "
              "The SD copies are left as they are.",
              n.files, n.bytes >> 10, rex::path_to_utf8(sd), n.skipped, n.failed, rc);
}

}  // namespace

void Setup(rex::Runtime* runtime) {
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  auto* content = kernel ? kernel->content_manager() : nullptr;
  if (!content || !REXCVAR_GET(me2_switch_save)) return;
  const std::string wanted_text = Trimmed(REXCVAR_GET(me2_switch_save_title_id));
  const u64 wanted = std::strtoull(wanted_text.c_str(), nullptr, 16);
  u64 program = 0;
  svcGetInfo(&program, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
  const AppletType type = appletGetAppletType();
  if (wanted == 0 || program != wanted ||
      (type != AppletType_Application && type != AppletType_SystemApplication)) {
    REXLOG_INFO("[switch-save] saves on the SD card in {} (running as {:016X}, applet type {}; the console's save data "
                "is used under the forwarder {:016X})",
                rex::path_to_utf8(content->root_path()), program, int(type), wanted);
    return;
  }
  AccountUid uid;
  std::string nickname;
  if (!PickUser(&uid, &nickname) || !Mount(program, uid)) {
    REXLOG_WARN("[switch-save] saves on the SD card in {}", rex::path_to_utf8(content->root_path()));
    return;
  }
  if (!Probe()) {
    REXLOG_ERROR("[switch-save] the save data does not take a write; saves on the SD card in {}",
                 rex::path_to_utf8(content->root_path()));
    fsdevUnmountDevice(kDevice);
    return;
  }
  std::error_code ec;
  if (REXCVAR_GET(me2_switch_save_copy_sd) && !std::filesystem::exists(kMarker, ec))
    CopyFromSd(content->root_path());
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mounted = true;
  }
  content->SetSavedGameRoot(kRoot, Written);
  REXLOG_INFO("[switch-save] saves of '{}' in the console's save data of {:016X}", nickname, program);
}

void CommitPending() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pending) CommitLocked("focus lost");
}

void Shutdown() {
  g_stop.store(true);
  g_retry_cv.notify_all();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pending) CommitLocked("shutdown");
}

#else

void Setup(rex::Runtime*) {}
void CommitPending() {}
void Shutdown() {}

#endif

}  // namespace me2::switch_save
