"""Round trip every 360 Wwise Vorbis stream: parse -> setup restore/strip -> build_360 -> must be byte identical."""
import collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afc_scan import scan
import wwvorbis as V

X = os.path.join(os.environ.get('ME2_X360_GAME', ''), 'BioGame', 'CookedXenon')
st = collections.Counter(); ex = {}
files = sorted(f for f in os.listdir(X) if f.lower().endswith('_s_int.afc'))
if len(sys.argv) > 1:
    files = files[:int(sys.argv[1])]
for fn in files:
    d = open(os.path.join(X, fn), 'rb').read()
    for s in scan(d):
        b = d[s['off']:s['off'] + s['size']]
        try:
            p = V.parse_360(b)
        except Exception:
            st['skip(no vorb)'] += 1; continue
        if not p['setup360']:
            st['skip(no setup)'] += 1; continue
        std = V.restore_setup_from_360(p['setup360'])
        try:
            again = V.strip_setup_for_360(std, p['channels'])
        except Exception as e:
            st['setup parse error'] += 1; ex.setdefault('setup parse error', (fn, s['off'], repr(e))); continue
        if again != p['setup360']:
            st['setup differs'] += 1; ex.setdefault('setup differs', (fn, s['off'], len(again), len(p['setup360']))); continue
        modes = V.setup_modes(std, p['channels'])
        g = V.granules(p['audio'], modes, p['bs0'], p['bs1'])
        g[-1] = p['samples']
        if g != p['grans']:
            st['granules differ'] += 1; ex.setdefault('granules differ', (fn, s['off'], [(i, a, b2) for i, (a, b2) in enumerate(zip(g, p['grans'])) if a != b2][:4])); continue
        out = V.build_360(p['channels'], p['rate'], p['bs0'], p['bs1'], p['setup360'], p['audio'], g,
                          p['samples'], p['uid'], p['alloc'])
        if out == b[:len(out)] and len(out) >= s['size'] - 1:
            st['identical'] += 1
        else:
            k = next((i for i in range(min(len(out), len(b))) if out[i] != b[i]), None)
            st['differs'] += 1; ex.setdefault('differs@%s' % k, (fn, s['off'], len(out), s['size']))
print(st)
for k, v in ex.items(): print(k, v)
