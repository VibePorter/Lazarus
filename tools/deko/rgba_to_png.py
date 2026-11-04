"""Converts a capture (<name>.rgba with <name>.txt "width height seconds frames") to PNGs, one per frame.

Usage: python rgba_to_png.py <capture .rgba> <out prefix>
"""
import struct
import sys
import zlib
from pathlib import Path

rgba = Path(sys.argv[1])
w, h, _s, n = Path(str(rgba)[:-5] + ".txt").read_text().split()
w, h, n = int(w), int(h), int(n)
data = rgba.read_bytes()
size = w * h * 4


def chunk(tag, body):
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)


for k in range(n):
    frame = data[k * size:(k + 1) * size]
    rgb = bytearray(frame)
    del rgb[3::4]  # RGB: the alpha channel is not part of the image
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    out = Path(f"{sys.argv[2]}_{k}.png")
    out.write_bytes(png)
    print(out)
