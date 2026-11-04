/**
 * me2-nx-deko (2026-11-01): a read-only folder laid over another device (see overlay_device.h).
 */

#include <rex/filesystem/devices/overlay_device.h>

#include <rex/filesystem/entry.h>
#include <rex/logging.h>
#include <rex/string/utf8.h>

namespace rex::filesystem {

namespace {

// An overlay file in the base device's tree: the directory listings and lookups of the base then show the overlay's
// file (its name, size, times), and opening it opens the overlay's.
class OverlayLinkEntry : public Entry {
 public:
  OverlayLinkEntry(Device* device, Entry* parent, const std::string_view path, Entry* target)
      : Entry(device, parent, path), target_(target) {
    attributes_ = target->attributes();
    size_ = target->size();
    allocation_size_ = target->allocation_size();
    create_timestamp_ = target->create_timestamp();
    access_timestamp_ = target->access_timestamp();
    write_timestamp_ = target->write_timestamp();
  }
  X_STATUS Open(uint32_t desired_access, File** out_file) override { return target_->Open(desired_access, out_file); }

 private:
  Entry* target_;
};

std::string ChildPath(const std::string& dir, const std::string& name) {
  return dir.empty() ? name : dir + "\\" + name;
}

}  // namespace

OverlayDevice::OverlayDevice(std::unique_ptr<Device> base, std::unique_ptr<Device> overlay)
    : Device(base->mount_path()), base_(std::move(base)), overlay_(std::move(overlay)) {}

OverlayDevice::~OverlayDevice() = default;

// Both devices are initialized by the caller. Each overlay file goes into the base's directory of the same path: it
// replaces a base file of that name (kept alive, unused: other code may still hold it) or is added. A directory only
// the overlay has stays the overlay's (ResolvePath below still finds what is in it).
bool OverlayDevice::Initialize() {
  Entry* root = overlay_->ResolvePath("");
  if (!root) return true;
  uint32_t replaced = 0, added = 0;
  MergeDir(root, "", replaced, added);
  REXFS_INFO("[overlay] {} file(s) replace the game's, {} added", replaced, added);
  return true;
}

void OverlayDevice::MergeDir(Entry* dir, const std::string& rel, uint32_t& replaced, uint32_t& added) {
  for (const auto& child : dir->children()) {
    const std::string path = ChildPath(rel, child->name());
    if (child->attributes() & kFileAttributeDirectory) {
      MergeDir(child.get(), path, replaced, added);
      continue;
    }
    Entry* base_dir = base_->ResolvePath(rel);
    if (!base_dir || !(base_dir->attributes() & kFileAttributeDirectory)) continue;
    auto link = std::make_unique<OverlayLinkEntry>(base_dir->device(), base_dir, path, child.get());
    auto& list = base_dir->children_;
    bool done = false;
    for (auto& slot : list) {
      if (rex::string::utf8_equal_case(slot->name(), child->name())) {
        replaced_.push_back(std::move(slot));
        slot = std::move(link);
        ++replaced;
        done = true;
        break;
      }
    }
    if (!done) {
      list.push_back(std::move(link));
      ++added;
    }
  }
}

void OverlayDevice::Dump(string::StringBuffer* string_buffer) {
  overlay_->Dump(string_buffer);
  base_->Dump(string_buffer);
}

Entry* OverlayDevice::ResolvePath(const std::string_view path) {
  // Files: the overlay's (the base's directories now list them too). Directories stay the base's, with all of the
  // base's children, so lookups and listings inside them are the base's plus the overlay's files.
  if (Entry* e = overlay_->ResolvePath(path); e && !(e->attributes() & kFileAttributeDirectory)) return e;
  return base_->ResolvePath(path);
}

}  // namespace rex::filesystem
