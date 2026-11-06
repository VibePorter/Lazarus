# qrenderdoc.exe --python tools\win\rdc_icons.py ; env RDC_IN (.rdc), RDC_DIR (output folder), RDC_LAST (draws from the
# end to examine, default 800)
# me2-nx-deko (2026-11-06, the hacking mini-game's button icons): the frame's last draw target as PNG, then every one
# of the last draws whose pixel shader reads a small texture (<= 64x64): its screen rectangle (vertex shader output
# through the viewport), the texture (saved once as PNG) and the sampler's filtering. Results in icons.txt.
import os
import struct
import traceback

import renderdoc as rd

folder = os.environ["RDC_DIR"]
os.makedirs(folder, exist_ok=True)
out = open(os.path.join(folder, "icons.txt"), "w")


def log(*a):
    out.write(" ".join(str(x) for x in a) + "\n")
    out.flush()


def walk(actions):
    for x in actions:
        yield x
        yield from walk(x.children)


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RDC_IN"], "", None)
    status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if ctl is None:
        raise RuntimeError(str(status))
    tex = {t.resourceId: t for t in ctl.GetTextures()}
    draws = [a for a in walk(ctl.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]
    log("draws", len(draws))
    last = int(os.environ.get("RDC_LAST", "800"))
    # the final picture: the last draw's colour target
    ctl.SetFrameEvent(draws[-1].eventId, True)
    t0 = ctl.GetPipelineState().GetOutputTargets()[0].resource
    s = rd.TextureSave()
    s.resourceId = t0
    s.destType = rd.FileType.PNG
    s.alpha = rd.AlphaMapping.Discard
    ctl.SaveTexture(s, os.path.join(folder, "final.png"))
    log("final target", t0, tex[t0].width, tex[t0].height, "eid", draws[-1].eventId)
    saved = set()
    for a in draws[-last:]:
        ctl.SetFrameEvent(a.eventId, True)
        pipe = ctl.GetPipelineState()
        small, filt = [], []
        try:
            for u in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel):
                t = tex.get(u.descriptor.resource)
                if t and t.width <= 64 and t.height <= 64 and t.width >= 8 and t not in small:
                    small.append(t)
                    smp = u.sampler
                    f = smp.filter
                    filt.append("min %s mag %s mip %s aniso %s bias %s lod %s-%s" % (
                        f.minify, f.magnify, f.mip, smp.maxAnisotropy, smp.mipBias, smp.minLOD, smp.maxLOD))
        except Exception:
            log("ro", traceback.format_exc().splitlines()[-1])
        if not small:
            continue
        tgt = pipe.GetOutputTargets()[0].resource
        rect = "?"
        try:
            vk = ctl.GetVulkanPipelineState()
            vp = vk.viewportScissor.viewportScissors[0].vp
            mesh = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            n = min(6, mesh.numIndices)
            data = ctl.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, n * mesh.vertexByteStride)
            xs, ys = [], []
            for k in range(n):
                x, y, z, w = struct.unpack_from("<4f", data, k * mesh.vertexByteStride)
                if w:
                    xs.append(vp.x + (x / w * 0.5 + 0.5) * vp.width)
                    ys.append(vp.y + (y / w * 0.5 + 0.5) * vp.height)
            if xs:
                rect = "x %.2f-%.2f (%.2f px) y %.2f-%.2f (%.2f px)" % (min(xs), max(xs), max(xs) - min(xs), min(ys),
                                                                      max(ys), max(ys) - min(ys))
        except Exception:
            rect = "err " + traceback.format_exc().splitlines()[-1]
        log("eid", a.eventId, "target", tgt, "%dx%d" % (tex[tgt].width, tex[tgt].height) if tgt in tex else "",
            "textures", ", ".join("%s %dx%d mips %d" % (t.resourceId, t.width, t.height, t.mips) for t in small),
            "| rect", rect, "|", "; ".join(filt))
        for t in small:
            if t.resourceId not in saved:
                saved.add(t.resourceId)
                s = rd.TextureSave()
                s.resourceId = t.resourceId
                s.destType = rd.FileType.PNG
                s.alpha = rd.AlphaMapping.Preserve
                ctl.SaveTexture(s, os.path.join(folder, "tex_%s.png" % str(t.resourceId).split("::")[-1]))
    log("done")
except Exception:
    log(traceback.format_exc())
finally:
    out.close()
    os._exit(0)
