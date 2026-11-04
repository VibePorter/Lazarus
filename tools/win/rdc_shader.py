# qrenderdoc.exe --python tools\win\rdc_shader.py ; env RDC_IN (.rdc), RDC_DIR (output folder), RDC_EIDS (';'-separated)
# For each event: the bound pixel shader's disassembly (eid<N>_ps.txt), its read-only resources (texture, size, format,
# sampler) and constant blocks, the depth / stencil / blend state, and the depth target.
import os, traceback
import renderdoc as rd

folder = os.environ["RDC_DIR"]
out = open(os.path.join(folder, "shader.txt"), "w")


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
    textures = {t.resourceId: t for t in ctl.GetTextures()}
    for eid in [int(x) for x in os.environ["RDC_EIDS"].split(";") if x]:
        ctl.SetFrameEvent(eid, True)
        st = ctl.GetPipelineState()
        vk = ctl.GetVulkanPipelineState()
        log("==== eid", eid)
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            refl = st.GetShaderReflection(stage)
            if refl is None:
                continue
            log(" stage", stage, "shader", st.GetShader(stage), "entry", st.GetShaderEntryPoint(stage))
            if stage == rd.ShaderStage.Pixel:
                targets = ctl.GetDisassemblyTargets(True)
                text = ctl.DisassembleShader(st.GetGraphicsPipelineObject(), refl, targets[0] if targets else "")
                with open(os.path.join(folder, "eid%d_ps.txt" % eid), "w") as f:
                    f.write(text)
            for ro in st.GetReadOnlyResources(stage):
                rid = ro.descriptor.resource
                t = textures.get(rid)
                if t is not None:
                    log("  ro", ro.access.index, ro.access.arrayElement, rid, "%dx%d" % (t.width, t.height),
                        t.format.Name(), "ms%d" % t.msSamp)
            for i, cb in enumerate(refl.constantBlocks):
                bind = st.GetConstantBlock(stage, i, 0)
                vars = ctl.GetCBufferVariableContents(st.GetGraphicsPipelineObject(), refl.resourceId, stage,
                                                       st.GetShaderEntryPoint(stage), i, bind.descriptor.resource,
                                                       bind.descriptor.byteOffset, bind.descriptor.byteSize)
                def dump(vs, prefix):
                    for v in vs:
                        if v.members:
                            dump(v.members, prefix + v.name + ".")
                        else:
                            n = v.rows * v.columns
                            vals = [v.value.f32v[k] for k in range(n)]
                            if any(x != 0 for x in vals):
                                log("   ", prefix + v.name, " ".join("%.6g" % x for x in vals),
                                    "| u32", " ".join("%d" % v.value.u32v[k] for k in range(n)))
                log("  cb", i, cb.name)
                dump(vars, "")
        ds = vk.depthStencil
        log(" depth test", ds.depthTestEnable, "write", ds.depthWriteEnable, "func", ds.depthFunction,
            "stencil", ds.stencilTestEnable)
        for i, b in enumerate(vk.colorBlend.blends):
            if b.enabled:
                log(" blend", i, b.colorBlend.source, b.colorBlend.operation, b.colorBlend.destination,
                    "alpha", b.alphaBlend.source, b.alphaBlend.operation, b.alphaBlend.destination, "mask", b.writeMask)
        d = st.GetDepthTarget()
        if d.resource != rd.ResourceId.Null():
            t = textures.get(d.resource)
            log(" depth target", d.resource, "%dx%d" % (t.width, t.height), t.format.Name(), "ms%d" % t.msSamp)
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    log(traceback.format_exc())
out.close()
os._exit(0)
