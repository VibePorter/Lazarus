#!/usr/bin/env python3
"""Reads an Xbox 360 Games on Demand (GOD / SVOD) package: the disc's GDFX file system stored in <header>.data/Data####.

Mass Effect 2 on Games on Demand is two packages, one per disc (the header file plus its .data folder, e.g.
454108CE/00007000/<header> and <header>.data/). The Russian release is commonly kept this way.

  python tools/god_extract.py <header file> list [filter]          list the disc's files
  python tools/god_extract.py <header file> extract <path> <out>   one file
  python tools/god_extract.py <header file> extract-all <folder>   the whole disc (resumes: finished files are kept)

Then merge both discs like the English ones: python tools/merge_discs.py <game> <disc 1 folder> <disc 2 folder>.

SVOD layout (as Xenia's SvodContainerDevice reads it): 0x800-byte data blocks; every 0x198 blocks are preceded by a
level-0 hash table (0x1000 bytes), every 0xA1C4 level-0 tables by a level-1 table; each Data part holds 0x14388
blocks (0xA290000 bytes). The GDFX volume descriptor is at disc sector 32 ("MICROSOFT*XBOX*MEDIA": root sector, size).
Only reads; nothing is written into the package.
"""
import os
import struct
import sys

BLOCK = 0x800
HASH = 0x1000
PER_L0 = 0x198
PER_L1 = 0xA1C4
PER_FILE = 0x14388
MAX_FILE = 0xA290000


class God:
    def __init__(self, header):
        self.dir = header + '.data'
        if not os.path.isdir(self.dir):
            sys.exit(f'{self.dir} not found: pass the header file that sits next to its .data folder')
        self.files = {}
        self.block_offset = 0

    def part(self, i):
        f = self.files.get(i)
        if f is None:
            f = self.files[i] = open(os.path.join(self.dir, 'Data%04d' % i), 'rb')
        return f

    def block_addr(self, block):
        true_block = block - self.block_offset * 2
        file_block = true_block % PER_FILE
        file_index = true_block // PER_FILE
        l0 = file_block // PER_L0 + 1
        l1 = l0 // PER_L1 + 1
        addr = file_block * BLOCK + (l0 + l1) * HASH
        if addr >= MAX_FILE:
            file_index += 1
            addr = addr % MAX_FILE + 0x2000
        return file_index, addr

    def read(self, sector, size):
        # Blocks are contiguous up to the next level-0 hash table (every 0x198 blocks) or the end of a part.
        out = bytearray()
        block = sector
        while len(out) < size:
            fi, addr = self.block_addr(block)
            true_block = block - self.block_offset * 2
            run = PER_L0 - (true_block % PER_FILE) % PER_L0
            need = (size - len(out) + BLOCK - 1) // BLOCK
            n = min(run, need)
            f = self.part(fi)
            f.seek(addr)
            chunk = f.read(n * BLOCK)
            if len(chunk) < n * BLOCK:  # a part ended early: single blocks from here
                n = 1
                f.seek(addr)
                chunk = f.read(BLOCK)
            out += chunk
            block += n
        return bytes(out[:size])

    def root(self):
        vd = self.read(32, BLOCK)
        if vd[:20] != b'MICROSOFT*XBOX*MEDIA':
            sys.exit('no GDFX volume descriptor: not a Games on Demand disc package')
        return struct.unpack_from('<II', vd, 20)

    def walk(self, sector, size, prefix=''):
        data = self.read(sector, size)
        out = []
        seen = set()

        def node(off):
            if off in seen or off * 4 + 14 > len(data):
                return
            seen.add(off)
            p = off * 4
            left, right, start, length, attr, nlen = struct.unpack_from('<HHIIBB', data, p)
            if left == 0xFFFF and right == 0xFFFF:
                return
            name = data[p + 14:p + 14 + nlen].decode('latin-1')
            if left:
                node(left)
            path = prefix + name
            if attr & 0x10:
                out.extend(self.walk(start, length, path + '/') if length else [])
            else:
                out.append((path, start, length))
            if right:
                node(right)

        node(0)
        return out

    def copy(self, start, length, dst):
        with open(dst, 'wb') as out:
            done = 0
            while done < length:
                n = min(length - done, 64 << 20)
                out.write(self.read(start + done // BLOCK, n))
                done += n


def main():
    if len(sys.argv) < 3 or sys.argv[2] not in ('list', 'extract', 'extract-all'):
        sys.exit(__doc__)
    g = God(sys.argv[1])
    entries = g.walk(*g.root())
    cmd = sys.argv[2]
    if cmd == 'list':
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        total = 0
        for path, _, length in entries:
            total += length
            if flt in path.lower():
                print(f'{length:12d}  {path}')
        print(f'{len(entries)} files, {total / 1e9:.2f} GB')
    elif cmd == 'extract':
        want = sys.argv[3].lower().replace('\\', '/')
        for path, start, length in entries:
            if path.lower() == want:
                g.copy(start, length, sys.argv[4])
                print(f'{path}: {length} bytes -> {sys.argv[4]}')
                return
        sys.exit('not found: ' + want)
    else:
        out_root = sys.argv[3]
        done_bytes = 0
        total = sum(e[2] for e in entries)
        for i, (path, start, length) in enumerate(entries):
            if path.startswith('$SystemUpdate/'):
                continue  # the console's system update, not part of the game
            dst = os.path.join(out_root, *path.split('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if not (os.path.exists(dst) and os.path.getsize(dst) == length):
                g.copy(start, length, dst + '.part')
                os.replace(dst + '.part', dst)
            done_bytes += length
            if i % 200 == 0:
                print(f'  {i}/{len(entries)} files, {done_bytes / 1e9:.2f} / {total / 1e9:.2f} GB', flush=True)
        print(f'{len(entries)} files, {total / 1e9:.2f} GB -> {out_root}')


if __name__ == '__main__':
    main()
