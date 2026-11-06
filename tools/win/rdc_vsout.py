# qrenderdoc.exe --python tools\win\rdc_vsout.py ; env RDC_IN (.rdc), RDC_OUT (text), RDC_EIDS (';'-separated)
# me2-nx-deko (2026-11-07): every vertex shader output of a draw scanned for NaN / inf / huge floats: how many vertices
# are bad, the worst values, and which output float (index in the vertex) they are in.
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
    for eid in [int(x) for x in os.environ["RDC_EIDS"].split(";") if x]:
        ctl.SetFrameEvent(eid, True)
        mesh = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        stride = mesh.vertexByteStride
        n = mesh.numIndices
        data = ctl.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, 0)
        verts = min(len(data) // stride, 200000) if stride else 0
        nf = stride // 4
        bad_verts = 0
        per_float = [0] * nf
        worst = []
        for v in range(verts):
            vals = struct.unpack_from("<%df" % nf, data, v * stride)
            bad = False
            for k, x in enumerate(vals):
                if x != x or abs(x) > 1e6:
                    per_float[k] += 1
                    bad = True
                    if len(worst) < 10:
                        worst.append((v, k, x))
            bad_verts += bad
        log("== eid", eid, "indices", n, "stride", stride, "vertices in buffer", verts, "bad vertices", bad_verts)
        log("  bad by output float:", [(k, c) for k, c in enumerate(per_float) if c])
        log("  first bad (vertex, float, value):", worst)
        if verts:
            log("  vertex 0:", struct.unpack_from("<%df" % nf, data, 0))
    log("done")
except Exception:
    log(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
