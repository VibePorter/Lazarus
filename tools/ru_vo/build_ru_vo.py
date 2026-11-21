"""Build the Russian VO overlay for the ME2 Xbox 360 recomp from the SynthVoiceRu-patched LE2 files.

  python -I build_ru_vo.py plan       -> map/plan.json + report
  python -I build_ru_vo.py convert    -> work/conv/<sha1>.wem (360 Wwise Vorbis, 24 kHz mono, libvorbis q4)
  python -I build_ru_vo.py assemble   -> out/<tree>/... (afc = original + appended Russian; LOC packages patched)

Every 360 WwiseStream that has a Russian LE counterpart (same object name; same package first, then name+Id, then
name only) is repointed to an appended copy of the converted Russian stream in the same afc file; English data stays
where it was. InterpData.InterpLength takes max(360 value, LE Russian value) where the mod lengthened it.
"""
import collections
import hashlib
import json
import os
import struct
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))  # tools/: me2_package.py
import wwvorbis as V  # noqa: E402

ROOT = os.environ.get('RU_VO_WORK') or os.path.join(os.getcwd(), 'ru_vo_work')  # work folder (README.md)
MAP = os.path.join(ROOT, 'map')
CONV = os.path.join(ROOT, 'work', 'conv')
OUT = os.path.join(ROOT, 'out')
LE_RU = os.path.join(ROOT, 'le_ru')
FFMPEG = os.environ.get('FFMPEG', 'ffmpeg')  # with libvorbis
QUALITY = '4'
X360_BASE = os.environ.get('ME2_X360_GAME', '')  # the 360 game data (TU2), BioGame/... inside
X360_DLC = os.environ.get('ME2_X360_DLC', '')  # the 360 DLC package folders
TEMPLATE_UID = 0xB7D746F0
TEMPLATE_ALLOC = (0x451C, 0x46C4)


def le_afc_index():
    idx = {}
    for dp, _, fs in os.walk(LE_RU):
        for f in fs:
            if f.lower().endswith('.afc'):
                idx[f[:-4].lower()] = os.path.join(dp, f)
    return idx


def x360_afc_index():
    idx = collections.defaultdict(list)
    for root in (X360_BASE, X360_DLC):
        for dp, _, fs in os.walk(root):
            for f in fs:
                if f.lower().endswith('.afc'):
                    idx[f[:-4].lower()].append(os.path.join(dp, f))
    return idx


def resolve_afc(idx, name, pkg_path):
    c = idx.get(name.lower(), [])
    if not c:
        return None
    pdir = os.path.dirname(pkg_path).lower()
    for p in c:
        if os.path.dirname(p).lower() == pdir:
            return p
    base = [p for p in c if p.lower().startswith(X360_BASE.lower())]
    return base[0] if base else c[0]


# ------------------------------------------------------------------ plan
def plan():
    scan = json.load(open(os.path.join(MAP, 'x360_scan.json')))
    le = json.load(open(os.path.join(MAP, 'le_streams.json')))
    le_all = json.load(open(os.path.join(MAP, 'le_streams_all.json')))
    le_interp = json.load(open(os.path.join(MAP, 'le_interp.json')))
    per_pkg = {}
    for s in le_all:
        per_pkg[(s['pkg'].lower().rsplit('.', 1)[0], s['name'])] = s['hash']
    by_name = {}
    for k, v in le.items():
        by_name.setdefault(k.rsplit('|', 1)[0], v['loc'][3])
    blobs = {}
    for s in le_all:
        blobs.setdefault(s['hash'], (s['afc'], s['ru_off'], s['ru_size']))
    interp = {}
    interp_by_name = collections.defaultdict(list)
    for i in le_interp:
        interp[(i['pkg'].lower().rsplit('.', 1)[0], i['name'])] = (i['en'], i['ru'])
        interp_by_name[i['name']].append((i['en'], i['ru']))
    idx = x360_afc_index()
    st = collections.Counter()
    refs = []
    lens = []
    missing_afc = collections.Counter()
    for path, v in scan.items():
        pk = os.path.basename(path).lower().rsplit('.', 1)[0]
        for s in v['streams']:
            if 'error' in s or s['afc'] is None:
                continue
            h = per_pkg.get((pk, s['name']))
            how = 'package'
            if h is None:
                k = '%s|%d' % (s['name'], s['id'])
                if k in le:
                    h, how = le[k]['loc'][3], 'name+id'
                elif s['name'] in by_name:
                    h, how = by_name[s['name']], 'name'
            if h is None:
                st['english kept'] += 1
                continue
            afc = resolve_afc(idx, s['afc'], path)
            if afc is None:
                missing_afc[s['afc']] += 1
                st['afc missing'] += 1
                continue
            st['russian by ' + how] += 1
            refs.append({'pkg': path, 'field': s['field'], 'afc': afc, 'hash': h, 'flags': s['flags'],
                         'off': s['off'], 'size': s['size']})
        for it in v['interps']:
            k = (pk, it['name'])
            hit = interp.get(k)
            if hit is None:
                # same conversation lengthened in another LE package: only where its English length agrees
                c = [(en, ru) for en, ru in interp_by_name.get(it['name'], []) if abs(en - it['len']) < 0.05]
                if c:
                    hit = max(c, key=lambda t: t[1])
                    st['interp by name (other package)'] += 1
            if hit is not None:
                en, ru = hit
                new = max(it['len'], ru)
                lens.append({'pkg': path, 'field': it['field'], 'old': it['len'], 'new': new, 'le_en': en})
    need = sorted({r['hash'] for r in refs})
    json.dump({'refs': refs, 'interps': lens, 'blobs': {h: blobs[h] for h in need}},
              open(os.path.join(MAP, 'plan.json'), 'w'))
    for k, n in sorted(st.items()):
        print(' ', k, n)
    print('unique Russian streams to convert', len(need), 'afc files touched', len({r['afc'] for r in refs}),
          'packages touched', len({r['pkg'] for r in refs} | {i['pkg'] for i in lens}))
    d = [abs(i['old'] - i['le_en']) for i in lens]
    print('interp lengths', len(lens), 'lengthened', sum(i['new'] > i['old'] for i in lens),
          '360 vs LE-en |diff|>0.05s', sum(x > 0.05 for x in d))
    if missing_afc:
        print('missing afc', missing_afc.most_common(10))
    # original formats of the replaced 360 streams
    fm = collections.Counter()
    seen = set()
    cache = {}
    for r in refs:
        if (r['afc'], r['off']) in seen:
            continue
        seen.add((r['afc'], r['off']))
        if r['afc'] not in cache:
            cache.clear()
            cache[r['afc']] = open(r['afc'], 'rb').read()
        b = cache[r['afc']][r['off']:r['off'] + 64]
        if b[:4] != b'RIFX':
            fm['not RIFX at offset'] += 1
            continue
        ch, rate = struct.unpack_from('>HI', b, 22)
        fm[(ch, rate)] += 1
    print('replaced 360 stream formats', fm.most_common())


# ------------------------------------------------------------------ convert
_template_setup = None


def template_setup():
    global _template_setup
    if _template_setup is None:
        p = os.path.join(X360_BASE, 'BioGame', 'CookedXenon', 'bchlml_ambientmaster_a_S_INT.afc')
        d = open(p, 'rb').read()
        _template_setup = V.parse_360(d[0x68B21:0x68B21 + 0x7B2A])['setup360']
    return _template_setup


def convert_one(args):
    h, (afc, off, size) = args
    dst = os.path.join(CONV, h + '.wem')
    if os.path.exists(dst):
        return h, 'cached', 0
    src_path = _le_idx[afc.lower()]
    with open(src_path, 'rb') as f:
        f.seek(off)
        blob = f.read(size)
    if hashlib.sha1(blob).hexdigest() != h:
        return h, 'hash mismatch', 0
    ident, com, setup, audio, samples = V.le_to_std(blob)
    ogg = V.std_to_ogg(ident, com, setup, audio, samples)
    r = subprocess.run([FFMPEG, '-hide_banner', '-loglevel', 'error', '-f', 'ogg', '-i', 'pipe:0', '-ar', '24000',
                        '-ac', '1', '-c:a', 'libvorbis', '-q:a', QUALITY, '-f', 'ogg', 'pipe:1'],
                       input=ogg, capture_output=True)
    if r.returncode != 0 or not r.stdout:
        return h, 'ffmpeg: ' + r.stderr.decode(errors='replace')[:200], 0
    pk = V.ogg_read(r.stdout)
    ch, rate, bs0, bs1 = V.parse_ident(pk[0][0])
    setup_std = pk[2][0]
    s360 = V.strip_setup_for_360(setup_std, ch)
    if s360 != template_setup() or (ch, rate, bs0, bs1) != (1, 24000, 9, 10):
        return h, 'setup differs from the 360 VO template', 0
    audio = [p for p, g in pk[3:]]
    total = pk[-1][1]
    modes = V.setup_modes(setup_std, ch)
    gr = V.granules(audio, modes, bs0, bs1)
    gr[-1] = total
    out = V.build_360(ch, rate, bs0, bs1, s360, audio, gr, total, TEMPLATE_UID, TEMPLATE_ALLOC)
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(out)
    os.replace(tmp, dst)
    return h, 'ok', total / rate - samples / V.parse_ident(ident)[1]


_le_idx = None


def _init():
    global _le_idx
    _le_idx = le_afc_index()


def convert():
    os.makedirs(CONV, exist_ok=True)
    p = json.load(open(os.path.join(MAP, 'plan.json')))
    jobs = sorted(p['blobs'].items())
    if len(sys.argv) > 2:
        jobs = jobs[:int(sys.argv[2])]
    st = collections.Counter()
    bad = []
    dd = []
    with ProcessPoolExecutor(max_workers=14, initializer=_init) as ex:
        for i, (h, res, ddur) in enumerate(ex.map(convert_one, jobs, chunksize=8)):
            st[res if res in ('ok', 'cached') else 'error'] += 1
            if res not in ('ok', 'cached'):
                bad.append((h, res))
            elif res == 'ok':
                dd.append(abs(ddur))
            if i % 2000 == 0:
                print(i, dict(st), flush=True)
    print('DONE', dict(st), 'max |duration change| %.4fs' % (max(dd) if dd else 0))
    for b in bad[:20]:
        print(' ', b)


# ------------------------------------------------------------------ assemble
def out_path(src):
    src = os.path.normpath(src)
    for root, tag in ((X360_BASE, 'game'), (X360_DLC, 'dlc')):
        if src.lower().startswith(os.path.normpath(root).lower()):
            return os.path.join(OUT, tag, os.path.relpath(src, root))
    raise ValueError(src)


def rebuild_package(path, patches):
    """Apply (offset in decompressed data -> bytes) patches; recompress only the LZO blocks they touch."""
    import lzo1x
    import me2_package as XP
    raw = open(path, 'rb').read()
    s = XP.summary(raw)
    dec = bytearray(XP.decompress(raw))
    for off, b in patches:
        dec[off:off + len(b)] = b
    if s['compression'] != 2:
        raise ValueError('not LZO')
    chunks = []
    changed_blocks = 0
    for uo, us, co, cs in s['chunks']:
        r = XP.Reader(raw, co)
        assert r.u32() == XP.TAG
        block, csum, usum = r.u32(), r.u32(), r.u32()
        blocks = []
        left = usum
        while left > 0:
            bc, bu = r.u32(), r.u32()
            blocks.append([bc, bu])
            left -= bu
        o = r.o
        pos = uo
        datas = []
        for bc, bu in blocks:
            old = raw[o:o + bc]
            o += bc
            datas.append(old)
            pos += bu
        chunks.append([uo, us, block, blocks, datas])
    # find touched blocks
    touched = set()
    for off, b in patches:
        for ci, (uo, us, block, blocks, datas) in enumerate(chunks):
            pos = uo
            for bi, (bc, bu) in enumerate(blocks):
                if off < pos + bu and off + len(b) > pos:
                    touched.add((ci, bi))
                pos += bu
    for ci, bi in touched:
        uo, us, block, blocks, datas = chunks[ci]
        pos = uo + sum(bu for _, bu in blocks[:bi])
        bu = blocks[bi][1]
        plain = bytes(dec[pos:pos + bu])
        c = lzo1x.compress(plain)
        assert bytes(XP.lzo1x_decompress(c, bu)) == plain
        datas[bi] = c
        blocks[bi][0] = len(c)
        changed_blocks += 1
    # lay out: summary (with updated chunk table) then chunks in original order
    first_co = min(co for _, _, co, _ in s['chunks'])
    out = bytearray(raw[:first_co])
    table_pos = s['summary_end'] - 16 * len(s['chunks'])
    for ci, (uo, us, block, blocks, datas) in enumerate(chunks):
        co = len(out)
        hdr = struct.pack('>IIII', XP.TAG, block, sum(len(d) for d in datas), sum(bu for _, bu in blocks))
        hdr += b''.join(struct.pack('>II', bc, bu) for bc, bu in blocks)
        body = hdr + b''.join(datas)
        out += body
        struct.pack_into('>IIII', out, table_pos + 16 * ci, uo, us, co, len(body))
    # verify
    check = XP.decompress(bytes(out))
    assert check == bytes(dec), 'rebuilt package does not decompress to the patched data'
    return bytes(out), changed_blocks


def assemble():
    p = json.load(open(os.path.join(MAP, 'plan.json')))
    refs, interps = p['refs'], p['interps']
    # afc layout: original + appended converted streams (one copy per afc per Russian stream)
    by_afc = collections.defaultdict(list)
    for r in refs:
        by_afc[r['afc']].append(r)
    new_loc = {}
    total_app = 0
    for afc, rs in sorted(by_afc.items()):
        orig = open(afc, 'rb').read()
        out = bytearray(orig)
        placed = {}
        for r in rs:
            h = r['hash']
            if h not in placed:
                w = open(os.path.join(CONV, h + '.wem'), 'rb').read()
                placed[h] = (len(out), len(w))
                out += w
            new_loc[(r['pkg'], r['field'])] = placed[h]
        total_app += len(out) - len(orig)
        dst = out_path(afc)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(out)
    print('afc files written', len(by_afc), 'appended %.1f MB' % (total_app / 1e6))
    # packages
    patches = collections.defaultdict(list)
    for r in refs:
        off, size = new_loc[(r['pkg'], r['field'])]
        patches[r['pkg']].append((r['field'], struct.pack('>IIII', r['flags'], size, size, off)))
    for i in interps:
        if i['new'] > i['old']:
            patches[i['pkg']].append((i['field'], struct.pack('>f', i['new'])))
    nblocks = 0
    for pkg, pl in sorted(patches.items()):
        data, nb = rebuild_package(pkg, pl)
        nblocks += nb
        dst = out_path(pkg)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(data)
    print('packages written', len(patches), 'LZO blocks recompressed', nblocks)


if __name__ == '__main__':
    {'plan': plan, 'convert': convert, 'assemble': assemble}[sys.argv[1]]()
