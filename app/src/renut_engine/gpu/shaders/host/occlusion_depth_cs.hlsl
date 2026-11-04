// ME2 (2026-10-28, me2_occlusion_umbra_cpu): the scene depth reduced to a small grid for Umbra's occlusion tests on the
// CPU (app/me2_src/me2_occlusion.cpp). One thread per cell: the nearest and the farthest depth of the host texels the
// cell covers, stored as two floats (cell row after row).
//   set 0 : bindless Texture2D table (t0, space0), the depth target's sample view
//   set 1 : RWByteAddressBuffer      (u0, space1), the cells
//   push  : TargetReadbackPush       (b0, space2): textureIndex, width / height = the grid, scaleNumerator /
//           scaleDenominator = the source's host size in texels (the guest area of the target, without tiling growth)
// Shares BuildTargetReadbackPipeline's layout (video.cpp).

#include "deko3d.hlsli"
struct TargetReadbackPush
{
    uint textureIndex;
    uint width;
    uint height;
    uint sourceWidth;
    uint sourceHeight;
    uint pad0;
    uint pad1;
    uint pad2;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<TargetReadbackPush> g_Push;
#else
ConstantBuffer<TargetReadbackPush> g_Push : register(b0, space2);
#endif

Texture2D<float4> g_Textures[] : register(t0, space0);
RWByteAddressBuffer g_Cells : register(u0, space1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Push.width || id.y >= g_Push.height)
        return;
    Texture2D<float4> source = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)];
    const uint2 first = uint2(BANJO_UDIV(id.x * g_Push.sourceWidth, g_Push.width),
                              BANJO_UDIV(id.y * g_Push.sourceHeight, g_Push.height));
    const uint2 end = max(uint2(BANJO_UDIV((id.x + 1u) * g_Push.sourceWidth, g_Push.width),
                                BANJO_UDIV((id.y + 1u) * g_Push.sourceHeight, g_Push.height)),
                          first + 1u);
    float nearest = 1.0e30;
    float farthest = -1.0e30;
    for (uint y = first.y; y < end.y; ++y)
    {
        for (uint x = first.x; x < end.x; ++x)
        {
            const float d = source.Load(int3(x, y, 0)).r;
            nearest = min(nearest, d);
            farthest = max(farthest, d);
        }
    }
    const uint at = (id.y * g_Push.width + id.x) * 8u;
    g_Cells.Store2(at, uint2(asuint(nearest), asuint(farthest)));
}
