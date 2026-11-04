"""Extracts Xenos shader containers at any byte offset (UE3 serializes them unaligned; XenosRecomp only scans at
4-byte steps) from the given files into <out>/<stem>_<offset>.bin, with XenosRecomp's own validity test.

Usage: python extract_unaligned_containers.py <out dir> <file> [<file> ...]
"""
import struct
import sys
from pathlib import Path

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
total = 0
for f in sys.argv[2:]:
    data = Path(f).read_bytes()
    n = 0
    i = 0
    while True:
        i = data.find(b"\x10\x2A\x11", i)
        if i < 0:
            break
        if i + 0x24 <= len(data):
            flags, virt, phys, _c, _ct, _dt, _so, f1c, f20 = struct.unpack(">9I", data[i:i + 0x24])
            size = virt + phys
            if 0x24 <= virt < 0x10000 and 0 < phys < 0x100000 and size <= len(data) - i and f1c == 0 and f20 == 0:
                (out / f"{Path(f).stem}_{i:08x}.bin").write_bytes(data[i:i + size])
                n += 1
        i += 1
    print(f"{f}: {n} containers")
    total += n
print(f"{total} containers written to {out}")
