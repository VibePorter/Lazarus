#!/usr/bin/env python3
"""Rewrites Renut's N&B device offsets (hex literals in the native device code) to Mass Effect 2's layout.

Renut's native device code (hooks/device.cpp, d3d_native_state.cpp, hooks/draw.cpp) addresses the CDevice with
Banjo-Kazooie: Nuts & Bolts' offsets as hex literals: d.W(0x2FA0), d.SetB(0x3384, ...). ME2's XDK 2.0.8955 has N&B's
layout with one shifted block, each rule read from ME2's own code (docs/d3d-map.md; base-game addresses):

  below 0x2E2C        same
  0x2E2C              +4  (the vertex declaration, InitializeApiState sub_82AA82E8 `stw r29,11824`)
  0x2E30 .. 0x3387    +8  (InitializeApiState, BeginTiling sub_82AA4398, ClearF sub_82AA2B30; the tiling extent
                          0x337C -> 13188, BeginTiling's flags 0x3384 -> 13196)
  0x3390 and up       same (BeginTiling's clear color 13200, Z 13216, stencil 13220; KickOff 13232, NewSegment
                          13516, present interval 13596, ...)
  0x3388 .. 0x338F    not in ME2's device: reported, left alone

Each rewritten literal gets a trailing `/*NB:0x....*/` marker, so a second run finds nothing to do (idempotent).

  python tools/me2_layout_port.py [--check]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GPU = ROOT / "app" / "src" / "renut_engine" / "gpu"
FILES = [GPU / "d3d" / "hooks" / "device.cpp", GPU / "d3d" / "d3d_native_state.cpp", GPU / "d3d" / "hooks" / "draw.cpp"]

ACCESS = re.compile(r"\b(d|device|dev)\.(W|SetW|B|SetB|H|SetH|Q|SetQ|F|SetF)\(0x([0-9A-Fa-f]{4})\b(?!/\*NB)")


def me2(nb: int):
    if nb < 0x2E2C:
        return nb
    if nb < 0x2E30:
        return nb + 4
    if nb < 0x3388:
        return nb + 8
    if nb < 0x3390:
        return None
    return nb


def main():
    check = "--check" in sys.argv
    unknown = []
    total = 0
    for path in FILES:
        text = path.read_text(encoding="utf-8")
        count = 0

        def sub(m):
            nonlocal count
            nb = int(m[3], 16)
            new = me2(nb)
            if new is None:
                unknown.append(f"{path.name}: 0x{nb:04X} at {m.group(0)}")
                return m.group(0)
            if new == nb:
                return m.group(0)
            count += 1
            return f"{m[1]}.{m[2]}(0x{new:04X}/*NB:0x{nb:04X}*/"

        out = ACCESS.sub(sub, text)
        total += count
        print(f"{path.name}: {count} literals rewritten")
        if count and not check:
            path.write_text(out, encoding="utf-8", newline="\n")
    for u in unknown:
        print("UNKNOWN (check by hand):", u)
    print(f"total {total}{' (check only)' if check else ''}")


if __name__ == "__main__":
    main()
