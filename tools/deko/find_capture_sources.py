"""Where do Renut's runtime shader captures come from? Searches files for each capture's microcode (physical part).

Usage: python find_capture_sources.py <captures dir> <file> [<file> ...]
"""
import struct
import sys
from pathlib import Path

caps = Path(sys.argv[1])
micro = {}
for p in caps.iterdir():
    d = p.read_bytes()
    virt, phys = struct.unpack(">II", d[4:12])
    m = d[virt:virt + phys]
    if len(m) >= 32:
        micro[p.name] = m
for f in sys.argv[2:]:
    data = Path(f).read_bytes()
    hits = [n for n, m in micro.items() if data.find(m) >= 0]
    print(f"{f}: {len(hits)} of {len(micro)} captures' microcode found")
