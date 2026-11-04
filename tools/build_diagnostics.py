"""Build physical-device diagnostics in the existing WSL mirror and deploy me2_opt.nro.

Run with WSL Python. Keeps the configured Release/LTO/PGO optimization settings.
No emulator/game launch or smoke test. The active SD TOML is preserved unless --config is supplied.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time

from deploy_switch import deploy


def main():
    if os.name == 'nt':
        raise SystemExit('Run this script with WSL Python; tools/win/build_switch.ps1 is the native build entry point.')
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, default=Path('out/diagnostics_latest'))
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--config', type=Path)
    a = ap.parse_args()
    if a.jobs < 1:
        ap.error('--jobs must be positive')
    mirror = Path('/root/me2deko')
    out = a.output if a.output.is_absolute() else root / a.output
    out.mkdir(parents=True, exist_ok=True)
    names = set()
    for patch in (root / 'out/fixes').rglob('*.patch'):
        for line in patch.read_text().splitlines():
            if line.startswith('+++ b/'):
                names.add(line[6:])
    for directory in ['app/src', 'app/me2_src', 'app/thirdparty/plume', 'sdk/src/input']:
        names.update(p.relative_to(root).as_posix() for p in (root / directory).rglob('*')
                     if p.is_file() and p.suffix in {'.cpp', '.c', '.h', '.hlsl', '.hlsli', '.inc', '.cmake', '.txt'})
    names.update(['app/CMakeLists.txt', 'app/src/renut_engine/gpu/gpu.cmake',
                  'sdk/src/kernel/xam/xam_content.cpp', 'sdk/src/system/xam/content_manager.cpp',
                  'AGENTS.md', 'tools/deploy_switch.py', 'tools/build_diagnostics.py',
                  'tools/post_codegen.py', 'tools/me2_option_hooks.py'])
    reroutes = root / 'out/fixes/traversal_perf_20261029/generated_reroutes.json'
    if reroutes.exists():
        for item in json.loads(reroutes.read_text()):
            names.add(item['path'].replace('\\', '/'))
    changed, hashes = [], {}
    for name in sorted(names):
        source, destination = root / name, mirror / name
        if not source.is_file():
            continue
        data = source.read_bytes()
        hashes[name] = hashlib.sha256(data).hexdigest()
        if not destination.is_file() or destination.read_bytes() != data:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            changed.append(name)
    (out / 'synced_sources.json').write_text(json.dumps(changed, indent=2) + '\n')
    (out / 'source_hashes.json').write_text(json.dumps(hashes, indent=2) + '\n')
    print('Synchronized', len(changed), 'changed files; building optimized physical-device diagnostics.', flush=True)
    with (out / 'configure.log').open('w') as log:
        subprocess.run(['cmake', '-S', str(mirror / 'app'), '-B', str(mirror / 'out/nx'),
                        '-DREX_SWITCH_INPUT_DIAGNOSTICS=OFF'], cwd=mirror,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    started = time.monotonic()
    with (out / 'build.log').open('w') as log:
        result = subprocess.run(['cmake', '--build', str(mirror / 'out/nx'), '--target', 'sa2', '-j', str(a.jobs)],
                                cwd=mirror, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((out / 'build.log').read_text()[-6000:], flush=True)
        raise SystemExit(result.returncode)
    elapsed = time.monotonic() - started
    for source, name in [('sa2.nro', 'me2_opt.nro'), ('sa2', 'me2_opt.elf')]:
        shutil.copy2(mirror / 'out/nx' / source, out / name)
    record = deploy(out / 'me2_opt.nro', out / 'me2_opt.elf', config=a.config)
    record.update(build_seconds=round(elapsed, 2), build_tree=str(mirror / 'out/nx'),
                  diagnostic_input_file_control='not compiled; physical-device diagnostics')
    (out / 'build_manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2), flush=True)


if __name__ == '__main__':
    main()
