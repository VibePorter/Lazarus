"""Wwise Vorbis conversions for the ME2 Russian VO port.

- LE (Legendary Edition, newer Wwise): RIFF, vorb info inside a 0x42 fmt, 2-byte packet headers, setup with external
  (aoTuV 603 packed) codebook ids and stripped floor/residue/mapping/mode headers, "mod packets" (no packet type /
  window bits). Converted losslessly to standard Vorbis packets (port of hcs' ww2ogg).
- 360 (ME2 Xbox, Wwise 2009): RIFX, vorb chunk 0x34, 6-byte packet headers (size, granule), setup = standard setup
  header without the "\\x05vorbis" prefix and without the time-domain section, standard audio packets.
- Ogg pages (read / write) so ffmpeg can decode / encode.
"""
import os
import struct
import zlib

CODEBOOKS = os.environ.get('WW_CODEBOOKS') or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                          'packed_codebooks_aoTuV_603.bin')  # ww2ogg's


# ------------------------------------------------------------------ bits (Vorbis: LSB first)
class BitReader:
    def __init__(self, data, pos=0):
        self.d = data
        self.bit = pos * 8

    def read(self, n):
        v = 0
        for i in range(n):
            b = self.bit
            v |= ((self.d[b >> 3] >> (b & 7)) & 1) << i
            self.bit += 1
        return v

    @property
    def total(self):
        return self.bit


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.bit = 0

    def write(self, v, n):
        for i in range(n):
            if self.bit & 7 == 0:
                self.out.append(0)
            if (v >> i) & 1:
                self.out[-1] |= 1 << (self.bit & 7)
            self.bit += 1

    def copy(self, br, n):
        v = br.read(n)
        self.write(v, n)
        return v

    def bytes(self):
        return bytes(self.out)


def ilog(v):
    r = 0
    while v:
        r += 1
        v >>= 1
    return r


def maptype1_quantvals(entries, dims):
    bits = ilog(entries)
    vals = entries >> ((bits - 1) * (dims - 1) // dims)
    while True:
        acc = acc1 = 1
        for _ in range(dims):
            acc *= vals
            acc1 *= vals + 1
        if acc <= entries < acc1:
            return vals
        if acc > entries:
            vals -= 1
        else:
            vals += 1


# ------------------------------------------------------------------ codebooks
def copy_std_codebook(br, bw):
    """Copy one standard Vorbis codebook bit for bit."""
    sync = bw.copy(br, 24)
    if sync != 0x564342:
        raise ValueError('codebook sync %x' % sync)
    dims = bw.copy(br, 16)
    entries = bw.copy(br, 24)
    ordered = bw.copy(br, 1)
    if ordered:
        bw.copy(br, 5)
        cur = 0
        while cur < entries:
            cur += bw.copy(br, ilog(entries - cur))
    else:
        sparse = bw.copy(br, 1)
        for _ in range(entries):
            if not sparse or bw.copy(br, 1):
                bw.copy(br, 5)
    lookup = bw.copy(br, 4)
    if lookup in (1, 2):
        bw.copy(br, 32)
        bw.copy(br, 32)
        vbits = bw.copy(br, 4) + 1
        bw.copy(br, 1)
        q = maptype1_quantvals(entries, dims) if lookup == 1 else entries * dims
        for _ in range(q):
            bw.copy(br, vbits)
    elif lookup != 0:
        raise ValueError('lookup type %d' % lookup)


class PackedCodebooks:
    def __init__(self, path=CODEBOOKS):
        d = open(path, 'rb').read()
        off = struct.unpack_from('<I', d, len(d) - 4)[0]
        n = (len(d) - off) // 4
        self.data = d[:off]
        self.offs = list(struct.unpack_from('<%dI' % n, d, off))

    def rebuild(self, i, bw):
        cb = self.data[self.offs[i]:self.offs[i + 1]]
        br = BitReader(cb)
        dims = br.read(4)
        entries = br.read(14)
        bw.write(0x564342, 24)
        bw.write(dims, 16)
        bw.write(entries, 24)
        ordered = bw.copy(br, 1)
        if ordered:
            bw.copy(br, 5)
            cur = 0
            while cur < entries:
                cur += bw.copy(br, ilog(entries - cur))
        else:
            cwll = br.read(3)
            sparse = br.read(1)
            if cwll == 0 or cwll > 5:
                raise ValueError('codeword length length')
            bw.write(sparse, 1)
            for _ in range(entries):
                if not sparse or bw.copy(br, 1):
                    bw.write(br.read(cwll), 5)
        lookup = br.read(1)
        bw.write(lookup, 4)
        if lookup == 1:
            bw.copy(br, 32)
            bw.copy(br, 32)
            vbits = bw.copy(br, 4) + 1
            bw.copy(br, 1)
            for _ in range(maptype1_quantvals(entries, dims)):
                bw.copy(br, vbits)
        if len(cb) and br.total // 8 + 1 != len(cb):
            raise ValueError('codebook %d size mismatch' % i)


_books = None


def books():
    global _books
    if _books is None:
        _books = PackedCodebooks()
    return _books


# ------------------------------------------------------------------ standard headers
def ident_header(channels, rate, bs0, bs1, nominal=0):
    return (b'\x01vorbis' + struct.pack('<IBIiii', 0, channels, rate, 0, nominal, 0) +
            bytes([(bs1 << 4) | bs0, 1]))


def comment_header(vendor=b'ME2 RU VO'):
    return b'\x03vorbis' + struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', 0) + b'\x01'


def parse_ident(p):
    assert p[:7] == b'\x01vorbis'
    ch = p[11]
    rate = struct.unpack_from('<I', p, 12)[0]
    bs = p[28]
    return ch, rate, bs & 15, bs >> 4


def setup_modes(setup, channels, want_end=False):
    """Parse a standard setup header; returns the mode block flags (and the bit after the framing bit)."""
    br = BitReader(setup, 7)
    bw = BitWriter()  # scratch
    n = br.read(8) + 1
    for _ in range(n):
        copy_std_codebook(br, bw)
    for _ in range(br.read(6) + 1):
        br.read(16)
    for _ in range(br.read(6) + 1):
        ftype = br.read(16)
        if ftype != 1:
            raise ValueError('floor type %d' % ftype)
        parts = br.read(5)
        cls = [br.read(4) for _ in range(parts)]
        mx = max(cls) if cls else -1
        dims = []
        for _ in range(mx + 1):
            dims.append(br.read(3) + 1)
            sub = br.read(2)
            if sub:
                br.read(8)
            for _ in range(1 << sub):
                br.read(8)
        br.read(2)
        rb = br.read(4)
        for c in cls:
            for _ in range(dims[c]):
                br.read(rb)
    for _ in range(br.read(6) + 1):
        br.read(16)
        br.read(24)
        br.read(24)
        br.read(24)
        ncls = br.read(6) + 1
        br.read(8)
        casc = []
        for _ in range(ncls):
            lo = br.read(3)
            hi = br.read(5) if br.read(1) else 0
            casc.append(hi * 8 + lo)
        for c in casc:
            for b in range(8):
                if c & (1 << b):
                    br.read(8)
    for _ in range(br.read(6) + 1):
        br.read(16)
        sub = br.read(4) + 1 if br.read(1) else 1
        if br.read(1):
            for _ in range(br.read(8) + 1):
                br.read(ilog(channels - 1))
                br.read(ilog(channels - 1))
        br.read(2)
        if sub > 1:
            for _ in range(channels):
                br.read(4)
        for _ in range(sub):
            br.read(8)
            br.read(8)
            br.read(8)
    modes = []
    for _ in range(br.read(6) + 1):
        modes.append(br.read(1))
        br.read(16)
        br.read(16)
        br.read(8)
    if br.read(1) != 1:
        raise ValueError('setup framing bit')
    return (modes, br.total) if want_end else modes


def granules(packets, modes, bs0, bs1):
    """Standard granule (end sample) per audio packet; the first packet yields nothing (-1)."""
    mb = ilog(len(modes) - 1)
    sizes = (1 << bs0, 1 << bs1)
    out = []
    prev = None
    g = 0
    for p in packets:
        if not p:
            out.append(g if prev is not None else -1)
            continue
        br = BitReader(p)
        if br.read(1) != 0:
            raise ValueError('not an audio packet')
        cur = sizes[modes[br.read(mb)]]
        if prev is None:
            out.append(-1)
        else:
            g += prev // 4 + cur // 4
            out.append(g)
        prev = cur
    return out


# ------------------------------------------------------------------ LE wem -> standard packets
def le_to_std(b):
    """LE wem (RIFF) -> (ident, comment, setup, [audio packets], sample_count)."""
    assert b[:4] == b'RIFF' and b[8:12] == b'WAVE'
    size = struct.unpack_from('<I', b, 4)[0] + 8
    q = 12
    ch = {}
    while q + 8 <= min(size, len(b)):
        cid = b[q:q + 4]
        cs = struct.unpack_from('<I', b, q + 4)[0]
        ch[cid] = (q + 8, cs)
        q += 8 + cs + (cs & 1)
    fo, fs = ch[b'fmt ']
    if fs != 0x42:
        raise ValueError('fmt size %x' % fs)
    channels, rate = struct.unpack_from('<HI', b, fo + 2)
    vo = fo + 0x18
    samples = struct.unpack_from('<I', b, vo)[0]
    mod_signal = struct.unpack_from('<I', b, vo + 4)[0]
    mod_packets = mod_signal not in (0x4A, 0x4B, 0x69, 0x70)
    setup_off, audio_off = struct.unpack_from('<II', b, vo + 0x10)
    bs0, bs1 = b[vo + 0x28], b[vo + 0x29]
    do, ds = ch[b'data']
    data = b[do:do + ds]

    # setup
    n = struct.unpack_from('<H', data, setup_off)[0]
    br = BitReader(data[setup_off + 2:setup_off + 2 + n])
    bw = BitWriter()
    for c in b'\x05vorbis':
        bw.write(c, 8)
    cnt = bw.copy(br, 8) + 1
    cbl = books()
    for _ in range(cnt):
        cbl.rebuild(br.read(10), bw)
    bw.write(0, 6)
    bw.write(0, 16)
    floors = bw.copy(br, 6) + 1
    for _ in range(floors):
        bw.write(1, 16)
        parts = bw.copy(br, 5)
        cls = [bw.copy(br, 4) for _ in range(parts)]
        mx = max(cls) if cls else -1
        dims = []
        for _ in range(mx + 1):
            dims.append(bw.copy(br, 3) + 1)
            sub = bw.copy(br, 2)
            if sub:
                bw.copy(br, 8)
            for _ in range(1 << sub):
                bw.copy(br, 8)
        bw.copy(br, 2)
        rb = bw.copy(br, 4)
        for c in cls:
            for _ in range(dims[c]):
                bw.copy(br, rb)
    residues = bw.copy(br, 6) + 1
    for _ in range(residues):
        bw.write(br.read(2), 16)
        bw.copy(br, 24)
        bw.copy(br, 24)
        bw.copy(br, 24)
        ncls = bw.copy(br, 6) + 1
        bw.copy(br, 8)
        casc = []
        for _ in range(ncls):
            lo = bw.copy(br, 3)
            hi = bw.copy(br, 5) if bw.copy(br, 1) else 0
            casc.append(hi * 8 + lo)
        for c in casc:
            for k in range(8):
                if c & (1 << k):
                    bw.copy(br, 8)
    maps = bw.copy(br, 6) + 1
    for _ in range(maps):
        bw.write(0, 16)
        sub = bw.copy(br, 4) + 1 if bw.copy(br, 1) else 1
        if bw.copy(br, 1):
            for _ in range(bw.copy(br, 8) + 1):
                bw.copy(br, ilog(channels - 1))
                bw.copy(br, ilog(channels - 1))
        if bw.copy(br, 2) != 0:
            raise ValueError('mapping reserved')
        if sub > 1:
            for _ in range(channels):
                bw.copy(br, 4)
        for _ in range(sub):
            bw.copy(br, 8)
            bw.copy(br, 8)
            bw.copy(br, 8)
    nmodes = bw.copy(br, 6) + 1
    modes = []
    for _ in range(nmodes):
        modes.append(bw.copy(br, 1))
        bw.write(0, 16)
        bw.write(0, 16)
        bw.copy(br, 8)
    bw.write(1, 1)
    setup = bw.bytes()

    # audio packets
    raw = []
    p = audio_off
    while p + 2 <= len(data):
        n = struct.unpack_from('<H', data, p)[0]
        raw.append(data[p + 2:p + 2 + n])
        p += 2 + n
    mb = ilog(nmodes - 1)
    out = []
    prev_bf = 0
    mmask = (1 << mb) - 1
    for k, pk in enumerate(raw):
        if not mod_packets or not pk:
            out.append(pk)
            continue
        first = pk[0]
        mode = first & mmask
        rem = first >> mb
        if modes[mode]:
            nxt = 0
            if k + 1 < len(raw) and raw[k + 1]:
                nxt = modes[raw[k + 1][0] & mmask]
            hdr = (mode << 1) | (prev_bf << (1 + mb)) | (nxt << (2 + mb))
            hb = 3 + mb
        else:
            hdr = mode << 1
            hb = 1 + mb
        prev_bf = modes[mode]
        rest = int.from_bytes(pk[1:], 'little')
        val = hdr | (rem << hb) | (rest << (hb + 8 - mb))
        nbits = hb + 8 - mb + 8 * (len(pk) - 1)
        out.append(val.to_bytes((nbits + 7) // 8, 'little'))
    ident = ident_header(channels, rate, bs0, bs1)
    return ident, comment_header(), setup, out, samples


# ------------------------------------------------------------------ Ogg pages
def _crc_table():
    t = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if r & 0x80000000 else (r << 1) & 0xFFFFFFFF
        t.append(r)
    return t


_CRC = _crc_table()


def ogg_crc(d):
    c = 0
    for b in d:
        c = ((c << 8) & 0xFFFFFFFF) ^ _CRC[((c >> 24) & 0xFF) ^ b]
    return c


def ogg_write(packets, grans, serial=0x4d453252):
    """packets: list of bytes; grans: granule per packet (-1 = none). One packet per page (simple, valid)."""
    out = bytearray()
    for i, (p, g) in enumerate(zip(packets, grans)):
        segs = []
        n = len(p)
        while n >= 255:
            segs.append(255)
            n -= 255
        segs.append(n)
        flags = (2 if i == 0 else 0) | (4 if i == len(packets) - 1 else 0)
        gp = g if g >= 0 else 0xFFFFFFFFFFFFFFFF
        if g < 0 and i >= 3:
            gp = 0xFFFFFFFFFFFFFFFF
        hdr = bytearray(b'OggS' + struct.pack('<BBQIII', 0, flags, gp & 0xFFFFFFFFFFFFFFFF, serial, i, 0) +
                        bytes([len(segs)]) + bytes(segs))
        page = hdr + p
        struct.pack_into('<I', page, 22, ogg_crc(page))
        out += page
    return bytes(out)


def ogg_read(d):
    """-> list of (packet, granule of the page it ends on)."""
    pk = []
    cur = bytearray()
    p = 0
    while p < len(d):
        assert d[p:p + 4] == b'OggS', 'bad ogg page'
        gran = struct.unpack_from('<q', d, p + 6)[0]
        nseg = d[p + 26]
        segs = d[p + 27:p + 27 + nseg]
        q = p + 27 + nseg
        ended = []
        for s in segs:
            cur += d[q:q + s]
            q += s
            if s < 255:
                ended.append(bytes(cur))
                cur = bytearray()
        for k, e in enumerate(ended):
            pk.append((e, gran if k == len(ended) - 1 else -1))
        p = q
    return pk


# ------------------------------------------------------------------ 360 RIFX
FMT_TAIL = bytes.fromhex('0016001000000004') + bytes.fromhex('0100000000001000800000aa00389b71')


def strip_setup_for_360(setup, channels):
    """Standard setup header -> Wwise 2009 'full setup': drop '\\x05vorbis' and the time-domain section."""
    _, end = setup_modes(setup, channels, want_end=True)
    br = BitReader(setup, 7)
    bw = BitWriter()
    n = bw.copy(br, 8) + 1
    for _ in range(n):
        copy_std_codebook(br, bw)
    t = br.read(6) + 1
    for _ in range(t):
        if br.read(16) != 0:
            raise ValueError('time domain value')
    while br.total < end:
        bw.write(br.read(1), 1)
    return bw.bytes()


def restore_setup_from_360(s):
    br = BitReader(s)
    bw = BitWriter()
    for c in b'\x05vorbis':
        bw.write(c, 8)
    n = bw.copy(br, 8) + 1
    for _ in range(n):
        copy_std_codebook(br, bw)
    bw.write(0, 6)
    bw.write(0, 16)
    total = len(s) * 8
    while br.total < total:
        bw.write(br.read(1), 1)
    return bw.bytes()


def build_360(channels, rate, bs0, bs1, setup360, audio, grans, samples, uid, alloc=(0x451c, 0x46c4),
              channel_mask=None):
    data = bytearray()
    data += struct.pack('>Hi', len(setup360), 0) + setup360
    audio_off = len(data)
    offs = []
    for p, g in zip(audio, grans):
        offs.append(len(data))
        data += struct.pack('>Hi', len(p), g)
        data += p
    npk = len(audio) + 1
    maxpk = max(len(p) for p in audio)
    # samples in the last packet - 1 (Wwise "last granule extra" style)
    prev_g = grans[-2] if len(grans) >= 2 and grans[-2] >= 0 else 0
    last_extra = samples - prev_g - 1
    vorb = struct.pack('>11I', samples, npk, 0, audio_off, last_extra, offs[-1], 0, audio_off, maxpk,
                       alloc[0], alloc[1]) + struct.pack('>I', uid) + bytes([bs0, bs1, 0, 0])
    assert len(vorb) == 52
    avg = int(len(data) * rate / samples) if samples else 0
    if channel_mask is None:
        channel_mask = 4 if channels == 1 else 3
    fmt = struct.pack('>HHIIHHHHI', 0xFFFF, channels, rate, avg, 0, 0, 0x16, 0x10 if channels == 1 else 0x18, channel_mask) + FMT_TAIL[8:]
    assert len(fmt) == 40
    body = b'WAVE' + b'fmt ' + struct.pack('>I', 40) + fmt + b'vorb' + struct.pack('>I', 52) + vorb
    body += b'data' + struct.pack('>I', len(data)) + bytes(data)  # Wwise 2009 does not pad odd data
    return b'RIFX' + struct.pack('>I', len(body)) + body


def parse_360(b):
    """360 RIFX -> dict with ident/setup(std)/audio packets/granules + raw fields."""
    import wwise360
    i = wwise360.parse(b)
    v = i['vorb_words']
    pk = i['packets']
    data = i['data']
    setup360 = data[pk[0][0] + 6:pk[0][0] + 6 + pk[0][1]]
    audio = [data[o + 6:o + 6 + n] for o, n, g in pk[1:]]
    grans = [g for o, n, g in pk[1:]]
    return {'channels': i['channels'], 'rate': i['rate'], 'bs0': i['bs0'], 'bs1': i['bs1'], 'uid': i['uid'],
            'samples': v[0], 'alloc': (v[9], v[10]), 'setup360': setup360, 'audio': audio, 'grans': grans,
            'fmt': i['fmt'], 'vorb': i['vorb'], 'avg': i['avg_bps']}


def x360_to_ogg(b):
    s = parse_360(b)
    ident = ident_header(s['channels'], s['rate'], s['bs0'], s['bs1'])
    setup = restore_setup_from_360(s['setup360'])
    pk = [ident, comment_header(), setup] + s['audio']
    gr = [0, 0, 0] + [g for g in s['grans']]
    return ogg_write(pk, gr)


def std_to_ogg(ident, comment, setup, audio, samples):
    ch, rate, bs0, bs1 = parse_ident(ident)
    modes = setup_modes(setup, ch)
    gr = granules(audio, modes, bs0, bs1)
    if gr:
        gr[-1] = samples
    return ogg_write([ident, comment, setup] + audio, [0, 0, 0] + gr)
