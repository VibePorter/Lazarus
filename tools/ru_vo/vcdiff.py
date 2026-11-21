"""Minimal VCDIFF (RFC 3284) decoder with xdelta3 extensions (LZMA secondary, adler32).

Usage: python -I vcdiff.py SOURCE PATCH OUT
"""
import lzma
import sys
import zlib

VCD_SOURCE, VCD_TARGET, VCD_ADLER32 = 0x01, 0x02, 0x04
NOOP, ADD, RUN, COPY = 0, 1, 2, 3


def _build_code_table():
    # RFC 3284 default code table: list of (type1,size1,mode1,type2,size2,mode2)
    t = []
    t.append((RUN, 0, 0, NOOP, 0, 0))
    t.append((ADD, 0, 0, NOOP, 0, 0))
    for s in range(1, 18):
        t.append((ADD, s, 0, NOOP, 0, 0))
    for m in range(9):
        t.append((COPY, 0, m, NOOP, 0, 0))
        for s in range(4, 19):
            t.append((COPY, s, m, NOOP, 0, 0))
    for m in range(6):
        for a in range(1, 5):
            for c in range(4, 7):
                t.append((ADD, a, 0, COPY, c, m))
    for m in range(6, 9):
        for a in range(1, 5):
            t.append((ADD, a, 0, COPY, 4, m))
    for m in range(9):
        t.append((COPY, 4, m, ADD, 1, 0))
    assert len(t) == 256, len(t)
    return t


CODE_TABLE = _build_code_table()


class Reader:
    __slots__ = ("b", "p")

    def __init__(self, b, p=0):
        self.b = b
        self.p = p

    def byte(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def varint(self):
        v = 0
        b = self.b
        p = self.p
        while True:
            c = b[p]
            p += 1
            v = (v << 7) | (c & 0x7F)
            if not c & 0x80:
                break
        self.p = p
        return v

    def take(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v


def _secondary(sec_id, data, state, key):
    r = Reader(data)
    size = r.varint()
    raw = data[r.p:]
    if sec_id == 2:
        # xdelta3 sync-flushes each section: no end-of-stream marker
        # and keeps one stream per section type across windows
        d = state.get(key)
        if d is None:
            d = state[key] = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
        out = d.decompress(raw, size)
    else:
        raise ValueError("unsupported secondary compressor %d" % sec_id)
    if len(out) != size:
        raise ValueError("secondary size mismatch %d != %d" % (len(out), size))
    return out


def decode(src, patch):
    r = Reader(patch)
    if bytes(r.take(3)) != b"\xd6\xc3\xc4" or r.byte() != 0:
        raise ValueError("not VCDIFF")
    hdr = r.byte()
    sec_id = 0
    if hdr & 0x01:
        sec_id = r.byte()
    if hdr & 0x02:
        raise ValueError("custom code table not supported")
    if hdr & 0x04:
        n = r.varint()
        r.take(n)  # application header (source/target names)
    out = bytearray()
    sec_state = {}
    n_patch = len(patch)
    while r.p < n_patch:
        win = r.byte()
        seg = b""
        if win & (VCD_SOURCE | VCD_TARGET):
            slen = r.varint()
            spos = r.varint()
            if win & VCD_SOURCE:
                seg = src[spos:spos + slen]
            else:
                seg = bytes(out[spos:spos + slen])
            if len(seg) != slen:
                raise ValueError("source segment out of range")
        slen = len(seg)
        r.varint()  # delta encoding length
        tlen = r.varint()
        delta_ind = r.byte()
        dlen = r.varint()
        ilen = r.varint()
        alen = r.varint()
        cksum = None
        if win & VCD_ADLER32:
            cksum = int.from_bytes(r.take(4), "big")
        data = r.take(dlen)
        inst = r.take(ilen)
        addr = r.take(alen)
        if delta_ind & 1:
            data = _secondary(sec_id, data, sec_state, "data")
        if delta_ind & 2:
            inst = _secondary(sec_id, inst, sec_state, "inst")
        if delta_ind & 4:
            addr = _secondary(sec_id, addr, sec_state, "addr")
        t = _window(seg, slen, tlen, data, inst, addr)
        if cksum is not None and (zlib.adler32(t) & 0xFFFFFFFF) != cksum:
            raise ValueError("adler32 mismatch at window %d" % len(out))
        out += t
    return bytes(out)


def _window(seg, slen, tlen, data, inst, addr):
    # target buffer is (source segment + target) for COPY addressing
    buf = bytearray(seg)
    base = slen
    near = [0, 0, 0, 0]
    next_slot = 0
    same = [0] * (3 * 256)
    ir = Reader(inst)
    ar = Reader(addr)
    dp = 0
    table = CODE_TABLE
    n_inst = len(inst)
    while ir.p < n_inst:
        code = table[ir.byte()]
        for typ, size, mode in ((code[0], code[1], code[2]), (code[3], code[4], code[5])):
            if typ == NOOP:
                continue
            if size == 0:
                size = ir.varint()
            if typ == ADD:
                buf += data[dp:dp + size]
                dp += size
            elif typ == RUN:
                buf += bytes((data[dp],)) * size
                dp += 1
            else:
                here = len(buf)  # = slen + target position
                if mode == 0:
                    a = ar.varint()
                elif mode == 1:
                    a = here - ar.varint()
                elif mode < 6:
                    a = near[mode - 2] + ar.varint()
                else:
                    a = same[(mode - 6) * 256 + ar.byte()]
                near[next_slot] = a
                next_slot = (next_slot + 1) & 3
                same[a % 768] = a
                end = a + size
                if end <= here:
                    buf += buf[a:end]
                else:
                    # overlapping copy (run-like)
                    for i in range(size):
                        buf.append(buf[a + i])
    t = bytes(buf[base:])
    if len(t) != tlen:
        raise ValueError("target length %d != %d" % (len(t), tlen))
    return t


def main():
    src = open(sys.argv[1], "rb").read()
    patch = open(sys.argv[2], "rb").read()
    out = decode(src, patch)
    open(sys.argv[3], "wb").write(out)


if __name__ == "__main__":
    main()
