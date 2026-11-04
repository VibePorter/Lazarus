"""Finds ME2 D3D functions containing all given instruction patterns (regex), from map/d3d_funcs.json.

  python d3d_find.py "li r5,872" "lwz r11,872\\(r31\\)"
  python d3d_find.py --dump 0x82A8E498
"""
import json
import re
import sys
from pathlib import Path

funcs = json.loads((Path(__file__).resolve().parents[2] / "out" / "d3d_map" / "d3d_funcs.json").read_text())
args = sys.argv[1:]
if args and args[0] == "--dump":
    for va in args[1:]:
        va = f"0x{int(va, 0):08X}"
        print(f"== {va} ({len(funcs[va])})")
        print("\n".join(funcs[va]))
    sys.exit()
pats = [re.compile(a) for a in args]
for va, body in sorted(funcs.items()):
    if all(any(p.search(i) for i in body) for p in pats):
        print(va, len(body))
