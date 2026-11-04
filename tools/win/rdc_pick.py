# qrenderdoc.exe --python tools\win\rdc_pick.py ; env RDC_IN (.rdc), RDC_OUT (text), RDC_PICK ("eid:x,y,x,y...;eid:...")
# After each listed event: the exact float values of its colour target 0 at the given texels (PNG saves clamp to 8 bits).
import os, traceback
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
    for item in [x for x in os.environ["RDC_PICK"].split(";") if x]:
        eid, coords = item.split(":")
        ctl.SetFrameEvent(int(eid), True)
        rid = ctl.GetPipelineState().GetOutputTargets()[0].resource
        c = [int(v) for v in coords.split(",")]
        for i in range(0, len(c), 2):
            v = ctl.PickPixel(rid, c[i], c[i + 1], rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            log("eid", eid, rid, "texel", c[i], c[i + 1], "= %.6g %.6g %.6g %.6g" % tuple(v.floatValue[:4]))
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    log(traceback.format_exc())
out.close()
os._exit(0)
