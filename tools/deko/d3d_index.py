"""Index of ME2's XDK Direct3D functions from the generated sources.

Writes <out>/d3d_funcs.json: {address: [instruction, ...]} for every function in the D3D range, and
<out>/d3d_calls.txt: caller -> callees. The instructions are the PowerPC comments the code generator writes.
"""
import argparse
import json
import re
from pathlib import Path

LO, HI = 0x82A7E000, 0x82AA3000


def main():
    p = argparse.ArgumentParser()
    root = Path(__file__).resolve().parent.parent.parent
    p.add_argument("--gen", type=Path, default=root / "app" / "generated" / "default")
    p.add_argument("--out", type=Path, default=root / "out" / "d3d_map")
    p.add_argument("--lo", type=lambda x: int(x, 0), default=LO)
    p.add_argument("--hi", type=lambda x: int(x, 0), default=HI)
    a = p.parse_args()
    funcs = {}
    head = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{(.*?)(?=^DEFINE_REX_FUNC|\Z)", re.M | re.S)
    for path in sorted(a.gen.glob("*_recomp.*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in head.finditer(text):
            va = int(m[1], 16)
            if a.lo <= va < a.hi:
                funcs[f"0x{va:08X}"] = re.findall(r"^\s*// ([a-z][a-z0-9.]* ?.*)$", m[2], re.M)
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "d3d_funcs.json").write_text(json.dumps(funcs, indent=0))
    with open(a.out / "d3d_calls.txt", "w") as f:
        for va, body in sorted(funcs.items()):
            calls = sorted({i.split()[1] for i in body if i.startswith("bl ")})
            f.write(f"{va} n={len(body)} -> {' '.join(calls)}\n")
    print(len(funcs), "functions")


if __name__ == "__main__":
    main()
