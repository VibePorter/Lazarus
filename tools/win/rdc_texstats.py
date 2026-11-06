# qrenderdoc.exe --python tools\win\rdc_texstats.py ; env RDC_IN (.rdc), RDC_OUT (text), RDC_EID (event), RDC_TEX
# (';'-separated resource id numbers)
# me2-nx-deko (2026-11-07): the contents of float textures at an event: count of NaN / inf / huge values, min and max per
# channel, and where the worst texels are (half-float formats via struct 'e', float32 via 'f').
import os
import struct
import traceback

import renderdoc as rd

out = open(os.environ["RDC_OUT"], "w")


def log(*a):
    out.write(" ".join(str(x) for x in a) + "\n")
    out.flush()


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RDC_IN"], "", None)
    status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if ctl is None:
        raise RuntimeError(str(status))
    ctl.SetFrameEvent(int(os.environ["RDC_EID"]), True)
    tex = {t.resourceId: t for t in ctl.GetTextures()}
    wanted = [x for x in os.environ["RDC_TEX"].split(";") if x]
    for t in tex.values():
        rid = str(t.resourceId).split("::")[-1]
        if rid not in wanted:
            continue
        fmt = t.format
        name = fmt.Name()
        comp = fmt.compCount
        width = fmt.compByteWidth
        code = {2: "e", 4: "f"}.get(width)
        log("== texture", rid, t.width, "x", t.height, name, "mips", t.mips)
        if code is None:
            log("  not a float format")
            continue
        sub = rd.Subresource(0, 0, 0)
        data = ctl.GetTextureData(t.resourceId, sub)
        n = len(data) // width
        vals = struct.unpack("<%d%s" % (n, code), data[:n * width])
        nan = inf = huge = 0
        lo = [float("inf")] * comp
        hi = [float("-inf")] * comp
        worst = []
        for i, v in enumerate(vals):
            c = i % comp
            if v != v:
                nan += 1
                if len(worst) < 12:
                    worst.append((i // comp % t.width, i // comp // t.width, c, v))
                continue
            if v in (float("inf"), float("-inf")):
                inf += 1
                if len(worst) < 12:
                    worst.append((i // comp % t.width, i // comp // t.width, c, v))
                continue
            if abs(v) > 1e4:
                huge += 1
                if len(worst) < 12:
                    worst.append((i // comp % t.width, i // comp // t.width, c, v))
            lo[c] = min(lo[c], v)
            hi[c] = max(hi[c], v)
        log("  values", n, "nan", nan, "inf", inf, "abs>1e4", huge)
        log("  min", lo, "max", hi)
        log("  first bad texels (x, y, channel, value):", worst)
    log("done")
except Exception:
    log(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
