// me2-nx-deko (2026-10-27, me2_renut_ui_fsr): AMD FidelityFX Super Resolution 1 EASU (ffx_fsr1.h, MIT, see
// ffx_fsr1_LICENSE.txt) for the HDR scene the frame's final pass reads (textures.cpp UiFsrSceneLocked). With the scene
// at 360p and the final pass (tonemap, HUD, menus) at 720p, the game's tonemap would stretch the scene bilinearly; this
// upscales it edge-aware into a target of the final pass's size first.
//   g_Push.textureIndex: the scene (its whole texture is the input viewport), samplerIndex: linear clamp,
//   param0 / param1: the output size in pixels. present_vs draws the fullscreen triangle (scale 1, translate 0).
// Alpha is not part of EASU: it is sampled bilinearly (UE3 keeps data there).
#include "utility.hlsli"

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p)
{
    return g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].GatherRed(
        g_Samplers[NonUniformResourceIndex(g_Push.samplerIndex)], p);
}
AF4 FsrEasuGF(AF2 p)
{
    return g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].GatherGreen(
        g_Samplers[NonUniformResourceIndex(g_Push.samplerIndex)], p);
}
AF4 FsrEasuBF(AF2 p)
{
    return g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].GatherBlue(
        g_Samplers[NonUniformResourceIndex(g_Push.samplerIndex)], p);
}
#include "ffx_fsr1.h"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    const Texture2D<float4> source = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)];
    uint width, height;
    source.GetDimensions(width, height);
    AU4 con0, con1, con2, con3;
    FsrEasuCon(con0, con1, con2, con3, AF1(width), AF1(height), AF1(width), AF1(height), g_Push.param0,
               g_Push.param1);
    AF3 color;
    FsrEasuF(color, AU2(position.xy), con0, con1, con2, con3);
    const float alpha =
        source.SampleLevel(g_Samplers[NonUniformResourceIndex(g_Push.samplerIndex)], uv, 0.0).a;
    return float4(color, alpha);
}
