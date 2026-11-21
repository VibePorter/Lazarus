"""Re-read every output package; check every WwiseStream points at a valid RIFX in the output (or original) afc."""
import collections, json, os, struct, sys
from concurrent.futures import ProcessPoolExecutor
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_map as BM
import build_ru_vo as B

OUT = B.OUT


def job(path):
    import io, contextlib
    with contextlib.redirect_stderr(io.StringIO()):
        _, streams, interps = BM.x360_job(path)
    idx = B.x360_afc_index()
    st = collections.Counter()
    for s in streams:
        if 'error' in s or not s['afc']:
            st['skip'] += 1; continue
        src = B.resolve_afc(idx, s['afc'], path.replace(os.path.join(OUT, 'game'), B.X360_BASE).replace(os.path.join(OUT, 'dlc'), B.X360_DLC))
        if src is None:
            st['afc missing'] += 1; continue
        o = B.out_path(src)
        f = o if os.path.exists(o) else src
        with open(f, 'rb') as fh:
            fh.seek(s['off']); b = fh.read(min(s['size'], 0x80))
            fh.seek(0, 2); fsize = fh.tell()
        if b[:4] != b'RIFX' or struct.unpack_from('>I', b, 4)[0] + 8 != s['size'] or s['off'] + s['size'] > fsize:
            st['BAD'] += 1; continue
        uid = struct.unpack_from('>I', b, 0x70)[0] if b[0x3c:0x40] == b'vorb' else None
        ru = s['off'] >= os.path.getsize(src)
        st[('russian' if ru else 'english') + (' uid ok' if (not ru or uid == 0xB7D746F0) else ' uid WRONG')] += 1
    return path, st, len(interps)


if __name__ == '__main__':
    pk = [os.path.join(dp, f) for dp, _, fs in os.walk(OUT) for f in fs if f.lower().endswith('.xxx')]
    tot = collections.Counter()
    with ProcessPoolExecutor(max_workers=12) as ex:
        for path, st, ni in ex.map(job, pk):
            tot.update(st)
    print(len(pk), 'packages', dict(tot))
