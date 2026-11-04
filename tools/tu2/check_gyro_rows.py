"""Offline check of the Options rows (me2_menu.cpp PatchOptions + me2_gyro_menu.cpp BuildBody: gyro rows on the
Controller page, Subtitle Size on the Gameplay page) and of the Conversation movie's htmlText patch
(me2_subtitles.cpp Me2PatchSubtitleHtml), mirrored in Python. Patches extracted movies and disassembles the touched
places.

  python check_gyro_rows.py options.gfx out.gfx [gyro_texts.txt] [--conversation conv.gfx conv_out.gfx]
"""
import struct
import subprocess
import sys

here = __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0]
args = sys.argv[1:]
conv = None
if '--conversation' in args:
    i = args.index('--conversation')
    conv = args[i + 1:i + 3]
    del args[i:i + 3]


def dis(path, lo, hi):
    subprocess.run([sys.executable, here + '/avm1dis.py', path, '--from', '%06X' % lo, '--to', '%06X' % hi])


def jump(at, to):
    off = to - (at + 5)
    return bytes([0x99, 0x02, 0x00]) + struct.pack('<h', off)


# ---- Options movie ----
m = bytearray(open(args[0], 'rb').read())
kOnline, kOnlineEnd, kCave, kLabel, kLabelBack = 0x17168, 0x17274, 0x17174, 0x1743D, 0x17447


def put(at, was, now):
    assert was is None or m[at:at + len(was)] == bytes(was), hex(at)
    m[at:at + len(now)] = now


put(kOnline, bytes([0x96, 0x0A, 0x00, 0x04, 0x01, 0x08, 0x53, 0x08, 0x61, 0x04, 0x01, 0x08, 0x8E, 0x4E, 0x96, 0x06]),
    bytes([0x96, 0x04, 0x00, 0x04, 0x01, 0x08, 0x53]) + jump(kOnline + 7, kOnlineEnd))
put(0x179A8, bytes([0x96, 0x05, 0x00, 0x07, 0x05, 0, 0, 0, 0x42]), bytes([0x96, 0x05, 0x00, 0x07, 0x04, 0, 0, 0, 0x42]))
label = 'Контроллер'.encode()  # the longest label in use (RU)
cave = bytes([0x96, len(label) + 4, 0x00, 0x08, 0x61, 0x00]) + label + b'\0'
cave += jump(kCave + len(cave), kLabelBack)
put(kLabel, bytes([0x96, 0x06, 0x00, 0x08, 0x61, 0x04, 0x01, 0x08, 0xD2, 0x4E]), jump(kLabel, kCave) + bytes([0x17] * 5))
put(kCave, None, cave)
dead = kCave + len(cave)
kGyroCave, kRowsSite, kRowsBack = kOnlineEnd - 28, 0x176C1, 0x176C9
kSubCave, kGameplaySite, kGameplayBack = kGyroCave - 33, 0x1799F, 0x179A7
kPcFlags, kPcBody, kPcBodyEnd = 0x16631, 0x16635, 0x170B4
assert dead <= kSubCave
push4 = bytes([0x96, 0x05, 0x00, 0x07, 0x04, 0x00, 0x00, 0x00])
put(dead, None, bytes([0x17] * (kSubCave - dead)))
sub = bytes([0x4C, 0x96, 0x0E, 0x00, 0x07, 1, 0, 0, 0, 0x07, 2, 0, 0, 0, 0x04, 0x01, 0x08, 0x5F, 0x52, 0x17]) + push4
sub += jump(kSubCave + len(sub), kGameplayBack)
assert len(sub) == 33
put(kSubCave, None, sub)
put(kGameplaySite, push4, jump(kGameplaySite, kSubCave) + bytes([0x17] * 3))
snippet = bytes([0x4C, 0x96, 0x09, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00, 0x04, 0x01, 0x08, 0x5F,
                 0x52, 0x17]) + push4
snippet += jump(kGyroCave + len(snippet), kRowsBack)
assert len(snippet) == 28
put(kGyroCave, None, snippet)
put(kRowsSite, push4, jump(kRowsSite, kGyroCave) + bytes([0x17] * 3))
put(kPcFlags, bytes([0x29, 0x01]), bytes([0x25, 0x01]))


class Avm:
    def __init__(self):
        self.code, self.p = bytearray(), bytearray()

    def flush(self):
        if self.p:
            self.code += bytes([0x96]) + struct.pack('<H', len(self.p)) + self.p
            self.p = bytearray()

    def C(self, i): self.p += bytes([8, i]); return self
    def I(self, v): self.p += bytes([7]) + struct.pack('<i', v); return self
    def R(self, r): self.p += bytes([4, r]); return self
    def S(self, s): self.p += b'\0' + s.encode() + b'\0'; return self
    def U(self): self.p += b'\x03'; return self

    def store(self, r):
        self.flush()
        self.code += bytes([0x87, 0x01, 0x00, r])

    def op(self, o):
        self.flush()
        self.code.append(o)

    def if_(self, skip):
        self.flush()
        self.code += bytes([0x9D, 0x02, 0x00]) + struct.pack('<h', skip)


def push_to_arg(a, splice_at=-1):
    if splice_at < 0:
        a.I(1).R(2).I(0); a.op(0x4E); a.S('push')
    else:
        a.I(0).I(splice_at).I(3).R(2).I(1); a.op(0x4E); a.S('splice')
    a.op(0x52); a.op(0x17)


def slider(a, label, story, id_, value, children, splice_at=-1):
    a.C(97).S(label).C(99).R(story).C(101).C(117).C(103).I(id_).C(104).C(105).R(1).C(value); a.op(0x4E); a.C(107)
    for c in reversed(children):  # InitArray: the last pushed is [0]
        a.C(97).S(c).C(99).R(story).I(2); a.op(0x43)
    a.I(len(children)); a.op(0x42); a.I(2); a.op(0x43); a.I(5); a.op(0x43)
    push_to_arg(a, splice_at)


def radio(a, label, story, id_, value):
    a.C(97).S(label).C(99).R(story).C(101).C(102).C(103).I(id_).C(104).C(105).R(1).C(value); a.op(0x4E); a.C(107)
    a.C(97).R(1).C(108); a.op(0x4E); a.I(1); a.op(0x43)
    a.C(97).R(1).C(109); a.op(0x4E); a.I(1); a.op(0x43)
    a.I(2); a.op(0x42); a.I(2); a.op(0x43); a.I(5); a.op(0x43)
    push_to_arg(a)


texts = ('Gyro Aiming|Aim by moving the console or controller: only while ZL is held, or all the time.|Off|While Aiming '
         '(ZL)|Always|Gyro Sensitivity|How fast the camera turns when you move the console or controller (L+R+X / L+R+Y '
         'also change it while playing).|Invert Gyro Horizontal|Swaps left and right for gyro aiming.|Invert Gyro '
         'Vertical|Swaps up and down for gyro aiming.').split('|')
sub_texts = 'Subtitle Size|Makes the subtitles bigger. Takes effect from the next line.'.split('|')
if len(args) > 2:
    lines = open(args[2], encoding='utf-8').read().strip().split('\n')
    texts = lines[0].split('|')
    if len(lines) > 1:
        sub_texts = lines[1].split('|')
g = Avm()
for reg, idx in ((4, 1), (5, 6), (6, 8), (7, 10)):
    g.S(texts[idx]); g.store(reg); g.op(0x17)
slider(g, texts[0], 4, 90, 181, texts[2:5])
slider(g, texts[5], 5, 91, 177, ['%.1f' % (i * 0.5) for i in range(1, 13)])
radio(g, texts[7], 6, 92, 174)
radio(g, texts[9], 7, 93, 171)
g.U(); g.op(0x3E)
s = Avm()
s.S(sub_texts[1]); s.store(4); s.op(0x17)
slider(s, sub_texts[0], 4, 94, 189, ['%d %%' % p for p in (100, 120, 140, 160, 180, 200)], splice_at=3)
s.U(); s.op(0x3E)
a = Avm()
a.R(2).C(138); a.op(0x4E); a.I(2); a.op(0x49)
a.if_(len(g.code))
a.code += g.code + s.code
size = kPcBodyEnd - kPcBody
print('builder %d bytes of %d' % (len(a.code), size))
assert len(a.code) <= size
m[kPcBody:kPcBodyEnd] = bytes(a.code) + bytes([0x17] * (size - len(a.code)))
open(args[1], 'wb').write(m)
for lo, hi in ((0x1662A, 0x16660), (kPcBody + len(a.code) - len(s.code) - 40, kPcBody + len(a.code) + 2),
               (kSubCave, kOnlineEnd), (0x176B0, 0x176D0), (0x17990, 0x179B2)):
    print('----')
    dis(args[1], lo, hi)

# ---- Conversation movie ----
if conv:
    c = bytearray(open(conv[0], 'rb').read())
    kPool, kEntry = 0x270, 123
    assert c[kPool] == 0x88 and struct.unpack_from('<H', c, kPool + 3)[0] == 140
    pool_end = kPool + 3 + struct.unpack_from('<H', c, kPool + 1)[0]
    p = kPool + 5
    for _ in range(kEntry):
        p = c.index(b'\0', p) + 1
    old_len = c.index(b'\0', p) - p + 1
    assert c[p:p + 16] == b'Nihlus: Obviousl'
    new = b'htmlText\0'
    gap = old_len - len(new)
    c[p + len(new):pool_end - gap] = c[p + old_len:pool_end]
    c[p:p + len(new)] = new
    struct.pack_into('<H', c, kPool + 1, struct.unpack_from('<H', c, kPool + 1)[0] - gap)
    hole = pool_end - gap
    c[hole:hole + 5] = bytes([0x99, 0x02, 0x00]) + struct.pack('<h', gap - 5)
    c[hole + 5:pool_end] = bytes([0x17] * (gap - 5))
    for site in (0x197C, 0x1A01, 0x1AA6):
        assert c[site:site + 7] == bytes([0x96, 0x04, 0x00, 0x08, 71, 0x04, 0x06]), hex(site)
        c[site + 4] = kEntry
    open(conv[1], 'wb').write(c)
    print('==== conversation: gap', gap)
    dis(conv[1], pool_end - gap - 8, pool_end + 4)
    dis(conv[1], 0x1970, 0x1990)
