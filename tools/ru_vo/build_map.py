"""Build the Russian VO line map.

LE side: every WwiseStream whose data changed between the original LE packages and the SynthVoiceRu-patched ones
(key = stream full name + Wwise Id) -> Russian afc / offset / size, plus the English LE location; every changed
InterpData -> new InterpLength (checks that nothing else changed).

  python -I build_map.py le      -> map/le_streams.json, map/le_interp.json
  python -I build_map.py x360 DIR [DIR...]  -> map/x360_coverage.json (+ report)
"""
import collections
import json
import os
import struct
import sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))  # tools/: me2_package.py
import le_package as LEP  # noqa: E402

ROOT = os.environ.get('RU_VO_WORK') or os.path.join(os.getcwd(), 'ru_vo_work')  # work folder (README.md)
BASE = os.environ.get('ME2_LE_DIR', '')  # the original LE2 install's Game/ME2 (README.md)
RU = os.path.join(ROOT, 'le_ru')
MAP = os.path.join(ROOT, 'map')


def props(data, names, be, start=4):
    """Walk UE3 tagged properties; returns (dict name -> (type, value_offset, size), offset after None)."""
    e = '>' if be else '<'
    o = start
    out = {}
    while True:
        ni = struct.unpack_from(e + 'I', data, o)[0]
        nm = names[ni]
        if nm == 'None':
            return out, o + 8
        tn = names[struct.unpack_from(e + 'I', data, o + 8)[0]]
        sz = struct.unpack_from(e + 'I', data, o + 16)[0]
        hdr = 24
        if tn in ('StructProperty', 'ByteProperty'):
            hdr += 8
        if tn == 'BoolProperty':
            sz = 1 if not be else 4  # LE: 1 byte; 360 ME2: 4-byte bool
        out[nm] = (tn, o + hdr, sz)
        o += hdr + sz


def name_val(data, off, names, be):
    e = '>' if be else '<'
    i, n = struct.unpack_from(e + 'II', data, off)
    s = names[i]
    return s if n == 0 else '%s_%d' % (s, n - 1)


def le_stream_info(pkg, e):
    d = pkg.data(e)
    p, end = props(d, pkg.names, False)
    sid = struct.unpack_from('<i', d, p['Id'][1])[0] if 'Id' in p else None
    afc = name_val(d, p['Filename'][1], pkg.names, False) if 'Filename' in p else None
    cnt, size, off = struct.unpack_from('<III', d, len(d) - 12)
    return sid, afc, size, off


def le_job(rel):
    a = LEP.Package(open(os.path.join(BASE, rel), 'rb').read())
    b = LEP.Package(open(os.path.join(RU, rel), 'rb').read())
    ia = {a.full_name(i + 1): e for i, e in enumerate(a.exports)}
    streams, interps, problems = [], [], []
    for i, e in enumerate(b.exports):
        c = b.class_name(e)
        if c not in ('WwiseStream', 'InterpData'):
            continue
        fn = b.full_name(i + 1)
        ea = ia.get(fn)
        if ea is None:
            problems.append(('new export', fn))
            continue
        x, y = a.data(ea), b.data(e)
        if c == 'WwiseStream':
            sid, afc, size, off = le_stream_info(b, e)
            sid0, afc0, size0, off0 = le_stream_info(a, ea)
            if x == y:
                continue
            if (sid, afc) != (sid0, afc0) or x[:-12] != y[:-12]:
                problems.append(('stream changed beyond size/offset', fn))
            streams.append({'name': fn, 'id': sid, 'afc': afc, 'ru_off': off, 'ru_size': size,
                            'en_off': off0, 'en_size': size0, 'pkg': os.path.basename(rel)})
        else:
            if x == y:
                continue
            p, _ = props(y, b.names, False)
            if 'InterpLength' not in p:
                problems.append(('interp without length', fn))
                continue
            _, vo, _ = p['InterpLength']
            extra = len(x) != len(y) or x[:vo] != y[:vo] or x[vo + 4:] != y[vo + 4:]
            if extra:
                problems.append(('interp changed beyond InterpLength (length still applied)', fn))
            pa, _ = props(x, a.names, False)
            if 'InterpLength' not in pa:
                continue
            x = x[:pa['InterpLength'][1]] + x[pa['InterpLength'][1]:]
            vo0 = pa['InterpLength'][1]
            interps.append({'pkg': os.path.basename(rel), 'name': fn, 'extra': extra,
                            'en': struct.unpack_from('<f', x, vo0)[0], 'ru': struct.unpack_from('<f', y, vo)[0]})
            continue
            interps.append({'pkg': os.path.basename(rel), 'name': fn,
                            'en': struct.unpack_from('<f', x, vo)[0], 'ru': struct.unpack_from('<f', y, vo)[0]})
    return rel, streams, interps, problems


_afc_index = None


def afc_path(afc):
    global _afc_index
    if _afc_index is None:
        _afc_index = {}
        for dp, _, fs in os.walk(RU):
            for f in fs:
                if f.lower().endswith('.afc'):
                    _afc_index[f[:-4].lower()] = os.path.join(dp, f)
    return _afc_index[afc.lower()]


def build_le():
    rows = [json.loads(l) for l in open(os.path.join(ROOT, 'manifest.jsonl'), encoding='utf-8')]
    rels = [r['path'][len('Game/ME2/'):] for r in rows if r['path'].lower().endswith('.pcc')]
    streams, interps, problems = [], [], []
    with ProcessPoolExecutor(max_workers=12) as ex:
        for rel, s, it, pr in ex.map(le_job, rels):
            streams += s
            interps += it
            problems += [(rel,) + p for p in pr]
    # dedupe by (name, id): copies in different packages may point at different (identical) appended copies
    import hashlib
    afc_cache = {}

    def blob_hash(afc, off, size):
        path = afc_path(afc)
        if path not in afc_cache:
            afc_cache.clear()
            afc_cache[path] = open(path, 'rb').read()
        return hashlib.sha1(afc_cache[path][off:off + size]).hexdigest()

    streams.sort(key=lambda s: (s['afc'].lower(), s['ru_off']))
    by_key = {}
    conflicts = 0
    for s in streams:
        k = '%s|%d' % (s['name'], s['id'])
        loc = (s['afc'].lower(), s['ru_off'], s['ru_size'], blob_hash(s['afc'], s['ru_off'], s['ru_size']))
        s['hash'] = loc[3]
        if k in by_key:
            if by_key[k]['loc'][3] != loc[3]:
                conflicts += 1
                by_key[k].setdefault('alt', []).append(loc)
            by_key[k]['pkgs'].append(s['pkg'])
        else:
            by_key[k] = {'loc': loc, 'en': (s['en_off'], s['en_size']), 'pkgs': [s['pkg']]}
    os.makedirs(MAP, exist_ok=True)
    json.dump(by_key, open(os.path.join(MAP, 'le_streams.json'), 'w'), indent=0)
    json.dump(interps, open(os.path.join(MAP, 'le_interp.json'), 'w'), indent=0)
    json.dump(streams, open(os.path.join(MAP, 'le_streams_all.json'), 'w'), indent=0)
    uniq_loc = len({v['loc'][3] for v in by_key.values()})
    print('changed stream exports', len(streams), 'unique keys', len(by_key), 'unique ru locations', uniq_loc,
          'key conflicts', conflicts)
    print('interp changes', len(interps), 'unique (name)', len({i['name'] for i in interps}))
    print('problems', len(problems))
    for p in problems[:20]:
        print(' ', p)


if __name__ == '__main__':
    if sys.argv[1] == 'le':
        build_le()


# ---------------------------------------------------------------- 360 side
def x360_job(path):
    import me2_package as XP
    d = XP.decompress(open(path, 'rb').read())
    p = XP.Package(d)
    streams, interps = [], []
    for i, e in enumerate(p.exports):
        c = p.class_name(e)
        if c not in ('WwiseStream', 'InterpData'):
            continue
        data = d[e['offset']:e['offset'] + e['size']]
        fn = p.full_name(i + 1)
        try:
            pr, end = props(data, p.names, True)
        except Exception as ex:  # noqa: BLE001
            streams.append({'name': fn, 'error': repr(ex)}) if c == 'WwiseStream' else None
            continue
        if c == 'WwiseStream':
            sid = struct.unpack_from('>i', data, pr['Id'][1])[0] if 'Id' in pr else None
            afc = name_val(data, pr['Filename'][1], p.names, True) if 'Filename' in pr else None
            flags, cnt, size, off = struct.unpack_from('>IIII', data, len(data) - 16)
            streams.append({'name': fn, 'id': sid, 'afc': afc, 'flags': flags, 'cnt': cnt, 'size': size, 'off': off,
                            'field': e['offset'] + e['size'] - 16})
        elif 'InterpLength' in pr:
            vo = pr['InterpLength'][1]
            interps.append({'name': fn, 'len': struct.unpack_from('>f', data, vo)[0], 'field': e['offset'] + vo})
    return path, streams, interps


def build_x360(dirs):
    paths = []
    for root in dirs:
        for dp, _, fs in os.walk(root):
            for f in fs:
                if f.lower().endswith('_loc_int.xxx'):
                    paths.append(os.path.join(dp, f))
    out = {}
    with ProcessPoolExecutor(max_workers=12) as ex:
        for path, s, it in ex.map(x360_job, paths):
            out[path] = {'streams': s, 'interps': it}
    json.dump(out, open(os.path.join(MAP, 'x360_scan.json'), 'w'), indent=0)
    report(out)


def report(out=None):
    if out is None:
        out = json.load(open(os.path.join(MAP, 'x360_scan.json')))
    le = json.load(open(os.path.join(MAP, 'le_streams.json')))
    le_names = collections.defaultdict(list)
    for k in le:
        le_names[k.rsplit('|', 1)[0]].append(k)
    le_interp = json.load(open(os.path.join(MAP, 'le_interp.json')))
    li = {(i['pkg'].lower().replace('.pcc', ''), i['name']) for i in le_interp}
    st = collections.Counter()
    unmatched_names = collections.Counter()
    for path, v in out.items():
        dlc = 'dlc' in path.lower()
        tag = 'dlc' if dlc else 'base'
        for s in v['streams']:
            if 'error' in s:
                st[tag + ' error'] += 1
                continue
            k = '%s|%d' % (s['name'], s['id'])
            if k in le:
                st[tag + ' matched'] += 1
            elif s['name'] in le_names:
                st[tag + ' name-only (id differs)'] += 1
            else:
                st[tag + ' no russian'] += 1
                unmatched_names[s['name'].split('.')[0]] += 1
        pk = os.path.basename(path).lower().replace('.xxx', '')
        for it in v['interps']:
            if (pk, it['name']) in li:
                st[tag + ' interp matched'] += 1
    print(len(out), 'packages')
    for k, v in sorted(st.items()):
        print(' ', k, v)
    print('top unmatched banks', unmatched_names.most_common(15))


if __name__ == '__main__' and sys.argv[1] == 'x360':
    build_x360(sys.argv[2:])
elif __name__ == '__main__' and sys.argv[1] == 'report':
    report()
