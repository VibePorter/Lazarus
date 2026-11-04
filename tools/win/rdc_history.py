# qrenderdoc.exe --python tools\win\rdc_history.py ; env RDC_IN (.rdc), RDC_OUT (text), RDC_EID (event whose colour
# target 0 is examined), RDC_PIXELS ("x,y;x,y")
# RenderDoc's pixel history of each pixel: every event that touched it, with the shader's output, the value after,
# and the tests that failed.
import os, traceback
import renderdoc as rd

out = open(os.environ["RDC_OUT"], "w")


def log(*a):
    out.write(" ".join(str(x) for x in a) + "\n")
    out.flush()


def v4(v):
    return "(%.4g %.4g %.4g %.4g)" % tuple(v.floatValue[:4])


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RDC_IN"], "", None)
    status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if ctl is None:
        raise RuntimeError(str(status))
    ctl.SetFrameEvent(int(os.environ["RDC_EID"]), True)
    rid = ctl.GetPipelineState().GetOutputTargets()[0].resource
    sf = ctl.GetStructuredFile()
    actions = {}
    def walk(xs):
        for x in xs:
            actions[x.eventId] = x
            walk(x.children)
    walk(ctl.GetRootActions())
    for p in os.environ["RDC_PIXELS"].split(";"):
        x, y = (int(c) for c in p.split(","))
        log("==== pixel", x, y, "of", rid)
        for m in ctl.PixelHistory(rid, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless):
            fails = []
            for name in ("sampleMasked", "backfaceCulled", "depthClipped", "viewClipped", "scissorClipped",
                         "shaderDiscarded", "depthTestFailed", "stencilTestFailed", "predicationSkipped"):
                if getattr(m, name, False):
                    fails.append(name)
            a = actions.get(m.eventId)
            log(m.eventId, a.GetName(sf)[:24] if a else "", "prim", m.primitiveID, "shader", v4(m.shaderOut.col),
                "pre", v4(m.preMod.col), "post", v4(m.postMod.col), "depth", "%.9g" % m.postMod.depth, "frag", "%.9g" % m.shaderOut.depth, "pre", "%.9g" % m.preMod.depth,
                " ".join(fails))
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    log(traceback.format_exc())
out.close()
os._exit(0)
