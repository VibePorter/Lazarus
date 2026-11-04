"""A small UnrealScript bytecode disassembler for ME2 (Xbox 360) packages decompressed by tools/me2_package.py.

  python tools/me2_ue3_disasm.py <pkg.upk> <export index | Class.Function>

UE3 tokens (EExprToken), ME2's sizes as seen in SFXGame: object references 4 bytes (package index), names 8 bytes
(index, number), jump offsets and skip sizes 2 bytes, all big-endian. A function's code starts 44 bytes into its export
(the UField / UStruct header: 11 words, the last the code's in-memory size) and ends at EX_EndOfScript (0x53).
Unknown tokens stop the listing (printed with their offset).
"""
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
import me2_package as mp  # noqa: E402

NATIVE_NAMES = {}  # optional: index -> name for native operators (not needed to follow the logic)


class Dis:
    def __init__(self, p, code):
        self.p, self.c, self.o = p, code, 0
        self.out = []

    def u8(self):
        v = self.c[self.o]
        self.o += 1
        return v

    def u16(self):
        v = struct.unpack_from('>H', self.c, self.o)[0]
        self.o += 2
        return v

    def i32(self):
        v = struct.unpack_from('>i', self.c, self.o)[0]
        self.o += 4
        return v

    def f32(self):
        v = struct.unpack_from('>f', self.c, self.o)[0]
        self.o += 4
        return v

    def obj(self):
        ref = self.i32()
        return self.p.full_name(ref).split('.')[-1] if ref else 'None'

    def objfull(self):
        ref = self.i32()
        return self.p.full_name(ref) if ref else 'None'

    def name(self):
        i, n = self.i32(), self.i32()
        s = self.p.names[i] if 0 <= i < len(self.p.names) else f'?name{i}'
        return s if n == 0 else f'{s}_{n - 1}'

    def cstr(self):
        e = self.c.index(b'\0', self.o)
        s = self.c[self.o:e].decode('latin1')
        self.o = e + 1
        return s

    def wstr(self):
        e = self.o
        while self.c[e:e + 2] != b'\0\0':
            e += 2
        s = self.c[self.o:e].decode('utf-16-be')
        self.o = e + 2
        return s

    def params(self):
        args = []
        while self.c[self.o] != 0x16:
            args.append(self.expr())
        self.o += 1
        return ', '.join(args)

    def expr(self):
        start = self.o
        t = self.u8()
        if t == 0x00:
            return self.obj()
        if t == 0x01:
            return 'self.' + self.obj()
        if t == 0x02:
            return 'default.' + self.obj()
        if t == 0x03:
            return 'state.' + self.obj()
        if t == 0x04:
            return 'return ' + self.expr()
        if t == 0x05:  # ME2: the switched value's size (one byte), then the value
            self.u8()
            return f'switch ({self.expr()})'
        if t == 0x06:
            return f'goto {self.u16():#06x}'
        if t == 0x07:
            off = self.u16()
            return f'if (!({self.expr()})) goto {off:#06x}'
        if t == 0x08:
            return 'stop'
        if t == 0x09:
            self.u16()
            self.u8()
            return f'assert({self.expr()})'
        if t == 0x0A:
            off = self.u16()
            if off == 0xFFFF:
                return 'default:'
            return f'case {self.expr()}: (next {off:#06x})'
        if t == 0x0B:
            return 'nothing'
        if t == 0x0D:
            return 'goto label ' + self.expr()
        if t == 0x0E:
            self.obj()
            return self.expr()
        if t == 0x0F:
            a = self.expr()
            return f'{a} = {self.expr()}'
        if t == 0x10:
            i = self.expr()
            return f'{self.expr()}[{i}]'
        if t == 0x12:
            ctx = self.expr()
            self.u16()
            self.u8()
            return f'{ctx}::{self.expr()}'
        if t == 0x13:
            cls = self.obj()
            return f'class<{cls}>({self.expr()})'
        if t == 0x14:
            a = self.expr()
            return f'{a} = {self.expr()}'
        if t == 0x15:
            return 'endparm'
        if t == 0x16:
            return ')'
        if t == 0x17:
            return 'self'
        if t == 0x18:
            self.u16()
            return self.expr()
        if t == 0x19:
            ctx = self.expr()
            self.u16()
            self.u8()  # ME2: the result size, no field reference
            return f'{ctx}.{self.expr()}'
        if t == 0x1A:
            i = self.expr()
            return f'{self.expr()}[{i}]'
        if t == 0x1B:
            n = self.name()
            return f'{n}({self.params()})'
        if t == 0x1C:
            f = self.obj()
            return f'{f}({self.params()})'
        if t == 0x1D:
            return str(self.i32())
        if t == 0x1E:
            return repr(self.f32())
        if t == 0x1F:
            return '"' + self.cstr() + '"'
        if t == 0x20:
            return self.objfull()
        if t == 0x21:
            return "'" + self.name() + "'"
        if t == 0x22:
            return f'rot({self.i32()},{self.i32()},{self.i32()})'
        if t == 0x23:
            return f'vect({self.f32()},{self.f32()},{self.f32()})'
        if t == 0x24:
            return str(self.u8())
        if t == 0x25:
            return '0'
        if t == 0x26:
            return '1'
        if t == 0x27:
            return 'true'
        if t == 0x28:
            return 'false'
        if t == 0x29:
            return 'nativeparm ' + self.obj()
        if t == 0x2A:
            return 'None'
        if t == 0x2C:
            return str(self.u8())
        if t == 0x2D:
            return self.expr()
        if t == 0x2E:
            cls = self.obj()
            return f'{cls}({self.expr()})'
        if t == 0x2F:
            it = self.expr()
            self.u16()
            return f'foreach {it}'
        if t in (0x30, 0x31):
            return 'iterator pop' if t == 0x30 else 'iterator next'
        if t in (0x32, 0x33):
            self.obj()
            a = self.expr()
            return f'({a} {"==" if t == 0x32 else "!="} {self.expr()})'
        if t == 0x34:
            return 'u"' + self.wstr() + '"'
        if t == 0x35:
            field = self.obj()
            self.obj()
            self.u8()
            self.u8()
            return f'{self.expr()}.{field}'
        if t == 0x36:
            return f'{self.expr()}.Length'
        if t == 0x37:
            n = self.name()
            return f'global.{n}({self.params()})'
        if t == 0x38:
            k = self.u8()
            return f'cast{k:#x}({self.expr()})'
        if t == 0x3A:
            return 'return'
        if t in (0x3B, 0x3C):
            a = self.expr()
            return f'({a} {"==" if t == 0x3B else "!="} {self.expr()})'
        if t in (0x3D, 0x3E):
            a = self.expr()
            return f'({a} {"==" if t == 0x3D else "!="} {self.expr()})'
        if t == 0x40:
            arr = self.expr()
            self.u16()
            return f'{arr}.AddItem({self.expr()})'
        if t == 0x41:
            self.i32()
            self.i32()
            self.i32()
            self.u8()
            return self.expr()
        if t == 0x42:
            return f'delegate {self.name()}'
        if t == 0x43:  # ME2: a delegate property / function by name
            return 'delegate ' + self.name()
        if t == 0x44:  # ME2: let delegate (lhs, rhs)
            a = self.expr()
            return f'{a} = {self.expr()}'
        if t == 0x45:
            condition = self.expr()
            self.u16()
            yes = self.expr()
            self.u16()
            no = self.expr()
            return f'({condition} ? {yes} : {no})'
        if t == 0x47:
            return 'abort'
        if t == 0x48:
            return f'outparm {self.obj()}'
        if t == 0x49:
            self.u16()
            v = self.expr()
            while self.c[self.o] != 0x15:
                self.expr()
            self.o += 1
            return f'(default {v})'
        if t == 0x4A:
            return 'noparm'
        if t == 0x4B:
            return f'instancedelegate {self.name()}'
        if t == 0x51:
            return f'{self.obj()}  /* interface */ ' + self.expr()
        if t == 0x52:
            return 'interface ' + self.expr()
        if t == 0x53:
            return 'end'
        if t == 0x55:  # ME2: dynamic array AddItem (array, skip, item)
            arr = self.expr()
            self.u16()
            return f'{arr}.AddItem({self.expr()})'
        if t >= 0x60:
            # native function by index: 0x60-0x6F with a second byte, 0x70+ the index itself
            idx = t if t >= 0x70 else ((t - 0x60) << 8) | self.u8()
            return f'native{idx}({self.params()})'
        raise ValueError(f'token {t:#04x} at {start:#06x}')


def disasm(p, ref):
    e = p.exports[ref - 1]
    b = p.d[e['offset']:e['offset'] + e['size']]
    code = b[44:]
    d = Dis(p, code)
    print(f'// {p.full_name(ref)} ({len(code)} bytes from +44)')
    while d.o < len(code):
        at = d.o
        try:
            s = d.expr()
        except Exception as ex:  # noqa: BLE001
            print(f'{at:#06x}: ?? {ex}; rest: {code[at:at + 32].hex(" ")}')
            return
        print(f'{at:#06x}: {s}')
        if s == 'end':
            return


def main():
    p = mp.Package(open(sys.argv[1], 'rb').read())
    for a in sys.argv[2:]:
        if a.lstrip('-').isdigit():
            disasm(p, int(a))
        else:
            for i in range(len(p.exports)):
                if p.full_name(i + 1) == a:
                    disasm(p, i + 1)
                    break
            else:
                print('not found:', a)
        print()


if __name__ == '__main__':
    main()
