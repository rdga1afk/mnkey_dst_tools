// Ground live consumer (ours, glue around the ORIGINAL post/fog.hlsl atmosphere_fog_fs, which includes atmospherefog.hlsl): the fog quad of the main compositor over the lit live
// ground (GroundLivePass::Lit), then the fixed exposure of the exe. The original's globals become statics (fog_planes_fs assigns to one of them, which a uniform cannot be) and are
// set from our own uniforms at the start of main; the original text is untouched. s0 = the original's gBuf2 (our distance / farClip target), s1 = the lit colour.
// A static sampler is dropped by the compiler, so the original's gBuf2 reads go through our own sampler at the original's register (tex2D is a macro while the original is included).
uniform sampler md_gBuf2 : register(s0);
// DEVIATION: our ground reaches past the original's farClip (the original's far plane culls it); the original program saturates distance / farClip there, and
// atmosphere_fog_fs treats distance == farClip as sky (no fog). A written ground pixel is kept just inside farClip, so it gets the fog of the far clip distance.
float4 md_tex2D(float2 uv) { float4 v = tex2D(md_gBuf2, uv); v.r = min(v.r, 0.99999); return v; }
#define tex2D(s, uv) md_tex2D(uv)
#define VPOS SV_Position
#define COLOR SV_Target0
#define uniform static
#include "@FOG@"
#undef uniform
#undef COLOR
#undef tex2D
uniform float3 md_sunDirectionReal; uniform float4 md_pFogParams; uniform float3 md_uCameraPos; uniform float3 md_uInvWaveLength;
uniform float md_uInnerRadius; uniform float md_uExposure; uniform float md_uKrESun; uniform float md_uKr4PI; uniform float md_uKm4PI;
uniform float md_uScale; uniform float md_uScaleDepth; uniform float md_uScaleOverScaleDepth; uniform float md_uSkydomeRadius; uniform float4 md_horizonClouds;
uniform float md_farClip; uniform float md_exposure;
uniform sampler litTex : register(s1);
float4 main(float4 fragCoord : SV_Position, float2 texCoord : TEXCOORD0, float3 ray : TEXCOORD1) : SV_Target0
{
    sunDirectionReal = md_sunDirectionReal; pFogParams = md_pFogParams; uCameraPos = md_uCameraPos; uInvWaveLength = md_uInvWaveLength;
    uInnerRadius = md_uInnerRadius; uExposure = md_uExposure; uKrESun = md_uKrESun; uKr4PI = md_uKr4PI; uKm4PI = md_uKm4PI;
    uScale = md_uScale; uScaleDepth = md_uScaleDepth; uScaleOverScaleDepth = md_uScaleOverScaleDepth; uSkydomeRadius = md_uSkydomeRadius; horizonClouds = md_horizonClouds;
    mat0 = float4(1, 0, 0, 0); mat1 = float4(0, 1, 0, 0); mat2 = float4(0, 0, 1, 0);   // the light VS already gives a world-space ray; identity view, camera at the origin
    camera = float3(0, 0, 0); farClip = md_farClip; offset = float4(0, 0, 0, 0); fogColour = float4(0, 0, 0, 0); fogDensity = 0.0; sunColour = float4(0, 0, 0, 0);
    float4 lit = tex2D(litTex, texCoord);
    if (lit.a < 0.5) discard;
    float4 f = atmosphere_fog_fs(fragCoord, texCoord, ray);
    return float4(lerp(lit.rgb, f.rgb, f.a) * md_exposure, 1.0);
}
