# qrenderdoc.exe --python tools\win\rdc_passes.py ; env RDC_IN (.rdc), RDC_OUT (text), RDC_FROM (optional first eid)
# Every draw / dispatch / copy in order: its colour and depth targets (size, format, resource id), vertex count and
# the pixel shader's resource id; consecutive draws into the same targets are collapsed into one line per run.
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
        raise RuntimeError("OpenCapture: %s" % str(status))
    sf = ctl.GetStructuredFile()
    tex = {t.resourceId: t for t in ctl.GetTextures()}
    first = int(os.environ.get("RDC_FROM", "0"))

    def walk(actions):
        for x in actions:
            yield x
            yield from walk(x.children)

    def desc(rid):
        if rid == rd.ResourceId.Null():
            return "-"
        t = tex.get(rid)
        if not t:
            return "?" + str(rid)
        return "%dx%d %s ms%d %s" % (t.width, t.height, t.format.Name(), t.msSamp, str(rid).replace("ResourceId::", "#"))

    run_key, run_first, run_last, run_n, run_ps, run_verts = None, 0, 0, 0, set(), 0

    def flush():
        if run_key is not None:
            log("%6d-%-6d x%-4d v%-7d %s | ps %s" % (run_first, run_last, run_n, run_verts, run_key,
                                                     ",".join(sorted(run_ps))[:120]))

    for a in walk(ctl.GetRootActions()):
        flags = a.flags
        if a.eventId < first:
            continue
        name = a.GetName(sf)
        if flags & rd.ActionFlags.Clear or flags & rd.ActionFlags.Copy or flags & rd.ActionFlags.Resolve or \
                flags & rd.ActionFlags.BeginPass or flags & rd.ActionFlags.EndPass:
            flush()
            run_key = None
            if flags & (rd.ActionFlags.Clear | rd.ActionFlags.Copy | rd.ActionFlags.Resolve):
                log("%6d  %s" % (a.eventId, name[:140]))
            continue
        if not (flags & rd.ActionFlags.Drawcall):
            continue
        ctl.SetFrameEvent(a.eventId, False)
        st = ctl.GetPipelineState()
        cols = [desc(o.resource) for o in st.GetOutputTargets() if o.resource != rd.ResourceId.Null()]
        dep = desc(st.GetDepthTarget().resource)
        key = "C[" + "; ".join(cols) + "] D[" + dep + "]"
        ps = str(st.GetShader(rd.ShaderStage.Pixel)).replace("ResourceId::", "#")
        if key != run_key:
            flush()
            run_key, run_first, run_n, run_ps, run_verts = key, a.eventId, 0, set(), 0
        run_last = a.eventId
        run_n += 1
        run_verts += a.numIndices * max(a.numInstances, 1)
        run_ps.add(ps)
    flush()
except Exception:
    log(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
