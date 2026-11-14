#!/usr/bin/env python3
"""Packs every DLC package of a content store into its own asset pack (ME2, 2026-11-07).

  python tools/build_dlc_packs.py SOURCE OUT [--verify] [--split-mb 3072]

SOURCE is a user data root holding the extracted DLC store (a folder of your own, or an SD card's
switch/me2-deko/me2): <xuid>/<title>/00000002/<package>/... with the license headers in
<xuid>/<title>/Headers/00000002/<package>.header. OUT gets the same tree, but each package folder holds only
<package>.nxpack + <package>.nxidx (tools/build_asset_pack.py --inside layout, the format of the game's pack):

  OUT/0000000000000000/454108CE/00000002/<package>/<package>.nxpack
                                                   <package>.nxidx
  OUT/0000000000000000/454108CE/Headers/00000002/<package>.header     (hardlinked, else copied)

The SDK's ContentManager (content_manager.cpp, 2026-11-07) mounts a DLC package from its pack when both files are
there, and from the loose files otherwise, so packed and loose packages can be mixed. ME2's 23 packages: ~1,400 files
-> 46 pack files + 23 headers, which is what an MTP upload to the SD card cares about. Everything else in SOURCE
(saves of other content types, caches) is NOT copied: OUT is the DLC store only.

Each package is far below 4 GB (ME2's largest: 1.2 GB), so the packs are single files (version 2); --split-mb only
matters for a package over that size. A package whose pack in OUT is newer than all of its files is skipped.
"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_asset_pack as pack  # noqa: E402

MARKETPLACE = '00000002'


def link_or_copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def packages(source):
    """(xuid, title, package name, folder) of every marketplace package in SOURCE."""
    found = []
    for xuid in sorted(os.listdir(source)):
        for title in sorted(os.listdir(os.path.join(source, xuid)) if os.path.isdir(os.path.join(source, xuid)) else []):
            store = os.path.join(source, xuid, title, MARKETPLACE)
            if not os.path.isdir(store):
                continue
            for name in sorted(os.listdir(store)):
                folder = os.path.join(store, name)
                if os.path.isdir(folder):
                    found.append((xuid, title, name, folder))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source', help='user data root with the extracted DLC store (<xuid>/<title>/00000002/...)')
    ap.add_argument('out', help='where to write the packed store (same layout)')
    ap.add_argument('--verify', action='store_true', help='reread every pack and check its data checksum')
    ap.add_argument('--split-mb', type=int, default=3072, help='parts of this many MB for a package over it (FAT32)')
    a = ap.parse_args()
    source, out = os.path.abspath(a.source), os.path.abspath(a.out)
    if source == out or out.startswith(source + os.sep):
        sys.exit('OUT must not be SOURCE or inside it')
    found = packages(source)
    if not found:
        sys.exit(f'no DLC packages under {source} (<xuid>/<title>/{MARKETPLACE}/<package>)')
    files = total = 0
    for i, (xuid, title, name, folder) in enumerate(found, 1):
        target = os.path.join(out, xuid, title, MARKETPLACE, name)
        os.makedirs(target, exist_ok=True)
        print(f'[{i}/{len(found)}] {name}')
        if pack.stale(folder, target, name):
            paths = pack.build(folder, target, name, split_mb=a.split_mb)
        else:
            print('  up to date')
            paths = (os.path.join(target, name + '.nxpack'), os.path.join(target, name + '.nxidx'))
        if a.verify:
            pack.verify(*paths)
        header = os.path.join(source, xuid, title, 'Headers', MARKETPLACE, name + '.header')
        if os.path.isfile(header):
            link_or_copy(header, os.path.join(out, xuid, title, 'Headers', MARKETPLACE, name + '.header'))
        else:
            print(f'  warning: no license header {header}')
        for root, _, names in os.walk(folder):
            files += len(names)
            total += sum(os.path.getsize(os.path.join(root, n)) for n in names)
    written = sum(len(n) for _, _, n in os.walk(out))
    print(f'done: {len(found)} packages, {files} files ({total >> 20} MB) -> {written} files in {out}')


if __name__ == '__main__':
    main()
