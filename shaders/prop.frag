#version 460 core
// Prop (rock + vegetation) fragment shader — shared by PropRenderer (Tier1)
// and ClutterRenderer (Tier2 baked ground clutter).
// Real Kenshi diffuse textures (PropTexShared): layer 0 = GenericRockTexture_DIF.dds
// (tiled, every rock/formation mesh), layer 1 = Trees&VegAtlas01.dds (shared
// foliage atlas — each vegetation mesh's baked UV already points at its own
// sub-rectangle, exactly as authored in the original game). Previously this
// shader ignored UV entirely and painted EVERY prop — rocks AND plants alike —
// with one hardcoded flat colour (ROCK_COLOR), which is why yucca/shrub/dead
// trees rendered as pale, illegible specks indistinguishable from rocks.
// set=2 binding=0/1: tex_rock / tex_veg (PropTexShared)
// set=2 binding=2: tex_custom — real per-mesh diffuse (PropMesh::has_custom_tex,
// task propmesh-materials 2026-08-02); ClutterRenderer binds a 1x1 white dummy
// here since its baked geometry has no per-mesh source, only PropRenderer ever
// has a real texture. vLayer selects: 0=rock, 1=veg, 2=custom.
// set=3 binding=0: fragment UBO (32 bytes, same layout as TerrainRenderer::SunParams):
//   vec4 sun_dir_str  — xyz=direction (normalised), w=strength
//   vec4 ambient      — xyz=ambient colour

#ifdef SPIRV_COMPILE
layout(std140, set=3, binding=0) uniform PropFrag {
    vec4 sun_dir_str;   // xyz=direction (normalised), w=strength
    vec4 ambient;       // xyz=ambient colour
} uf;
layout(set=2, binding=0) uniform sampler2DArray tex_rock;
layout(set=2, binding=1) uniform sampler2DArray tex_veg;
layout(set=2, binding=2) uniform sampler2D      tex_custom;
layout(location = 0) in  vec3  vNormal;
layout(location = 1) in  vec2  vUV;
layout(location = 2) in  float vLayer;
layout(location = 0) out vec4 fragColor;
#else
uniform vec4 sun_dir_str;
uniform vec4 ambient;
in  vec3 vNormal;
out vec4 fragColor;
#endif

void main() {
    vec3 N = normalize(vNormal);
#ifdef SPIRV_COMPILE
    vec4 rock   = texture(tex_rock,   vec3(vUV, 0.0));
    vec4 veg    = texture(tex_veg,    vec3(vUV, 0.0));
    vec4 custom = texture(tex_custom, vUV);
    // No alpha-cutout discard here (removed 2026-07-18): Trees&VegAtlas01.dds'
    // real alpha range is 19-114/255 (measured), nowhere near an opaque-vs-
    // transparent 0/255 cutout mask, so any fixed threshold either discarded
    // 100% of the atlas (the former 0.6 threshold did exactly this) or was an
    // arbitrary guess. Until the real meaning of this channel is identified,
    // render fully opaque instead of silently hiding clutter geometry.
    vec3 albedo = mix(rock.rgb, veg.rgb, step(0.5, vLayer));
    albedo = mix(albedo, custom.rgb, step(1.5, vLayer));
    if (vLayer > 2.5 && custom.a < 0.5) discard;   // layer 3: custom texture with alpha cut-out (#119 spike foliage)
    float ndl  = max(dot(N, uf.sun_dir_str.xyz), 0.0);
    // Clamp ambient so props never go below 0.30 brightness on the shadow side.
    vec3  amb   = max(uf.ambient.xyz, vec3(0.30));
    vec3  light = amb + ndl * uf.sun_dir_str.w;
#else
    vec3 albedo = vec3(0.58, 0.52, 0.44);  // legacy GL path — no texture wired
    float ndl  = max(dot(N, sun_dir_str.xyz), 0.0);
    vec3  light = ambient.xyz + ndl * sun_dir_str.w;
#endif
    fragColor = vec4(albedo * light, 1.0);
}
