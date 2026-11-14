// banjo-nx (2026-10-17): Renut's renderer on plume's deko3d back end (banjo_renut_deko3d.h, docs/deko3d-renderer.md).
#include "renut_engine/gpu/banjo_renut_deko3d.h"

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <vector>

#include <unistd.h>  // ftruncate (LoadCache)

#include <fmt/format.h>
#include <rex/logging.h>
#include <zstd.h>

#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/shaders/shader_cache.h"  // peggle2-nx: MissingGameShaders

#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>
#endif

// The GLSL package (tools/deko3d_shader_package.py: deko3d_glsl_switch.cpp embeds it); absent from builds made without
// one, hence weak.
extern const uint8_t g_deko3dGlslPackage[] __attribute__((weak));
extern const size_t g_deko3dGlslPackageSize __attribute__((weak));

// ME2 (2026-11-14, from the Dunwall port 2026-11-12, the GPU timeout 0xD5C at a level load): every piece of GPU work numbered, the GPU writing the
// number when done; a failed channel kickoff logs the work the GPU stopped in ("breadcrumbs:" in deko3d.log). Costs a
// GPU report per piece of work: diagnostic runs only.
REXCVAR_DEFINE_BOOL(me2_gpu_breadcrumbs, false, "ME2",
                    "Diagnostic: GPU breadcrumbs (deko3d), dumped to deko3d.log when the GPU channel dies")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Dunwall (2026-11-13): exact breadcrumbs (plume_deko3d.h Deko3DSetBreadcrumbsExact): a full GPU barrier before each
// number, so the dump names the work that hung, not just the oldest unfinished one. Slow: diagnostic runs only.
REXCVAR_DEFINE_BOOL(me2_gpu_breadcrumbs_exact, false, "ME2",
                    "Diagnostic: with me2_gpu_breadcrumbs, the GPU finishes each piece of work before the next starts "
                    "(the dump then names the piece that hung). Slow")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Dunwall (2026-11-13): the level-load GPU timeout happened with the block arena and not without it; the arena's best
// fit gives a just-destroyed block's range straight to the next block of the same size (the upload ring's 32 MB chunks).
REXCVAR_DEFINE_INT32(me2_deko3d_arena_quarantine_ms, 0, "ME2",
                     "deko3d: a destroyed memory block's arena range is given to a new block only this many ms after "
                     "the destroy (0 = at once, as before)")
    .range(0, 60000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::renut_gpu::deko3d {

namespace {

#pragma pack(push, 1)
struct PackageHeader {
  char magic[4];
  uint32_t version, count, dictSize, tableOffset, dataOffset, reserved[2];
};
struct PackageEntry {
  uint64_t hash;
  uint32_t offset, compressedSize, size, stage;
  uint32_t boolMask[8];
};
#pragma pack(pop)
static_assert(sizeof(PackageEntry) == 56, "tools/deko3d_shader_package.py's entry");

// One GLSL package: the one built into the NRO, or (peggle2-nx, 2026-10-20; sa2-nx 1.0.3) deko3d_glsl.bin beside it,
// which the web installer makes from the player's own game files (a build for distribution carries no translated
// game shader: SA2_RENUT_RELEASE_SHADERS).
struct Package {
  bool ok = false;
  std::vector<uint8_t> file;  // the file's bytes (the built-in package is used in place)
  const uint8_t *base = nullptr;
  size_t size = 0;
  const PackageHeader *header = nullptr;
  const PackageEntry *entries = nullptr;
  const uint8_t *data = nullptr;
  ZSTD_DDict *dict = nullptr;  // none when the package has no dictionary (the installer's: raw zstd frames)
  std::mutex mutex;            // the decompression context
  ZSTD_DCtx *dctx = nullptr;
};

// Index 0: beside the NRO (looked up first), 1: built in.
struct Packages {
  std::once_flag once;
  Package source[2];
};
Packages g_packages;

bool Attach(Package &p, const uint8_t *base, size_t size, const char *what) {
  if (size < sizeof(PackageHeader))
    return false;
  const auto *h = reinterpret_cast<const PackageHeader *>(base);
  const uint64_t table_end = uint64_t(h->tableOffset) + uint64_t(h->count) * sizeof(PackageEntry);
  if (std::memcmp(h->magic, "BDKG", 4) != 0 || h->version != 1 || sizeof(PackageHeader) + uint64_t(h->dictSize) > size ||
      table_end > size || h->dataOffset > size) {
    REXLOG_ERROR("[deko3d] {}: not a GLSL package this build reads", what);
    return false;
  }
  const auto *entries = reinterpret_cast<const PackageEntry *>(base + h->tableOffset);
  for (uint32_t i = 0; i < h->count; ++i) {
    if (uint64_t(h->dataOffset) + entries[i].offset + entries[i].compressedSize > size ||
        (i && entries[i - 1].hash >= entries[i].hash)) {
      REXLOG_ERROR("[deko3d] {}: entry {} out of bounds or out of order", what, i);
      return false;
    }
  }
  p.base = base;
  p.size = size;
  p.header = h;
  p.entries = entries;
  p.data = base + h->dataOffset;
  p.dict = h->dictSize ? ZSTD_createDDict(base + sizeof(PackageHeader), h->dictSize) : nullptr;
  p.dctx = ZSTD_createDCtx();
  p.ok = p.dctx && (p.dict || !h->dictSize);
  return p.ok;
}

Packages &Open() {
  std::call_once(g_packages.once, [] {
    Packages &all = g_packages;
    // peggle2-nx: deko3d_glsl.bin beside the NRO first.
    const std::string path = NextToExecutable("deko3d_glsl.bin").string();
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
      Package &p = all.source[0];
      std::fseek(f, 0, SEEK_END);
      const long n = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      if (n > 0) {
        p.file.resize(size_t(n));
        if (std::fread(p.file.data(), 1, p.file.size(), f) != p.file.size())
          p.file.clear();
      }
      std::fclose(f);
      if (Attach(p, p.file.data(), p.file.size(), path.c_str()))
        REXLOG_INFO("[deko3d] GLSL package {}: {} shaders, {} KB", path, p.header->count, p.size >> 10);
    }
    if (&g_deko3dGlslPackageSize != nullptr && g_deko3dGlslPackageSize >= sizeof(PackageHeader) &&
        Attach(all.source[1], g_deko3dGlslPackage, g_deko3dGlslPackageSize, "the NRO's GLSL package"))
      REXLOG_INFO("[deko3d] GLSL package: {} shaders, {} KB in the NRO", all.source[1].header->count,
                  g_deko3dGlslPackageSize >> 10);
    if (!all.source[0].ok && !all.source[1].ok)
      REXLOG_ERROR("[deko3d] no GLSL package (deko3d_glsl.bin beside the NRO, or built in): no game shaders");
  });
  return g_packages;
}

struct Found {
  Package *package = nullptr;
  const PackageEntry *entry = nullptr;
};

Found Find(Packages &all, uint64_t hash) {
  for (Package &p : all.source) {
    if (!p.ok)
      continue;
    const PackageEntry *begin = p.entries, *end = p.entries + p.header->count;
    const PackageEntry *it =
        std::lower_bound(begin, end, hash, [](const PackageEntry &e, uint64_t h) { return e.hash < h; });
    if (it != end && it->hash == hash)
      return {&p, it};
  }
  return {};
}

// ---- the DKSH cache next to the NRO: "BDKC", version, the compiler's tag, then records (u64 key, u32 size, DKSH)
// uam 1.1.0 with SwitchWakerHD's 8 patches and banjo-nx's passes (bnx3, 2026-10-18: with the value numbering).
// banjo-nx (2026-10-20): bnx4 with uam banjo flag 1024 (the 128-bit bone rows) in the default flags: a cache
// compiled before is another compiler's.
constexpr char kCacheTag[16] = "uam110-sw8-bnx4";

constexpr uint32_t kDkshMagic = 0x48534B44u;  // "DKSH", the first word of every record's data

struct DkshCache {
  std::mutex mutex;
  FILE *file = nullptr;
  std::unordered_map<uint64_t, std::pair<long, uint32_t>> index;  // key -> offset of the DKSH, size
  long end = 0;  // the end of the last whole record: where the next one is written
  bool read_only = false;  // a write failed: no more records
  uint64_t added = 0, hits = 0, misses = 0;
  std::string path;
};
DkshCache g_cache;
// Dunwall (2026-11-12, the stalls after Press Start): what the cache's reads cost (DkshReadReport).
std::atomic<uint64_t> g_dksh_reads{0}, g_dksh_read_ns{0}, g_dksh_lock_ns{0}, g_dksh_max_ns{0}, g_dksh_block_hits{0};
// me2-nx-deko (2026-11-12): a second handle of the cache file could not be opened (the console's file system, the file
// open for writing): reads go through DkshCache::file under its mutex.
std::atomic<bool> g_dksh_shared_handle{false};

// Dunwall (2026-11-12): the startup preparation (PrewarmGameShaders) loaded the default variant of all ~45,000 game
// shaders in hash order, each with its own fseek + fread of the cache under the cache's lock: 234 s of random SD reads
// (2-8 ms each, beside the game's streaming) from the start through Press Start and the first level, and the render
// thread and the pipeline workers waited for the lock (5 s in the 10 s after Press Start). It now goes through the
// shaders in the order of their records in the file and reads the file ahead in blocks of kPrefetchBytes on a handle of
// its own; the lookup answers from that thread's block when it holds the record.
constexpr size_t kPrefetchBytes = 4u << 20;
struct PrefetchBlock {
  long begin = 0;
  std::vector<uint8_t> data;
};
thread_local PrefetchBlock t_prefetch;

uint64_t Fnv1a64(const void *data, size_t size) {  // plume_deko3d.cpp's fnv1a64: a variant's key
  const uint8_t *p = static_cast<const uint8_t *>(data);
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < size; i++) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

// me2-nx-deko (2026-10-28): the DKSH cache's record index beside it (<cache>.idx): "BDKI", version 1, the cache's
// length it describes, the record count, then per record (u64 key, u32 offset of the DKSH, u32 size). Used only when
// the length matches (a cache appended to or cut since is walked again, and the index written anew). The build
// (tools/nx/deko_build.sh, step shaders) writes one for the shipped cache too.
struct CacheIndexHeader {
  char magic[4];
  uint32_t version;
  uint64_t length;
  uint32_t count;
  uint32_t reserved;
};
struct CacheIndexEntry {
  uint64_t key;
  uint32_t offset, size;
};
static_assert(sizeof(CacheIndexHeader) == 24 && sizeof(CacheIndexEntry) == 16);

bool LoadCacheIndex(DkshCache &c, long length) {
  FILE *f = std::fopen((c.path + ".idx").c_str(), "rb");
  if (!f)
    return false;
  CacheIndexHeader h{};
  std::vector<CacheIndexEntry> entries;
  bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, "BDKI", 4) == 0 && h.version == 1 &&
            h.length == uint64_t(length) && h.count < (1u << 22);
  if (ok) {
    entries.resize(h.count);
    ok = entries.empty() || std::fread(entries.data(), sizeof(CacheIndexEntry), entries.size(), f) == entries.size();
  }
  std::fclose(f);
  if (!ok)
    return false;
  for (const CacheIndexEntry &e : entries)
    if (e.size < 64 || uint64_t(e.offset) + e.size > uint64_t(length))
      return false;
  // The first and the last record's data must be a DKSH (a cache replaced by another of the same length).
  for (const CacheIndexEntry *e : {entries.empty() ? nullptr : &entries.front(), entries.empty() ? nullptr : &entries.back()}) {
    uint32_t magic = 0;
    if (e && (std::fseek(c.file, long(e->offset), SEEK_SET) != 0 || std::fread(&magic, sizeof(magic), 1, c.file) != 1 ||
              magic != kDkshMagic))
      return false;
  }
  c.index.reserve(entries.size());
  for (const CacheIndexEntry &e : entries)
    c.index[e.key] = {long(e.offset), e.size};
  REXLOG_INFO("[deko3d] DKSH cache index {}.idx: {} records", c.path, entries.size());
  return true;
}

void SaveCacheIndex(const DkshCache &c, long length) {
  std::vector<CacheIndexEntry> entries;
  entries.reserve(c.index.size());
  for (const auto &[key, at] : c.index)
    entries.push_back({key, uint32_t(at.first), at.second});
  std::sort(entries.begin(), entries.end(), [](const CacheIndexEntry &a, const CacheIndexEntry &b) { return a.offset < b.offset; });
  CacheIndexHeader h{{'B', 'D', 'K', 'I'}, 1, uint64_t(length), uint32_t(entries.size()), 0};
  const std::string path = c.path + ".idx";
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f)
    return;
  const bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1 &&
                  (entries.empty() || std::fwrite(entries.data(), sizeof(CacheIndexEntry), entries.size(), f) == entries.size());
  std::fflush(f);
  fsync(fileno(f));
  std::fclose(f);
  if (!ok)
    std::remove(path.c_str());
  else
    REXLOG_INFO("[deko3d] DKSH cache index {} written: {} records", path, entries.size());
}

void LoadCache() {
  DkshCache &c = g_cache;
  // banjo-nx (2026-10-18): the code uam makes with other flags (banjo_deko3d_uam_prmt, banjo_deko3d_uam_gvn off) has a
  // cache of its own.
  const unsigned flags = Deko3DUamFlags();
  c.path = NextToExecutable(flags == 1471u ? std::string("deko3d_shaders.bin")
                                          : "deko3d_shaders_f" + std::to_string(flags) + ".bin").string();
  c.file = std::fopen(c.path.c_str(), "r+b");
  bool fresh = c.file == nullptr;
  if (c.file) {
    char head[24] = {};
    if (std::fread(head, 1, sizeof(head), c.file) != sizeof(head) || std::memcmp(head, "BDKC", 4) != 0 ||
        std::memcmp(head + 8, kCacheTag, sizeof(kCacheTag)) != 0) {
      REXLOG_INFO("[deko3d] DKSH cache {}: another compiler's or unreadable; starting it over", c.path);
      std::fclose(c.file);
      c.file = nullptr;
      fresh = true;
    }
    else {
      std::fseek(c.file, 0, SEEK_END);
      const long length = std::ftell(c.file);
      // me2-nx-deko (2026-10-28): the record index from the sidecar file (LoadCacheIndex) when it describes this file;
      // the walk below took 11 s on the console for 33,370 records (an fseek + three small freads each: every fseek
      // drops stdio's buffer, so each record was a random SD read through the FS service).
      long at = LoadCacheIndex(c, length) ? length : long(sizeof(head));
      // Records up to the first that is not whole: its size runs past the end, or its data is not a DKSH (an append
      // the process did not finish). The file is cut there and new records follow the good ones. Before, appends
      // went after such a record, where the next start's walk never found them: Ryujinx runs 20-22 compiled the same
      // ~30 shaders on every start.
      // me2-nx-deko: walked in 4 MB blocks read in order (record headers parsed from memory).
      const bool walked = at != length;
      std::vector<uint8_t> block;
      long block_at = 0;
      auto bytes_at = [&](long pos, size_t n) -> const uint8_t * {
        if (pos < block_at || pos + long(n) > block_at + long(block.size())) {
          block_at = pos;
          block.resize(size_t(std::min<long>(4l << 20, length - pos)));
          if (block.size() < n || std::fseek(c.file, pos, SEEK_SET) != 0 ||
              std::fread(block.data(), 1, block.size(), c.file) != block.size()) {
            block.clear();
            return nullptr;
          }
        }
        return block.data() + (pos - block_at);
      };
      for (;;) {
        uint64_t key;
        uint32_t size, magic;
        const uint8_t *h = at + 16 <= length ? bytes_at(at, 16) : nullptr;
        if (!h)
          break;
        std::memcpy(&key, h, sizeof(key));
        std::memcpy(&size, h + 8, sizeof(size));
        std::memcpy(&magic, h + 12, sizeof(magic));
        if (size < 64 || size > (16u << 20) || at + 12 + long(size) > length || magic != kDkshMagic)
          break;
        c.index[key] = {at + 12, size};
        at += 12 + long(size);
      }
      c.end = at;
      if (walked && at == length)
        SaveCacheIndex(c, length);
      if (at != length) {
        REXLOG_WARN("[deko3d] DKSH cache {}: {} bytes after the last whole record ({} of them) dropped", c.path,
                    length - at, c.index.size());
        std::fflush(c.file);
        if (ftruncate(fileno(c.file), at) != 0)
          REXLOG_WARN("[deko3d] DKSH cache {}: could not be cut; new records overwrite the rest", c.path);
      }
    }
  }
  if (fresh) {
    c.file = std::fopen(c.path.c_str(), "w+b");
    if (c.file) {
      char head[24] = {'B', 'D', 'K', 'C', 1, 0, 0, 0};
      std::memcpy(head + 8, kCacheTag, sizeof(kCacheTag));
      std::fwrite(head, 1, sizeof(head), c.file);
      std::fflush(c.file);
      c.end = sizeof(head);
    }
  }
  REXLOG_INFO("[deko3d] DKSH cache {}: {} variants{}", c.path, c.index.size(), c.file ? "" : " (cannot write it)");
}

}  // namespace

bool PackageAvailable() {
  Packages &all = Open();
  return all.source[0].ok || all.source[1].ok;
}

bool PackageScales() {
  // sa2-nx (2026-10-20, 1.0.3.1): a package made with tools/deko3d_glsl_opt.py's pass scale1 (1.0.3's, from an older
  // installer page) has every texture's scale code folded to 0: drawn above 1x, its shaders would take host texel
  // sizes for guest ones. The passes leave the code's read, "(... >> 26u) & 7u", in every shader that has one (SA2:
  // 5 pixel shaders without scale1, none with it), so a package scales when each package in use has it somewhere.
  static const bool scales = [] {
    Packages &all = Open();
    static constexpr char kScaleCode[] = ">> 26u) & 7u";
    bool any = false;
    for (Package &p : all.source) {
      if (!p.ok)
        continue;
      any = true;
      bool found = false;
      std::vector<char> text;
      std::lock_guard<std::mutex> lock(p.mutex);
      for (uint32_t i = 0; i < p.header->count && !found; ++i) {
        const PackageEntry &e = p.entries[i];
        text.resize(e.size);
        const size_t n = p.dict ? ZSTD_decompress_usingDDict(p.dctx, text.data(), e.size, p.data + e.offset,
                                                             e.compressedSize, p.dict)
                                : ZSTD_decompressDCtx(p.dctx, text.data(), e.size, p.data + e.offset, e.compressedSize);
        if (ZSTD_isError(n) || n != e.size)
          continue;
        found = std::search(text.begin(), text.end(), kScaleCode, kScaleCode + sizeof(kScaleCode) - 1) != text.end();
      }
      if (!found) {
        REXLOG_WARN("[deko3d] the {} GLSL package was made for 1280x720 only (pass scale1): the game draws at "
                    "1280x720; make deko3d_glsl.bin again with the installer page for 1920x1080 when docked",
                    &p == &all.source[0] ? "deko3d_glsl.bin" : "built-in");
        return false;
      }
    }
    return any;
  }();
  return scales;
}

#if defined(PLUME_DEKO3D)

std::unique_ptr<plume::RenderShader> CreateGameShader(plume::RenderDevice *device, uint64_t hash) {
  Packages &all = Open();
  const Found found = Find(all, hash);
  if (!found.entry || !device) {
    static int logged = 0;
    if ((all.source[0].ok || all.source[1].ok) && logged++ < 16)
      REXLOG_ERROR("[deko3d] shader {:016X} is not in the GLSL package (deko3d_glsl.bin beside the NRO: made by the "
                   "installer from your game files?)", hash);
    return nullptr;
  }
  Package &p = *found.package;
  const PackageEntry *e = found.entry;
  std::vector<uint8_t> blob(sizeof(plume::Deko3DShaderBlob) + e->size);
  plume::Deko3DShaderBlob header;
  header.stage = e->stage;
  header.glslSize = e->size;
  header.dkshSize = 0;
  std::memcpy(blob.data(), &header, sizeof(header));
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    const size_t n = p.dict ? ZSTD_decompress_usingDDict(p.dctx, blob.data() + sizeof(header), e->size,
                                                         p.data + e->offset, e->compressedSize, p.dict)
                            : ZSTD_decompressDCtx(p.dctx, blob.data() + sizeof(header), e->size, p.data + e->offset,
                                                  e->compressedSize);
    if (ZSTD_isError(n) || n != e->size) {
      REXLOG_ERROR("[deko3d] shader {:016X}: the GLSL did not unpack ({})", hash,
                   ZSTD_isError(n) ? ZSTD_getErrorName(n) : "short");
      return nullptr;
    }
  }
  auto shader = device->createShader(blob.data(), blob.size(), "main", plume::RenderShaderFormat::DEKO3D);
  if (shader)
    shader->setName(fmt::format("{}_{:016X}", e->stage == 4 ? "ps" : "vs", hash));
  return shader;
}

void InstallShaderCache() {
  // The back end's messages into this log (plume writes them to stderr otherwise), and into deko3d.log next to the NRO,
  // flushed at each line: the log's buffer is lost when the process dies.
  static FILE *diag = std::fopen(NextToExecutable("deko3d.log").string().c_str(), "w");
  static std::mutex diag_mutex;
  plume::SetDeko3DLogCallback([](const char *message) {
    REXLOG_INFO("[deko3d] {}", message);
    std::lock_guard<std::mutex> lock(diag_mutex);
    if (diag) {
      std::fprintf(diag, "%s\n", message);
      std::fflush(diag);
    }
  });
  // banjo-nx (2026-10-18, GPU bandwidth): depth compression and Zcull, which the device and its textures take.
  plume::Deko3DOptions options;
  options.depthCompression = Deko3DDepthCompressionOn();
  options.zcull = Deko3DZcullOn();
  options.vsOutputs = Deko3DVsOutputsOn();
  options.uamFlags = Deko3DUamFlags();
  options.mergeBarriers = Deko3DBarrierMergeOn();  // banjo-nx (2026-10-19, GPU)
  options.leanBarriers = Deko3DBarrierLeanOn();
  // The uam thread on the background compile core, below the game's threads (2026-10-18).
  options.compileCore = Deko3DCompileCore();
  options.compilePriority = Deko3DCompilePriority();
  plume::Deko3DSetOptions(options);
  // ME2 (2026-11-14, from the Dunwall port): GPU breadcrumbs (plume_deko3d.h Deko3DSetBreadcrumbs), dumped at a
  // failed kickoff, and the block arena's quarantine of destroyed ranges.
  plume::Deko3DSetBreadcrumbs(REXCVAR_GET(me2_gpu_breadcrumbs));
  plume::Deko3DSetBreadcrumbsExact(REXCVAR_GET(me2_gpu_breadcrumbs_exact));
  plume::g_deko3dArenaQuarantineMs.store(uint32_t(REXCVAR_GET(me2_deko3d_arena_quarantine_ms)), std::memory_order_relaxed);
  LoadCache();
  plume::SetDeko3DShaderCache(
      [](uint64_t key, std::vector<uint8_t> &dksh) {
        DkshCache &c = g_cache;
        const auto t_lock = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(c.mutex);
        const auto t_read = std::chrono::steady_clock::now();
        auto it = c.index.find(key);
        if (it == c.index.end() || !c.file) {
          c.misses++;
          return false;
        }
        // Dunwall (2026-11-12): from this thread's read-ahead block (the startup preparation's), when it has it.
        {
          const PrefetchBlock &b = t_prefetch;
          const long at = it->second.first;
          const uint32_t size = it->second.second;
          if (!b.data.empty() && at >= b.begin && at + long(size) <= b.begin + long(b.data.size())) {
            const uint8_t *src = b.data.data() + (at - b.begin);
            uint32_t magic = 0;
            std::memcpy(&magic, src, sizeof(magic));
            if (magic == kDkshMagic) {
              c.hits++;
              lock.unlock();
              g_dksh_block_hits.fetch_add(1, std::memory_order_relaxed);
              dksh.assign(src, src + size);
              return true;
            }
          }
        }
        // Dunwall (2026-11-12): read through this thread's own handle of the file, without the lock (held for the
        // whole SD read before, so the startup preparation, the pipeline workers and the render thread queued behind
        // each other's reads: seconds of waiting in the 10 s after Press Start). A record is in the index only once
        // it is whole in the file (the store writes, flushes and syncs before it adds it).
        const long record_at = it->second.first;
        const uint32_t record_size = it->second.second;
        // me2-nx-deko (2026-11-12, black screen after a save load): on the console the file cannot be opened a second
        // time while c.file has it open for writing ("r+b"), so every thread's fopen failed, every lookup was a miss
        // and every cached shader was compiled again by uam (~0.5 s a pipeline on the render thread, the startup
        // preparation never finished). Without an own handle the read goes through c.file under the lock (as before).
        thread_local FILE *t_file = nullptr;
        if (!t_file && !g_dksh_shared_handle.load(std::memory_order_relaxed)) {
          t_file = std::fopen(c.path.c_str(), "rb");
          if (!t_file && !g_dksh_shared_handle.exchange(true))
            REXLOG_WARN("[deko3d] DKSH cache {}: a second handle could not be opened (errno {}); reads go through the "
                        "cache's own handle under its lock",
                        c.path, errno);
        }
        if (t_file)
          lock.unlock();
        dksh.resize(record_size);
        uint32_t magic = 0;
        FILE *const from = t_file ? t_file : c.file;
        const bool ok = from && std::fseek(from, record_at, SEEK_SET) == 0 &&
                        std::fread(dksh.data(), 1, dksh.size(), from) == dksh.size() &&
                        (std::memcpy(&magic, dksh.data(), sizeof(magic)), magic == kDkshMagic);
        // Dunwall (2026-11-12): counted (DkshReadReport).
        const auto t_end = std::chrono::steady_clock::now();
        const uint64_t read_ns =
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_read).count());
        g_dksh_reads.fetch_add(1, std::memory_order_relaxed);
        g_dksh_read_ns.fetch_add(read_ns, std::memory_order_relaxed);
        g_dksh_lock_ns.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t_read - t_lock).count()),
            std::memory_order_relaxed);
        if (read_ns > g_dksh_max_ns.load(std::memory_order_relaxed))
          g_dksh_max_ns.store(read_ns, std::memory_order_relaxed);
        if (!lock.owns_lock())
          lock.lock();
        if (!ok) {
          static std::atomic<uint32_t> failed_logged{0};
          if (failed_logged.fetch_add(1, std::memory_order_relaxed) < 8)
            REXLOG_WARN("[deko3d] DKSH cache {}: the record of {:016X} ({} bytes at {}) could not be read; compiled "
                        "again",
                        c.path, key, record_size, record_at);
          c.misses++;
          if (auto again = c.index.find(key); again != c.index.end() && again->second.first == record_at)
            c.index.erase(again);
          return false;
        }
        c.hits++;
        return true;
      },
      [](uint64_t key, const std::vector<uint8_t> &dksh) {
        DkshCache &c = g_cache;
        std::lock_guard<std::mutex> lock(c.mutex);
        if (!c.file || c.read_only || c.index.count(key) || dksh.size() < 64)
          return;
        // One write of the whole record at the end of the last whole one, then flushed: a record cut short (the game
        // closed while writing) is the last one, and the next start drops it (LoadCache).
        const uint32_t size = uint32_t(dksh.size());
        std::vector<uint8_t> record(12 + dksh.size());
        std::memcpy(record.data(), &key, sizeof(key));
        std::memcpy(record.data() + 8, &size, sizeof(size));
        std::memcpy(record.data() + 12, dksh.data(), dksh.size());
        if (std::fseek(c.file, c.end, SEEK_SET) == 0 &&
            std::fwrite(record.data(), 1, record.size(), c.file) == record.size() && std::fflush(c.file) == 0) {
          // peggle2-nx (2026-10-20): fflush only hands the record to the FS service; fsync (libnx: fsFileFlush) has it
          // written out. Without it the last records stayed in Ryujinx's file buffer and were lost when the process was
          // closed (every run compiled the same variant again: 8 of 9 records in the file). Once per new record.
          fsync(fileno(c.file));
          c.index[key] = {c.end + 12, size};
          c.end += long(record.size());
          c.added++;
        }
        else {
          // The SD card full or failing: the records so far keep being read; no more are written (a part of this
          // one may be in the file, after c.end, and the next start drops it).
          REXLOG_WARN("[deko3d] DKSH cache {}: a record could not be written; no more are", c.path);
          c.read_only = true;
        }
      });
}

namespace {
std::atomic<bool> g_prewarm_cancel{false};
}  // namespace

void CancelPrewarm() { g_prewarm_cancel.store(true, std::memory_order_release); }

std::string DkshReadReport() {
  const auto take = [](std::atomic<uint64_t> &a) { return a.exchange(0, std::memory_order_relaxed); };
  const uint64_t reads = take(g_dksh_reads), read_ns = take(g_dksh_read_ns), lock_ns = take(g_dksh_lock_ns),
                 max_ns = take(g_dksh_max_ns);
  const uint64_t block_hits = take(g_dksh_block_hits);
  return fmt::format("DKSH cache: {} reads from the card ({:.1f} ms, slowest {:.1f} ms), {:.1f} ms waiting for its "
                     "lock, {} from the startup preparation's read-ahead",
                     reads, double(read_ns) / 1e6, double(max_ns) / 1e6, double(lock_ns) / 1e6, block_hits);
}

bool PrewarmGameShaders(plume::RenderDevice *device, const std::function<void(const PrewarmProgress &)> &progress) {
  if (!device || !renut::gpu::IsDeko3D())
    return false;
  auto *dev = static_cast<plume::Deko3DDevice *>(device);
  Packages &all = Open();
  std::vector<uint64_t> hashes;
  for (Package &p : all.source) {
    if (!p.ok)
      continue;
    for (uint32_t i = 0; i < p.header->count; ++i)
      hashes.push_back(p.entries[i].hash);
  }
  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  PrewarmProgress state;
  state.total = uint32_t(hashes.size());
  progress(state);
  // Dunwall (2026-11-12): the shaders in the order of their default variant's record in the cache (see
  // kPrefetchBytes); those without one last.
  const auto t_order = std::chrono::steady_clock::now();
  // The keys first, without the cache's lock (a reader holds it for a whole SD read, and the pipeline workers read
  // a lot at the start: one lookup under the lock per shader made this pass take minutes on the console), then every
  // key's record under the lock at once.
  std::vector<std::pair<uint64_t, uint64_t>> keys;  // (variant key or 0, shader hash)
  keys.reserve(hashes.size());
  {
    std::vector<uint8_t> glsl;
    for (const uint64_t hash : hashes) {
      uint64_t key = 0;
      const Found found = Find(all, hash);
      if (found.entry) {
        Package &p = *found.package;
        const PackageEntry *e = found.entry;
        glsl.resize(e->size);
        size_t n;
        {
          std::lock_guard<std::mutex> lock(p.mutex);
          n = p.dict ? ZSTD_decompress_usingDDict(p.dctx, glsl.data(), e->size, p.data + e->offset, e->compressedSize,
                                                  p.dict)
                     : ZSTD_decompressDCtx(p.dctx, glsl.data(), e->size, p.data + e->offset, e->compressedSize);
        }
        if (!ZSTD_isError(n) && n == e->size)
          key = Fnv1a64(glsl.data(), glsl.size());
      }
      keys.emplace_back(key, hash);
    }
  }
  std::vector<std::pair<long, uint64_t>> order;
  order.reserve(keys.size());
  size_t with_record = 0;
  {
    std::lock_guard<std::mutex> lock(g_cache.mutex);
    for (const auto &[key, hash] : keys) {
      long at = LONG_MAX;
      if (key)
        if (auto it = g_cache.index.find(key); it != g_cache.index.end())
          at = it->second.first;
      with_record += at != LONG_MAX;
      order.emplace_back(at, hash);
    }
  }
  std::sort(order.begin(), order.end());
  const double order_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_order).count();
  REXLOG_INFO("[deko3d] startup preparation: {} shaders ordered by their record in the cache in {:.0f} ms ({} have one)",
              order.size(), order_ms, with_record);
  FILE *ahead = g_dksh_shared_handle.load(std::memory_order_relaxed) ? nullptr : std::fopen(g_cache.path.c_str(), "rb");
  // me2-nx-deko (2026-11-12): no second handle on the console (the file is open for writing): the blocks are read
  // through the cache's own handle, under its lock in 512 KB pieces (a lookup waits for one piece at most).
  if (!ahead && !g_dksh_shared_handle.exchange(true))
    REXLOG_WARN("[deko3d] DKSH cache {}: a second handle could not be opened (errno {}); the read-ahead goes through "
                "the cache's own handle",
                g_cache.path, errno);
  const auto read_block = [&](long at, uint8_t *dst, size_t len) -> bool {
    if (ahead)
      return std::fseek(ahead, at, SEEK_SET) == 0 && std::fread(dst, 1, len, ahead) == len;
    for (size_t done = 0; done < len;) {
      const size_t piece = std::min<size_t>(len - done, 512u << 10);
      std::lock_guard<std::mutex> lock(g_cache.mutex);
      if (!g_cache.file || std::fseek(g_cache.file, at + long(done), SEEK_SET) != 0 ||
          std::fread(dst + done, 1, piece, g_cache.file) != piece)
        return false;
      done += piece;
    }
    return true;
  };
  long cache_end = 0;
  {
    std::lock_guard<std::mutex> lock(g_cache.mutex);
    cache_end = g_cache.end;
  }
  uint64_t blocks = 0, block_bytes = 0;
  double block_ms = 0.0;
  const auto t_start = std::chrono::steady_clock::now();
  const plume::Deko3DStats &st = plume::GetDeko3DStats();
  for (const auto &[at, hash] : order) {
    if (g_prewarm_cancel.load(std::memory_order_acquire))
      break;
    PrefetchBlock &b = t_prefetch;
    if (at != LONG_MAX && at < cache_end &&
        (b.data.empty() || at < b.begin || at + (64l << 10) > b.begin + long(b.data.size()))) {
      // A record of up to 64 KB from 'at' fits the block; a larger one is read alone by the lookup.
      const auto t0 = std::chrono::steady_clock::now();
      const size_t len = size_t(std::min<long>(long(kPrefetchBytes), cache_end - at));
      b.data.resize(len);
      if (read_block(at, b.data.data(), len)) {
        b.begin = at;
        ++blocks;
        block_bytes += len;
      } else {
        b.data.clear();
      }
      block_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    const uint64_t compiles = st.shaderCompiles;
    if (auto shader = CreateGameShader(device, hash)) {
      if (!dev->loadVariant(static_cast<const plume::Deko3DShader *>(shader.get()), nullptr, 0))
        ++state.failed;
    } else {
      ++state.failed;
    }
    state.compiled += st.shaderCompiles != compiles ? 1 : 0;
    ++state.done;
    progress(state);
  }
  t_prefetch = PrefetchBlock{};  // its memory back
  if (ahead)
    std::fclose(ahead);
  REXLOG_INFO("[deko3d] startup preparation in file order: ordered in {:.0f} ms, then {:.1f} s with {} read-ahead "
              "blocks ({:.1f} MB read in {:.0f} ms)",
              order_ms, std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count(), blocks,
              block_bytes / 1e6, block_ms);
  SaveShaderCache();
  REXLOG_INFO("[deko3d] startup preparation: {} shaders, {} compiled now (not in the DKSH cache), {} failed",
              state.total, state.compiled, state.failed);
  return true;
}

std::string Report() {
  const plume::Deko3DStats &st = plume::GetDeko3DStats();
  const uint64_t compiles = st.shaderCompiles;
  return fmt::format("deko3d: {} draws ({} through the address window), {} pipeline binds ({} state binds); uam {} compiles (avg {:.0f} ms, {} failed), "
                     "DKSH cache {} hits, {} added; memory: images {} MB, buffers {} MB, code {} KB, commands {} KB; "
                     "barriers: {} primitives, {} fragments, {} full ({} invalidating textures); copy and 2D engines: {} "
                     "buffer copies, {} texture uploads, {} readbacks, {} image copies, {} resolves, {} blits",
                     st.draws.load(), st.windowDraws.load(), st.pipelineBinds.load(), st.stateBinds.load(), compiles,
                     compiles ? double(st.shaderCompileNs.load()) / 1e6 / double(compiles) : 0.0,
                     st.shaderCompileFailures.load(), g_cache.hits, g_cache.added, st.imageBytes.load() >> 20,
                     st.bufferBytes.load() >> 20, st.codeBytes.load() >> 10, st.commandBytes.load() >> 10,
                     st.barriersPrimitives.load(), st.barriersFragments.load(), st.barriersFull.load(),
                     st.barriersImageInvalidate.load(), st.copyBuffer.load(), st.copyUpload.load(), st.copyReadback.load(),
                     st.copyImage.load(), st.copyResolve.load(), st.copyBlit.load());
}

#else

std::unique_ptr<plume::RenderShader> CreateGameShader(plume::RenderDevice *, uint64_t) { return nullptr; }
void InstallShaderCache() {}
bool PrewarmGameShaders(plume::RenderDevice *, const std::function<void(const PrewarmProgress &)> &) { return false; }
void CancelPrewarm() {}
std::string Report() { return {}; }

#endif

size_t MissingGameShaders() {
  Packages &all = Open();
  size_t missing = 0;
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i)
    missing += Find(all, g_shaderCacheEntries[i].hash).entry ? 0 : 1;
  return missing;
}

bool BoolSpecMask(uint64_t hash, uint32_t mask[8]) {
  const Found found = Find(Open(), hash);
  if (!found.entry)
    return false;
  std::memcpy(mask, found.entry->boolMask, sizeof(found.entry->boolMask));
  return true;
}

void SaveShaderCache() {
  std::lock_guard<std::mutex> lock(g_cache.mutex);
  if (g_cache.file) {
    std::fflush(g_cache.file);
    fsync(fileno(g_cache.file));  // banjo-nx (2026-10-20): to the card, not only to the file system service
  }
}

}  // namespace banjo::renut_gpu::deko3d
