#version 460 core
// КРОК 7 (docs/KROK7_SKYVIS_AO_DESIGN.md): apply pass for the toroidal
// heightfield-horizon-sample sky-visibility AO -- multiplicative darkening
// of the scene, same blend contract as ssao_apply.frag (src=(0,0,0),
// src_alpha=1-skyVis -> result = skyVis*scene).
//
// Reads gbufPacked (terrain_gbuffer_mini.frag's packed output, xyz=world
// position -- same decode terrain_shading_screenspace.frag already uses,
// gb.x/gb.z are plain-float world XZ, no bit unpack needed) + gbufDepth
// (terrain-presence mask, cleared to 1.0 = no geometry) to find each
// pixel's world XZ, looks it up in TerrainSkyVisAO's toroidally-wrapped
// probe grid (main_origin/spacing convention matches ToroidalUpdate's,
// see terrain_skyvis_ao.h).

layout(set=2, binding=0) uniform sampler2D probeTex;
layout(set=2, binding=1) uniform usampler2D gbufPacked; // R16G16B16A16_UINT (terrain_gbuf_pack.glsl); not read since #142 phase 1b: world XZ always from depth
layout(set=2, binding=2) uniform sampler2D gbufDepth;

layout(std140, set=3, binding=0) uniform SkyVisApplyUBO {
    // x=probe spacing (m), y=probe grid size (texels), z=mainOrigin.x
    // (probe-tex texel units), w=mainOrigin.y
    vec4 probeParams;
    // x=camWorldX, y=camWorldZ, z=fade radius (probe-tex texel units,
    // inner edge of the fade-to-neutral band), w=unused
    vec4 camParams;
    // S7 (terrain_s7): inverse view-projection; used when camParams.w > 0.5 (G-buffer colour is not written then)
    mat4 invVp;
} u;

layout(location=0) out vec4 fragColor;

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float d = texelFetch(gbufDepth, texel, 0).r;
    if (d >= 1.0) discard; // no terrain rasterized here -- leave scene untouched

    // #142 phase 1b: the packed G-buffer holds only the fraction of the world position, so the probe lookup always uses the depth reconstruction
    // (error of metres is far below the probe spacing). SDL_GPU framebuffer y is down, projection y is up; depth [0,1].
    vec2 fb  = gl_FragCoord.xy / vec2(textureSize(gbufDepth, 0));
    vec2 ndc = vec2(fb.x * 2.0 - 1.0, 1.0 - fb.y * 2.0);
    vec4 c   = u.invVp * vec4(ndc, d, 1.0);
    vec2 worldXZ = c.xz / c.w;

    float spacing   = u.probeParams.x;
    float gridSize  = u.probeParams.y;
    vec2  mainOrigin = u.probeParams.zw;

    // Same wrap arithmetic as CPU-side ToroidalWrap (toroidal_update.h):
    // physical texel = wrap(worldTexel - (mainOrigin - gridSize/2), gridSize).
    vec2 probeTexelF = worldXZ / spacing;
    vec2 shifted = probeTexelF - mainOrigin + vec2(gridSize * 0.5);
    vec2 uv = mod(shifted, vec2(gridSize)) / gridSize;

    float skyVis = textureLod(probeTex, uv, 0.0).r;

    // Fade to neutral (1.0 = no darkening) near the window edge -- avoids
    // a hard pop as a probe cell leaves the 256m window, and keeps the
    // toroidal wrap seam (always at the edge opposite the camera-relative
    // center) inside the faded-out band instead of visible on screen.
    vec2 camProbeTexel = u.camParams.xy / spacing;
    float offset = max(abs(probeTexelF.x - camProbeTexel.x), abs(probeTexelF.y - camProbeTexel.y));
    float fadeRadius = u.camParams.z;
    float edgeFactor = 1.0 - smoothstep(fadeRadius * 0.8, fadeRadius, offset);
    skyVis = mix(1.0, skyVis, clamp(edgeFactor, 0.0, 1.0));

    fragColor = vec4(0.0, 0.0, 0.0, 1.0 - skyVis);
}
