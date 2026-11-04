// banjo-nx (2026-10-18): a colour resolve drawn instead of copied (draw.cpp, DrawResolveCopy). deko3d copies images on
// the copy engine, which needs the whole GPU idle before and after it; a draw is ordered like any other. The destination
// pixel plus g_Push.translate is the source texel, read with Load through a view of the surface's own format (the
// destination has the same format, so every texel is written back with its bits). present_vs draws the fullscreen
// triangle; the scissor limits it to the resolved rect.
#include "utility.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    int2 texel = int2(position.xy) + int2(g_Push.translate);
    // me2-nx-deko (2026-10-25): param0, when not 0, scales the texel: a resolve's exponent bias (2^bias, D3DRESOLVE
    // Flags bits 26-31; UE3 resolves its HDR scene with -3), applied by ScaleTextureRegionLocked.
    const float scale = g_Push.param0 != 0.0 ? g_Push.param0 : 1.0;
    float4 value;
    // me2-nx-deko (2026-10-25): param1 = 3, a resolve of a 4x view from the single-sample view over its EDRAM: each
    // destination pixel is the mean of the 2x2 quad its four samples are (g_Push.translate in destination pixels).
    if (g_Push.param1 == 3.0)
    {
        const Texture2D<float4> source = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)];
        const int2 quad = texel * 2;
        value = (source.Load(int3(quad, 0)) + source.Load(int3(quad + int2(1, 0), 0)) +
                 source.Load(int3(quad + int2(0, 1), 0)) + source.Load(int3(quad + int2(1, 1), 0))) * 0.25;
    }
    else
        value = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0));
    // me2-nx-deko (2026-10-25): param1 = 1, the source is EDRAM written through a 2_10_10_10 view and the destination
    // reads it as 2_10_10_10_FLOAT (7e3: 3-bit exponent, 7-bit mantissa) - the same bits (SyncEdramAliasLocked).
    if (g_Push.param1 == 1.0)
    {
        const uint3 bits = uint3(round(saturate(value.rgb) * 1023.0));
        const float3 mantissa = float3(bits & 0x7Fu);
        const float3 exponent = float3(bits >> 7);
        value.rgb = select(exponent == 0.0, mantissa * (1.0 / 512.0),
                           (1.0 + mantissa * (1.0 / 128.0)) * exp2(exponent - 3.0));
        value.a = round(saturate(value.a) * 3.0) * (1.0 / 3.0);
    }
    // param1 = 2, the other way: the source is the 7e3 view's float value, the destination the 2_10_10_10 view, which
    // holds the 7e3 bits (exponent above a 7-bit mantissa; under 0.25 a denormal of 1/512 steps) as unorm.
    else if (g_Push.param1 == 2.0)
    {
        const float3 f = clamp(value.rgb, 0.0, 31.875);
        const float3 e = clamp(floor(log2(max(f, 1.0e-30))), -2.0, 4.0);
        float3 bits = (e + 3.0) * 128.0 + round((f / exp2(e) - 1.0) * 128.0);
        bits = select(f < 0.25, round(f * 512.0), bits);
        value.rgb = min(bits, 1023.0) * (1.0 / 1023.0);
        value.a = round(saturate(value.a) * 3.0) * (1.0 / 3.0);
    }
    return value * scale;
}
