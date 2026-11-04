// Depth resolve: a host depth target to what a Xenos resolve of the EDRAM
// depth writes to memory. Drawn with present_vs over the destination; the
// source texel is the destination pixel plus g_Push.translate.
//   g_Push.textureIndex : the depth target's slot in the bindless table
//   g_Push.param0       : bit 0: 1 for D24FS8 (20e4 float depth), 0 for D24S8; bit 1 (me2-nx-deko,
//                         me2_renut_depth_resolve_average): a multisampled source's depth is the mean of its samples, whose
//                         count is bits 2 and up
// SV_Target0 is the depth a 24_8 / 24_8_FLOAT texture fetch returns; SV_Target1 is
// the guest word (depth << 8 | stencil) as bytes, least significant first,
// for 8_8_8_8 textures over the same memory.
//   g_Push.param1       : banjo-nx (2026-10-18): 1 when g_Push.samplerIndex is the
//                         depth target's stencil view (a uint table on set 2), whose
//                         value goes in the word's low byte; 0: stencil reads as 0.
// N&B's depth of field reads that byte (a fract(z * 128) test of its bit 0 marks
// the characters it keeps sharp): without it, Kazooie blurred in front of the focus.
#define UTILITY_OWN_TEXTURE_TABLE
#include "utility.hlsli"

#if DEPTH_RESOLVE_MS
Texture2DMS<float> g_Depth[] : register(t0, space0);
Texture2DMS<uint> g_Stencil[] : register(t0, space2);
#else
Texture2D<float> g_Depth[] : register(t0, space0);
Texture2D<uint> g_Stencil[] : register(t0, space2);
#endif

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PSOutput
{
    float depth : SV_Target0;
    float4 words : SV_Target1;
};

uint EncodeUnorm24(float depth)
{
    return uint(saturate(depth) * 16777215.0 + 0.5);
}

// 20e4: a 4-bit exponent (bias 15) above a 20-bit mantissa, covering [0, 2).
// From 2^-14 up a value is 1.mantissa * 2^(exponent - 15), which a float's
// exponent and top 20 mantissa bits give directly; below, the exponent is 0
// and the mantissa counts steps of 2^-34. Truncated, like the unorm path.
uint EncodeFloat20e4(float depth)
{
    const float d = clamp(depth, 0.0, 1.99999988);
    if (d < 6.103515625e-5) // 2^-14
        return uint(d * 17179869184.0); // 2^34
    const uint bits = asuint(d);
    return (((bits >> 23) - 112) << 20) | ((bits >> 3) & 0xFFFFF);
}

PSOutput main(PSInput input)
{
    const int2 texel = int2(input.position.xy) + int2(g_Push.translate);
    const uint index = NonUniformResourceIndex(g_Push.textureIndex);
    uint stencil = 0;
    const uint flags = uint(g_Push.param0);
#if DEPTH_RESOLVE_MS
    // me2-nx-deko (2026-10-25): sample 0 of a 2x target is a quarter pixel off the centre, so its depth is not the
    // depth a single-sample draw over it computes. ME2 draws its scene 2x, copies the depth to a 1x target and lights
    // it with GREATER_EQUAL: on sloped surfaces half of every face failed by up to 3e-5 (half-black faces). The
    // samples' mean is the centre's depth on a plane (the standard positions are symmetric).
    float depth = g_Depth[index].Load(texel, 0);
    if (flags & 2u)
    {
        const uint samples = flags >> 2;
        for (uint i = 1; i < samples; ++i)
            depth += g_Depth[index].Load(texel, i);
        depth /= float(samples);
    }
    if (g_Push.param1 != 0.0)
        stencil = g_Stencil[NonUniformResourceIndex(g_Push.samplerIndex)].Load(texel, 0) & 0xFF;
#else
    const float depth = g_Depth[index].Load(int3(texel, 0));
    if (g_Push.param1 != 0.0)
        stencil = g_Stencil[NonUniformResourceIndex(g_Push.samplerIndex)].Load(int3(texel, 0)) & 0xFF;
#endif
    const uint word = (((flags & 1u) != 0 ? EncodeFloat20e4(depth) : EncodeUnorm24(depth)) << 8) | stencil;

    PSOutput output;
    output.depth = depth;
    output.words = float4(word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF, word >> 24) / 255.0;
    return output;
}
