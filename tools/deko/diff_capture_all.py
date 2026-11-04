"""For each runtime capture: every container in the given files with the same microcode, and whether one is identical.

Usage: python diff_capture_all.py <captures dir> <file> [<file> ...]
"""
import struct
import sys
from pathlib import Path

containers = []  # (file, start, bytes)
for f in sys.argv[2:]:
    data = Path(f).read_bytes()
    i = 0
    while True:
        i = data.find(b"\x10\x2A\x11", i)
        if i < 0:
            break
        if i % 4 == 0 and i + 12 <= len(data):
            v, p = struct.unpack(">II", data[i + 4:i + 12])
            if 0x24 <= v < 0x10000 and 0 < p < 0x100000 and i + v + p <= len(data):
                containers.append((Path(f).name, i, data[i:i + v + p], v))
        i += 1
by_micro = {}
for name, start, c, v in containers:
    by_micro.setdefault(c[v:], []).append((name, start, c))
exact = micro_only = none = 0
for cap in sorted(Path(sys.argv[1]).iterdir()):
    d = cap.read_bytes()
    v, p = struct.unpack(">II", d[4:12])
    cands = by_micro.get(d[v:v + p], [])
    if any(c == d for _, _, c in cands):
        exact += 1
    elif cands:
        micro_only += 1
        if micro_only <= 4:
            print(f"{cap.name}: {len(cands)} containers with its microcode, header sizes {[len(c) - p for _, _, c in cands]}, "
                  f"capture's {v}")
    else:
        none += 1
print(f"{len(containers)} containers scanned; captures: exact {exact}, same microcode {micro_only}, none {none}")
