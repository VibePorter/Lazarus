"""Mass Effect 2 Legendary Edition (.pcc) packages: Oodle decompression (game's own oo2core DLL), name / import /
export tables, little-endian.

  python -I le_package.py exports <pkg.pcc> [class regex]
"""
import ctypes
import os
import re
import struct
import sys

TAG = 0x9E2A83C1
OODLE_DLL = os.environ.get('OODLE_DLL') or os.path.join(os.environ.get('ME2_LE_DIR', ''), 'Binaries', 'Win64', 'oo2core_8_win64.dll')  # the LE install's own
_oodle = None


def oodle_decompress(src, out_len):
    global _oodle
    if _oodle is None:
        _oodle = ctypes.WinDLL(OODLE_DLL)
        f = _oodle.OodleLZ_Decompress
        f.restype = ctypes.c_ssize_t
        f.argtypes = [ctypes.c_char_p, ctypes.c_ssize_t, ctypes.c_char_p, ctypes.c_ssize_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_void_p,
                      ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_int]
    out = ctypes.create_string_buffer(out_len)
    n = _oodle.OodleLZ_Decompress(bytes(src), len(src), out, out_len, 1, 0, 0, None, 0, None, None, None, 0, 3)
    if n != out_len:
        raise ValueError('oodle: %d != %d' % (n, out_len))
    return out.raw


class R:
    def __init__(self, d, o=0):
        self.d, self.o = d, o

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.o)[0]
        self.o += 4
        return v

    def i32(self):
        v = struct.unpack_from('<i', self.d, self.o)[0]
        self.o += 4
        return v

    def fstring(self):
        n = self.i32()
        if n >= 0:
            s = self.d[self.o:self.o + n].rstrip(b'\0').decode('latin1')
            self.o += n
        else:
            s = self.d[self.o:self.o - 2 * n].decode('utf-16-le').rstrip('\0')
            self.o += -2 * n
        return s


def summary(d):
    r = R(d)
    assert r.u32() == TAG
    s = {'version': r.u32(), 'header_size': r.u32(), 'folder': r.fstring(), 'flags': r.u32()}
    for k in ('name_count', 'name_offset', 'export_count', 'export_offset', 'import_count', 'import_offset',
              'depends_offset'):
        s[k] = r.u32()
    # compression flags, chunk count, first chunk's uncompressed offset (= name table offset)
    for k in range(r.o, r.o + 128):
        f, c, first = struct.unpack_from('<III', d, k)
        if (f in (1, 2, 4, 0x400) and 0 < c < 4096 and first == s['name_offset']) or (f == 0 and c == 0 and k > r.o + 40):
            r.o = k
            break
    else:
        raise ValueError('compression flags not found')
    s['compression'] = r.u32()
    s['chunks'] = [(r.u32(), r.u32(), r.u32(), r.u32()) for _ in range(r.u32())]
    s['summary_end'] = r.o
    return s


def decompress(d):
    s = summary(d)
    if not s['chunks']:
        return d
    total = max(uo + us for uo, us, _, _ in s['chunks'])
    out = bytearray(total)
    first = min(uo for uo, _, _, _ in s['chunks'])
    out[:first] = d[:first]
    for uo, us, co, cs in s['chunks']:
        r = R(d, co)
        assert r.u32() == TAG
        block, csum, usum = r.u32(), r.u32(), r.u32()
        blocks = []
        left = usum
        while left > 0:
            bc, bu = r.u32(), r.u32()
            blocks.append((bc, bu))
            left -= bu
        o, pos = r.o, uo
        for bc, bu in blocks:
            data = d[o:o + bc]
            o += bc
            out[pos:pos + bu] = oodle_decompress(data, bu) if bc != bu else data
            pos += bu
    return bytes(out)


class Package:
    def __init__(self, raw):
        d = self.d = decompress(raw)
        s = self.s = summary(raw)
        r = R(d, s['name_offset'])
        self.names = []
        for _ in range(s['name_count']):
            self.names.append(r.fstring())
        r = R(d, s['import_offset'])
        self.imports = []
        for _ in range(s['import_count']):
            cp, cn, outer, nm = self.name(r), self.name(r), r.i32(), self.name(r)
            self.imports.append({'class_package': cp, 'class': cn, 'outer': outer, 'name': nm})
        r = R(d, s['export_offset'])
        self.exports = []
        for _ in range(s['export_count']):
            e = {'class': r.i32(), 'super': r.i32(), 'outer': r.i32(), 'name': self.name(r), 'archetype': r.i32()}
            e['flags'] = struct.unpack_from('<Q', d, r.o)[0]
            r.o += 8
            e['size'], e['offset'] = r.u32(), r.u32()
            # LE2: no component map (the 360 ME2 table has one)
            e['export_flags'] = r.u32()
            nets = r.u32()
            r.o += 4 * nets
            r.o += 16 + 4
            self.exports.append(e)

    def name(self, r):
        i, n = r.u32(), r.u32()
        s = self.names[i] if i < len(self.names) else '?%d' % i
        return s if n == 0 else '%s_%d' % (s, n - 1)

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

    def data(self, e):
        return self.d[e['offset']:e['offset'] + e['size']]


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    p = Package(open(path, 'rb').read())
    pat = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
    if cmd == 'exports':
        for i, e in enumerate(p.exports):
            c = p.class_name(e)
            if not pat or pat.search(c):
                print(i + 1, c, p.full_name(i + 1), hex(e['offset']), e['size'])
    elif cmd == 'names':
        for i, n in enumerate(p.names):
            print(i, n)


if __name__ == '__main__':
    main()
