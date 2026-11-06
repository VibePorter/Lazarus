# qrenderdoc.exe --python tools\win\rdc_resinfo.py ; env RDC_IN (.rdc), RDC_OUT (text), RDC_TEX (';'-separated ids)
# me2-nx-deko (2026-11-07): names, sizes, formats and the events that use each listed resource (RenderDoc's usage).
import os
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
    wanted = [x for x in os.environ.get("RDC_TEX", "").split(";") if x]
    fmt_filter = os.environ.get("RDC_FORMAT", "")  # e.g. R16G16_FLOAT: every texture of that format
    names = {str(r.resourceId).split("::")[-1]: r.name for r in ctl.GetResources()}
    for t in ctl.GetTextures():
        rid = str(t.resourceId).split("::")[-1]
        if rid not in wanted and not (fmt_filter and t.format.Name() == fmt_filter):
            continue
        log("==", rid, names.get(rid, "?"), t.width, "x", t.height, t.format.Name(), "mips", t.mips,
            "creation flags", t.creationFlags)
        uses = ctl.GetUsage(t.resourceId)
        log("  uses", len(uses), ":", ", ".join("%d %s" % (u.eventId, str(u.usage).split(".")[-1]) for u in uses[:60]))
    log("done")
except Exception:
    log(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
