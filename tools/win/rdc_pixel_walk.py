# qrenderdoc.exe --python tools\win\rdc_pixel_walk.py ; env RDC_IN (.rdc), RDC_OUT (text)
# Every draw / clear / copy of the capture: its first colour target and that target's pixels at a few points after it,
# so the event where a target turns white (or black) shows. Also saves PNGs of the targets at RDC_SAVE eids
# (';'-separated) into RDC_OUT's folder.
import os, traceback
import renderdoc as rd

out = open(os.environ["RDC_OUT"], "w")


def log(*a):
    out.write(" ".join(str(x) for x in a) + "\n")
    out.flush()


def walk(actions):
    for a in actions:
        yield a
        yield from walk(a.children)


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RDC_IN"], "", None)
    status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    textures = {t.resourceId: t for t in ctl.GetTextures()}
    save = set(int(x) for x in os.environ.get("RDC_SAVE", "").split(";") if x)
    folder = os.path.dirname(os.environ["RDC_OUT"])
    names = {}
    for a in walk(ctl.GetRootActions()):
        if not (a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Clear | rd.ActionFlags.Copy |
                           rd.ActionFlags.Resolve | rd.ActionFlags.Dispatch)):
            continue
        ctl.SetFrameEvent(a.eventId, True)
        state = ctl.GetPipelineState()
        targets = [o for o in state.GetOutputTargets() if o.resource != rd.ResourceId.Null()]
        rid = targets[0].resource if targets else (a.copyDestination if a.copyDestination != rd.ResourceId.Null() else None)
        if rid is None:
            log(a.eventId, a.GetName(ctl.GetStructuredFile()), "(no colour target)")
            continue
        t = textures.get(rid)
        if t is None:
            log(a.eventId, a.GetName(ctl.GetStructuredFile()), "target", rid, "(not a texture)")
            continue
        if rid not in names:
            names[rid] = "T%d" % len(names)
        px = []
        for fx, fy in ((0.5, 0.5), (0.1, 0.1), (0.9, 0.9), (0.3, 0.7)):
            v = ctl.PickPixel(rid, int(t.width * fx), int(t.height * fy), rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            px.append("(%.3g %.3g %.3g %.3g)" % tuple(v.floatValue[:4]))
        mm = ""
        if a.eventId in save:
            for smp in range(max(1, t.msSamp)):
                lo, hi = ctl.GetMinMax(rid, rd.Subresource(0, 0, smp), rd.CompType.Typeless)
                mm += " s%d min(%.3g %.3g %.3g %.3g) max(%.3g %.3g %.3g %.3g)" % ((smp,) + tuple(lo.floatValue[:4]) + tuple(hi.floatValue[:4]))
        ps = state.GetShader(rd.ShaderStage.Pixel)
        log(a.eventId, a.GetName(ctl.GetStructuredFile())[:40], names[rid], "%dx%d" % (t.width, t.height),
            str(t.format.Name()), "ms%d" % t.msSamp, "ps", ps, " ".join(px), mm)
        if a.eventId in save:
            s = rd.TextureSave()
            s.resourceId = rid
            s.destType = rd.FileType.PNG
            s.alpha = rd.AlphaMapping.Discard
            ctl.SaveTexture(s, os.path.join(folder, "eid%d_%s.png" % (a.eventId, names[rid])))
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    log(traceback.format_exc())
out.close()
os._exit(0)
