"""Merge Mass Effect 2 disc trees into one game root using hard links.

Disc 1 is linked first; files from later discs are added when missing. Files present on
several discs are compared by size + SHA-1, and differing ones are reported (disc 1 wins).
"""
import hashlib, os, sys


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main(out, *discs):
    seen = {}  # lowercase relpath -> source path
    linked = same = differ = 0
    for disc in discs:
        for dirpath, _, files in os.walk(disc):
            for name in files:
                src = os.path.join(dirpath, name)
                rel = os.path.relpath(src, disc)
                key = rel.lower()
                if key in seen:
                    other = seen[key]
                    if os.path.getsize(other) == os.path.getsize(src) and sha1(other) == sha1(src):
                        same += 1
                    else:
                        differ += 1
                        print(f"DIFFER: {rel} ({os.path.getsize(other)} vs {os.path.getsize(src)})")
                    continue
                seen[key] = src
                dst = os.path.join(out, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if not os.path.exists(dst):
                    os.link(src, dst)
                linked += 1
    print(f"linked={linked} identical_dupes={same} differing_dupes={differ}")


main(*sys.argv[1:])
