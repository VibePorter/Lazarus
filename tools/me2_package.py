"""Mass Effect 2 (Xbox 360) cooked packages (.xxx): decompress, list names / exports / imports, dump an export.

  python tools/me2_package.py decompress <in.xxx> <out.upk>
  python tools/me2_package.py names <pkg.upk> [regex]
  python tools/me2_package.py exports <pkg.upk> [regex]          (index, class, full name, offset, size)
  python tools/me2_package.py imports <pkg.upk> [regex]
  python tools/me2_package.py dump <pkg.upk> <export index> <out.bin>

The package is UE3 (file version 512, licensee 130), big-endian. Its summary (ME2 adds five words after the cooker
version: 0, a CRC-like word, 0x0002001E, -1, -1) is followed by the compression flags (2 = LZO) and the chunk table
(uncompressed offset, uncompressed size, compressed offset, compressed size); each chunk is UE3's compressed chunk
(tag 0x9E2A83C1, block size, total compressed / uncompressed sizes, then per block compressed / uncompressed sizes and
the LZO1X data). The first chunk starts after the summary, which is copied as it is.
"""
import re
import struct
import sys

TAG = 0x9E2A83C1


def lzo1x_decompress(src, out_len):
    """LZO1X decompression, a direct port of minilzo's lzo1x_decompress (its gotos as flags)."""
    out = bytearray()
    ip = 0

    def ext(ip, base):
        t = base
        while src[ip] == 0:
            t += 255
            ip += 1
        return t + src[ip], ip + 1

    def copy_back(dist_from_end, count):
        m = len(out) - dist_from_end
        for _ in range(count):
            out.append(out[m])
            m += 1

    t = 0
    goto = 'outer'
    if src[0] > 17:
        t = src[0] - 17
        ip = 1
        if t < 4:
            goto = 'match_next'
        else:
            out += src[ip:ip + t]
            ip += t
            goto = 'first_literal_run'
    while True:
        if goto == 'outer':
            t = src[ip]
            ip += 1
            if t >= 16:
                goto = 'match'
            else:
                if t == 0:
                    t, ip = ext(ip, 15)
                out += src[ip:ip + t + 3]
                ip += t + 3
                goto = 'first_literal_run'
        if goto == 'first_literal_run':
            t = src[ip]
            ip += 1
            if t >= 16:
                goto = 'match'
            else:
                dist = 1 + 0x0800 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_back(dist, 3)
                goto = 'match_done'
        if goto == 'match':
            if t >= 64:
                dist = 1 + ((t >> 2) & 7) + (src[ip] << 3)
                ip += 1
                t = (t >> 5) - 1
                copy_back(dist, t + 2)
            elif t >= 32:
                t &= 31
                if t == 0:
                    t, ip = ext(ip, 31)
                dist = 1 + ((src[ip] | (src[ip + 1] << 8)) >> 2)
                ip += 2
                copy_back(dist, t + 2)
            elif t >= 16:
                dist = (t & 8) << 11
                t &= 7
                if t == 0:
                    t, ip = ext(ip, 7)
                dist += (src[ip] | (src[ip + 1] << 8)) >> 2
                ip += 2
                if dist == 0:
                    break  # end of stream
                copy_back(dist + 0x4000, t + 2)
            else:
                dist = 1 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_back(dist, 2)
            goto = 'match_done'
        if goto == 'match_done':
            t = src[ip - 2] & 3
            if t == 0:
                goto = 'outer'
                continue
            goto = 'match_next'
        if goto == 'match_next':
            out += src[ip:ip + t]
            ip += t
            t = src[ip]
            ip += 1
            goto = 'match'
    if len(out) != out_len:
        raise ValueError(f'LZO: {len(out)} bytes out, expected {out_len}')
    return bytes(out)


class Reader:
    def __init__(self, d, o=0):
        self.d, self.o = d, o

    def u32(self):
        v = struct.unpack_from('>I', self.d, self.o)[0]
        self.o += 4
        return v

    def i32(self):
        v = struct.unpack_from('>i', self.d, self.o)[0]
        self.o += 4
        return v

    def u64(self):
        v = struct.unpack_from('>Q', self.d, self.o)[0]
        self.o += 8
        return v

    def fstring(self):
        n = self.i32()
        if n >= 0:
            s = self.d[self.o:self.o + n].rstrip(b'\0').decode('latin1')
            self.o += n
        else:
            s = self.d[self.o:self.o - 2 * n].decode('utf-16-be').rstrip('\0')
            self.o += -2 * n
        return s


def summary(d):
    r = Reader(d)
    s = {}
    assert r.u32() == TAG
    s['version'] = r.u32()
    s['header_size'] = r.u32()
    s['folder'] = r.fstring()
    s['flags'] = r.u32()
    for k in ('name_count', 'name_offset', 'export_count', 'export_offset', 'import_count', 'import_offset',
              'depends_offset'):
        s[k] = r.u32()
    r.o += 16  # guid
    gens = r.u32()
    r.o += 12 * gens
    s['engine'], s['cooker'] = r.u32(), r.u32()
    # ME2 adds fields after the cooker version (0, a CRC-like word, then 2-byte-aligned values and -1s): the compression
    # flags are found as (flags, chunk count, first chunk's uncompressed offset = the name table's offset).
    for k in range(r.o, r.o + 64, 2):
        f, c, first = struct.unpack_from('>III', d, k)
        if (f in (1, 2, 4) and 0 < c < 4096 and first == s['name_offset']) or (f == 0 and c == 0):
            r.o = k
            break
    else:
        raise ValueError('compression flags not found after the cooker version')
    s['compression'] = r.u32()
    chunks = []
    for _ in range(r.u32()):
        chunks.append((r.u32(), r.u32(), r.u32(), r.u32()))
    s['chunks'] = chunks
    s['summary_end'] = r.o
    return s


def decompress(d):
    s = summary(d)
    if not s['compression']:
        return d
    if s['compression'] != 2:
        raise ValueError(f'compression {s["compression"]} (only LZO, 2)')
    total = max(uo + us for uo, us, _, _ in s['chunks'])
    out = bytearray(total)
    first = min(uo for uo, _, _, _ in s['chunks'])
    out[:first] = d[:first]
    for i, (uo, us, co, cs) in enumerate(s['chunks']):
        r = Reader(d, co)
        assert r.u32() == TAG, f'chunk {i}: no tag'
        block, csum, usum = r.u32(), r.u32(), r.u32()
        blocks = []
        left = usum
        while left > 0:
            bc, bu = r.u32(), r.u32()
            blocks.append((bc, bu))
            left -= bu
        o = r.o
        pos = uo
        for bc, bu in blocks:
            data = d[o:o + bc]
            o += bc
            plain = lzo1x_decompress(data, bu) if bc != bu else data
            out[pos:pos + bu] = plain
            pos += bu
        print(f'chunk {i + 1}/{len(s["chunks"])}: {us} bytes', file=sys.stderr)
    # The decompressed package says it is not compressed (flags word) and has no chunk table: left as it is, readers
    # here go by the table offsets, which are in the uncompressed layout.
    return bytes(out)


class Package:
    def __init__(self, d):
        self.d = d
        s = self.s = summary(d)
        r = Reader(d, s['name_offset'])
        self.names = []
        for _ in range(s['name_count']):
            self.names.append(r.fstring())
            r.o += 4  # flags (ME2: one word)
        r = Reader(d, s['import_offset'])
        self.imports = []
        for _ in range(s['import_count']):
            cls_pkg, cls_name, outer, name = self.name(r), self.name(r), r.i32(), self.name(r)
            self.imports.append({'class_package': cls_pkg, 'class': cls_name, 'outer': outer, 'name': name})
        r = Reader(d, s['export_offset'])
        self.exports = []
        for _ in range(s['export_count']):
            e = {'class': r.i32(), 'super': r.i32(), 'outer': r.i32(), 'name': self.name(r), 'archetype': r.i32()}
            e['flags'] = r.u64()
            e['size'], e['offset'] = r.u32(), r.u32()
            r.o += 4  # component map count is next in UE3; ME2: count then pairs
            # UE3 (v512): ComponentMap (count + (name, index) pairs), ExportFlags, NetObjectCount + counts, GUID,
            # PackageFlags. Undo the guess above and read them.
            r.o -= 4
            cm = r.u32()
            r.o += 12 * cm
            e['export_flags'] = r.u32()
            nets = r.u32()
            r.o += 4 * nets
            r.o += 16 + 4  # guid, package flags
            self.exports.append(e)

    def name(self, r):
        i, n = r.u32(), r.u32()
        s = self.names[i] if i < len(self.names) else f'?{i}'
        return s if n == 0 else f'{s}_{n - 1}'

    def obj_name(self, ref):
        if ref > 0:
            return self.exports[ref - 1]['name']
        if ref < 0:
            return self.imports[-ref - 1]['name']
        return 'None'

    def full_name(self, ref):
        parts = []
        while ref:
            parts.append(self.obj_name(ref))
            ref = self.exports[ref - 1]['outer'] if ref > 0 else self.imports[-ref - 1]['outer']
        return '.'.join(reversed(parts))

    def class_name(self, e):
        return self.obj_name(e['class']) if e['class'] else 'Class'


def main():
    cmd = sys.argv[1]
    if cmd == 'decompress':
        d = open(sys.argv[2], 'rb').read()
        open(sys.argv[3], 'wb').write(decompress(d))
        return
    p = Package(open(sys.argv[2], 'rb').read())
    pat = re.compile(sys.argv[3]) if len(sys.argv) > 3 and cmd != 'dump' else None
    if cmd == 'names':
        for i, n in enumerate(p.names):
            if not pat or pat.search(n):
                print(i, n)
    elif cmd == 'imports':
        for i, im in enumerate(p.imports):
            full = p.full_name(-(i + 1))
            if not pat or pat.search(full):
                print(-(i + 1), im['class'], full)
    elif cmd == 'exports':
        for i, e in enumerate(p.exports):
            full = p.full_name(i + 1)
            if not pat or pat.search(full):
                print(i + 1, p.class_name(e), full, hex(e['offset']), e['size'])
    elif cmd == 'dump':
        e = p.exports[int(sys.argv[3]) - 1]
        open(sys.argv[4], 'wb').write(p.d[e['offset']:e['offset'] + e['size']])


if __name__ == '__main__':
    main()
