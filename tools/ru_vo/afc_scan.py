"""List the Wwise RIFF/RIFX streams in an .afc: offset, size, channels, rate, sample count, duration."""
import struct


def scan(d):
    out = []
    p = 0
    n = len(d)
    while p + 12 <= n:
        tag = d[p:p + 4]
        if tag not in (b'RIFF', b'RIFX') or d[p + 8:p + 12] != b'WAVE':
            q1 = d.find(b'RIFF', p + 1)
            q2 = d.find(b'RIFX', p + 1)
            qs = [q for q in (q1, q2) if q >= 0]
            if not qs:
                break
            p = min(qs)
            continue
        e = '>' if tag == b'RIFX' else '<'
        size = struct.unpack_from(e + 'I', d, p + 4)[0] + 8
        info = {'off': p, 'size': size, 'be': e == '>'}
        q = p + 12
        while q + 8 <= p + size:
            cid = d[q:q + 4]
            cs = struct.unpack_from(e + 'I', d, q + 4)[0]
            if cid == b'fmt ':
                info['ch'], info['rate'] = struct.unpack_from(e + 'HI', d, q + 10)
                if cs >= 0x42 - 0 and not info['be']:
                    # LE (new Wwise): vorb fields inside fmt extension; sample count at fmt+0x18
                    info['samples'] = struct.unpack_from('<I', d, q + 8 + 0x18)[0]
            elif cid == b'vorb':
                info['samples'] = struct.unpack_from(e + 'I', d, q + 8)[0]
            elif cid == b'data':
                break
            q += 8 + cs + (cs & 1)
        if 'samples' in info and info.get('rate'):
            info['dur'] = info['samples'] / info['rate']
        out.append(info)
        p += size
    return out
