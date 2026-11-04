"""Run the real deko3d allocator methods with a fragmented heap and fake GPU blocks.

Only libnx/deko3d and the process heap are replaced. No Switch hardware is required.
Usage: python3 app/tests/run_me_deko_memory_test.py [--cxx clang++]
"""
from pathlib import Path
import argparse
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='clang++')
args = parser.parse_args()
out = root / 'out/fixes/crashes_20261029/allocator_test'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'app/thirdparty/plume/plume_deko3d.cpp').read_text()

def method(name):
    at = source.index('Deko3DDevice::' + name + '(')
    start = source.rfind('\n', 0, at) + 1
    end = source.index('\n    }', at) + len('\n    }')
    return source[start:end]

harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
using DkMemBlock = void *;
constexpr uint32_t DK_MEMBLOCK_ALIGNMENT = 4096;
constexpr uint32_t DkMemBlockFlags_GpuCached = 1, DkMemBlockFlags_Image = 2;
constexpr uint64_t MiB = 1ull << 20;
constexpr uint32_t ImageChunkSize = 64u << 20;
static uint64_t heapFree = 400 * MiB, largestHeap = 32 * MiB, nextStorage = 0x100000000ull;
static uint32_t creates = 0, destroys = 0, frees = 0, internalAllocations = 0;
static uint32_t heapWalks = 0;
static bool failCreate = false;
static std::map<void *, size_t> storage;
static std::map<DkMemBlock, void *> live;
static void memoryFigures(uint64_t &heap, uint64_t &process) { ++heapWalks; heap = heapFree; process = 0; }
static void logf(const char *, ...) {}
static void *memalign(size_t, size_t size) {
    if (size > largestHeap) return nullptr;
    void *p = reinterpret_cast<void *>(nextStorage);
    nextStorage += size + DK_MEMBLOCK_ALIGNMENT;
    storage[p] = size;
    return p;
}
static void fakeFree(void *p) { if (p) { assert(storage.erase(p) == 1); ++frees; } }
#define free fakeFree
struct DkMemBlockMaker { uint32_t size, flags; void *storage = nullptr; };
static void dkMemBlockMakerDefaults(DkMemBlockMaker *m, void *, uint32_t size) { m->size = size; }
static DkMemBlock dkMemBlockCreate(DkMemBlockMaker *m) {
    ++creates;
    if (!m->storage) { ++internalAllocations; std::abort(); }
    if (failCreate) return nullptr;
    auto block = reinterpret_cast<void *>(uintptr_t(creates));
    live[block] = m->storage;
    return block;
}
static void dkMemBlockDestroy(DkMemBlock block) {
    assert(live.erase(block) == 1);
    ++destroys;
}
static constexpr uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
static constexpr uint64_t alignUp64(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
struct Deko3DStats { std::atomic<uint64_t> imageBytes{0}, bufferBytes{0}, codeBytes{0}, commandBytes{0}; };
static Deko3DStats stats;
static Deko3DStats &GetDeko3DStats() { return stats; }
struct Deko3DImageAlloc { DkMemBlock block = nullptr; uint32_t offset = 0, size = 0; int32_t chunk = -1; };
struct Deko3DDevice {
    void *dk = nullptr;
    struct BlockArena {
        std::mutex mutex;
        bool tried = true;
        uint8_t *base = reinterpret_cast<uint8_t *>(0x80000000ull);
        uint64_t size = 1024 * MiB, used = 0, peak = 0, fallbacks = 0;
        std::map<uint64_t, uint64_t> free;
        std::unordered_map<DkMemBlock, std::pair<uint64_t, uint64_t>> blocks;
        std::unordered_map<DkMemBlock, void *> heapBlocks;
    } arena;
    struct Chunk { DkMemBlock block = nullptr; uint32_t size = 0; std::vector<std::pair<uint32_t,uint32_t>> freeList; };
    std::mutex heapMutex;
    std::vector<Chunk> chunks;
    DkMemBlock makeBlock(uint32_t, uint32_t, const char *, bool = false);
    void destroyBlock(DkMemBlock);
    uint64_t arenaLargestFree();
    void memoryBudget(uint64_t &, uint64_t &);
    void *arenaTake(uint32_t);
    void arenaGive(DkMemBlock, void *, uint32_t);
    bool arenaDetach(DkMemBlock, uint64_t &, uint64_t &);
    void arenaFreeRange(uint64_t, uint64_t);
    void arenaFreeRangeLocked(uint64_t, uint64_t);
    bool imageAlloc(uint32_t, uint32_t, Deko3DImageAlloc &);
    void collectRetired() {} // retirement is separately fenced by the production code
};
static constexpr uint64_t ArenaMax = 1024 * MiB, ArenaMin = 256 * MiB, ArenaLeave = 1700 * MiB;
'''
chunk_start = source.index('    static void chunkFree(')
chunk_end = source.index('\n    bool Deko3DDevice::imageAlloc', chunk_start)
harness += source[chunk_start:chunk_end]
for name in ('makeBlock', 'arenaLargestFree', 'memoryBudget', 'arenaTake', 'arenaGive',
             'arenaDetach', 'arenaFreeRange', 'arenaFreeRangeLocked', 'destroyBlock', 'imageAlloc'):
    harness += '\n' + method(name) + '\n'
harness += r'''
int main() {
    Deko3DDevice d;
    // 400 MB free in the process heap must not hide an arena split into ranges below the 192 MB reserve.
    d.arena.free = {{0, 48 * MiB}, {64 * MiB, 32 * MiB}, {128 * MiB, 16 * MiB}};
    d.arena.used = d.arena.size - 96 * MiB;
    stats.imageBytes = 680 * MiB;
    uint64_t used = 0, budget = 0;
    d.memoryBudget(used, budget);
    assert(used == 680 * MiB && budget - used == 48 * MiB);
    assert(heapWalks == 0); // the normal periodic pressure check must not walk the fragmented process heap
    assert(budget - used < 192 * MiB); // cold eviction is now reachable
    Deko3DImageAlloc image;
    assert(d.imageAlloc(1 * MiB, 65536, image));
    assert(d.chunks[0].size == 48 * MiB && d.arena.heapBlocks.empty());
    assert(d.imageAlloc(2 * MiB, 65536, image));
    assert(d.chunks.size() == 1); // reuse space in the chunk
    d.destroyBlock(d.chunks[0].block);
    assert(d.arenaLargestFree() == 48 * MiB);
    // Empty arena, fragmented heap: a 16 MB image chunk succeeds where the old 64 MB allocation aborted.
    d.chunks.clear(); d.arena.free.clear(); d.arena.used = d.arena.size;
    assert(d.imageAlloc(MiB, 65536, image));
    assert(d.chunks[0].size == 16 * MiB && d.arena.heapBlocks.size() == 1);
    void *owned = live.at(image.block);
    assert(storage.count(owned));
    d.destroyBlock(image.block);
    assert(!storage.count(owned) && d.arena.heapBlocks.empty());
    // Enough aggregate free bytes, but no single allocation fits: fail without invoking deko3d's fatal allocator.
    d.chunks.clear(); largestHeap = MiB;
    const auto before = creates;
    assert(!d.imageAlloc(2 * MiB, 65536, image));
    assert(creates == before && d.chunks.empty());
    // A failed GPU block creation returns its caller-owned storage or its arena range exactly once.
    largestHeap = 32 * MiB; failCreate = true;
    assert(!d.imageAlloc(MiB, 65536, image));
    assert(storage.empty() && d.arena.heapBlocks.empty());
    d.arena.free = {{0, 8 * MiB}}; d.arena.used = d.arena.size - 8 * MiB;
    assert(!d.imageAlloc(MiB, 65536, image));
    assert(d.arenaLargestFree() == 8 * MiB && d.arena.blocks.empty());
    failCreate = false;
    // No arena uses heap headroom; exhausting both never returns the zero/unknown budget sentinel.
    d.arena.base = nullptr; heapFree = 80 * MiB;
    d.memoryBudget(used, budget); assert(budget - used == 80 * MiB);
    heapFree = 0; stats.imageBytes = 0;
    d.memoryBudget(used, budget); assert(budget == 1);
    assert(!d.imageAlloc(UINT32_MAX, 65536, image));
    assert(live.empty() && internalAllocations == 0 && destroys == 2 && frees == 2);
    std::puts("PASS: pressure eviction budget, fragmented arena, chunk reuse, 16 MB heap fallback, OOM, failed-create ownership, exhausted budget, size overflow");
}
'''
path = out / 'allocator_test.cpp'
path.write_text(harness)
exe = out / 'allocator_test'
subprocess.run([args.cxx, '-std=c++20', '-O1', '-g', '-fsanitize=undefined',
                str(path), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
