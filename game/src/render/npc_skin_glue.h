#pragma once
// The glue of the NPC skin route (docs/research/NPC_INSTALL_SKIN_DESIGN_2026-11-14.md), shared with its CPU test.
#include <string>

namespace md::npcskin {

// deferred/skin.program: Skinned_FP_HLSL = skin.hlsl main_fs with this define; deferred/skin.material (Skinned, pass main): the texture units in register order and the diffuse bias.
inline constexpr const char* kDefine = "COLOURING";
inline constexpr const char* kEntry = "main_fs";
inline constexpr const char* kTextureUnits[3] = { "diffuseMap", "normalMap", "colorMap" };   // s0, s1, s2
inline constexpr float kDiffuseMipBias = -2.f;

// The install vertex program (skin.hlsl main_vs) takes a bone-matrix uniform array; ours keeps the engine's bone / instance buffers, so the vertex side is our glue with the same outputs, in the same order as main_fs reads them.
inline constexpr const char* kVert = R"([[vk::binding(0, 0)]] StructuredBuffer<float4> md_xzyr;
[[vk::binding(1, 0)]] StructuredBuffer<uint> md_visible;
[[vk::binding(3, 0)]] StructuredBuffer<float4> md_bones;
[[vk::binding(4, 0)]] StructuredBuffer<float4> md_skin;
uniform float4 vp[4]; uniform float4 base_inst;
struct VO { float4 pos : SV_Position; [[vk::location(0)]] float2 uv : TEXCOORD0; [[vk::location(1)]] float3 n : TEXCOORD1; [[vk::location(2)]] float3 b : TEXCOORD2; [[vk::location(3)]] float3 t : TEXCOORD3;
            [[vk::location(4)]] float3 cam : TEXCOORD4; [[vk::location(5)]] float4 wp : TEXCOORD5; [[vk::location(6)]] float3 wet : TEXCOORD6; [[vk::location(7)]] float4 skin : TEXCOORD7; };
float3 md_rot(float s, float c, float3 v) { return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z); }
VO main([[vk::location(0)]] float3 aPos : POSITION, [[vk::location(2)]] float3 aN : NORMAL, [[vk::location(5)]] float2 aUv : TEXCOORD0, [[vk::location(8)]] uint4 aJ : BLENDINDICES,
        [[vk::location(9)]] float4 aW : BLENDWEIGHT, [[vk::location(11)]] float4 aT : TANGENT, [[vk::location(12)]] float3 aB : BINORMAL, uint inst : SV_InstanceID) {
    uint ii = md_visible[(uint)base_inst.x + inst];
    float4 d = md_xzyr[ii];
    float s = sin(d.w), c = cos(d.w);
    uint bb = ii < 500u ? ii * 64u : 0u;
    float3 sp = 0, sn = 0, st = 0, sb = 0;
    for (int k = 0; k < 4; ++k) {
        uint o = (bb + aJ[k]) * 4u; float4 c0 = md_bones[o], c1 = md_bones[o + 1u], c2 = md_bones[o + 2u], c3 = md_bones[o + 3u]; float w = aW[k];
        sp += w * (c0.xyz * aPos.x + c1.xyz * aPos.y + c2.xyz * aPos.z + c3.xyz);
        sn += w * (c0.xyz * aN.x + c1.xyz * aN.y + c2.xyz * aN.z);
        st += w * (c0.xyz * aT.x + c1.xyz * aT.y + c2.xyz * aT.z);
        sb += w * (c0.xyz * aB.x + c1.xyz * aB.y + c2.xyz * aB.z);
    }
    float3 wp = md_rot(s, c, sp) + float3(d.x, d.z + 0.09, d.y);
    VO o;
    o.pos = vp[0] * wp.x + vp[1] * wp.y + vp[2] * wp.z + vp[3];
    o.uv = aUv; o.n = md_rot(s, c, sn); o.t = md_rot(s, c, st); o.b = md_rot(s, c, sb) * aT.w;
    o.cam = float3(0, 1, 0); o.wp = float4(wp, 1.0); o.wet = float3(0, 0, 0); o.skin = md_skin[ii];
    return o;
}
)";

// main_fs keeps its own signature; the wrapper adds the depth test against the terrain G-buffer and feeds the per-NPC skin colour as color1.
inline constexpr const char* kFragWrap = R"(sampler2D md_diffuse : register(s0); sampler2D md_normal : register(s1); sampler2D md_colour : register(s2);
Texture2D<float4> md_gdepth : register(s3);
uniform float farClip; uniform float3 cameraPos; uniform float glossMult; uniform float waterHeightRel;
void main(float4 fragCoord : SV_Position, [[vk::location(0)]] float2 texCoord : TEXCOORD0, [[vk::location(1)]] float3 normal : TEXCOORD1, [[vk::location(2)]] float3 binormal : TEXCOORD2,
          [[vk::location(3)]] float3 tangent : TEXCOORD3, [[vk::location(4)]] float3 camDir : TEXCOORD4, [[vk::location(5)]] float4 worldPos : TEXCOORD5, [[vk::location(6)]] float3 wet : TEXCOORD6,
          [[vk::location(7)]] float4 skin : TEXCOORD7, out GBuffer buffer) {
    if (fragCoord.z > md_gdepth.Load(int3(int2(fragCoord.xy), 0)).r) discard;
    sk_orig_fs(fragCoord, texCoord, normal, binormal, tangent, camDir, worldPos, wet, md_diffuse, md_normal, md_colour, farClip, cameraPos, glossMult, waterHeightRel,
               float4(skin.rgb, 0.0), float4(1.0, 1.0, 1.0, 0.0), buffer);
}
)";


inline std::string FragmentGlue(const std::string& skin_path) {
    return std::string("#define VPOS SV_Position\n#define ") + kDefine + "\n#define " + kEntry + " sk_orig_fs\n#include \"" + skin_path + "\"\n" + kFragWrap;
}

}  // namespace md::npcskin
