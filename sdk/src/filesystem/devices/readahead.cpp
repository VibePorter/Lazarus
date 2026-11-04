/**
 * me2-nx-deko (2026-11-03): asynchronous read-ahead for sequentially read game files.
 *
 * UE3 streams level packages (and Textures.tfc) through its async loading thread: a read, then CPU work on what
 * arrived (LZX decompression, serialization), then the next read, which starts exactly where the previous ended. The
 * SD sits idle during the CPU part and the CPU waits during the read. This overlaps them: once a file has been read
 * sa2_io_readahead_start times in a row sequentially, one low-priority thread reads the next block (2x the last read,
 * clamped to sa2_io_readahead_kb .. sa2_io_readahead_max_kb) while the guest is still busy with the current one, so
 * the guest's next read is a memcpy. A file whose prefetched blocks keep going unread (sa2_io_readahead_backoff in a
 * row) is left alone until it shows a longer sequential run.
 *
 * Unlike the synchronous window (sa2_io_window_kb, host_path_file.cpp), which reads 256 KB on the guest's own thread
 * for small reads, nothing here is ever read on the guest's thread that would not have been read anyway:
 *   - a block already in RAM is copied;
 *   - a block the thread is reading right now is waited for (the bytes are on their way; reading them a second
 *     time would only queue behind that read on the same card);
 *   - a block still in the queue is cancelled and the guest reads it itself, so a starved read-ahead thread can
 *     never make the game wait longer than it would have without it.
 * Only read-only game data (the callers check), so a stale copy cannot exist. Blocks are dropped once read past:
 * sequential data is not reread soon (the exact-range cache, sa2_io_ranges_mb, handles rereads).
 *
 * Report every 30 s while active: "[io] readahead: ...".
 */

#include "readahead.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/pack_device.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchSetCurrentThreadPriority(int priority);
extern "C" bool RexSwitchSetCurrentThreadCore(int core);
#endif

REXCVAR_DEFINE_BOOL(sa2_io_readahead, false, "Filesystem",
                    "Asynchronous read-ahead of sequentially read game files (level streaming): a background "
                    "thread reads the next block while the game works on the current one");
// 2026-11-03 console run (TU2, 7.5 min): with 1..4 MB blocks, 2 ahead and prefetching after 2 reads in a row, 75 % of
// the prefetched bytes were never read (604 of 807 MB; UE3 often reads 2-3 blocks in a row and jumps). Smaller blocks,
// 1 ahead, 3 reads in a row to start and a per-file back-off (sa2_io_readahead_backoff) since.
REXCVAR_DEFINE_INT32(sa2_io_readahead_kb, 256, "Filesystem",
                     "Smallest read-ahead block, in KB (a block is 2x the game's last read, at least this)");
REXCVAR_DEFINE_INT32(sa2_io_readahead_max_kb, 1024, "Filesystem", "Largest read-ahead block, in KB");
REXCVAR_DEFINE_INT32(sa2_io_readahead_ahead, 1, "Filesystem",
                     "Blocks kept ahead of each sequential stream (1..4)");
REXCVAR_DEFINE_INT32(sa2_io_readahead_start, 3, "Filesystem",
                     "Sequential reads in a row before a file is prefetched (2..8)");
REXCVAR_DEFINE_INT32(sa2_io_readahead_backoff, 2, "Filesystem",
                     "Prefetched blocks of a file dropped unread in a row before it is no longer prefetched, until it "
                     "shows a longer sequential run (start + 2 reads in a row); 0 = never back off");
REXCVAR_DEFINE_INT32(sa2_io_readahead_mb, 32, "Filesystem",
                     "Cap of all read-ahead blocks in RAM (queued, reading and ready), in MB");
REXCVAR_DEFINE_INT32(sa2_io_readahead_priority, 44, "Filesystem",
                     "Horizon priority of the read-ahead thread (Switch; it mostly waits on the SD card)");
REXCVAR_DEFINE_INT32(sa2_io_readahead_core, -1, "Filesystem",
                     "Preferred core of the read-ahead thread (Switch; -1 = the kernel's choice)");
REXCVAR_DEFINE_BOOL(sa2_io_readahead_verify, false, "Filesystem",
                    "Test aid: every read served by the read-ahead is read again from the disk and compared "
                    "(mismatches are logged). Slower than without it; never for timing");

namespace rex::filesystem::readahead {
namespace {

using Clock = std::chrono::steady_clock;

enum class St : uint8_t { kQueued, kReading, kReady, kFailed };

struct Block {
  uint32_t id = 0;
  uint64_t start = 0;
  size_t len = 0;
  std::vector<uint8_t> data;
  size_t got = 0;
  size_t used = 0;
  St st = St::kQueued;
  Source src;
};

struct Stream {
  uint64_t last_end = 0;
  uint32_t seq = 0;
  uint64_t touch = 0;
  uint32_t misses = 0;   // prefetched blocks dropped unread in a row
  bool backed_off = false;
  std::deque<std::shared_ptr<Block>> blocks;  // sorted by start
};

constexpr size_t kMaxStreams = 16;
constexpr size_t kMaxHandles = 8;

struct State {
  std::mutex m;
  std::condition_variable work;
  std::condition_variable done;
  std::unordered_map<uint32_t, Stream> streams;
  std::deque<std::shared_ptr<Block>> queue;
  uint64_t live = 0;  // bytes of every block (queued, reading, ready)
  uint64_t touch = 0;
  bool thread_started = false;
  bool thread_failed = false;
  // stats (under m)
  uint64_t reads_served = 0, partial = 0, bytes_served = 0, bytes_fetched = 0, bytes_unused = 0, waits = 0,
           cancelled = 0, failed = 0, blocks = 0, mismatches = 0, backoffs = 0;
  double wait_ms = 0, read_ms = 0;
};

State& G() {
  static State* s = new State();  // never destroyed: the thread outlives static destruction
  return *s;
}

// Returns true when the block's SD read was wasted: read (or being read) and nothing of it was served.
bool DropBlock(State& g, const std::shared_ptr<Block>& b) {
  const bool wasted = (b->st == St::kReady || b->st == St::kReading) && b->used == 0;
  if (b->st == St::kReady && b->got > b->used) g.bytes_unused += b->got - b->used;
  if (b->st == St::kQueued) {
    auto it = std::find(g.queue.begin(), g.queue.end(), b);
    if (it != g.queue.end()) g.queue.erase(it);
  }
  // A block being read stays referenced by the thread; its bytes leave `live` when it finishes (see Worker).
  if (b->st != St::kReading) g.live -= b->len;
  b->st = b->st == St::kReading ? St::kReading : St::kFailed;
  return wasted;
}

// A dropped block of stream `s`: count the misses in a row; enough of them and the stream stops being prefetched.
void NoteDrop(State& g, Stream& s, bool wasted) {
  if (!wasted) return;
  const int limit = REXCVAR_GET(sa2_io_readahead_backoff);
  if (limit > 0 && ++s.misses >= uint32_t(limit) && !s.backed_off) {
    s.backed_off = true;
    ++g.backoffs;
  }
}

bool ReadSource(const Source& src, uint32_t id, uint64_t offset, uint8_t* out, size_t len, size_t* got,
                std::vector<std::pair<uint32_t, std::unique_ptr<FileHandle>>>& handles) {
  *got = 0;
  if (src.storage) {
    if (!src.storage->ReadAt(src.base + offset, out, len)) return false;
    *got = len;
    return true;
  }
  FileHandle* h = nullptr;
  for (size_t i = 0; i < handles.size(); ++i) {
    if (handles[i].first == id) {
      std::rotate(handles.begin(), handles.begin() + i, handles.begin() + i + 1);  // most recent first
      h = handles.front().second.get();
      break;
    }
  }
  if (!h) {
    auto fresh = FileHandle::OpenExisting(src.host_path, FileAccess::kGenericRead | FileAccess::kFileReadData);
    if (!fresh) return false;
    if (handles.size() >= kMaxHandles) handles.pop_back();
    handles.insert(handles.begin(), {id, std::move(fresh)});
    h = handles.front().second.get();
  }
  size_t total = 0;
  while (total < len) {
    size_t n = 0;
    if (!h->Read(offset + total, out + total, len - total, &n)) return total > 0 ? (*got = total, true) : false;
    if (n == 0) break;
    total += n;
  }
  *got = total;
  return true;
}

void Worker() {
#if REX_PLATFORM_SWITCH
  RexSwitchSetCurrentThreadPriority(REXCVAR_GET(sa2_io_readahead_priority));
  if (const int core = REXCVAR_GET(sa2_io_readahead_core); core >= 0) RexSwitchSetCurrentThreadCore(core);
#endif
  State& g = G();
  std::vector<std::pair<uint32_t, std::unique_ptr<FileHandle>>> handles;
  auto next_report = Clock::now() + std::chrono::seconds(30);
  uint64_t reported_served = 0;
  for (;;) {
    std::shared_ptr<Block> b;
    {
      std::unique_lock lock(g.m);
      g.work.wait_for(lock, std::chrono::seconds(5), [&] { return !g.queue.empty(); });
      if (Clock::now() >= next_report) {
        next_report = Clock::now() + std::chrono::seconds(30);
        if (g.reads_served != reported_served || !g.queue.empty()) {
          reported_served = g.reads_served;
          REXFS_INFO(
              "[io] readahead: {} reads served ({} partly), {:.1f} MB from RAM of {:.1f} MB prefetched, {:.1f} MB "
              "unused; {} waits ({:.0f} ms), {} cancelled, {} failed; SD {:.0f} ms in {} blocks; {} streams, "
              "{:.1f} MB live, {} back-offs{}",
              g.reads_served, g.partial, g.bytes_served / 1048576.0, g.bytes_fetched / 1048576.0,
              g.bytes_unused / 1048576.0, g.waits, g.wait_ms, g.cancelled, g.failed, g.read_ms, g.blocks,
              g.streams.size(), g.live / 1048576.0, g.backoffs,
              REXCVAR_GET(sa2_io_readahead_verify)
                  ? ", " + std::to_string(g.mismatches) + " verify mismatches"
                  : std::string());
        }
      }
      if (g.queue.empty()) continue;
      b = g.queue.front();
      g.queue.pop_front();
      b->st = St::kReading;
    }
    std::vector<uint8_t> data;
    size_t got = 0;
    bool ok = false;
    const auto t0 = Clock::now();
    try {
      data.resize(b->len);
      ok = ReadSource(b->src, b->id, b->start, data.data(), b->len, &got, handles);
    } catch (const std::bad_alloc&) {
      ok = false;
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    {
      std::lock_guard lock(g.m);
      g.read_ms += ms;
      ++g.blocks;
      // Still wanted? DropBlock leaves a block being read in kReading; check it is still in its stream.
      bool wanted = false;
      if (auto it = g.streams.find(b->id); it != g.streams.end()) {
        wanted = std::find(it->second.blocks.begin(), it->second.blocks.end(), b) != it->second.blocks.end();
      }
      if (ok && got && wanted) {
        b->data = std::move(data);
        b->got = got;
        b->st = St::kReady;
        g.bytes_fetched += got;
      } else {
        if (!ok) ++g.failed;
        b->st = St::kFailed;
        g.live -= b->len;
        if (wanted) {
          auto& blocks = g.streams[b->id].blocks;
          blocks.erase(std::find(blocks.begin(), blocks.end(), b));
        }
      }
    }
    g.done.notify_all();
  }
}

bool StartThread(State& g) {
  if (g.thread_started) return true;
  if (g.thread_failed) return false;
  try {
    new std::thread(Worker);  // leaked on purpose: never detach() on the Switch (it throws there)
    g.thread_started = true;
    REXFS_INFO("[io] readahead: on ({} KB .. {} KB blocks, {} ahead, {} MB cap, after {} reads in a row, back-off "
               "after {} unread blocks)",
               REXCVAR_GET(sa2_io_readahead_kb), REXCVAR_GET(sa2_io_readahead_max_kb),
               REXCVAR_GET(sa2_io_readahead_ahead), REXCVAR_GET(sa2_io_readahead_mb),
               REXCVAR_GET(sa2_io_readahead_start), REXCVAR_GET(sa2_io_readahead_backoff));
    return true;
  } catch (const std::exception& e) {
    g.thread_failed = true;
    REXFS_WARN("[io] readahead: thread not created ({}): off", e.what());
    return false;
  }
}

}  // namespace

bool Enabled() {
  return REXCVAR_GET(sa2_io_readahead);
}

size_t Take(uint32_t id, uint64_t offset, uint8_t* out, size_t request) {
  if (!id || !request || !Enabled()) return 0;
  State& g = G();
  std::unique_lock lock(g.m);
  auto it = g.streams.find(id);
  if (it == g.streams.end() || it->second.blocks.empty()) return 0;
  size_t copied = 0;
  while (copied < request) {
    // Looked up again every round: waiting below releases the lock, and the stream may be evicted meanwhile.
    it = g.streams.find(id);
    if (it == g.streams.end()) break;
    Stream& s = it->second;
    const uint64_t pos = offset + copied;
    // Blocks wholly before the read position were read past (or skipped): drop them.
    while (!s.blocks.empty() && s.blocks.front()->start + s.blocks.front()->len <= pos &&
           s.blocks.front()->st != St::kReading) {
      NoteDrop(g, s, DropBlock(g, s.blocks.front()));
      s.blocks.pop_front();
    }
    std::shared_ptr<Block> b;
    for (auto& c : s.blocks) {
      if (c->start <= pos && pos < c->start + c->len) {
        b = c;
        break;
      }
    }
    if (!b) break;
    if (b->st == St::kQueued) {
      // Not started: the caller reads faster itself than it would wait for the queue.
      ++g.cancelled;
      DropBlock(g, b);
      s.blocks.erase(std::find(s.blocks.begin(), s.blocks.end(), b));
      break;
    }
    if (b->st == St::kReading) {
      ++g.waits;
      const auto t0 = Clock::now();
      g.done.wait(lock, [&] { return b->st != St::kReading; });
      g.wait_ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
    if (b->st != St::kReady || pos >= b->start + b->got) break;
    const size_t n = std::min<size_t>(b->start + b->got - pos, request - copied);
    std::memcpy(out + copied, b->data.data() + (pos - b->start), n);  // ready blocks never change
    b->used += n;
    if (auto hit = g.streams.find(id); hit != g.streams.end()) hit->second.misses = 0;
    copied += n;
  }
  if (copied) {
    ++g.reads_served;
    if (copied < request) ++g.partial;
    g.bytes_served += copied;
  }
  return copied;
}

void Note(uint32_t id, uint64_t offset, size_t got, const Source& source) {
  if (!id || !got || !Enabled()) return;
  State& g = G();
  bool queued = false;
  {
    std::lock_guard lock(g.m);
    auto it = g.streams.find(id);
    if (it == g.streams.end()) {
      if (g.streams.size() >= kMaxStreams) {
        auto old = g.streams.begin();
        for (auto i = g.streams.begin(); i != g.streams.end(); ++i) {
          if (i->second.touch < old->second.touch) old = i;
        }
        for (auto& b : old->second.blocks) DropBlock(g, b);
        g.streams.erase(old);
      }
      it = g.streams.emplace(id, Stream{}).first;
    }
    Stream& s = it->second;
    s.touch = ++g.touch;
    const uint64_t end = offset + got;
    if (offset == s.last_end && s.last_end != 0) {
      ++s.seq;
    } else {
      s.seq = 0;
      // A jump: blocks that do not lie ahead of the new position are useless now.
      for (auto b = s.blocks.begin(); b != s.blocks.end();) {
        if ((*b)->start + (*b)->len <= end || (*b)->start > end + (uint64_t(64) << 20)) {
          if ((*b)->st == St::kReading) {
            ++b;  // the thread finishes it and finds it still listed; dropped by the next Take past it
            continue;
          }
          NoteDrop(g, s, DropBlock(g, *b));
          b = s.blocks.erase(b);
        } else {
          ++b;
        }
      }
    }
    s.last_end = end;
    // sa2_io_readahead_start reads in a row before anything is prefetched; a backed-off file needs two more.
    const uint32_t start = uint32_t(std::clamp(REXCVAR_GET(sa2_io_readahead_start), 2, 8));
    if (s.backed_off) {
      if (s.seq + 1 < start + 2) return;
      s.backed_off = false;
      s.misses = 0;
    }
    if (s.seq + 1 < start) return;
    const size_t min_b = size_t(std::max(64, REXCVAR_GET(sa2_io_readahead_kb))) * 1024;
    const size_t max_b = std::max(min_b, size_t(std::max(64, REXCVAR_GET(sa2_io_readahead_max_kb))) * 1024);
    const size_t block = std::clamp(got * 2, min_b, max_b);
    const int ahead = std::clamp(REXCVAR_GET(sa2_io_readahead_ahead), 1, 4);
    const uint64_t cap = uint64_t(std::max(1, REXCVAR_GET(sa2_io_readahead_mb))) << 20;
    // Where the blocks already ahead of this read end.
    uint64_t cover = end;
    int count = 0;
    for (auto& b : s.blocks) {
      if (b->start + b->len <= end) continue;
      if (b->start <= cover) {
        cover = std::max(cover, b->start + b->len);
        ++count;
      }
    }
    while (count < ahead && cover < source.size && g.live + block <= cap) {
      auto b = std::make_shared<Block>();
      b->id = id;
      b->start = cover;
      b->len = size_t(std::min<uint64_t>(block, source.size - cover));
      b->src = source;
      s.blocks.push_back(b);
      std::sort(s.blocks.begin(), s.blocks.end(), [](const auto& a, const auto& c) { return a->start < c->start; });
      g.queue.push_back(b);
      g.live += b->len;
      cover += b->len;
      ++count;
      queued = true;
    }
    if (queued && !StartThread(g)) {
      for (auto& b : s.blocks) DropBlock(g, b);
      s.blocks.clear();
      return;
    }
  }
  if (queued) g.work.notify_one();
}

void ReportVerify(bool same) {
  if (same) return;
  State& g = G();
  std::lock_guard lock(g.m);
  if (++g.mismatches <= 10) REXFS_ERROR("[io] readahead: verify MISMATCH (data served from RAM differs from the disk)");
}

bool VerifyEnabled() {
  return REXCVAR_GET(sa2_io_readahead_verify);
}

}  // namespace rex::filesystem::readahead
