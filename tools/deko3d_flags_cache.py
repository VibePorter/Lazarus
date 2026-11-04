"""DKSH cache for other uam flags, compiled on the PC.

The renderer keeps the code uam makes with flags other than the default 1471 in a cache of its own,
deko3d_shaders_f<flags>.bin next to the NRO (banjo_renut_deko3d.cpp, LoadCache; the flags come from the
banjo_deko3d_uam_* cvars, Deko3DUamFlags). Without one, every shader the game meets is compiled on the device first
(minutes in Ryujinx). This compiles every default variant's GLSL with UAM_BANJO_FLAGS=<flags> (the uam tool reads it,
uam/source/main.cpp) and writes that file: records keyed like deko3d_shaders.bin's (fnv1a64 of the GLSL), then any
records of an existing file of that name that are not among them (the variants the device compiled).

  python tools/deko3d_flags_cache.py --flags 158 --glsl out/me2-deko-shaders/conversion/glsl --out <dir>
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deko3d_variant_cache  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UAM = os.path.join(ROOT, 'out', 'deko3d_tools', 'uam-build', 'uam.exe')


def compile_one(glsl, flags, work):
    stem = os.path.splitext(os.path.basename(glsl))[0]
    stage = 'frag' if stem.startswith('ps_') else 'vert'
    dksh = os.path.join(work, stem + '.dksh')
    env = dict(os.environ, UAM_BANJO_FLAGS=str(flags))
    r = subprocess.run([UAM, '-s', stage, '-o', dksh, glsl], capture_output=True, text=True, timeout=300, env=env)
    if r.returncode != 0 or not os.path.isfile(dksh):
        return stem, None, None
    data = open(dksh, 'rb').read()
    os.remove(dksh)
    return stem, deko3d_variant_cache.fnv1a64(open(glsl, 'rb').read()), data


def read_records(path):
    out = {}
    if not os.path.isfile(path):
        return out
    b = open(path, 'rb').read()
    if b[:4] != b'BDKC' or b[8:24] != deko3d_variant_cache.TAG:
        return out
    at = 24
    while at + 12 <= len(b):
        key, size = struct.unpack_from('<QI', b, at)
        if at + 12 + size > len(b):
            break
        out[key] = b[at + 12:at + 12 + size]
        at += 12 + size
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--flags', type=int, required=True)
    ap.add_argument('--glsl', required=True)
    ap.add_argument('--out', required=True, help='folder of the NRO: deko3d_shaders_f<flags>.bin is written there')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    a = ap.parse_args()
    glsls = sorted(os.path.join(a.glsl, f) for f in os.listdir(a.glsl) if f.endswith('.glsl'))
    target = os.path.join(a.out, f'deko3d_shaders_f{a.flags}.bin')
    records, failed = {}, 0
    with tempfile.TemporaryDirectory() as work, concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for i, (stem, key, data) in enumerate(ex.map(lambda g: compile_one(g, a.flags, work), glsls)):
            if key is None:
                failed += 1
            else:
                records[key] = data
            if (i + 1) % 2000 == 0:
                print(f'{i + 1}/{len(glsls)}', flush=True)
    kept = {k: v for k, v in read_records(target).items() if k not in records}
    out = bytearray(b'BDKC' + struct.pack('<I', 1) + deko3d_variant_cache.TAG)
    for key, data in sorted(records.items()):
        out += struct.pack('<QI', key, len(data)) + data
    for key, data in kept.items():
        out += struct.pack('<QI', key, len(data)) + data
    with open(target, 'wb') as f:
        f.write(out)
    print(f'{target}: {len(records)} compiled ({failed} failed), {len(kept)} kept from the old file, '
          f'{len(out) >> 20} MB')


if __name__ == '__main__':
    main()
