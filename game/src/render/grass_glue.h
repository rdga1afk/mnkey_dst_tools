#pragma once
// Glue of the grass route: the install program deferred/foliage.hlsl (grass_vs, grass_fs) with our wrapper entry points.
// Units: the program's distances are Ogre units (0.1 m); our vertices are metres, so the wrapper's uniforms are scaled (frequency x 10, direction / 10, fadeRange / 10, grassHeight / 10).
#include <string>

namespace md::grasspass {

inline std::string VertexGlue(const std::string& foliage_path) {
    return "#define VPOS SV_Position\n#define grass_vs gr_orig_vs\n#define grass_fs gr_unused_fs\n#include \"" + foliage_path + "\"\n" + R"(
uniform float4 md_vp[4]; uniform float md_time; uniform float md_frequency; uniform float3 md_direction; uniform float3 md_camPos; uniform float md_fadeRange; uniform float md_grassHeight;
struct VO { float4 pos : SV_Position; [[vk::location(0)]] float4 col : COLOR; [[vk::location(1)]] float2 uv : TEXCOORD0; [[vk::location(2)]] float4 wp : TEXCOORD1; };
VO main([[vk::location(0)]] float3 aPos : POSITION, [[vk::location(1)]] float4 aCol : COLOR, [[vk::location(2)]] float2 aUv : TEXCOORD0) {
    float4x4 wvp = float4x4(md_vp[0].x, md_vp[1].x, md_vp[2].x, md_vp[3].x, md_vp[0].y, md_vp[1].y, md_vp[2].y, md_vp[3].y,
                            md_vp[0].z, md_vp[1].z, md_vp[2].z, md_vp[3].z, md_vp[0].w, md_vp[1].w, md_vp[2].w, md_vp[3].w);
    float4x4 world = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    VO o; float4 p, c, w; float2 t;
    gr_orig_vs(float4(aPos, 1.0), aCol, aUv, p, c, t, w, md_time, md_frequency, md_direction, md_camPos, md_fadeRange, md_grassHeight, world, wvp);
    o.pos = p; o.col = c; o.uv = t; o.wp = w;
    return o;
}
)";
}

inline std::string FragmentGlue(const std::string& foliage_path) {
    return "#define VPOS SV_Position\n#define grass_fs gr_orig_fs\n#define grass_vs gr_unused_vs\n#include \"" + foliage_path + "\"\n" + R"(
sampler2D md_s0 : register(s0); sampler2D md_s1 : register(s1); sampler2D md_s2 : register(s2);
Texture2D<float4> md_gdepth : register(s3);
uniform float md_threshold; uniform float md_farClip; uniform float3 md_cameraPos; uniform float4 md_viewport; uniform float md_waterHeightRel; uniform float md_wetness; uniform float md_depthTol;
void main(float4 fragCoord : SV_Position, [[vk::location(0)]] float4 color : COLOR, [[vk::location(1)]] float2 texCoord : TEXCOORD0, [[vk::location(2)]] float4 worldPos : TEXCOORD1, out GBuffer buffer) {
    if (fragCoord.z > md_gdepth.Load(int3(int2(fragCoord.xy), 0)).r + md_depthTol) discard;
    gr_orig_fs(fragCoord, color, texCoord, worldPos, md_threshold, md_farClip, md_cameraPos, md_viewport, md_s0, md_s1, md_s2, md_waterHeightRel, md_wetness, buffer);
}
)";
}

}  // namespace md::grasspass
