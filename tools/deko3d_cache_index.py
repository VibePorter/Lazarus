#!/usr/bin/env python3
# me2-nx-deko (2026-10-28): the record index of a DKSH cache (deko3d_shaders.bin -> deko3d_shaders.bin.idx), the
# format LoadCacheIndex reads (app/src/renut_engine/gpu/banjo_renut_deko3d.cpp): "BDKI", u32 version 1, u64 cache
# length, u32 count, u32 0, then per record u64 key, u32 offset of the DKSH, u32 size (little-endian). Without it the
# console walked the 33,370 records at every start (11 s); with it the start reads ~530 KB.
#   python tools/deko3d_cache_index.py <deko3d_shaders.bin> [...]
import struct
import sys

DKSH = 0x48534B44  # "DKSH" (the first word of every record's data, read little-endian)


def index(path):
    data = open(path, 'rb').read()
    if data[:4] != b'BDKC':
        raise SystemExit(f'{path}: not a DKSH cache')
    at, entries = 24, {}
    while at + 16 <= len(data):
        key, size, magic = struct.unpack_from('<QII', data, at)
        if size < 64 or size > (16 << 20) or at + 12 + size > len(data) or magic != DKSH:
            break
        entries[key] = (at + 12, size)
        at += 12 + size
    if at != len(data):
        raise SystemExit(f'{path}: {len(data) - at} bytes after the last whole record; not indexed')
    rows = sorted(((off, key, size) for key, (off, size) in entries.items()))
    out = bytearray(b'BDKI' + struct.pack('<IQII', 1, len(data), len(rows), 0))
    for off, key, size in rows:
        out += struct.pack('<QII', key, off, size)
    open(path + '.idx', 'wb').write(out)
    print(f'{path}.idx: {len(rows)} records for {len(data)} bytes')


for p in sys.argv[1:]:
    index(p)
