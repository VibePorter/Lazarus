/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <cstring>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"

using namespace renut::gpu;

namespace {

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x8264D468]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x8264D4A8]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x8264D3D8]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82276AC0]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x3168 + i * 4)));
  viewport[4] = d.W(0x3178);
  viewport[5] = d.W(0x317C);
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x8264D680]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x8264D6D0]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x82221930], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x82655C60]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3484));
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x8222F6F8]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x8222ABB8]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x8220EE38]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x82205700]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x82222EA0]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x82222F20]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x82223270]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x8227AB68]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x8227AF40]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x8227AEB0]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x8222B078]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x82658098]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x82220B20]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x8264DE58]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x826583A8]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x8264DD40]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82287D38]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x8264DCC0]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82656EA8] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// Render states, by their index in N&B's table (off_82E60360).
BANJO_RENUT_HOOK(sub_8264BAC8, SetRenderStateHook<10>);  // D3DDevice_SetRenderState_ZEnable
BANJO_RENUT_HOOK(sub_821FC9A8, SetRenderStateHook<11>);  // D3DDevice_SetRenderState_ZFunc
BANJO_RENUT_HOOK(sub_8264BB08, SetRenderStateHook<12>);  // D3DDevice_SetRenderState_ZWriteEnable
BANJO_RENUT_HOOK(sub_8228F330, SetRenderStateHook<13>);  // D3DDevice_SetRenderState_FillMode
BANJO_RENUT_HOOK(sub_8228F350, SetRenderStateHook<14>);  // D3DDevice_SetRenderState_CullMode
BANJO_RENUT_HOOK(sub_821CDFE0, SetRenderStateHook<15>);  // D3DDevice_SetRenderState_AlphaBlendEnable
BANJO_RENUT_HOOK(sub_8222A690, SetRenderStateHook<16>);  // D3DDevice_SetRenderState_SeparateAlphaBlendEnable
BANJO_RENUT_HOOK(sub_8226E8E0, SetRenderStateHook<17>);  // D3DDevice_SetRenderState_BlendFactor
BANJO_RENUT_HOOK(sub_82230378, SetRenderStateHook<18>);  // D3DDevice_SetRenderState_SrcBlend
BANJO_RENUT_HOOK(sub_82230AA0, SetRenderStateHook<19>);  // D3DDevice_SetRenderState_DestBlend
BANJO_RENUT_HOOK(sub_82226A68, SetRenderStateHook<20>);  // D3DDevice_SetRenderState_BlendOp
BANJO_RENUT_HOOK(sub_822672B8, SetRenderStateHook<21>);  // D3DDevice_SetRenderState_SrcBlendAlpha
BANJO_RENUT_HOOK(sub_822208E8, SetRenderStateHook<22>);  // D3DDevice_SetRenderState_DestBlendAlpha
BANJO_RENUT_HOOK(sub_8221F5F8, SetRenderStateHook<23>);  // D3DDevice_SetRenderState_BlendOpAlpha
BANJO_RENUT_HOOK(sub_8264B820, SetRenderStateHook<24>);  // D3DDevice_SetRenderState_AlphaTestEnable
BANJO_RENUT_HOOK(sub_8264B8D8, SetRenderStateHook<25>);  // D3DDevice_SetRenderState_AlphaRef
BANJO_RENUT_HOOK(sub_8264B940, SetRenderStateHook<26>);  // D3DDevice_SetRenderState_AlphaFunc
BANJO_RENUT_HOOK(sub_8264BB48, SetRenderStateHook<27>);  // D3DDevice_SetRenderState_StencilEnable
BANJO_RENUT_HOOK(sub_8264BB88, SetRenderStateHook<28>);  // D3DDevice_SetRenderState_TwoSidedStencilMode
BANJO_RENUT_HOOK(sub_8264BBF0, SetRenderStateHook<29>);  // D3DDevice_SetRenderState_StencilFail
BANJO_RENUT_HOOK(sub_8264BC28, SetRenderStateHook<30>);  // D3DDevice_SetRenderState_StencilZFail
BANJO_RENUT_HOOK(sub_8264BC60, SetRenderStateHook<31>);  // D3DDevice_SetRenderState_StencilPass
BANJO_RENUT_HOOK(sub_8264BBC0, SetRenderStateHook<32>);  // D3DDevice_SetRenderState_StencilFunc
BANJO_RENUT_HOOK(sub_8264BD60, SetRenderStateHook<33>);  // D3DDevice_SetRenderState_StencilRef
BANJO_RENUT_HOOK(sub_8264BD80, SetRenderStateHook<34>);  // D3DDevice_SetRenderState_StencilMask
BANJO_RENUT_HOOK(sub_8264BDA0, SetRenderStateHook<35>);  // D3DDevice_SetRenderState_StencilWriteMask
BANJO_RENUT_HOOK(sub_8264BCC0, SetRenderStateHook<36>);  // D3DDevice_SetRenderState_CCWStencilFail
BANJO_RENUT_HOOK(sub_8264BCF8, SetRenderStateHook<37>);  // D3DDevice_SetRenderState_CCWStencilZFail
BANJO_RENUT_HOOK(sub_8264BD30, SetRenderStateHook<38>);  // D3DDevice_SetRenderState_CCWStencilPass
BANJO_RENUT_HOOK(sub_8264BC90, SetRenderStateHook<39>);  // D3DDevice_SetRenderState_CCWStencilFunc
BANJO_RENUT_HOOK(sub_8264BDC0, SetRenderStateHook<40>);  // D3DDevice_SetRenderState_CCWStencilRef
BANJO_RENUT_HOOK(sub_8264BDE0, SetRenderStateHook<41>);  // D3DDevice_SetRenderState_CCWStencilMask
BANJO_RENUT_HOOK(sub_8264BE00, SetRenderStateHook<42>);  // D3DDevice_SetRenderState_CCWStencilWriteMask
BANJO_RENUT_HOOK(sub_8228F370, SetRenderStateHook<43>);  // D3DDevice_SetRenderState_ClipPlaneEnable
BANJO_RENUT_HOOK(sub_8264C048, SetRenderStateHook<44>);  // D3DDevice_SetRenderState_PointSize
BANJO_RENUT_HOOK(sub_8264C0A8, SetRenderStateHook<45>);  // D3DDevice_SetRenderState_PointSizeMin
BANJO_RENUT_HOOK(sub_8264C038, SetRenderStateHook<46>);  // D3DDevice_SetRenderState_PointSpriteEnable
BANJO_RENUT_HOOK(sub_822687D0, SetRenderStateHook<47>);  // D3DDevice_SetRenderState_PointSizeMax
BANJO_RENUT_HOOK(sub_8264BF20, SetRenderStateHook<48>);  // D3DDevice_SetRenderState_MultiSampleAntiAlias
BANJO_RENUT_HOOK(sub_8264BF50, SetRenderStateHook<49>);  // D3DDevice_SetRenderState_MultiSampleMask
BANJO_RENUT_HOOK(sub_8264BE30, SetRenderStateHook<50>);  // D3DDevice_SetRenderState_ScissorTestEnable
BANJO_RENUT_HOOK(sub_8264BE48, SetRenderStateHook<51>);  // D3DDevice_SetRenderState_SlopeScaleDepthBias
BANJO_RENUT_HOOK(sub_821FC910, SetRenderStateHook<52>);  // D3DDevice_SetRenderState_DepthBias
BANJO_RENUT_HOOK(sub_821FC8D8, SetRenderStateHook<53>);  // D3DDevice_SetRenderState_ColorWriteEnable
BANJO_RENUT_HOOK(sub_8264BF78, SetRenderStateHook<54>);  // D3DDevice_SetRenderState_ColorWriteEnable1
BANJO_RENUT_HOOK(sub_8264BFB8, SetRenderStateHook<55>);  // D3DDevice_SetRenderState_ColorWriteEnable2
BANJO_RENUT_HOOK(sub_8264BFF8, SetRenderStateHook<56>);  // D3DDevice_SetRenderState_ColorWriteEnable3
BANJO_RENUT_HOOK(sub_8264C748, SetRenderStateHook<57>);  // D3DDevice_SetRenderState_TessellationMode
BANJO_RENUT_HOOK(sub_8264C6D8, SetRenderStateHook<58>);  // D3DDevice_SetRenderState_MinTessellationLevel
BANJO_RENUT_HOOK(sub_8264C710, SetRenderStateHook<59>);  // D3DDevice_SetRenderState_MaxTessellationLevel
BANJO_RENUT_HOOK(sub_8264C108, SetRenderStateHook<60>);  // D3DDevice_SetRenderState_Wrap0
BANJO_RENUT_HOOK(sub_8264C128, SetRenderStateHook<61>);  // D3DDevice_SetRenderState_Wrap1
BANJO_RENUT_HOOK(sub_8264C150, SetRenderStateHook<62>);  // D3DDevice_SetRenderState_Wrap2
BANJO_RENUT_HOOK(sub_8264C178, SetRenderStateHook<63>);  // D3DDevice_SetRenderState_Wrap3
BANJO_RENUT_HOOK(sub_8264C1A0, SetRenderStateHook<64>);  // D3DDevice_SetRenderState_Wrap4
BANJO_RENUT_HOOK(sub_8264C1C8, SetRenderStateHook<65>);  // D3DDevice_SetRenderState_Wrap5
BANJO_RENUT_HOOK(sub_8264C1F0, SetRenderStateHook<66>);  // D3DDevice_SetRenderState_Wrap6
BANJO_RENUT_HOOK(sub_8264C218, SetRenderStateHook<67>);  // D3DDevice_SetRenderState_Wrap7
BANJO_RENUT_HOOK(sub_8264C238, SetRenderStateHook<68>);  // D3DDevice_SetRenderState_Wrap8
BANJO_RENUT_HOOK(sub_8264C258, SetRenderStateHook<69>);  // D3DDevice_SetRenderState_Wrap9
BANJO_RENUT_HOOK(sub_8264C280, SetRenderStateHook<70>);  // D3DDevice_SetRenderState_Wrap10
BANJO_RENUT_HOOK(sub_8264C2A8, SetRenderStateHook<71>);  // D3DDevice_SetRenderState_Wrap11
BANJO_RENUT_HOOK(sub_8264C2D0, SetRenderStateHook<72>);  // D3DDevice_SetRenderState_Wrap12
BANJO_RENUT_HOOK(sub_8264C2F8, SetRenderStateHook<73>);  // D3DDevice_SetRenderState_Wrap13
BANJO_RENUT_HOOK(sub_8264C320, SetRenderStateHook<74>);  // D3DDevice_SetRenderState_Wrap14
BANJO_RENUT_HOOK(sub_8264C348, SetRenderStateHook<75>);  // D3DDevice_SetRenderState_Wrap15
BANJO_RENUT_HOOK(sub_82205A80, SetRenderStateHook<76>);  // D3DDevice_SetRenderState_ViewportEnable
BANJO_RENUT_HOOK(sub_8264C478, SetRenderStateHook<77>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable
BANJO_RENUT_HOOK(sub_8264C510, SetRenderStateHook<78>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable1
BANJO_RENUT_HOOK(sub_8264C5A8, SetRenderStateHook<79>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable2
BANJO_RENUT_HOOK(sub_8264C640, SetRenderStateHook<80>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable3
BANJO_RENUT_HOOK(sub_8264C780, SetRenderStateHook<81>);  // D3DDevice_SetRenderState_HalfPixelOffset
BANJO_RENUT_HOOK(sub_8264C7B8, SetRenderStateHook<82>);  // D3DDevice_SetRenderState_PrimitiveResetEnable
BANJO_RENUT_HOOK(sub_8264C7E8, SetRenderStateHook<83>);  // D3DDevice_SetRenderState_PrimitiveResetIndex
BANJO_RENUT_HOOK(sub_8264C810, SetRenderStateHook<84>);  // D3DDevice_SetRenderState_AlphaToMaskEnable
BANJO_RENUT_HOOK(sub_8264C840, SetRenderStateHook<85>);  // D3DDevice_SetRenderState_AlphaToMaskOffsets
BANJO_RENUT_HOOK(sub_8264C878, SetRenderStateHook<86>);  // D3DDevice_SetRenderState_GuardBand_X
BANJO_RENUT_HOOK(sub_8264C8A8, SetRenderStateHook<87>);  // D3DDevice_SetRenderState_GuardBand_Y
BANJO_RENUT_HOOK(sub_8264C8D8, SetRenderStateHook<88>);  // D3DDevice_SetRenderState_DiscardBand_X
BANJO_RENUT_HOOK(sub_8264C900, SetRenderStateHook<89>);  // D3DDevice_SetRenderState_DiscardBand_Y
BANJO_RENUT_HOOK(sub_8264C970, SetRenderStateHook<90>);  // D3DDevice_SetRenderState_HiStencilEnable
BANJO_RENUT_HOOK(sub_8264C9A0, SetRenderStateHook<91>);  // D3DDevice_SetRenderState_HiStencilWriteEnable
BANJO_RENUT_HOOK(sub_8264C9D0, SetRenderStateHook<92>);  // D3DDevice_SetRenderState_HiStencilFunc
BANJO_RENUT_HOOK(sub_8264CA00, SetRenderStateHook<93>);  // D3DDevice_SetRenderState_HiStencilRef
BANJO_RENUT_HOOK(sub_8264CA20, SetRenderStateHook<94>);  // D3DDevice_SetRenderState_PresentInterval
BANJO_RENUT_HOOK(sub_8264CA30, SetRenderStateHook<95>);  // D3DDevice_SetRenderState_PresentImmediateThreshold
BANJO_RENUT_HOOK(sub_8264C930, SetRenderStateHook<96>);  // D3DDevice_SetRenderState_HiZEnable
BANJO_RENUT_HOOK(sub_822899D0, SetRenderStateHook<97>);  // D3DDevice_SetRenderState_HiZWriteEnable
BANJO_RENUT_HOOK(sub_8264B9E8, SetRenderStateHook<98>);  // D3DDevice_SetRenderState_LastPixel
BANJO_RENUT_HOOK(sub_8264BA20, SetRenderStateHook<99>);  // D3DDevice_SetRenderState_LineWidth
BANJO_RENUT_HOOK(sub_8264BA98, SetRenderStateHook<100>);  // D3DDevice_SetRenderState_Buffer2Frames

// Sampler states, by their index in N&B's table (off_82E60840).
BANJO_RENUT_HOOK(sub_8227D2D0, SetSamplerStateHook<0>);  // D3DDevice_SetSamplerState_AddressU
BANJO_RENUT_HOOK(sub_8220C4E0, SetSamplerStateHook<1>);  // D3DDevice_SetSamplerState_AddressV
BANJO_RENUT_HOOK(sub_8264D1C8, SetSamplerStateHook<2>);  // D3DDevice_SetSamplerState_AddressW
BANJO_RENUT_HOOK(sub_8264D128, SetSamplerStateHook<3>);  // D3DDevice_SetSamplerState_BorderColor
BANJO_RENUT_HOOK(sub_8222F4A0, SetSamplerStateHook<4>);  // D3DDevice_SetSamplerState_MagFilter
BANJO_RENUT_HOOK(sub_8222F568, SetSamplerStateHook<5>);  // D3DDevice_SetSamplerState_MinFilter
BANJO_RENUT_HOOK(sub_8228F2F0, SetSamplerStateHook<6>);  // D3DDevice_SetSamplerState_MipFilter
BANJO_RENUT_HOOK(sub_8264CFF8, SetSamplerStateHook<7>);  // D3DDevice_SetSamplerState_MipMapLodBias
BANJO_RENUT_HOOK(sub_8222F878, SetSamplerStateHook<8>);  // D3DDevice_SetSamplerState_MaxMipLevel
BANJO_RENUT_HOOK(sub_8264CCC8, SetSamplerStateHook<9>);  // D3DDevice_SetSamplerState_MaxAnisotropy
BANJO_RENUT_HOOK(sub_8264CB68, SetSamplerStateHook<10>);  // D3DDevice_SetSamplerState_MagFilterZ
BANJO_RENUT_HOOK(sub_8264CA88, SetSamplerStateHook<11>);  // D3DDevice_SetSamplerState_MinFilterZ
BANJO_RENUT_HOOK(sub_8264CC20, SetSamplerStateHook<12>);  // D3DDevice_SetSamplerState_SeparateZFilterEnable
BANJO_RENUT_HOOK(sub_8264D0A8, SetSamplerStateHook<13>);  // D3DDevice_SetSamplerState_MinMipLevel
BANJO_RENUT_HOOK(sub_8264D218, SetSamplerStateHook<14>);  // D3DDevice_SetSamplerState_TrilinearThreshold
BANJO_RENUT_HOOK(sub_8264CD40, SetSamplerStateHook<15>);  // D3DDevice_SetSamplerState_AnisotropyBias
BANJO_RENUT_HOOK(sub_8264D270, SetSamplerStateHook<16>);  // D3DDevice_SetSamplerState_HGradientExpBias
BANJO_RENUT_HOOK(sub_8264D2C8, SetSamplerStateHook<17>);  // D3DDevice_SetSamplerState_VGradientExpBias
BANJO_RENUT_HOOK(sub_8264D320, SetSamplerStateHook<18>);  // D3DDevice_SetSamplerState_WhiteBorderColorW
BANJO_RENUT_HOOK(sub_8264D378, SetSamplerStateHook<19>);  // D3DDevice_SetSamplerState_PointBorderEnable

BANJO_RENUT_HOOK(sub_8222F6F8, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture
BANJO_RENUT_HOOK(sub_82BA6580, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture_Inline
BANJO_RENUT_HOOK(sub_8222ABB8, D3DDevice_SetStreamSource_hook);  // D3DDevice_SetStreamSource
BANJO_RENUT_HOOK(sub_8220EE38, D3DDevice_SetIndices_hook);  // D3DDevice_SetIndices
BANJO_RENUT_HOOK(sub_82205700, D3DDevice_SetVertexDeclaration_hook);  // D3DDevice_SetVertexDeclaration
BANJO_RENUT_HOOK(sub_82222EA0, D3DDevice_SetViewport_hook);  // D3DDevice_SetViewport
BANJO_RENUT_HOOK(sub_82222F20, D3D_SetViewport_hook);  // D3D_SetViewport
BANJO_RENUT_HOOK(sub_82223178, D3DDevice_SetScissorRect_hook);  // D3DDevice_SetScissorRect
BANJO_RENUT_HOOK_SHARED(sub_82223270, D3D_SetSurfaceClip_hook);  // D3D_SetSurfaceClip
BANJO_RENUT_HOOK(sub_8227AB68, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget
BANJO_RENUT_HOOK(sub_8227AF40, D3DDevice_SetDepthStencilSurface_hook);  // D3DDevice_SetDepthStencilSurface
BANJO_RENUT_HOOK(sub_8227AEB0, D3D_SetSurfaceInfo_hook);  // D3D_SetSurfaceInfo
BANJO_RENUT_HOOK_SHARED(sub_8222B078, D3DDevice_SetPredication_hook);  // D3DDevice_SetPredication
BANJO_RENUT_HOOK_SHARED(sub_82658098, D3DDevice_SetCommandBufferPredication_hook);  // D3DDevice_SetCommandBufferPredication
BANJO_RENUT_HOOK_SHARED(sub_82220B20, D3DDevice_SetShaderGPRAllocation_hook);  // D3DDevice_SetShaderGPRAllocation
BANJO_RENUT_HOOK(sub_8264DE58, D3D_CleanupGprAllocationSetup_hook);  // D3D_CleanupGprAllocationSetup
BANJO_RENUT_HOOK_SHARED(sub_826583A8, D3DDevice_SetScreenExtentQueryMode_hook);  // D3DDevice_SetScreenExtentQueryMode
BANJO_RENUT_HOOK(sub_8264DD40, D3DDevice_UnsetAll_hook);  // D3DDevice_UnsetAll
BANJO_RENUT_HOOK_SHARED(sub_82287D38, D3D_ResetAllState_hook);  // D3D_ResetAllState
BANJO_RENUT_HOOK_SHARED(sub_8264DCC0, D3D_InvalidateConstants_hook);  // D3D_InvalidateConstants
BANJO_RENUT_HOOK_SHARED(sub_82656EA8, D3DDevice_BlockUntilIdle_hook);  // D3DDevice_BlockUntilIdle
// Through a thunk (j_D3DDevice_SetRenderTarget 0x8264D460).
BANJO_RENUT_HOOK(sub_8264D460, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget_Thunk
BANJO_RENUT_HOOK(sub_8264D468, D3DDevice_GetRenderTarget_hook);  // D3DDevice_GetRenderTarget
BANJO_RENUT_HOOK(sub_8264D4A8, D3DDevice_GetDepthStencilSurface_hook);  // D3DDevice_GetDepthStencilSurface
BANJO_RENUT_HOOK(sub_8264D3D8, D3DDevice_GetStreamSource_hook);  // D3DDevice_GetStreamSource
BANJO_RENUT_HOOK(sub_82276AC0, D3DDevice_GetViewport_hook);  // D3DDevice_GetViewport
BANJO_RENUT_HOOK(sub_8264D680, D3DDevice_SetClipPlane_hook);  // D3DDevice_SetClipPlane
BANJO_RENUT_HOOK(sub_8264D6D0, D3DDevice_SetVertexFetchConstant_hook);  // D3DDevice_SetVertexFetchConstant
BANJO_RENUT_HOOK(sub_82221930, D3DDevice_SetVertexShaderConstantB_hook);  // D3DDevice_SetVertexShaderConstantB
BANJO_RENUT_HOOK(sub_82268810, D3DDevice_SetPixelShaderConstantB_hook);  // D3DDevice_SetPixelShaderConstantB
BANJO_RENUT_HOOK(sub_8264E108, D3DDevice_SetVertexShaderConstantI_hook);  // D3DDevice_SetVertexShaderConstantI
BANJO_RENUT_HOOK(sub_8264E160, D3DDevice_SetPixelShaderConstantI_hook);  // D3DDevice_SetPixelShaderConstantI
BANJO_RENUT_HOOK(sub_82655C60, D3DDevice_EndIndexedVertices_hook);  // D3DDevice_EndIndexedVertices

// Render state getters, by their index in N&B's table (off_82E60360, field 0).
BANJO_RENUT_HOOK(sub_8264BB00, GetRenderStateHook<10>);  // D3DDevice_GetRenderState_ZEnable
BANJO_RENUT_HOOK(sub_8264BB38, GetRenderStateHook<11>);  // D3DDevice_GetRenderState_ZFunc
BANJO_RENUT_HOOK(sub_8264BB28, GetRenderStateHook<12>);  // D3DDevice_GetRenderState_ZWriteEnable
BANJO_RENUT_HOOK(sub_8264B810, GetRenderStateHook<13>);  // D3DDevice_GetRenderState_FillMode
BANJO_RENUT_HOOK(sub_8264B800, GetRenderStateHook<14>);  // D3DDevice_GetRenderState_CullMode
BANJO_RENUT_HOOK(sub_8264B858, GetRenderStateHook<15>);  // D3DDevice_GetRenderState_AlphaBlendEnable
BANJO_RENUT_HOOK(sub_8264B8C8, GetRenderStateHook<16>);  // D3DDevice_GetRenderState_SeparateAlphaBlendEnable
BANJO_RENUT_HOOK(sub_8264B970, GetRenderStateHook<17>);  // D3DDevice_GetRenderState_BlendFactor
BANJO_RENUT_HOOK(sub_8264B878, GetRenderStateHook<18>);  // D3DDevice_GetRenderState_SrcBlend
BANJO_RENUT_HOOK(sub_8264B888, GetRenderStateHook<19>);  // D3DDevice_GetRenderState_DestBlend
BANJO_RENUT_HOOK(sub_8264B868, GetRenderStateHook<20>);  // D3DDevice_GetRenderState_BlendOp
BANJO_RENUT_HOOK(sub_8264B8A8, GetRenderStateHook<21>);  // D3DDevice_GetRenderState_SrcBlendAlpha
BANJO_RENUT_HOOK(sub_8264B8B8, GetRenderStateHook<22>);  // D3DDevice_GetRenderState_DestBlendAlpha
BANJO_RENUT_HOOK(sub_8264B898, GetRenderStateHook<23>);  // D3DDevice_GetRenderState_BlendOpAlpha
BANJO_RENUT_HOOK(sub_8264B848, GetRenderStateHook<24>);  // D3DDevice_GetRenderState_AlphaTestEnable
BANJO_RENUT_HOOK(sub_8264B910, GetRenderStateHook<25>);  // D3DDevice_GetRenderState_AlphaRef
BANJO_RENUT_HOOK(sub_8264B960, GetRenderStateHook<26>);  // D3DDevice_GetRenderState_AlphaFunc
BANJO_RENUT_HOOK(sub_8264BB80, GetRenderStateHook<27>);  // D3DDevice_GetRenderState_StencilEnable
BANJO_RENUT_HOOK(sub_8264BBB0, GetRenderStateHook<28>);  // D3DDevice_GetRenderState_TwoSidedStencilMode
BANJO_RENUT_HOOK(sub_8264BC18, GetRenderStateHook<29>);  // D3DDevice_GetRenderState_StencilFail
BANJO_RENUT_HOOK(sub_8264BC50, GetRenderStateHook<30>);  // D3DDevice_GetRenderState_StencilZFail
BANJO_RENUT_HOOK(sub_8264BC80, GetRenderStateHook<31>);  // D3DDevice_GetRenderState_StencilPass
BANJO_RENUT_HOOK(sub_8264BBE0, GetRenderStateHook<32>);  // D3DDevice_GetRenderState_StencilFunc
BANJO_RENUT_HOOK(sub_8264BD78, GetRenderStateHook<33>);  // D3DDevice_GetRenderState_StencilRef
BANJO_RENUT_HOOK(sub_8264BD98, GetRenderStateHook<34>);  // D3DDevice_GetRenderState_StencilMask
BANJO_RENUT_HOOK(sub_8264BDB8, GetRenderStateHook<35>);  // D3DDevice_GetRenderState_StencilWriteMask
BANJO_RENUT_HOOK(sub_8264BCE8, GetRenderStateHook<36>);  // D3DDevice_GetRenderState_CCWStencilFail
BANJO_RENUT_HOOK(sub_8264BD20, GetRenderStateHook<37>);  // D3DDevice_GetRenderState_CCWStencilZFail
BANJO_RENUT_HOOK(sub_8264BD50, GetRenderStateHook<38>);  // D3DDevice_GetRenderState_CCWStencilPass
BANJO_RENUT_HOOK(sub_8264BCB0, GetRenderStateHook<39>);  // D3DDevice_GetRenderState_CCWStencilFunc
BANJO_RENUT_HOOK(sub_8264BDD8, GetRenderStateHook<40>);  // D3DDevice_GetRenderState_CCWStencilRef
BANJO_RENUT_HOOK(sub_8264BDF8, GetRenderStateHook<41>);  // D3DDevice_GetRenderState_CCWStencilMask
BANJO_RENUT_HOOK(sub_8264BE18, GetRenderStateHook<42>);  // D3DDevice_GetRenderState_CCWStencilWriteMask
BANJO_RENUT_HOOK(sub_8264BE20, GetRenderStateHook<43>);  // D3DDevice_GetRenderState_ClipPlaneEnable
BANJO_RENUT_HOOK(sub_8264C098, GetRenderStateHook<44>);  // D3DDevice_GetRenderState_PointSize
BANJO_RENUT_HOOK(sub_8264C0E8, GetRenderStateHook<45>);  // D3DDevice_GetRenderState_PointSizeMin
BANJO_RENUT_HOOK(sub_8264C040, GetRenderStateHook<46>);  // D3DDevice_GetRenderState_PointSpriteEnable
BANJO_RENUT_HOOK(sub_8264C0F8, GetRenderStateHook<47>);  // D3DDevice_GetRenderState_PointSizeMax
BANJO_RENUT_HOOK(sub_8264BF40, GetRenderStateHook<48>);  // D3DDevice_GetRenderState_MultiSampleAntiAlias
BANJO_RENUT_HOOK(sub_8264BF68, GetRenderStateHook<49>);  // D3DDevice_GetRenderState_MultiSampleMask
BANJO_RENUT_HOOK(sub_8264BE40, GetRenderStateHook<50>);  // D3DDevice_GetRenderState_ScissorTestEnable
BANJO_RENUT_HOOK(sub_8264BEF0, GetRenderStateHook<51>);  // D3DDevice_GetRenderState_SlopeScaleDepthBias
BANJO_RENUT_HOOK(sub_8264BF10, GetRenderStateHook<52>);  // D3DDevice_GetRenderState_DepthBias
BANJO_RENUT_HOOK(sub_8264BF70, GetRenderStateHook<53>);  // D3DDevice_GetRenderState_ColorWriteEnable
BANJO_RENUT_HOOK(sub_8264BFB0, GetRenderStateHook<54>);  // D3DDevice_GetRenderState_ColorWriteEnable1
BANJO_RENUT_HOOK(sub_8264BFF0, GetRenderStateHook<55>);  // D3DDevice_GetRenderState_ColorWriteEnable2
BANJO_RENUT_HOOK(sub_8264C030, GetRenderStateHook<56>);  // D3DDevice_GetRenderState_ColorWriteEnable3
BANJO_RENUT_HOOK(sub_8264C770, GetRenderStateHook<57>);  // D3DDevice_GetRenderState_TessellationMode
BANJO_RENUT_HOOK(sub_8264C700, GetRenderStateHook<58>);  // D3DDevice_GetRenderState_MinTessellationLevel
BANJO_RENUT_HOOK(sub_8264C738, GetRenderStateHook<59>);  // D3DDevice_GetRenderState_MaxTessellationLevel
BANJO_RENUT_HOOK(sub_8264C368, GetRenderStateHook<60>);  // D3DDevice_GetRenderState_Wrap0
BANJO_RENUT_HOOK(sub_8264C378, GetRenderStateHook<61>);  // D3DDevice_GetRenderState_Wrap1
BANJO_RENUT_HOOK(sub_8264C388, GetRenderStateHook<62>);  // D3DDevice_GetRenderState_Wrap2
BANJO_RENUT_HOOK(sub_8264C398, GetRenderStateHook<63>);  // D3DDevice_GetRenderState_Wrap3
BANJO_RENUT_HOOK(sub_8264C3A8, GetRenderStateHook<64>);  // D3DDevice_GetRenderState_Wrap4
BANJO_RENUT_HOOK(sub_8264C3B8, GetRenderStateHook<65>);  // D3DDevice_GetRenderState_Wrap5
BANJO_RENUT_HOOK(sub_8264C3C8, GetRenderStateHook<66>);  // D3DDevice_GetRenderState_Wrap6
BANJO_RENUT_HOOK(sub_8264C3D8, GetRenderStateHook<67>);  // D3DDevice_GetRenderState_Wrap7
BANJO_RENUT_HOOK(sub_8264C3E8, GetRenderStateHook<68>);  // D3DDevice_GetRenderState_Wrap8
BANJO_RENUT_HOOK(sub_8264C3F8, GetRenderStateHook<69>);  // D3DDevice_GetRenderState_Wrap9
BANJO_RENUT_HOOK(sub_8264C408, GetRenderStateHook<70>);  // D3DDevice_GetRenderState_Wrap10
BANJO_RENUT_HOOK(sub_8264C418, GetRenderStateHook<71>);  // D3DDevice_GetRenderState_Wrap11
BANJO_RENUT_HOOK(sub_8264C428, GetRenderStateHook<72>);  // D3DDevice_GetRenderState_Wrap12
BANJO_RENUT_HOOK(sub_8264C438, GetRenderStateHook<73>);  // D3DDevice_GetRenderState_Wrap13
BANJO_RENUT_HOOK(sub_8264C448, GetRenderStateHook<74>);  // D3DDevice_GetRenderState_Wrap14
BANJO_RENUT_HOOK(sub_8264C458, GetRenderStateHook<75>);  // D3DDevice_GetRenderState_Wrap15
BANJO_RENUT_HOOK(sub_8264C468, GetRenderStateHook<76>);  // D3DDevice_GetRenderState_ViewportEnable
BANJO_RENUT_HOOK(sub_8264C508, GetRenderStateHook<77>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable
BANJO_RENUT_HOOK(sub_8264C5A0, GetRenderStateHook<78>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable1
BANJO_RENUT_HOOK(sub_8264C638, GetRenderStateHook<79>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable2
BANJO_RENUT_HOOK(sub_8264C6D0, GetRenderStateHook<80>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable3
BANJO_RENUT_HOOK(sub_8264C7A8, GetRenderStateHook<81>);  // D3DDevice_GetRenderState_HalfPixelOffset
BANJO_RENUT_HOOK(sub_8264C7D8, GetRenderStateHook<82>);  // D3DDevice_GetRenderState_PrimitiveResetEnable
BANJO_RENUT_HOOK(sub_8264C808, GetRenderStateHook<83>);  // D3DDevice_GetRenderState_PrimitiveResetIndex
BANJO_RENUT_HOOK(sub_8264C830, GetRenderStateHook<84>);  // D3DDevice_GetRenderState_AlphaToMaskEnable
BANJO_RENUT_HOOK(sub_8264C860, GetRenderStateHook<85>);  // D3DDevice_GetRenderState_AlphaToMaskOffsets
BANJO_RENUT_HOOK(sub_8264C8A0, GetRenderStateHook<86>);  // D3DDevice_GetRenderState_GuardBand_X
BANJO_RENUT_HOOK(sub_8264C8D0, GetRenderStateHook<87>);  // D3DDevice_GetRenderState_GuardBand_Y
BANJO_RENUT_HOOK(sub_8264C8F8, GetRenderStateHook<88>);  // D3DDevice_GetRenderState_DiscardBand_X
BANJO_RENUT_HOOK(sub_8264C928, GetRenderStateHook<89>);  // D3DDevice_GetRenderState_DiscardBand_Y
BANJO_RENUT_HOOK(sub_8264C990, GetRenderStateHook<90>);  // D3DDevice_GetRenderState_HiStencilEnable
BANJO_RENUT_HOOK(sub_8264C9C0, GetRenderStateHook<91>);  // D3DDevice_GetRenderState_HiStencilWriteEnable
BANJO_RENUT_HOOK(sub_8264C9F0, GetRenderStateHook<92>);  // D3DDevice_GetRenderState_HiStencilFunc
BANJO_RENUT_HOOK(sub_8264CA18, GetRenderStateHook<93>);  // D3DDevice_GetRenderState_HiStencilRef
BANJO_RENUT_HOOK(sub_8264CA28, GetRenderStateHook<94>);  // D3DDevice_GetRenderState_PresentInterval
BANJO_RENUT_HOOK(sub_8264CA40, GetRenderStateHook<95>);  // D3DDevice_GetRenderState_PresentImmediateThreshold
BANJO_RENUT_HOOK(sub_8264C950, GetRenderStateHook<96>);  // D3DDevice_GetRenderState_HiZEnable
BANJO_RENUT_HOOK(sub_8264C960, GetRenderStateHook<97>);  // D3DDevice_GetRenderState_HiZWriteEnable
BANJO_RENUT_HOOK(sub_8264BA10, GetRenderStateHook<98>);  // D3DDevice_GetRenderState_LastPixel
BANJO_RENUT_HOOK(sub_8264BA60, GetRenderStateHook<99>);  // D3DDevice_GetRenderState_LineWidth
BANJO_RENUT_HOOK(sub_8264BAB8, GetRenderStateHook<100>);  // D3DDevice_GetRenderState_Buffer2Frames

// Sampler state getters (off_82E60840, field 0).
BANJO_RENUT_HOOK(sub_8264D198, GetSamplerStateHook<0>);  // D3DDevice_GetSamplerState_AddressU
BANJO_RENUT_HOOK(sub_8264D1B0, GetSamplerStateHook<1>);  // D3DDevice_GetSamplerState_AddressV
BANJO_RENUT_HOOK(sub_8264D200, GetSamplerStateHook<2>);  // D3DDevice_GetSamplerState_AddressW
BANJO_RENUT_HOOK(sub_8264D178, GetSamplerStateHook<3>);  // D3DDevice_GetSamplerState_BorderColor
BANJO_RENUT_HOOK(sub_8264CB30, GetSamplerStateHook<4>);  // D3DDevice_GetSamplerState_MagFilter
BANJO_RENUT_HOOK(sub_8264CA50, GetSamplerStateHook<5>);  // D3DDevice_GetSamplerState_MinFilter
BANJO_RENUT_HOOK(sub_8264CC08, GetSamplerStateHook<6>);  // D3DDevice_GetSamplerState_MipFilter
BANJO_RENUT_HOOK(sub_8264D058, GetSamplerStateHook<7>);  // D3DDevice_GetSamplerState_MipMapLodBias
BANJO_RENUT_HOOK(sub_8264D098, GetSamplerStateHook<8>);  // D3DDevice_GetSamplerState_MaxMipLevel
BANJO_RENUT_HOOK(sub_8264CD30, GetSamplerStateHook<9>);  // D3DDevice_GetSamplerState_MaxAnisotropy
BANJO_RENUT_HOOK(sub_8264CBF8, GetSamplerStateHook<10>);  // D3DDevice_GetSamplerState_MagFilterZ
BANJO_RENUT_HOOK(sub_8264CB20, GetSamplerStateHook<11>);  // D3DDevice_GetSamplerState_MinFilterZ
BANJO_RENUT_HOOK(sub_8264CCB8, GetSamplerStateHook<12>);  // D3DDevice_GetSamplerState_SeparateZFilterEnable
BANJO_RENUT_HOOK(sub_8264D118, GetSamplerStateHook<13>);  // D3DDevice_GetSamplerState_MinMipLevel
BANJO_RENUT_HOOK(sub_8264D258, GetSamplerStateHook<14>);  // D3DDevice_GetSamplerState_TrilinearThreshold
BANJO_RENUT_HOOK(sub_8264CFB0, GetSamplerStateHook<15>);  // D3DDevice_GetSamplerState_AnisotropyBias
BANJO_RENUT_HOOK(sub_8264D2B0, GetSamplerStateHook<16>);  // D3DDevice_GetSamplerState_HGradientExpBias
BANJO_RENUT_HOOK(sub_8264D308, GetSamplerStateHook<17>);  // D3DDevice_GetSamplerState_VGradientExpBias
BANJO_RENUT_HOOK(sub_8264D360, GetSamplerStateHook<18>);  // D3DDevice_GetSamplerState_WhiteBorderColorW
BANJO_RENUT_HOOK(sub_8264D3C0, GetSamplerStateHook<19>);  // D3DDevice_GetSamplerState_PointBorderEnable
