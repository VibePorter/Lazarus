"""Xbox 360 ME2 Wwise Vorbis (RIFX, vorb chunk 0x34, full setup, 6-byte packet headers): parse."""
import struct


def parse(b):
    assert b[:4] == b'RIFX' and b[8:12] == b'WAVE'
    size = struct.unpack_from('>I', b, 4)[0] + 8
    q = 12
    ch = {}
    order = []
    while q + 8 <= min(size, len(b)):
        cid = b[q:q + 4]
        cs = struct.unpack_from('>I', b, q + 4)[0]
        ch[cid] = (q + 8, cs)
        order.append((cid, cs))
        q += 8 + cs + (cs & 1)
    fo, fs = ch[b'fmt ']
    vo, vs = ch[b'vorb']
    do, ds = ch[b'data']
    fmt = b[fo:fo + fs]
    vorb = b[vo:vo + vs]
    data = b[do:do + ds]
    v = struct.unpack('>11I', vorb[:44])
    info = {'chunks': order, 'fmt': fmt, 'vorb': vorb, 'vorb_words': v,
            'channels': struct.unpack_from('>H', fmt, 2)[0], 'rate': struct.unpack_from('>I', fmt, 4)[0],
            'avg_bps': struct.unpack_from('>I', fmt, 8)[0],
            'uid': struct.unpack_from('>I', vorb, 0x2c)[0], 'bs0': vorb[0x30], 'bs1': vorb[0x31]}
    setup_off, audio_off = v[6], v[7]
    pk = []
    p = 0
    while p + 6 <= len(data):
        n, g = struct.unpack_from('>Hi', data, p)
        pk.append((p, n, g))
        p += 6 + n
    info['packets'] = pk
    info['data'] = data
    info['tail'] = len(data) - p
    return info
