/**
 * me2-nx-deko (2026-11-03): asynchronous read-ahead for sequentially read game files (see readahead.cpp).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace rex::filesystem {
class PackStorage;
}

namespace rex::filesystem::readahead {

// How the read-ahead thread reads a file on its own (it never uses the guest's handle, which the guest may close).
struct Source {
  std::filesystem::path host_path;  // a game-folder file: the thread opens its own handle
  // An asset-pack file: positional reads of the pack. Shared (2026-11-07): a DLC package's pack is unmounted when the
  // game closes the package, and a block still queued keeps the pack open until the thread is done with it.
  std::shared_ptr<PackStorage> storage;
  uint64_t base = 0;                // where the file starts in the pack
  uint64_t size = 0;                // file size (prefetches stop at the end)
};

bool Enabled();

// Before going to the disk: copies what the read-ahead already has for the head of [offset, offset + request) and
// returns how many bytes that was (0 = nothing; the caller reads the rest itself). Waits for a block that is being
// read right now; a block that is only queued is cancelled instead (the caller is faster reading it itself).
size_t Take(uint32_t id, uint64_t offset, uint8_t* out, size_t request);

// After the read: `got` bytes at `offset`. Detects a sequential stream and queues the next block.
void Note(uint32_t id, uint64_t offset, size_t got, const Source& source);

// sa2_io_readahead_verify (test aid): the caller rereads served data from the disk and reports the comparison.
bool VerifyEnabled();
void ReportVerify(bool same);

}  // namespace rex::filesystem::readahead
