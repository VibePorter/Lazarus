#!/usr/bin/env python3
"""Mass Effect 2 save: PC (.pcsav, little-endian) <-> Xbox 360 (.xbsav, big-endian).

The two are the same UE3 serialisation with the other byte order: every 32-bit value, string length, list count,
UTF-16 character and GUID part swapped, then Gibbed's CRC32 (poly 0x04C11DB7, MSB-first) of the body appended in the
output's order. The field layout is read from Gibbed's ME2 save editor sources (tools/ext/Gibbed.MassEffect2,
projects/Gibbed.MassEffect2.FileFormats: SaveFile.cs + Save/*.cs), so nothing is hand-copied. Byte enums stay one byte.

  python tools/me2_save_convert.py IN OUT [--to xbox|pc]     (default: the other format of IN)

Checks: the whole input is consumed, the input's checksum matches, and converting the output back gives the input
byte for byte.
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GIBBED = os.path.join(HERE, 'ext', 'Gibbed.MassEffect2', 'projects', 'Gibbed.MassEffect2.FileFormats')

PRIMS = {'int': 4, 'uint': 4, 'float': 4, 'bool': 4, 'byte': 1, 'string': 's', 'Guid': 'g', 'BitArray': 'bits'}

# --- schema from the C# sources --------------------------------------------------------------------------------------
def load_schema():
    files = [os.path.join(GIBBED, 'SaveFile.cs'), os.path.join(GIBBED, 'SaveFileBase.cs')]
    files += [os.path.join(GIBBED, 'Save', f) for f in sorted(os.listdir(os.path.join(GIBBED, 'Save')))]
    enums, fields, bodies = {}, {}, {}
    for path in files:
        text = open(path, encoding='utf-8-sig').read()
        for m in re.finditer(r'enum\s+(\w+)\s*:\s*(\w+)', text):
            enums[m.group(1)] = 1 if m.group(2) == 'byte' else 4
        for m in re.finditer(r'enum\s+(\w+)\s*\n', text):
            enums.setdefault(m.group(1), 4)
        cm = re.search(r'(?:class|struct)\s+(\w+)', text)
        if not cm:
            continue
        cls = cm.group(1)
        cls = 'SaveFile' if cls in ('SaveFile', 'SaveFileBase') else cls
        decl = fields.setdefault(cls, {})
        for m in re.finditer(r'\n\s*(?:public|private|internal|protected)\s+([\w<>.]+)\s+(\w+)\s*(?:=[^;]*)?;', text):
            decl[m.group(2)] = re.sub(r'\w+\.', '', m.group(1))  # Save.SaveTimeStamp -> SaveTimeStamp
        bm = re.search(r'void Serialize\(IUnrealStream stream\)\s*\{', text)
        if bm:
            bodies[cls] = parse_body(text, bm.end())
    return enums, fields, bodies


def parse_body(text, start):
    """Statements of a Serialize body: ('f', kind, name) | ('if', cond, [then], [else])."""
    pos = start

    def block():
        nonlocal pos
        out = []
        while True:
            m = re.compile(r'\s*(\}|if\s*\(([^)]*)\)\s*\{|else\s*\{|throw[^;]*;|'
                           r'stream\.(SerializeEnum|Serialize)(?:<\w+>)?\(ref this\.(\w+)\);)').match(text, pos)
            if not m:
                raise SystemExit('schema: cannot parse near: ' + text[pos:pos + 80].replace('\n', ' '))
            pos = m.end()
            tok = m.group(1)
            if tok == '}':
                return out
            if tok.startswith('if'):
                then = block()
                els = None
                m2 = re.compile(r'\s*else\s*\{').match(text, pos)
                if m2:
                    pos = m2.end()
                    els = block()
                out.append(('if', m.group(2).strip(), then, els))
            elif tok.startswith('throw'):
                out.append(('throw',))
            else:
                out.append(('f', m.group(3), m.group(4)))

    return block()


# --- transcoding -----------------------------------------------------------------------------------------------------
class Coder:
    def __init__(self, data, src, dst, version, schema):
        self.d, self.p, self.out = data, 0, bytearray()
        self.s, self.t = src, dst  # '<' or '>'
        self.version = version
        self.enums, self.fields, self.bodies = schema

    def u32(self):
        v = struct.unpack_from(self.s + 'I', self.d, self.p)[0]
        self.out += struct.pack(self.t + 'I', v)
        self.p += 4
        return v

    def raw(self, n):
        self.out += self.d[self.p:self.p + n]
        self.p += n

    def prim(self, kind):
        size = PRIMS[kind]
        if size == 4:
            return self.u32()
        if size == 1:
            v = self.d[self.p]
            self.raw(1)
            return v
        if size == 's':
            n = struct.unpack(self.t + 'i', struct.pack(self.t + 'I', self.u32()))[0]
            if n < 0:
                for _ in range(-n):  # UTF-16 code units
                    c = struct.unpack_from(self.s + 'H', self.d, self.p)[0]
                    self.out += struct.pack(self.t + 'H', c)
                    self.p += 2
            else:
                self.raw(n)
            return None
        if size == 'g':
            a, b, c = struct.unpack_from(self.s + 'IHH', self.d, self.p)
            self.out += struct.pack(self.t + 'IHH', a, b, c)
            self.p += 8
            self.raw(8)
            return None
        if size == 'bits':
            for _ in range(self.u32()):
                self.u32()
            return None
        raise SystemExit('unknown primitive ' + kind)

    def value(self, typ, enum=False):
        if enum:
            size = self.enums.get(typ, 4)
            if size == 1:
                v = self.d[self.p]
                self.raw(1)
                return v
            return self.u32()
        lm = re.fullmatch(r'List<(\w+)>', typ)
        if lm:
            inner = lm.group(1)
            count = self.u32()
            if count >= 0x7FFFFF:
                raise SystemExit('sanity: list count %d at 0x%x' % (count, self.p))
            for _ in range(count):
                self.value(inner)
            return count
        if typ in PRIMS:
            return self.prim(typ)
        if typ in self.enums:
            return self.value(typ, enum=True)
        if typ in self.bodies:
            self.obj(typ)
            return None
        raise SystemExit('unknown type %s' % typ)

    def obj(self, cls):
        vals = {}
        self.run(cls, self.bodies[cls], vals)

    def run(self, cls, stmts, vals):
        for st in stmts:
            if st[0] == 'f':
                _, how, name = st
                typ = self.fields[cls].get(name)
                if typ is None:
                    raise SystemExit('no declaration of %s.%s' % (cls, name))
                vals[name] = self.value(typ, enum=(how == 'SerializeEnum'))
            elif st[0] == 'if':
                _, cond, then, els = st
                vm = re.fullmatch(r'stream\.Version\s*>=\s*(\d+)', cond)
                fm = re.fullmatch(r'this\.(\w+)\s*==\s*true', cond)
                if vm:
                    ok = self.version >= int(vm.group(1))
                elif fm:
                    ok = bool(vals.get(fm.group(1)))
                else:
                    raise SystemExit('unknown condition: ' + cond)
                self.run(cls, then if ok else (els or []), vals)
            elif st[0] == 'throw':
                raise SystemExit('the schema rejects this save version (%d)' % self.version)


def crc32_gibbed(data):
    table = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        table.append(c)
    h = 0xFFFFFFFF
    for b in data:
        h = table[((h >> 24) ^ b) & 0xFF] ^ ((h << 8) & 0xFFFFFFFF)
    return h ^ 0xFFFFFFFF


def detect(data):
    if struct.unpack_from('<I', data)[0] < 0x100:
        return '<'
    if struct.unpack_from('>I', data)[0] < 0x100:
        return '>'
    raise SystemExit('not an ME2 save (version word)')


def convert(data, dst, schema):
    src = detect(data)
    body, stored = data[:-4], struct.unpack_from(src + 'I', data, len(data) - 4)[0]
    if crc32_gibbed(body) != stored:
        raise SystemExit('input checksum mismatch: stored %08X, computed %08X' % (stored, crc32_gibbed(body)))
    version = struct.unpack_from(src + 'I', body)[0]
    c = Coder(body, src, dst, version, schema)
    c.u32()  # version
    c.obj('SaveFile')
    if c.p != len(body):
        raise SystemExit('schema did not consume the save: stopped at 0x%x of 0x%x' % (c.p, len(body)))
    out = bytes(c.out)
    return out + struct.pack(dst + 'I', crc32_gibbed(out)), version


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('output')
    ap.add_argument('--to', choices=['xbox', 'pc'])
    a = ap.parse_args()
    schema = load_schema()
    data = open(a.input, 'rb').read()
    src = detect(data)
    dst = {'xbox': '>', 'pc': '<', None: '>' if src == '<' else '<'}[a.to]
    out, version = convert(data, dst, schema)
    back, _ = convert(out, src, schema)
    if back != data:
        raise SystemExit('round trip differs from the input')
    with open(a.output, 'wb') as f:
        f.write(out)
    print('%s (%s, version %d, %d bytes) -> %s (%s, %d bytes); round trip identical' % (
        a.input, 'PC' if src == '<' else 'Xbox 360', version, len(data), a.output,
        'PC' if dst == '<' else 'Xbox 360', len(out)))


if __name__ == '__main__':
    main()
