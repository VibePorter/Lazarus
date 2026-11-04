"""Generate load-time vector edits for Conversation's Xbox interrupt labels.

python tools/me2_interrupt_icons.py Startup_INT.upk output.inc
The movie keeps its length, IDs, outlines, colors, timing and ActionScript.
Only the holes forming LT / RT change to ZL / ZR. No raster textures are added.
"""
from pathlib import Path
import struct
import sys

import me2_package as mp


class Bits:
    def __init__(self, data, offset=0):
        self.data, self.bit = data, offset * 8

    def u(self, count):
        value = 0
        for _ in range(count):
            value = value * 2 + ((self.data[self.bit // 8] >> (7 - self.bit % 8)) & 1)
            self.bit += 1
        return value

    def s(self, count):
        value = self.u(count)
        return value - (1 << count) if value & (1 << (count - 1)) else value

    def align(self):
        self.bit = (self.bit + 7) // 8 * 8

    def rect(self):
        count = self.u(5)
        values = [self.s(count) for _ in range(4)]
        self.align()
        return values


class Writer:
    def __init__(self):
        self.bits = []

    def put(self, value, count):
        self.bits.extend((value >> i) & 1 for i in reversed(range(count)))

    def bytes(self):
        self.bits.extend([0] * (-len(self.bits) % 8))
        return bytes(sum(self.bits[i + j] << (7 - j) for j in range(8))
                     for i in range(0, len(self.bits), 8))


def signed_bits(values):
    return max(2, max(v.bit_length() + 1 if v >= 0 else (~v).bit_length() + 1 for v in values))


def read_shape(movie, at):
    header, size = struct.unpack_from('<HI', movie, at)
    assert header >> 6 == 83 and header & 63 == 63
    data = movie[at + 6:at + 6 + size]
    r = Bits(data, 2)
    r.rect()
    r.rect()
    assert r.u(8) == 1  # scaling strokes; even-odd fills
    assert bytes(r.u(8) for _ in range(6)) == b'\x01\x00\xff\x66\x66\xff'
    assert bytes(r.u(8) for _ in range(9)) == b'\x01\x36\x01\x00\x00\xff\x66\x66\xff'
    prefix = data[:r.bit // 8]
    fill_bits, line_bits = r.u(4), r.u(4)
    assert (fill_bits, line_bits) == (1, 1)
    paths, path = [], None
    style = [0, 0, 0]
    x = y = 0
    while True:
        if r.u(1):
            straight, count = r.u(1), r.u(4) + 2
            if straight:
                if r.u(1):
                    dx, dy = r.s(count), r.s(count)
                elif r.u(1):
                    dx, dy = 0, r.s(count)
                else:
                    dx, dy = r.s(count), 0
                path['edges'].append(('L', dx, dy))
                x, y = x + dx, y + dy
            else:
                values = [r.s(count) for _ in range(4)]
                path['edges'].append(('Q', *values))
                x, y = x + values[0] + values[2], y + values[1] + values[3]
        else:
            flags = r.u(5)
            if not flags:
                break
            assert not flags & 16
            if path:
                paths.append(path)
            if flags & 1:
                count = r.u(5)
                x, y = r.s(count), r.s(count)
            for bit, i in ((2, 0), (4, 1), (8, 2)):
                if flags & bit:
                    style[i] = r.u(fill_bits if i < 2 else line_bits)
            path = dict(start=(x, y), style=tuple(style), edges=[])
    if path:
        paths.append(path)
    return data, prefix, paths


def write_shape(prefix, paths):
    w = Writer()
    w.put(1, 4)
    w.put(1, 4)
    style = (0, 0, 0)
    for path in paths:
        flags = 1
        for bit, i in ((2, 0), (4, 1), (8, 2)):
            if style[i] != path['style'][i]:
                flags |= bit
        w.put(0, 1)
        w.put(flags, 5)
        count = signed_bits(path['start'])
        w.put(count, 5)
        for v in path['start']:
            w.put(v, count)
        for bit, i in ((2, 0), (4, 1), (8, 2)):
            if flags & bit:
                w.put(path['style'][i], 1)
        style = path['style']
        for edge in path['edges']:
            straight = edge[0] == 'L'
            count = signed_bits(edge[1:])
            assert count <= 17
            w.put(1, 1)
            w.put(straight, 1)
            w.put(count - 2, 4)
            if straight:
                dx, dy = edge[1:]
                w.put(bool(dx and dy), 1)
                if dx and dy:
                    w.put(dx, count)
                    w.put(dy, count)
                else:
                    w.put(bool(dy), 1)
                    w.put(dy if dy else dx, count)
            else:
                for v in edge[1:]:
                    w.put(v, count)
    w.put(0, 6)  # EndShapeRecord. Remaining tag bytes are zero padding.
    return prefix + w.bytes()


def polygon(points):
    return dict(start=points[0], style=(0, 1, 0),
                edges=[('L', b[0] - a[0], b[1] - a[1]) for a, b in zip(points, points[1:] + points[:1])])


def z_hole(left, right):
    # Same 596-twip cap height and stroke weight as the movie's original lettering.
    top, bottom, stroke = -181, 415, 100
    return polygon([(left, top), (left, top + stroke), (right - 135, top + stroke),
                    (left, bottom), (right, bottom), (right, bottom - stroke),
                    (left + 135, bottom - stroke), (right, top)])


def shifted(path, dx):
    return dict(path, start=(path['start'][0] + dx, path['start'][1]))


def edits(movie):
    assert movie[:8] == b'GFX\x08\xb2\xf2\x00\x00'
    result = []
    for at, key in ((0x7EC7, 'ZR'), (0x8753, 'ZL')):
        old, prefix, paths = read_shape(movie, at)
        if key == 'ZR':
            assert len(paths) == 5
            # Collapse two almost collinear outline edges (<0.12 source pixels deviation)
            # to keep the new shape within the original tag. The timing ring stays exact.
            body = dict(paths[1], edges=paths[1]['edges'][:7] + [('L', -62, 132)] + paths[1]['edges'][9:])
            new_paths = [paths[0], body, shifted(paths[2], 390), shifted(paths[3], 390), z_hole(2615, 2965)]
        else:
            assert len(paths) == 4
            body = dict(paths[1], edges=paths[1]['edges'][:1] + [('L', -61, -132)] + paths[1]['edges'][3:])
            new_paths = [paths[0], body, z_hole(-3342, -2984), shifted(paths[3], 390)]
        new = write_shape(prefix, new_paths)
        assert len(new) <= len(old), (key, len(old), len(new))
        print(key, 'shape bytes', len(old), '->', len(new), '(before padding)')
        result.append((at + 6, old, new.ljust(len(old), b'\0')))
    return result


def conversation(package):
    p = mp.Package(Path(package).read_bytes())
    ref = next(i + 1 for i, e in enumerate(p.exports) if p.full_name(i + 1) == 'GUI_SF_Conversation.Conversation')
    e = p.exports[ref - 1]
    data = p.d[e['offset']:e['offset'] + e['size']]
    at = data.index(b'GFX\x08')
    size = struct.unpack_from('<I', data, at + 4)[0]
    return data[at:at + size]


def generate(movie):
    lines = ['// Generated by tools/me2_interrupt_icons.py; Conversation vector LT/RT -> ZL/ZR.']
    for i, (at, old, new) in enumerate(edits(movie)):
        lines.append(f'constexpr uint32_t kInterruptOffset{i} = 0x{at:X};')
        for label, data in (('Old', old), ('New', new)):
            lines.append(f'constexpr uint8_t kInterrupt{label}{i}[] = {{')
            lines.extend('  ' + ','.join(str(v) for v in data[j:j + 24]) + ',' for j in range(0, len(data), 24))
            lines.append('};')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    Path(sys.argv[2]).write_text(generate(conversation(sys.argv[1])), newline='\n')
