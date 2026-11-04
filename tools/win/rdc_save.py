# qrenderdoc.exe --python tools\win\rdc_save.py ; env RDC_IN (.rdc), RDC_DIR (output folder), RDC_SAVE (';'-separated eids)
# Saves, after each listed event, its colour targets (and the depth target with RDC_DEPTH=1) as PNG, without walking
# the whole capture.
import os, traceback
import renderdoc as rd

folder = os.environ["RDC_DIR"]
out = open(os.path.join(folder, "save.txt"), "w")
try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RDC_IN"], "", None)
    status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if ctl is None:
        raise RuntimeError(str(status))
    for eid in [int(x) for x in os.environ["RDC_SAVE"].split(";") if x]:
        ctl.SetFrameEvent(eid, True)
        state = ctl.GetPipelineState()
        targets = [(i, o.resource) for i, o in enumerate(state.GetOutputTargets()) if o.resource != rd.ResourceId.Null()]
        if os.environ.get("RDC_DEPTH") == "1":
            d = state.GetDepthTarget()
            if d.resource != rd.ResourceId.Null():
                targets.append(("d", d.resource))
        for i, rid in targets:
            s = rd.TextureSave()
            s.resourceId = rid
            s.destType = rd.FileType.PNG
            s.alpha = rd.AlphaMapping.Discard
            path = os.path.join(folder, "eid%d_rt%s.png" % (eid, i))
            ctl.SaveTexture(s, path)
            out.write("%d rt%s %s -> %s\n" % (eid, i, rid, path))
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    out.write(traceback.format_exc())
out.close()
os._exit(0)
