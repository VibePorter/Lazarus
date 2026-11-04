// me2-nx-deko (2026-10-27, me2_renut_fxaa): NVIDIA FXAA 3.11 (ffx_fxaa3_11.h, BSD-3, see ffx_fxaa3_11_LICENSE.txt) on
// the HDR scene the frame's final pass reads, at the scene's resolution and before FSR 1 EASU (textures.cpp
// UiFsrSceneLocked; AMD's advice: EASU wants anti-aliased input). The scene is linear HDR: FXAA gets a perceptual luma
// (sqrt(L / (1 + L))) with every fetch, and the scene's alpha (UE3 keeps data there) is passed through untouched.
//   g_Push.textureIndex: the scene, samplerIndex: linear clamp. present_vs draws the fullscreen triangle.
#include "utility.hlsli"

float4 MeFxaaFetch(float2 position, float2 offset, float2 rcpFrame)
{
    const float4 c = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].SampleLevel(
        g_Samplers[NonUniformResourceIndex(g_Push.samplerIndex)], position + offset * rcpFrame, 0.0);
    const float luma = dot(max(c.rgb, 0.0), float3(0.299, 0.587, 0.114));
    return float4(c.rgb, sqrt(luma / (1.0 + luma)));
}

#define ME_FXAA 1
#define FXAA_PC 1
#define FXAA_HLSL_5 0
#define FXAA_GREEN_AS_LUMA 0
#define FXAA_GATHER4_ALPHA 0
#define FXAA_QUALITY__PRESET 20
#include "ffx_fxaa3_11.h"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    const Texture2D<float4> source = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)];
    uint width, height;
    source.GetDimensions(width, height);
    const float2 rcpFrame = 1.0 / float2(width, height);
    const float2 pos = position.xy * rcpFrame;
    const float4 unused4 = float4(0.0, 0.0, 0.0, 0.0);
    // Quality settings: subpixel 0.75 (FXAA's default), edge threshold 0.166, minimum 0.0625 (ME2's scenes are dark).
    const float4 color = FxaaPixelShader(pos, unused4, 0u, 0u, 0u, rcpFrame, unused4, unused4, unused4, 0.75, 0.166,
                                         0.0625, 0.0, 0.0, 0.0, unused4);
    return float4(color.rgb, source.Load(int3(int2(position.xy), 0)).a);
}
