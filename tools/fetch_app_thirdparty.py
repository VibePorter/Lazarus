"""Fetches the unmodified upstream libraries inside app/thirdparty that the repository does not carry.

plume and XenosRecomp are tracked with this port's changes, but their own submodules (Vulkan headers, memory
allocator, volk; zstd, fmt, xxHash, smol-v and the DXC binaries) are upstream code, ~300 MB, the DXC libraries alone
larger than GitHub accepts. This script clones each at the exact commit the tested builds used and copies it in place.
(D3D12MemoryAllocator matches no upstream commit exactly, so its sources stay in the repository.)
Files already there (the ones this port changed are tracked in git) are kept; --force overwrites them.

Usage:
    python tools/fetch_app_thirdparty.py [--force] [--only NAME ...]
"""
import argparse
import os
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, 'app', 'thirdparty')

# (folder under app/thirdparty, repository, commit). Commits found by matching the files of the tested tree against
# each repository's history (2026-11-04): the closest commit, plus the files that differ from it, tracked in git.
LIBS = [
    ('plume/contrib/volk', 'https://github.com/zeux/volk.git',
     'be3dbd49bf77052665e96b6c7484af855e7e5f67'),  # volk.c: tracked (changed)
    ('plume/contrib/Vulkan-Headers', 'https://github.com/KhronosGroup/Vulkan-Headers.git',
     '2fa203425eb4af9dfc6b03f97ef72b0b5bcb8350'),  # v1.4.335
    ('plume/contrib/VulkanMemoryAllocator', 'https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git',
     '9d17673c9cd64494933370362f989ee55b2a543f'),
    ('reblue-XenosRecomp/thirdparty/zstd', 'https://github.com/facebook/zstd.git',
     'f7a8bb1263448e5028aceeba606a08fe3809550f'),
    ('reblue-XenosRecomp/thirdparty/fmt', 'https://github.com/fmtlib/fmt.git',
     '873670ba3f9e7bc77ec2c1c94b04f1f8bef77e9f'),  # include/fmt/format.h: tracked (changed)
    ('reblue-XenosRecomp/thirdparty/xxHash', 'https://github.com/Cyan4973/xxHash.git',
     '2bf8313b934633b2a5b7e8fd239645b85e10c852'),
    ('reblue-XenosRecomp/thirdparty/smol-v', 'https://github.com/aras-p/smol-v.git',
     '9dd54c379ac29fa148cb1b829bb939ba7381d8f4'),
    ('reblue-XenosRecomp/thirdparty/dxc-bin', 'https://github.com/renderbag/dxc-bin.git',
     '12395e36295f950361ca842d8cc461492412facc'),
]


def run(*args, cwd=None):
    print('>', ' '.join(args))
    subprocess.run(args, cwd=cwd, check=True)


def remove_readonly(func, path, _):
    os.chmod(path, stat.S_IWRITE)
    func(path)


def fetch(folder, url, commit, force):
    """Clones url at commit and copies every file the target folder does not have yet (all of them with force).
    Files this port changed are tracked in git and so already there: they are kept."""
    target = os.path.join(TP, *folder.split('/'))
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        work = os.path.join(tmp, 'repo')
        run('git', 'init', '-q', work)
        run('git', 'remote', 'add', 'origin', url, cwd=work)
        run('git', 'fetch', '-q', '--depth', '1', 'origin', commit, cwd=work)
        run('git', 'checkout', '-q', 'FETCH_HEAD', cwd=work)
        copied = kept = 0
        for d, dirs, names in os.walk(work):
            dirs[:] = [x for x in dirs if x != '.git']
            for n in names:
                src = os.path.join(d, n)
                dst = os.path.join(target, os.path.relpath(src, work))
                if os.path.lexists(dst) and not force:
                    kept += 1
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst, follow_symlinks=False)
                copied += 1
    print(f'{folder}: {commit[:12]}: {copied} files copied, {kept} kept')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--force', action='store_true', help='overwrite files that already exist (also the tracked, changed ones)')
    ap.add_argument('--only', nargs='*', help='folder names to fetch (e.g. volk dxc-bin)')
    a = ap.parse_args()
    for folder, url, commit in LIBS:
        if a.only and os.path.basename(folder) not in a.only:
            continue
        fetch(folder, url, commit, a.force)
    # The DXC binary must be executable for the Linux build of the shader tools.
    dxc = os.path.join(TP, 'reblue-XenosRecomp', 'thirdparty', 'dxc-bin', 'bin', 'x64', 'dxc-linux')
    if os.path.isfile(dxc):
        os.chmod(dxc, os.stat(dxc).st_mode | 0o111)
    return 0


if __name__ == '__main__':
    sys.exit(main())
