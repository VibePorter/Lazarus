"""Diff a Renut runtime capture against the container in a game file that has the same microcode.

Usage: python diff_capture.py <capture.bin> <game file>
"""
import struct
import sys
from pathlib import Path

cap = Path(sys.argv[1]).read_bytes()
data = Path(sys.argv[2]).read_bytes()
virt, phys = struct.unpack(">II", cap[4:12])
micro = cap[virt:virt + phys]
pos = data.find(micro)
if pos < 0:
    sys.exit("microcode not found")
# The file's container: search backwards for its 0x102A11xx header.
start = None
for back in range(0, 0x4000, 4):
    s = pos - back
    if s < 0:
        break
    if data[s:s + 3] == b"\x10\x2A\x11":
        fv, fp = struct.unpack(">II", data[s + 4:s + 12])
        if s + fv == pos:
            start = s
            break
print(f"capture: virt {virt} phys {phys}; microcode in file at {pos:#x}; container start {start if start is None else hex(start)}")
if start is None:
    sys.exit()
orig = data[start:start + virt + phys]
fv, fp = struct.unpack(">II", orig[4:12])
print(f"file container: virt {fv} phys {fp}")
diffs = [i for i in range(min(len(orig), len(cap))) if orig[i] != cap[i]]
print(f"{len(diffs)} differing bytes; first at: {diffs[:40]}")
for i in diffs[:24]:
    w = i & ~3
    print(f"  +{w:#06x}: file {orig[w:w + 4].hex()} capture {cap[w:w + 4].hex()}")
