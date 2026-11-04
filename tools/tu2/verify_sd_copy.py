"""Compare the SD card's copy of the game with the PC folder it was copied from (2026-11-03: a console me2.toml came
back as random bytes after a copy).

  python verify_sd_copy.py <SD card's switch\\me2-deko folder> [PC folder, default C:\\me2-deko-tu2\\switch\\me2-deko]

Every file of the PC folder must be on the card with the same size and the same bytes: files up to 64 MB are hashed
whole; bigger ones by their first, middle and last 4 MB. Files the game writes (logs, caches, saves, me2_gyro.cfg) are
skipped. Prints the bad and missing files and a summary.
"""
import hashlib
import os
import sys

SKIP_DIRS = {'logs', 'cache'}
SKIP_FILES = {'me2_gyro.cfg', 'deko3d.log', 'pipeline_list.bin'}
BIG = 64 << 20
PIECE = 4 << 20


def digest(path, size):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        if size <= BIG:
            for block in iter(lambda: f.read(1 << 20), b''):
                h.update(block)
        else:
            for at in (0, size // 2, size - PIECE):
                f.seek(at)
                h.update(f.read(PIECE))
    return h.hexdigest()


def main():
    sd = sys.argv[1]
    pc = sys.argv[2] if len(sys.argv) > 2 else r'C:\me2-deko-tu2\switch\me2-deko'
    bad, missing, ok = [], [], 0
    for root, dirs, files in os.walk(pc):
        rel_root = os.path.relpath(root, pc)
        dirs[:] = [d for d in dirs if d.lower() not in SKIP_DIRS]
        for name in files:
            if name in SKIP_FILES or name.lower().endswith('.log'):
                continue
            rel = os.path.normpath(os.path.join(rel_root, name))
            a, b = os.path.join(pc, rel), os.path.join(sd, rel)
            if not os.path.exists(b):
                missing.append(rel)
                continue
            sa, sb = os.path.getsize(a), os.path.getsize(b)
            if sa != sb or digest(a, sa) != digest(b, sb):
                bad.append('%s (PC %d bytes, SD %d bytes)' % (rel, sa, sb))
            else:
                ok += 1
            if (ok + len(bad)) % 500 == 0:
                print('  ... %d files checked' % (ok + len(bad)), flush=True)
    for x in missing:
        print('MISSING', x)
    for x in bad:
        print('DIFFERENT', x)
    print('%d files identical, %d different, %d missing' % (ok, len(bad), len(missing)))
    for old in ('game.nxpack', 'game.nxidx'):
        if os.path.exists(os.path.join(sd, old)):
            print('WARNING: %s is still on the card: the game would use it instead of the game folder' % old)
    sys.exit(1 if bad or missing else 0)


if __name__ == '__main__':
    main()
