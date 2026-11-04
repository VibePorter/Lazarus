"""Minimal SWF / GFx AVM1 disassembler (ActionScript 2), offsets relative to the movie start.

  python avm1dis.py movie.gfx [--tags] [--grep REGEX] [--from HEX --to HEX] [--tag N]

Prints DoAction / DoInitAction tags (also inside DefineSprite) as a listing:
  <offset> <action> <operands>
Constant pool references are resolved inline ("c12='label'").
"""
import re
import struct
import sys

ACTIONS = {
    0x00: 'End', 0x04: 'NextFrame', 0x05: 'PrevFrame', 0x06: 'Play', 0x07: 'Stop', 0x08: 'ToggleQuality',
    0x09: 'StopSounds', 0x0A: 'Add', 0x0B: 'Subtract', 0x0C: 'Multiply', 0x0D: 'Divide', 0x0E: 'Equals',
    0x0F: 'Less', 0x10: 'And', 0x11: 'Or', 0x12: 'Not', 0x13: 'StringEquals', 0x14: 'StringLength',
    0x15: 'StringExtract', 0x17: 'Pop', 0x18: 'ToInteger', 0x1C: 'GetVariable', 0x1D: 'SetVariable',
    0x20: 'SetTarget2', 0x21: 'StringAdd', 0x22: 'GetProperty', 0x23: 'SetProperty', 0x24: 'CloneSprite',
    0x25: 'RemoveSprite', 0x26: 'Trace', 0x27: 'StartDrag', 0x28: 'EndDrag', 0x29: 'StringLess', 0x2A: 'Throw',
    0x2B: 'CastOp', 0x2C: 'ImplementsOp', 0x30: 'RandomNumber', 0x31: 'MBStringLength', 0x32: 'CharToAscii',
    0x33: 'AsciiToChar', 0x34: 'GetTime', 0x35: 'MBStringExtract', 0x36: 'MBCharToAscii', 0x37: 'MBAsciiToChar',
    0x3A: 'Delete', 0x3B: 'Delete2', 0x3C: 'DefineLocal', 0x3D: 'CallFunction', 0x3E: 'Return', 0x3F: 'Modulo',
    0x40: 'NewObject', 0x41: 'DefineLocal2', 0x42: 'InitArray', 0x43: 'InitObject', 0x44: 'TypeOf',
    0x45: 'TargetPath', 0x46: 'Enumerate', 0x47: 'Add2', 0x48: 'Less2', 0x49: 'Equals2', 0x4A: 'ToNumber',
    0x4B: 'ToString', 0x4C: 'PushDuplicate', 0x4D: 'StackSwap', 0x4E: 'GetMember', 0x4F: 'SetMember',
    0x50: 'Increment', 0x51: 'Decrement', 0x52: 'CallMethod', 0x53: 'NewMethod', 0x54: 'InstanceOf',
    0x55: 'Enumerate2', 0x60: 'BitAnd', 0x61: 'BitOr', 0x62: 'BitXor', 0x63: 'BitLShift', 0x64: 'BitRShift',
    0x65: 'BitURShift', 0x66: 'StrictEquals', 0x67: 'Greater', 0x68: 'StringGreater', 0x69: 'Extends',
    0x81: 'GotoFrame', 0x83: 'GetURL', 0x87: 'StoreRegister', 0x88: 'ConstantPool', 0x8A: 'WaitForFrame',
    0x8B: 'SetTarget', 0x8C: 'GoToLabel', 0x8D: 'WaitForFrame2', 0x8E: 'DefineFunction2', 0x8F: 'Try',
    0x94: 'With', 0x96: 'Push', 0x99: 'Jump', 0x9A: 'GetURL2', 0x9B: 'DefineFunction', 0x9D: 'If', 0x9E: 'Call',
    0x9F: 'GotoFrame2',
}


def cstr(b, o):
    e = b.index(b'\0', o)
    return b[o:e].decode('latin-1'), e + 1


def disasm(b, start, end, out, pool):
    o = start
    while o < end:
        a = b[o]
        name = ACTIONS.get(a, 'Op%02X' % a)
        if a < 0x80:
            out.append((o, name, ''))
            o += 1
            continue
        ln = struct.unpack_from('<H', b, o + 1)[0]
        p = o + 3
        q = p + ln
        arg = ''
        if a == 0x88:
            n = struct.unpack_from('<H', b, p)[0]
            p += 2
            pool[:] = []
            for _ in range(n):
                s, p = cstr(b, p)
                pool.append(s)
            arg = '%d entries' % n
        elif a == 0x96:
            vals = []
            while p < q:
                t = b[p]
                p += 1
                if t == 0:
                    s, p = cstr(b, p)
                    vals.append(repr(s))
                elif t == 1:
                    vals.append('f%g' % struct.unpack_from('<f', b, p)[0]); p += 4
                elif t == 2:
                    vals.append('null')
                elif t == 3:
                    vals.append('undef')
                elif t == 4:
                    vals.append('r%d' % b[p]); p += 1
                elif t == 5:
                    vals.append('true' if b[p] else 'false'); p += 1
                elif t == 6:
                    hi, lo = struct.unpack_from('<II', b, p)
                    vals.append('d%g' % struct.unpack('<d', struct.pack('<II', lo, hi))[0]); p += 8
                elif t == 7:
                    vals.append('i%d' % struct.unpack_from('<i', b, p)[0]); p += 4
                elif t == 8:
                    i = b[p]; p += 1
                    vals.append('c%d=%r' % (i, pool[i] if i < len(pool) else '?'))
                elif t == 9:
                    i = struct.unpack_from('<H', b, p)[0]; p += 2
                    vals.append('c%d=%r' % (i, pool[i] if i < len(pool) else '?'))
                else:
                    vals.append('?type%d' % t)
                    break
            arg = ', '.join(vals)
        elif a in (0x99, 0x9D):
            off = struct.unpack_from('<h', b, p)[0]
            arg = '-> %06X' % (q + off)
        elif a == 0x9B:
            fname, p2 = cstr(b, p)
            n = struct.unpack_from('<H', b, p2)[0]
            p2 += 2
            params = []
            for _ in range(n):
                s, p2 = cstr(b, p2)
                params.append(s)
            code = struct.unpack_from('<H', b, p2)[0]
            arg = '%s(%s) body %06X..%06X' % (fname, ', '.join(params), q, q + code)
        elif a == 0x8E:
            fname, p2 = cstr(b, p)
            n, regs, flags = struct.unpack_from('<HBH', b, p2)
            p2 += 5
            params = []
            for _ in range(n):
                r = b[p2]; p2 += 1
                s, p2 = cstr(b, p2)
                params.append('%s@r%d' % (s, r))
            code = struct.unpack_from('<H', b, p2)[0]
            arg = '%s(%s) regs=%d flags=%04X body %06X..%06X' % (fname, ', '.join(params), regs, flags, q, q + code)
        elif a == 0x87:
            arg = 'r%d' % b[p]
        elif a == 0x83:
            u, p2 = cstr(b, p)
            t, _ = cstr(b, p2)
            arg = '%r %r' % (u, t)
        else:
            arg = b[p:q].hex()
        out.append((o, name, arg))
        o = q


def tags(b, o, end, depth, found):
    while o < end:
        cl = struct.unpack_from('<H', b, o)[0]
        code, ln = cl >> 6, cl & 0x3F
        o += 2
        if ln == 0x3F:
            ln = struct.unpack_from('<I', b, o)[0]
            o += 4
        body = o
        found.append((depth, code, body, ln))
        if code == 39:  # DefineSprite
            tags(b, body + 4, body + ln, depth + 1, found)
        o = body + ln
        if code == 0 and depth > 0:
            break


def main():
    path = sys.argv[1]
    b = open(path, 'rb').read()
    if b[:3] in (b'CWS', b'CFX'):
        import zlib
        b = b[:8] + zlib.decompress(b[8:])
    nbits = b[8] >> 3
    rect_bytes = (5 + 4 * nbits + 7) // 8
    o = 8 + rect_bytes + 4
    found = []
    tags(b, o, len(b), 0, found)
    if '--tags' in sys.argv:
        for depth, code, body, ln in found:
            print('  ' * depth + 'tag %d at %06X len %d' % (code, body, ln))
        return
    rng = None
    if '--from' in sys.argv:
        rng = (int(sys.argv[sys.argv.index('--from') + 1], 16), int(sys.argv[sys.argv.index('--to') + 1], 16))
    only = int(sys.argv[sys.argv.index('--tag') + 1], 16) if '--tag' in sys.argv else None
    grep = re.compile(sys.argv[sys.argv.index('--grep') + 1]) if '--grep' in sys.argv else None
    for depth, code, body, ln in found:
        if code not in (12, 59):
            continue
        if only is not None and body != only:
            continue
        start = body + (2 if code == 59 else 0)
        if rng and (start + ln < rng[0] or start > rng[1]):
            continue
        out = []
        disasm(b, start, body + ln, out, [])
        if not grep or any(grep.search(n + ' ' + a) for _, n, a in out):
            print('== %s tag at %06X len %d' % ('DoInitAction' if code == 59 else 'DoAction', body, ln))
            for off, n, a in out:
                if rng and not (rng[0] <= off <= rng[1]):
                    continue
                print('%06X %-16s %s' % (off, n, a))


if __name__ == '__main__':
    main()
