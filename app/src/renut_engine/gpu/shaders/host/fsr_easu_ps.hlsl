// me2-nx-deko (2026-10-27, me2_renut_ui_fsr): AMD FidelityFX Super Resolution 1 EASU (ffx_fsr1.h, MIT, see
// ffx_fsr1_LICENSE.txt) for the HDR scene the frame's final pass reads (textures.cpp UiFsrSceneLocked). With the scene
// at 360p and the final pass (tonemap, HUD, menus) at 720p, the game's tonemap would stretch the scene bilinearly; this
// upscales it edge-aware into a target of the final pass's size first.
//   g_Push.textureIndex: the scene (its whole texture is the input viewport), samplerIndex: linear clamp,
//   easuCon0..3: AMD's constants supplied once per draw by MakeEasuConstants.
//   present_vs draws the fullscreen triangle (scale 1, translate 0).
// Alpha is not part of EASU: it is sampled bilinearly (UE3 keeps data there).
#define UTILITY_EASU_CONSTANTS 1
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
    AF3 color;
    // Preserve the aliases and literal zeros of FsrEasuCon so the compiler can
    // still fold equal offsets and the zero X offset of the final gather.
    const AU4 con1 = AU4(g_Push.easuCon1.x, g_Push.easuCon1.y, g_Push.easuCon1.x, g_Push.easuCon1.w);
    const AU4 con2 = AU4(g_Push.easuCon2.x, g_Push.easuCon2.y, g_Push.easuCon1.x, g_Push.easuCon2.y);
    const AU4 con3 = AU4(0u, g_Push.easuCon3.y, 0u, 0u);
    FsrEasuF(color, AU2(position.xy), g_Push.easuCon0, con1, con2, con3);
    const float alpha =
        source.SampleLevel(g_Samplers[NonUniformResourceIndex(g_Push.samplerIndex)], uv, 0.0).a;
    return float4(color, alpha);
}
