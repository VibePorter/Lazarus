"""Deploy a completed Switch build to the user's canonical SD package directory.

Does not build or launch the game. The active TOML is preserved unless --config is supplied.
"""
from pathlib import Path
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import shutil


def workspace():
    # The repository root (tools/..), or LAZARUS_WORKSPACE when set.
    env = os.environ.get('LAZARUS_WORKSPACE')
    return Path(env) if env else Path(__file__).resolve().parent.parent


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def checked_copy(source, target):
    source, target = Path(source), Path(target)
    expected = digest(source)
    if target.exists() and digest(target) == expected:
        return expected
    target.parent.mkdir(parents=True, exist_ok=True)
    stage = target.with_name(target.name + '.deploying')
    shutil.copyfile(source, stage)
    if digest(stage) != expected:
        raise RuntimeError('Copy verification failed: ' + str(target))
    os.replace(stage, target)
    return expected


def deploy(nro, elf, sd_dir=None, config=None):
    nro, elf = Path(nro).resolve(), Path(elf).resolve()
    sd_dir = Path(sd_dir) if sd_dir else workspace() / 'out/sd/switch/me2-deko'
    with nro.open('rb') as f:
        f.seek(16)
        if f.read(4) != b'NRO0':
            raise ValueError('Not an NRO: ' + str(nro))
    with elf.open('rb') as f:
        if f.read(4) != b'\x7fELF':
            raise ValueError('Not an ELF: ' + str(elf))
    if config:
        config = Path(config).resolve()
        if not config.is_file():
            raise FileNotFoundError(config)
    sha = digest(nro)
    archive = workspace() / 'out/deployments' / sha
    # Preserve the exact ELF before replacing the runnable binary; later rebuilds may overwrite the build tree.
    elf_copy = archive / 'me2_opt.elf'
    elf_sha = checked_copy(elf, elf_copy)
    sd_dir.mkdir(parents=True, exist_ok=True)
    target = sd_dir / 'me2_opt.nro'
    previous_sha = digest(target) if target.exists() else None
    previous_copy = None
    if previous_sha and previous_sha != sha:
        previous_copy = workspace() / 'out/deployments' / previous_sha / 'me2_opt.nro'
        checked_copy(target, previous_copy)
    checked_copy(nro, target)
    if config:
        active = sd_dir / 'me2.toml'
        if active.exists() and digest(active) != digest(config):
            checked_copy(active, archive / ('me2.toml.before-' + digest(active)[:12]))
        checked_copy(config, active)
    record = dict(deployed_utc=datetime.now(timezone.utc).isoformat(), nro=str(target),
                  nro_bytes=target.stat().st_size, nro_sha256=sha, source_nro=str(nro),
                  elf=str(elf_copy), elf_sha256=elf_sha, previous_nro_sha256=previous_sha,
                  previous_nro_backup=str(previous_copy) if previous_copy else None,
                  config_copied=bool(config), game_launched=False)
    text = json.dumps(record, indent=2) + '\n'
    (archive / 'deployment.json').write_text(text)
    (sd_dir / 'me2_opt.build.json').write_text(text)
    assert digest(target) == sha
    return record


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--nro', type=Path, required=True)
    ap.add_argument('--elf', type=Path, required=True)
    ap.add_argument('--sd-dir', type=Path)
    ap.add_argument('--config', type=Path)
    a = ap.parse_args()
    print(json.dumps(deploy(a.nro, a.elf, a.sd_dir, a.config), indent=2))
