"""me2_scale34_patch.py LIST: (LIST = the conversion GLSL files containing ">> 26u) & 7u", one per line) the deko3d GLSL of the shaders that read the render-scale code (bits 26-28 of a descriptor word):
code 4 = 3/4 (540p) instead of 3x (2160p, never used on the Switch). Backs up glsl + dksh, patches, recompiles the
default variant's DKSH with uam (as tools/deko3d_shaders.py does)."""
import os
import re
import shutil
import subprocess
import sys

ROOT = __import__('os').path.dirname(__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
CONV = ROOT + '/out/me2-deko-shaders/conversion'
UAM = ROOT + '/out/deko3d_tools/uam-build/uam.exe'
BACKUP = ROOT + '/out/me2-deko-shaders/scale34_backup'
PAT = re.compile(r'\((_\d+) == 4u\) \? 3\.0 :')

names = [os.path.basename(l.strip()) for l in open(sys.argv[1]) if l.strip()]
os.makedirs(BACKUP + '/glsl', exist_ok=True)
os.makedirs(BACKUP + '/dksh', exist_ok=True)
for n in names:
    stem = n[:-5]
    g = f'{CONV}/glsl/{n}'
    d = f'{CONV}/dksh/{stem}.dksh'
    text = open(g, encoding='utf-8', newline='').read()
    codes = len(re.findall(r'>> 26u\) & 7u', text))
    new, k = PAT.subn(lambda m: f'({m.group(1)} == 4u) ? 0.75 :', text)
    if k == 0:
        print(stem, 'already patched' if '== 4u) ? 0.75 :' in text else 'NO MATCH', codes)
        continue
    if not os.path.exists(f'{BACKUP}/glsl/{n}'):
        shutil.copy2(g, f'{BACKUP}/glsl/{n}')
        if os.path.exists(d):
            shutil.copy2(d, f'{BACKUP}/dksh/{stem}.dksh')
    open(g, 'w', encoding='utf-8', newline='').write(new)
    stage = 'frag' if stem.startswith('ps_') else 'vert'
    r = subprocess.run([UAM, '-s', stage, '-o', d, g], capture_output=True, text=True, timeout=300)
    print(stem, f'{k} of {codes} scale reads patched;', 'uam ok' if r.returncode == 0 and os.path.exists(d)
          else 'uam FAILED ' + (r.stdout + r.stderr)[:300], os.path.getsize(d) if os.path.exists(d) else 0)
