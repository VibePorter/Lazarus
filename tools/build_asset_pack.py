#!/usr/bin/env python3
"""Builds the asset cache of a game data folder on the PC: <folder>.nxpack + <folder>.nxidx next to it, the format
of the SDK's PackDevice (sdk/include/rex/filesystem/devices/pack_device.h), which is Fruit Ninja NX's asset pack
version 2 byte for byte. The console then mounts the pack instead of opening thousands of files on the SD card, and
does not have to build it on its first boot.

  python tools/build_asset_pack.py assets/game_root [--out-dir DIR] [--name NAME] [--exfat] [--if-stale] [--verify]

--name: the pack's base name (default: the folder's). The console mounts <game_data_root>.nxpack/.nxidx, so a PC
folder called game_root for a console folder called game needs --name game.
--exfat (ME2, 2026-10-31): allow a pack of 4 GB or more (one file: fine on an exFAT SD card, which the Switch's
system supports through its exFAT driver; FAT32 cannot hold it). ME2's game data is 9.2 GB.
--split-mb N (ME2, 2026-11-07): version 3, the same logical pack in parts of N MB (FAT32: N <= 4095; 3072 suggested)
named <name>.nxpack, <name>.nxpack.1, .2, ... A FAT32 card can hold it, and an MTP upload copies a few big files
instead of thousands (each file costs MTP a round of requests). PackHeader.reserved = N; index offsets stay logical.
--inside (ME2, 2026-11-07): write the pack INTO the folder, named after it (game/game.nxpack(.N), game/game.nxidx);
the SDK looks there first, so an SD card's game/ folder can hold only the pack. Pack files found at the top of the
folder are never packed themselves.

Layout: PackHeader (40 bytes) then every file 16-byte aligned, in path order (strcmp of the UTF-8 '/'-separated
relative path); the index is IndexHeader (56 bytes), DiskEntry[count] (24 bytes each) and the NUL-terminated paths.
Checksums are 64-bit FNV-1a. --verify rereads a pack and checks the data checksum (asset_pack.c only compares the
headers at mount time).
"""
import argparse
import os
import struct
import sys
import time

FNV_BASIS = 1469598103934665603
FNV_PRIME = 1099511628211
MASK = (1 << 64) - 1
CHUNK = 1 << 20


def _fnv_python(h, data):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def _load_fnv_c():
    """tools/fnv64.c compiled once into out/tools (ctypes); None if no C compiler is found."""
    import ctypes
    import shutil
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    lib = os.path.join(root, 'out', 'tools', 'fnv64' + ('.dll' if os.name == 'nt' else '.so'))
    if not os.path.exists(lib):
        os.makedirs(os.path.dirname(lib), exist_ok=True)
        candidates = [os.environ.get('SA2_LLVM') and os.path.join(os.environ['SA2_LLVM'], 'bin', 'clang.exe'),
                      os.path.join(root, '..', '_tools', 'llvm', 'bin', 'clang.exe'),
                      shutil.which('clang'), shutil.which('gcc'), shutil.which('cc')]
        for cc in [c for c in candidates if c and os.path.exists(c)]:
            if subprocess.run([cc, '-O2', '-shared', '-o', lib, os.path.join(here, 'fnv64.c')] +
                              ([] if os.name == 'nt' else ['-fPIC']), capture_output=True).returncode == 0:
                break
        else:
            return None
    dll = ctypes.CDLL(lib)
    dll.fnv64.restype = ctypes.c_uint64
    dll.fnv64.argtypes = [ctypes.c_uint64, ctypes.c_char_p, ctypes.c_size_t]
    return lambda h, data: dll.fnv64(h, bytes(data), len(data))


_FNV_C = _load_fnv_c()
if _FNV_C is None:
    print('warning: no C compiler for tools/fnv64.c: hashing in Python (slow)', file=sys.stderr)


def fnv(h, data):
    return _FNV_C(h, data) if _FNV_C else _fnv_python(h, data)


fnv_fast = fnv


def valid_relative(path):
    if not path or path.startswith('/') or path.endswith('/'):
        return False
    for part in path.split('/'):
        if part in ('', '.', '..') or '\\' in part or ':' in part:
            return False
    return True


def collect(folder):
    items = []
    for root, _, files in os.walk(folder):
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, folder).replace(os.sep, '/')
            if '/' not in rel and ('.nxpack' in rel or '.nxidx' in rel):
                continue  # a pack kept inside the folder (--inside) is not game data
            if not valid_relative(rel):
                sys.exit(f'invalid path for the pack: {rel}')
            items.append([rel.encode('utf-8'), full, os.path.getsize(full), 0])
    items.sort(key=lambda item: item[0])
    return items


def part_path(pack_path, i):
    return pack_path if i == 0 else f'{pack_path}.{i}'


def remove_parts(pack_path, keep=0, suffix=''):
    """Deletes <pack>.N (+suffix) for N >= max(keep, 1): leftovers of an older split build."""
    i = max(keep, 1)
    while os.path.exists(part_path(pack_path, i) + suffix):
        os.remove(part_path(pack_path, i) + suffix)
        i += 1


class PartWriter:
    """Writes one logical byte stream into part files of part_size bytes (one file when part_size is None)."""

    def __init__(self, pack_path, part_size, suffix='.tmp'):
        self.pack_path, self.part_size, self.suffix = pack_path, part_size, suffix
        self.index, self.pos = 0, 0
        self.files = [open(part_path(pack_path, 0) + suffix, 'wb')]

    def write(self, data):
        view = memoryview(data)
        while len(view):
            if self.part_size:
                room = self.part_size - (self.pos - self.index * self.part_size)
                if room == 0:
                    self.index += 1
                    self.files.append(open(part_path(self.pack_path, self.index) + self.suffix, 'wb'))
                    room = self.part_size
                piece = view[:room]
            else:
                piece = view
            self.files[-1].write(piece)
            self.pos += len(piece)
            view = view[len(piece):]

    def write_header(self, header):
        self.files[0].seek(0)
        self.files[0].write(header)

    def close(self):
        for f in self.files:
            f.close()
        return len(self.files)


class PartReader:
    """Positional reads from a (split) pack."""

    def __init__(self, pack_path, part_size, count):
        self.part_size = part_size
        self.files = [open(part_path(pack_path, i), 'rb') for i in range(count)]

    def read(self, offset, size):
        out = bytearray()
        while size:
            if self.part_size:
                i = offset // self.part_size
                local = offset - i * self.part_size
                n = min(size, self.part_size - local)
            else:
                i, local, n = 0, offset, size
            self.files[i].seek(local)
            piece = self.files[i].read(n)
            if not piece:
                raise IOError(f'short read at {offset}')
            out += piece
            offset += len(piece)
            size -= len(piece)
        return bytes(out)

    def close(self):
        for f in self.files:
            f.close()


def build(folder, out_dir, name=None, exfat=False, split_mb=None):
    folder = os.path.abspath(folder)
    base = os.path.join(out_dir or os.path.dirname(folder), name or os.path.basename(folder))
    pack_path, index_path = base + '.nxpack', base + '.nxidx'
    part_size = split_mb << 20 if split_mb else None
    items = collect(folder)
    if not items or len(items) > 100000:
        sys.exit('no files, or too many files, to pack')
    pack_size, paths_size, total = 40, 0, 0
    pack_id = FNV_BASIS
    for item in items:
        pack_size = (pack_size + 15) & ~15
        item[3] = pack_size
        pack_size += item[2]
        paths_size += len(item[0]) + 1
        total += item[2]
        pack_id = fnv(pack_id, item[0])
        pack_id = fnv(pack_id, struct.pack('<Q', item[2]))
    if part_size and pack_size <= part_size:
        part_size = None  # fits in one part: an ordinary version 2 pack
    if pack_size >= 4 << 30 and not exfat and not part_size:  # one plain file: FAT32 cannot hold 4 GB or more
        sys.exit(f'the pack would be {pack_size >> 20} MB, over the FAT32 4 GB file limit: not built '
                 '(--exfat for an exFAT SD card)')
    nonce = ((int(time.time()) << 32) ^ time.perf_counter_ns()) & MASK
    pack_id = fnv(pack_id, struct.pack('<Q', nonce)) or 1

    data_checksum = FNV_BASIS
    t0 = time.time()
    version = 3 if part_size else 2
    remove_parts(pack_path, suffix='.tmp')
    out = PartWriter(pack_path, part_size)
    try:
        out.write(b'\0' * 40)
        pos, done, last = 40, 0, -1
        for rel, full, size, offset in items:
            if offset > pos:
                out.write(b'\0' * (offset - pos))
            pos = offset
            with open(full, 'rb') as f:
                while True:
                    piece = f.read(CHUNK)
                    if not piece:
                        break
                    out.write(piece)
                    data_checksum = fnv_fast(data_checksum, piece)
                    pos += len(piece)
                    done += len(piece)
            percent = done * 100 // total if total else 100
            if percent // 10 != last:
                last = percent // 10
                print(f'  packing {percent} %  ({done >> 20} / {total >> 20} MB, {time.time() - t0:.0f} s)')
        out.write_header(b'SSNXPAK1' + struct.pack('<IIQQQ', version, split_mb if part_size else 0, pack_id,
                                                   pack_size, data_checksum))
    finally:
        parts = out.close()

    entries = bytearray()
    paths = bytearray()
    for rel, full, size, offset in items:
        entries += struct.pack('<QQII', offset, size, len(paths), len(rel))
        paths += rel + b'\0'
    checksum = fnv(fnv(FNV_BASIS, bytes(entries)), bytes(paths))
    with open(index_path + '.tmp', 'wb') as out:
        out.write(b'SSNXIDX1' + struct.pack('<IIQQQQQ', version, len(items), pack_id, pack_size, len(paths), checksum,
                                            data_checksum))
        out.write(entries)
        out.write(paths)
    for final in [part_path(pack_path, i) for i in range(parts)] + [index_path]:
        if os.path.exists(final):
            os.remove(final)
    remove_parts(pack_path, keep=parts)
    for i in range(parts):
        os.rename(part_path(pack_path, i) + '.tmp', part_path(pack_path, i))
    os.rename(index_path + '.tmp', index_path)
    split = f', {parts} parts of up to {split_mb} MB' if part_size else ''
    print(f'{pack_path}: {len(items)} files, {pack_size >> 20} MB{split}, data checksum {data_checksum:016x}')
    return pack_path, index_path


def verify(pack_path, index_path):
    with open(index_path, 'rb') as f:
        head = f.read(56)
        magic, version, count, pack_id, pack_size, paths_size, checksum, data_checksum = struct.unpack(
            '<8sIIQQQQQ', head)
        entries = f.read(count * 24)
        paths = f.read(paths_size)
    assert magic == b'SSNXIDX1' and version in (2, 3), 'not an asset index'
    assert fnv(fnv(FNV_BASIS, entries), paths) == checksum, 'index checksum mismatch'
    h = FNV_BASIS
    with open(pack_path, 'rb') as f:
        pmagic, pversion, preserved, ppack_id, pfile_size, pdata = struct.unpack('<8sIIQQQ', f.read(40))
    assert pmagic == b'SSNXPAK1' and pversion == version and ppack_id == pack_id and pfile_size == pack_size
    part_size, count_parts = None, 1
    if version == 3:
        part_size = preserved << 20
        count_parts = (pack_size + part_size - 1) // part_size
        for i in range(count_parts):
            expected = part_size if i + 1 < count_parts else pack_size - part_size * (count_parts - 1)
            assert os.path.getsize(part_path(pack_path, i)) == expected, f'part {i} has the wrong size'
    else:
        assert os.path.getsize(pack_path) == pack_size
    reader = PartReader(pack_path, part_size, count_parts)
    try:
        for i in range(count):
            offset, size, _, _ = struct.unpack_from('<QQII', entries, i * 24)
            while size:
                piece = reader.read(offset, min(size, CHUNK))
                h = fnv_fast(h, piece)
                offset += len(piece)
                size -= len(piece)
    finally:
        reader.close()
    assert h == data_checksum == pdata, f'data checksum mismatch: {h:016x} != {data_checksum:016x}'
    print(f'{pack_path}: {count} files verified' + (f' ({count_parts} parts)' if version == 3 else ''))


def stale(folder, out_dir, name=None):
    base = os.path.join(out_dir or os.path.dirname(os.path.abspath(folder)), name or os.path.basename(os.path.abspath(folder)))
    if not (os.path.exists(base + '.nxpack') and os.path.exists(base + '.nxidx')):
        return True
    built = min(os.path.getmtime(base + '.nxpack'), os.path.getmtime(base + '.nxidx'))
    for root, dirs, files in os.walk(folder):
        for name in files + dirs:
            if os.path.getmtime(os.path.join(root, name)) > built:
                return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folder')
    ap.add_argument('--out-dir', help='where to write <name>.nxpack/.nxidx (default: next to the folder)')
    ap.add_argument('--if-stale', action='store_true', help='only when missing or older than a file of the folder')
    ap.add_argument('--verify', action='store_true', help='reread the pack and check its data checksum')
    ap.add_argument('--name', help='base name of the pack files (default: the folder name)')
    ap.add_argument('--exfat', action='store_true', help='allow a pack of 4 GB or more (exFAT SD card)')
    ap.add_argument('--split-mb', type=int, help='split the pack into parts of this many MB (FAT32: <= 4095)')
    ap.add_argument('--inside', action='store_true',
                    help='write the pack into the folder itself (<folder>/<name>.nxpack), where the SDK looks first')
    a = ap.parse_args()
    if a.inside:
        a.out_dir = os.path.abspath(a.folder)
        a.name = a.name or os.path.basename(os.path.normpath(a.out_dir))
    if a.split_mb is not None and not 1 <= a.split_mb <= 4095:
        sys.exit('--split-mb must be 1..4095')
    if a.if_stale and not stale(a.folder, a.out_dir, a.name):
        print('asset pack is up to date')
        base = os.path.join(a.out_dir or os.path.dirname(os.path.abspath(a.folder)),
                            a.name or os.path.basename(os.path.abspath(a.folder)))
        paths = (base + '.nxpack', base + '.nxidx')
    else:
        paths = build(a.folder, a.out_dir, a.name, a.exfat, a.split_mb)
    if a.verify:
        verify(*paths)


if __name__ == '__main__':
    main()
