import json, sys, collections
g = json.load(open(sys.argv[1]))
lo, hi = int(sys.argv[2],16), int(sys.argv[3],16)
def addr(n):
    return int(n[4:],16) if n.startswith("sub_") else None
def inr(n):
    a = addr(n); return a is not None and lo <= a < hi
# d3d set: functions in range
d3d = {n for n in g if inr(n)}
callers = collections.defaultdict(set)
for f, info in g.items():
    if inr(f): continue
    for c in info["calls"]:
        if c in d3d: callers[f].add(c)
print(len(d3d), "d3d funcs;", len(callers), "outside callers")
# group callers by address region (1MB)
reg = collections.Counter((addr(f) or 0) >> 20 for f in callers)
for r, n in sorted(reg.items()): print(hex(r<<20), n)
json.dump({k: sorted(v) for k, v in callers.items()}, open(sys.argv[4], "w"), indent=0)
