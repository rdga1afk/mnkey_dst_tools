#version 460
// Depth-only early-Z prepass vertex shader for animated NPCs.
// Identical to animated.vert except for a small positive depth bias (pushes
// the written depth slightly FARTHER from the camera).
//
// Root cause this works around: the prepass ("dp") and main color draw ("d")
// share what looks like identical vertex-shader source and use
// depth_compare_op=LESS_OR_EQUAL specifically so the main draw's fragments
// should always at least tie the prepass's stored depth. In practice, on
// Intel Gen9 ANV, the two pipelines pair this vertex stage with DIFFERENT
// fragment shaders (shadow_csm.frag vs npc_instanced.frag), and the driver's
// codegen for the "same" vertex shader is not guaranteed bit-identical
// across differently-paired pipelines — confirmed empirically (2026-07-21):
// forcing npc_instanced.frag's fsMain to unconditionally return solid
// magenta still left patches of the earlier-drawn sky color visible in the
// exact same screen locations, proving those fragments never survive the
// depth test at all (color logic is irrelevant to them). A tiny guaranteed
// bias removes the ambiguity regardless of the exact driver-side mechanism —
// same technique already used for hair/head Z-fighting in this codebase.
#ifdef SPIRV_COMPILE
#define INSTANCE_IDX gl_InstanceIndex
#else
#define INSTANCE_IDX (gl_BaseInstance + gl_InstanceID)
#endif

layout(location = 0) in vec3  aPos;
layout(location = 2) in vec3  aNormal;
#ifndef SPIRV_COMPILE
layout(location = 6) in vec4  aWeights;
layout(location = 7) in uvec4 aJoints;
#endif

#ifdef SPIRV_COMPILE
layout(std430, set=0, binding=0) readonly buffer TransformBuf  { vec4 xzyr[]; };
layout(std430, set=0, binding=1) readonly buffer VisibleBuf    { uint visible_indices[]; };
layout(std430, set=0, binding=2) readonly buffer FactionBuf    { uint factions[]; };
layout(std430, set=0, binding=3) readonly buffer FinalBonesBuf { mat4 final_bones_mat4[]; };
layout(std140, set=1, binding=0) uniform U {
    mat4 viewProj;
    vec4 factionColors[4];
} u;
#define VIEWPROJ       u.viewProj
#define FACTION_COLORS u.factionColors
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vWorldPos;
layout(location = 2) out vec3 vNormal;
layout(location = 3) out vec2 vTexCoord;
layout(location = 4) out vec4 vBodySample;
layout(location = 8) in uvec4 aJoints;
layout(location = 9) in vec4  aWeights;
layout(location = 5) in vec2  aTexCoord;
layout(location =10) in vec4  aBodyColor;
#else
layout(std430, binding = 0) readonly buffer TransformBuf  { vec4 xzyr[]; };
layout(std430, binding = 1) readonly buffer VisibleBuf    { uint visible_indices[]; };
layout(std430, binding = 3) readonly buffer FactionBuf    { uint factions[]; };
layout(std430, binding = 4) readonly buffer FinalBonesBuf { mat4 final_bones_mat4[]; };
uniform mat4 viewProj;
uniform vec4 factionColors[4];
#define VIEWPROJ       viewProj
#define FACTION_COLORS factionColors
out vec4 vColor;
out vec3 vWorldPos;
out vec3 vNormal;
#endif

void main() {
    uint instanceIdx = visible_indices[INSTANCE_IDX];
    vec4 data = xzyr[instanceIdx];
    float x    = data.x, z = data.y, y = data.z, rotY = data.w;

    float s = sin(rotY), c = cos(rotY);
    mat4 model = mat4(
         c,   0.0, -s,  0.0,
         0.0, 1.0,  0.0, 0.0,
         s,   0.0,  c,  0.0,
         x,   y + 0.09, z, 1.0
    );

    uint boneBase = (instanceIdx < 500u) ? instanceIdx * 64u : 0u;
#ifdef SPIRV_COMPILE
    vec4 lp = vec4(aPos, 1.0);
    mat4 bm0 = final_bones_mat4[boneBase + aJoints.x];
    mat4 bm1 = final_bones_mat4[boneBase + aJoints.y];
    mat4 bm2 = final_bones_mat4[boneBase + aJoints.z];
    mat4 bm3 = final_bones_mat4[boneBase + aJoints.w];
    vec3 skinnedPos =
        aWeights.x * (bm0 * lp).xyz +
        aWeights.y * (bm1 * lp).xyz +
        aWeights.z * (bm2 * lp).xyz +
        aWeights.w * (bm3 * lp).xyz;
    vec3 skinnedNorm =
        aWeights.x * (mat3(bm0) * aNormal) +
        aWeights.y * (mat3(bm1) * aNormal) +
        aWeights.z * (mat3(bm2) * aNormal) +
        aWeights.w * (mat3(bm3) * aNormal);
#else
    mat4 bm0 = final_bones_mat4[boneBase + aJoints.x];
    mat4 bm1 = final_bones_mat4[boneBase + aJoints.y];
    mat4 bm2 = final_bones_mat4[boneBase + aJoints.z];
    mat4 bm3 = final_bones_mat4[boneBase + aJoints.w];
    vec3 skinnedPos =
        aWeights.x * (bm0 * vec4(aPos, 1.0)).xyz +
        aWeights.y * (bm1 * vec4(aPos, 1.0)).xyz +
        aWeights.z * (bm2 * vec4(aPos, 1.0)).xyz +
        aWeights.w * (bm3 * vec4(aPos, 1.0)).xyz;
    vec3 skinnedNorm = aNormal;
#endif

    vec4 worldPos4 = model * vec4(skinnedPos, 1.0);
    gl_Position = VIEWPROJ * worldPos4;
    // Prepass-only bias: guarantees the main color draw's unbiased depth for
    // the same vertex is always <= this value, so LESS_OR_EQUAL reliably
    // keeps the main pass's fragment instead of intermittently losing to
    // floating-point noise between the two pipelines' vertex-stage codegen.
    gl_Position.z += 0.2 * gl_Position.w;
    vWorldPos   = worldPos4.xyz;
    vNormal     = normalize(mat3(model) * skinnedNorm);
    vTexCoord   = aTexCoord;

    uint fid = min(factions[instanceIdx], 3u);
    vColor = FACTION_COLORS[fid];

#ifdef SPIRV_COMPILE
    vBodySample = aBodyColor;
#else
    vBodySample = vec4(1.0);
#endif
}
