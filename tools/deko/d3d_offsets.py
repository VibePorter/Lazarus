"""Which ME2 D3D functions load/store a device offset (any base register), for a list of offsets.

  python d3d_offsets.py 12444 12448 12452
Prints per offset: stores (stw/stb/sth/std) and loads with function addresses.
"""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

funcs = json.loads((Path(__file__).resolve().parents[2] / "out" / "d3d_map" / "d3d_funcs.json").read_text())
for off in sys.argv[1:]:
    off = int(off, 0)
    st, ld = defaultdict(int), defaultdict(int)
    pat = re.compile(rf"^(st[bhwd]u?|l[bhwd]zu?|ld|lha|lwa|lfs|stfs|addi) r?f?\d+,(?:r\d+,)?{off}\((r\d+)\)$")
    padd = re.compile(rf"^addi r\d+,r\d+,{off}$")
    for va, body in funcs.items():
        for i in body:
            m = pat.match(i)
            if m:
                (st if m[1].startswith("st") else ld)[va] += 1
            elif padd.match(i):
                ld[va + "(addr)"] += 1
    print(f"{off} (0x{off:X}): stores {dict(sorted(st.items()))}")
    print(f"{' ' * len(str(off))}           loads {len(ld)} fns: {', '.join(sorted(ld)[:12])}{' ...' if len(ld) > 12 else ''}")
