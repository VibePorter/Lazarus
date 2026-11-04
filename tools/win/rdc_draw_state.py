# qrenderdoc.exe --python tools\win\rdc_draw_state.py ; env RDC_IN (.rdc), RDC_OUT (text), RDC_EIDS (';'-separated)
# For each event: viewport, scissor, rasterizer / depth / blend state and the vertex shader's first output positions.
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
    def walk(actions):
        for x in actions:
            yield x
            yield from walk(x.children)
    by_eid = {x.eventId: x for x in walk(ctl.GetRootActions())}
    for eid in [int(x) for x in os.environ["RDC_EIDS"].split(";") if x]:
        ctl.SetFrameEvent(eid, True)
        a = by_eid.get(eid)
        log("== eid", eid, a.GetName(ctl.GetStructuredFile()) if a else "?", "indices", a.numIndices if a else "?",
            "instances", a.numInstances if a else "?")
        vk = ctl.GetVulkanPipelineState()
        for v in vk.viewportScissor.viewportScissors[:2]:
            vp, sc = v.vp, v.scissor
            log("  viewport", vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth, "scissor", sc.x, sc.y,
                sc.width, sc.height)
        r = vk.rasterizer
        log("  raster cull", r.cullMode, "front", r.frontCCW, "discard", r.rasterizerDiscardEnable, "depthClamp",
            r.depthClampEnable, "fill", r.fillMode)
        log("  depth bias const", r.depthBias, "slope", r.slopeScaledDepthBias, "clamp", r.depthBiasClamp)
        d = vk.depthStencil
        log("  depth test", d.depthTestEnable, "write", d.depthWriteEnable, "func", d.depthFunction, "stencil",
            d.stencilTestEnable, "front", d.frontFace.function, d.frontFace.reference, d.frontFace.compareMask)
        for i, b in enumerate(vk.colorBlend.blends[:2]):
            log("  blend", i, "enable", b.enabled, "mask", b.writeMask, "color", b.colorBlend.source,
                b.colorBlend.destination, b.colorBlend.operation)
        ms = vk.multisample
        log("  msaa", ms.rasterSamples, "sampleMask", hex(ms.sampleMask))
        try:
            import struct
            ins = ctl.GetPipelineState().GetVertexInputs()
            vbs = ctl.GetPipelineState().GetVBuffers()
            for vi in ins[:4]:
                log("  vsin", vi.name, "vb", vi.vertexBuffer, "off", vi.byteOffset, "fmt", vi.format.Name())
            for i, vb in enumerate(vbs[:3]):
                log("  vb", i, vb.resourceId, "off", vb.byteOffset, "stride", vb.byteStride, "size", vb.byteSize)
                if vb.resourceId != rd.ResourceId.Null() and vb.byteStride:
                    data = ctl.GetBufferData(vb.resourceId, vb.byteOffset, min(vb.byteSize, 3 * vb.byteStride))
                    for k in range(min(3, len(data) // vb.byteStride)):
                        log("    vertex", k, data[k * vb.byteStride:(k + 1) * vb.byteStride][:32].hex())
            # VS constant buffers: the first bytes of each bound one
            refl = ctl.GetPipelineState().GetShaderReflection(rd.ShaderStage.Vertex)
            if refl:
                for i, cb in enumerate(refl.constantBlocks[:4]):
                    log("  cblock", i, cb.name, "vars", len(cb.variables), "size", cb.byteSize, "bufferBacked", cb.bufferBacked)
                for i, ro in enumerate(refl.readOnlyResources[:4]):
                    log("  ro", i, ro.name)
                for i, rw in enumerate(refl.readWriteResources[:4]):
                    log("  rw", i, rw.name)
        except Exception:
            log(traceback.format_exc())
        try:
            mesh = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            log("  vsout numIndices", mesh.numIndices, "stride", mesh.vertexByteStride, "fmt",
                mesh.format.Name(), "buffer", mesh.vertexResourceId)
            if mesh.vertexResourceId != rd.ResourceId.Null():
                data = ctl.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset,
                                         min(6, mesh.numIndices) * mesh.vertexByteStride)
                import struct
                for k in range(min(6, mesh.numIndices)):
                    base = k * mesh.vertexByteStride
                    if base + 16 <= len(data):
                        log("    pos", struct.unpack_from("<4f", data, base))
        except Exception:
            log(traceback.format_exc())
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    log(traceback.format_exc())
out.close()
os._exit(0)
