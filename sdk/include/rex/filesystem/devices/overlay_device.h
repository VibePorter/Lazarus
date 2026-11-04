/**
 * me2-nx-deko (2026-11-01): a read-only folder laid over another device.
 *
 * A path the overlay folder has resolves there; every other path resolves on the base device. ME2's Russian edition
 * differs from the English one in five files (default.xex, Unreap.xex, BioGame/CookedXenon/BIOGame_RUS.tlk,
 * Engine.xxx, SFXGame.xxx; the other 3,064 are byte-identical), so a ~28 MB folder over the English asset pack is the
 * whole Russian game (game_overlay_root, system/runtime.cpp). Initialize puts the overlay's files into the base's
 * directories too (2026-11-01: the first version only answered lookups by name, so a listing of CookedXenon had no
 * BIOGame_RUS.tlk and the game showed its "$123456" string ids instead of the Russian text). The overlay's own device gets the base's mount path, so
 * its entries' paths are the ones the game would see without it.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rex/filesystem/device.h>

namespace rex::filesystem {

class OverlayDevice : public Device {
 public:
  OverlayDevice(std::unique_ptr<Device> base, std::unique_ptr<Device> overlay);
  ~OverlayDevice() override;

  bool Initialize() override;
  bool is_read_only() const override { return base_->is_read_only(); }
  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  const std::string& name() const override { return base_->name(); }
  uint32_t attributes() const override { return base_->attributes(); }
  uint32_t component_name_max_length() const override { return base_->component_name_max_length(); }
  uint32_t total_allocation_units() const override { return base_->total_allocation_units(); }
  uint32_t available_allocation_units() const override { return base_->available_allocation_units(); }
  uint32_t sectors_per_allocation_unit() const override { return base_->sectors_per_allocation_unit(); }
  uint32_t bytes_per_sector() const override { return base_->bytes_per_sector(); }

 private:
  // The overlay files of 'dir' (path 'rel') into the base's directory of that path.
  void MergeDir(Entry* dir, const std::string& rel, uint32_t& replaced, uint32_t& added);

  std::unique_ptr<Device> base_;
  std::unique_ptr<Device> overlay_;
  std::vector<std::unique_ptr<Entry>> replaced_;  // base entries an overlay file took the place of (kept alive)
};

}  // namespace rex::filesystem
