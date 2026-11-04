"""Compares Renut's runtime shader captures with the scanned ME2 containers.

For each capture: exact match, same microcode with a different header (container differs only before the shader
offset), or no match. Usage: python capture_vs_containers.py <captures dir> <containers dir>
"""
import struct
import sys
from collections import defaultdict
from pathlib import Path


def parse(data: bytes):
    # ShaderContainer: flags, virtualSize, physicalSize, fieldC, constantTableOffset, definitionTableOffset,
    # shaderOffset, field1C, field20 (big endian)
    if len(data) < 0x24:
        return None
    f = struct.unpack(">9I", data[:0x24])
    return {"flags": f[0], "virt": f[1], "phys": f[2], "shader_off": f[6]}


caps = Path(sys.argv[1])
conts = Path(sys.argv[2])
by_bytes = {}
by_phys = defaultdict(list)
for p in conts.iterdir():
    d = p.read_bytes()
    by_bytes[d] = p.name
    h = parse(d)
    if h:
        by_phys[d[h["virt"]:h["virt"] + h["phys"]]].append(p.name)
exact = phys_only = none = 0
for p in sorted(caps.iterdir()):
    d = p.read_bytes()
    h = parse(d)
    if d in by_bytes:
        exact += 1
        continue
    micro = d[h["virt"]:h["virt"] + h["phys"]] if h else b""
    if micro in by_phys:
        phys_only += 1
        if phys_only <= 3:
            other = (conts / by_phys[micro][0]).read_bytes()
            diff = [i for i in range(min(len(d), len(other), h["virt"])) if d[i] != other[i]]
            print(f"{p.name}: microcode matches {by_phys[micro][0]}; header bytes differ at {diff[:16]} "
                  f"(sizes {len(d)} vs {len(other)})")
        continue
    none += 1
    if none <= 3:
        print(f"{p.name}: no match (size {len(d)}, header {h})")
print(f"exact {exact}, same microcode {phys_only}, none {none} of {len(list(caps.iterdir()))}")
