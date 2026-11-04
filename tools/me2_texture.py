"""ME2 (Xbox 360) Texture2D exports: properties, the top mip, and PNG export (A8R8G8B8 / DXT1 / DXT5, Xenos 2D tiling).

  python tools/me2_texture.py info <pkg.upk> <export index>
  python tools/me2_texture.py png <pkg.upk> <export index> <out.png> [--linear]
  python tools/me2_texture.py pngs <pkg.upk> <regex> <out dir>        (every Texture2D whose full name matches)

Export layout (ME2, Startup_INT GUI_GlobalIcons.Buttons.*): 4-byte net index, tagged properties up to None (bool
values 4 bytes, byte enums as a name), then the source art bulk data header (flags, count, size, offset) and the mips:
count, per mip a bulk data header (flags, element count, size on disk, offset in file), the data when stored inline
(flags bit 0x20 clear), SizeX, SizeY. The pixel data is tiled with the Xenos 2D layout (XGAddress2DTiledOffset) unless
--linear; A8R8G8B8 is stored as bytes A, R, G, B; DXT blocks are 16-bit big-endian words.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import me2_package as mp  # noqa: E402


def tiled_offset_2d(x, y, pitch_blocks, log2_bytes):
    # XGAddress2DTiledOffset in bytes (Renut's texture_layout.cpp TiledOffset2D).
    tiles_per_row = ((pitch_blocks + 31) & ~31) >> 5
    shift = log2_bytes + 6
    tile = (((x >> 5) + (y >> 5) * tiles_per_row) << shift) & 0x1FFFFFFF
    micro = (((x & 7) + ((y << 2) & 0x18)) << shift) >> 6
    a = 2 * (tile + (micro & ~0xF)) + (micro & 0xF) + (((y >> 3) & 1) << shift) + ((y & 1) << 4)
    b = ((2 * ((x >> 3) + ((y >> 2) & 2))) & 6) + ((y >> 4) & 1)
    return ((b & 1) << 11) + (((a >> 6) & 7) << 8) + ((b & ~1) << 5) + ((a & ~0x1FF) << 3) + (a & 0x3F)


class Texture:
    def __init__(self, p, ref):
        self.p, self.ref = p, ref
        e = p.exports[ref - 1]
        b = self.b = p.d[e['offset']:e['offset'] + e['size']]
        self.export_offset = e['offset']
        self.props = {}
        o = 4
        while True:
            nm = self.name(o)
            if nm == 'None':
                o += 8
                break
            typ = self.name(o + 8)
            size = struct.unpack_from('>i', b, o + 16)[0]
            v = b[o + 24:o + 24 + size]
            if typ == 'IntProperty':
                v = struct.unpack('>i', v)[0]
            elif typ == 'ByteProperty' and size == 8:
                v = self.name(o + 24)
            elif typ == 'BoolProperty':
                v = struct.unpack_from('>I', b, o + 24)[0] != 0
                size = 4 if size == 0 else size
            self.props[nm] = v
            o += 24 + size
        o += 16  # source art bulk data header
        count = struct.unpack_from('>I', b, o)[0]
        o += 4
        self.mips = []
        for _ in range(count):
            flags, n, disk, off = struct.unpack_from('>IIII', b, o)
            o += 16
            data_at = o
            if not flags & 0x20 and disk:
                o += disk
            sx, sy = struct.unpack_from('>II', b, o)
            o += 8
            self.mips.append({'flags': flags, 'size': disk, 'data_at': data_at, 'x': sx, 'y': sy,
                              'inline': not flags & 0x20 and disk > 0})

    def name(self, o):
        i, n = struct.unpack_from('>II', self.b, o)
        s = self.p.names[i] if i < len(self.p.names) else f'?{i}'
        return s if n == 0 else f'{s}_{n - 1}'

    def top(self):
        m = self.mips[0]
        return m, self.b[m['data_at']:m['data_at'] + m['size']] if m['inline'] else None

    def rgba(self, linear=False):
        m, data = self.top()
        if data is None:
            raise ValueError('top mip not stored in the export')
        fmt = self.props.get('Format')
        w, h = m['x'], m['y']
        out = bytearray(w * h * 4)
        if fmt == 'PF_A8R8G8B8':
            pitch = max(w, 32)
            for y in range(h):
                for x in range(w):
                    s = (y * w + x) * 4 if linear else tiled_offset_2d(x, y, pitch, 2)
                    a, r, g, bb = data[s:s + 4]
                    d = (y * w + x) * 4
                    out[d:d + 4] = bytes((r, g, bb, a))
            return w, h, bytes(out)
        raise ValueError(f'format {fmt} not handled')


def main():
    cmd = sys.argv[1]
    p = mp.Package(open(sys.argv[2], 'rb').read())
    if cmd == 'info':
        t = Texture(p, int(sys.argv[3]))
        print(p.full_name(int(sys.argv[3])), t.props)
        for m in t.mips:
            print(m)
    elif cmd == 'png':
        from PIL import Image
        t = Texture(p, int(sys.argv[3]))
        w, h, px = t.rgba('--linear' in sys.argv)
        Image.frombytes('RGBA', (w, h), px).save(sys.argv[4])
    elif cmd == 'pngs':
        from PIL import Image
        pat = re.compile(sys.argv[3])
        os.makedirs(sys.argv[4], exist_ok=True)
        for i, e in enumerate(p.exports):
            full = p.full_name(i + 1)
            if p.class_name(e) != 'Texture2D' or not pat.search(full):
                continue
            try:
                t = Texture(p, i + 1)
                w, h, px = t.rgba()
            except Exception as ex:  # noqa: BLE001
                print('skip', full, ex)
                continue
            Image.frombytes('RGBA', (w, h), px).save(os.path.join(sys.argv[4], full + '.png'))
            print(i + 1, full, w, h, t.props.get('Format'))


if __name__ == '__main__':
    main()
