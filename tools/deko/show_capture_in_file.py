"""Shows where a capture's header words appear in a game file, around its microcode.

Usage: python show_capture_in_file.py <capture.bin> <game file>
"""
import struct
import sys
from pathlib import Path

cap = Path(sys.argv[1]).read_bytes()
data = Path(sys.argv[2]).read_bytes()
v, p = struct.unpack(">II", cap[4:12])
micro = cap[v:v + p]
pos = data.find(micro)
print(f"capture header {v} bytes, microcode {p}; microcode in file at {pos:#x}")
hdr = cap[:v]
# Longest runs of the header found in the file before the microcode.
start = max(0, pos - 0x2000)
window = data[start:pos]
i = 0
while i < v:
    best = (0, -1)
    for n in (64, 32, 16, 8):
        if i + n <= v:
            j = window.find(hdr[i:i + n])
            if j >= 0:
                best = (n, j)
                break
    if best[0] == 0:
        print(f"  header +{i:#05x}: {hdr[i:i + 8].hex()} not found before the microcode")
        i += 8
        continue
    n, j = best
    while i + n < v and start + j + n < pos and hdr[i + n] == window[j + n]:
        n += 1
    print(f"  header +{i:#05x}..+{i + n:#05x} at file {start + j:#x} (microcode {pos:#x}, gap {pos - (start + j + n)})")
    i += n
