/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/cvar.h>
#include <rex/thread/mutex.h>

REXCVAR_DECLARE(bool, allow_game_relative_writes);

namespace rex::filesystem {

// Dunwall (2026-11-09): when set, called with the name (no folder) of every file the guest opens that exists
// (input_hold_trigger_file, rex/input/read_hook.h). None by default.
using OpenObserver = void (*)(std::string_view file_name);
void SetOpenObserver(OpenObserver observer);

class VirtualFileSystem {
 public:
  VirtualFileSystem();
  ~VirtualFileSystem();

  bool RegisterDevice(std::unique_ptr<Device> device);
  bool UnregisterDevice(const std::string_view path);

  bool RegisterSymbolicLink(const std::string_view path, const std::string_view target);
  bool UnregisterSymbolicLink(const std::string_view path);
  bool FindSymbolicLink(const std::string_view path, std::string& target);

  Entry* ResolvePath(const std::string_view path);

  Entry* CreatePath(const std::string_view path, uint32_t attributes);
  bool DeletePath(const std::string_view path);

  X_STATUS OpenFile(Entry* root_entry, const std::string_view path,
                    FileDisposition creation_disposition, uint32_t desired_access,
                    bool is_directory, bool is_non_directory, File** out_file,
                    FileAction* out_action);

  // Dunwall (2026-11-09): opening "<name>.<from>" for reading when it does not exist opens "<name>.<to>" from
  // the same folder instead (extensions without the dot, compared without case). Set once, before the game opens
  // such files (the game's localized voice banks exist as .RUS only, see app/dunwall_src/dh_language.cpp).
  void SetExtensionFallback(const std::string& from, const std::string& to);

  // Dunwall (2026-11-09): opening a file named 'from' (the name alone, any folder, compared without case) for
  // reading opens 'to' from the same folder instead, when that exists. Set before the game opens such files.
  void AddFileAlias(const std::string& from, const std::string& to);

 private:
  std::string FallbackName(const std::string_view file_name) const;
  std::string AliasName(const std::string_view file_name) const;
  std::vector<std::pair<std::string, std::string>> aliases_;  // lower-case 'from', 'to'
  std::atomic<bool> aliases_set_{false};

  rex::thread::global_critical_region global_critical_region_;
  std::vector<std::unique_ptr<Device>> devices_;
  std::string fallback_from_, fallback_to_;
  std::atomic<bool> fallback_set_{false};
  std::unordered_map<std::string, std::string> symlinks_;

  bool ResolveSymbolicLink(const std::string_view path, std::string& result);
};

}  // namespace rex::filesystem
