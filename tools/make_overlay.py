#!/usr/bin/env python3
"""Makes a language overlay (e.g. game_ru) from two editions of Mass Effect 2: the files of the other edition that
differ from the base game, plus that edition's Title Update 2 files.

  python tools/make_overlay.py <base game> <other edition> <overlay out> [--tu2 <other edition's TU2 disc001 folder>]

  <base game>       the English game folder (both discs merged, as on the SD card's game/)
  <other edition>   the other edition, both discs merged the same way (tools/merge_discs.py)
  --tu2             the extracted Title Update 2 of the other edition (tools/stfs_extract.py): its default.xexp goes
                    to the overlay root, its BioGame_Update_*.tlk to update/

For the Russian release the result is 7 files, ~27 MB: default.xex, Unreap.xex, BioGame/CookedXenon/BIOGame_RUS.tlk,
Engine.xxx, SFXGame.xxx, default.xexp and update/BioGame_Update_RUS.tlk. The runtime lays the overlay over the base
game (game_overlay_root = "/switch/me2-deko/game_ru"); the game then picks its language from the overlay's default.xex.
Files are compared by size, then content. Files only in the base stay visible (the overlay cannot hide them; the
English text files are simply not used by the Russian program). Nothing in either input is changed.
"""
import argparse
import filecmp
import os
import shutil
import sys


def files(root):
    out = {}
    for d, _, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            out[os.path.relpath(p, root).replace(os.sep, '/').lower()] = p
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base')
    ap.add_argument('other')
    ap.add_argument('out')
    ap.add_argument('--tu2', help="the other edition's Title Update 2 folder (disc001)")
    a = ap.parse_args()
    for p in (a.base, a.other):
        if not os.path.isfile(os.path.join(p, 'default.xex')):
            sys.exit(f'{p}: no default.xex (pass the merged game folder)')

    base, other = files(a.base), files(a.other)
    picked = []
    for key, src in sorted(other.items()):
        rel = os.path.relpath(src, a.other)
        if rel.split(os.sep)[0].lower() in ('$systemupdate', 'update') or key == 'default.xexp':
            continue  # not game data / the base's TU2 files are replaced below
        b = base.get(key)
        if b and os.path.getsize(b) == os.path.getsize(src) and filecmp.cmp(b, src, shallow=False):
            continue
        picked.append((rel, src))

    if a.tu2:
        xexp = os.path.join(a.tu2, 'default.xexp')
        if not os.path.isfile(xexp):
            sys.exit(f'{a.tu2}: no default.xexp (pass the disc001 folder of the extracted update)')
        picked.append(('default.xexp', xexp))
        for n in sorted(os.listdir(a.tu2)):
            if n.lower().startswith('biogame_update_') and n.lower().endswith('.tlk'):
                picked.append((os.path.join('update', n), os.path.join(a.tu2, n)))

    total = 0
    for rel, src in picked:
        dst = os.path.join(a.out, rel)
        os.makedirs(os.path.dirname(dst) or a.out, exist_ok=True)
        shutil.copyfile(src, dst)
        total += os.path.getsize(src)
        print(f'{os.path.getsize(src):12d}  {rel.replace(os.sep, "/")}')
    print(f'{len(picked)} files, {total / 1e6:.1f} MB -> {a.out}')
    if not a.tu2:
        print('note: no --tu2: add the edition\'s Title Update 2 (default.xexp, update/*.tlk) before playing')
    return 0


if __name__ == '__main__':
    sys.exit(main())
