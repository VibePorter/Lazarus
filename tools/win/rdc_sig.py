# qrenderdoc.exe --python tools\win\rdc_sig.py ; env RDC_IN (.rdc), RDC_OUT (text), RDC_EID (event)
# me2-nx-deko (2026-11-07): a draw's vertex shader output and pixel shader input signatures (name, location, components).
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
    ctl.SetFrameEvent(int(os.environ["RDC_EID"]), True)
    st = ctl.GetPipelineState()
    for stage, name in ((rd.ShaderStage.Vertex, "VS out"), (rd.ShaderStage.Pixel, "PS in")):
        refl = st.GetShaderReflection(stage)
        sig = refl.outputSignature if stage == rd.ShaderStage.Vertex else refl.inputSignature
        log("==", name)
        for i, s in enumerate(sig):
            log("  [%d] %s reg %d comps %d sys %s" % (i, s.varName, s.regIndex, s.compCount, str(s.systemValue)))
    log("done")
except Exception:
    log(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
