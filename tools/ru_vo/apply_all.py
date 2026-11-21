"""Apply the SynthVoiceRu xdelta patches to an LE2 install, verifying hashes.

python -I apply_all.py MANIFEST PATCH_ROOT LE_GAME_ME2_DIR OUT_DIR
Output mirrors the manifest path minus the leading 'Game/ME2/'.
"""
import hashlib
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vcdiff  # noqa: E402


def find_ci(path):
    if os.path.exists(path):
        return path
    d, fn = os.path.split(path)
    for x in os.listdir(d):
        if x.lower() == fn.lower():
            return os.path.join(d, x)
    raise FileNotFoundError(path)


def job(args):
    row, patch_root, le_root, out_root = args
    rel = row["path"][len("Game/ME2/"):]
    dst = os.path.join(out_root, rel)
    if os.path.exists(dst):
        h = hashlib.sha256(open(dst, "rb").read()).hexdigest()
        if h == row["new_sha256"]:
            return rel, "skip"
    src = open(find_ci(os.path.join(le_root, rel)), "rb").read()
    if hashlib.sha256(src).hexdigest() != row["base_sha256"]:
        return rel, "BASE MISMATCH"
    patch = open(os.path.join(patch_root, row["patch_file"]), "rb").read()
    out = vcdiff.decode(src, patch)
    if hashlib.sha256(out).hexdigest() != row["new_sha256"]:
        return rel, "OUT MISMATCH"
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".tmp", "wb") as f:
        f.write(out)
    os.replace(dst + ".tmp", dst)
    return rel, "ok"


def main():
    man, patch_root, le_root, out_root = sys.argv[1:5]
    rows = [json.loads(l) for l in open(man, encoding="utf-8")]
    rows.sort(key=lambda r: -os.path.getsize(os.path.join(patch_root, r["patch_file"])))
    stats = {}
    bad = []
    with ProcessPoolExecutor(max_workers=max(1, (os.cpu_count() or 4) - 2)) as ex:
        for i, (rel, st) in enumerate(ex.map(job, [(r, patch_root, le_root, out_root) for r in rows])):
            stats[st] = stats.get(st, 0) + 1
            if st not in ("ok", "skip"):
                bad.append((st, rel))
            if i % 100 == 0:
                print(i, stats, flush=True)
    print("DONE", stats)
    for b in bad:
        print(*b)


if __name__ == "__main__":
    main()
