#version 460
// gl_BaseInstance + gl_InstanceIndex are core in GLSL 4.60 (no extension needed).
// SPIRV_COMPILE: Vulkan uses gl_BaseInstance (core) and gl_InstanceIndex.
// gl_BaseInstance: base from indirect draw call
// gl_InstanceID:   0-based instance within this draw
#ifdef SPIRV_COMPILE
// Vulkan 460: gl_InstanceIndex = INSTANCE_IDX
#define INSTANCE_IDX gl_InstanceIndex
#else
#define INSTANCE_IDX (gl_BaseInstance + gl_InstanceID)
#endif


// Raylib standard attribute locations:
layout(location = 0) in vec3  aPos;
layout(location = 2) in vec3  aNormal;
#ifndef SPIRV_COMPILE
// aWeights/aJoints not used in SDL_GPU path (bone-0 rigid fallback):
layout(location = 6) in vec4  aWeights;
layout(location = 7) in uvec4 aJoints;
#endif

// CPU LBS path: OzzAnimator::Sample writes skinning_mat = world * inv_bind as mat4 (64B/bone).
// skinning.comp (future GPU path) would write QsT; keep this mat4 format until GPU path enabled.

#ifdef SPIRV_COMPILE
// SDL_GPU vertex stage: set=0 = storage bufs, set=1 = UBOs
layout(std430, set=0, binding=0) readonly buffer TransformBuf  { vec4 xzyr[]; };
layout(std430, set=0, binding=1) readonly buffer VisibleBuf    { uint visible_indices[]; };
layout(std430, set=0, binding=2) readonly buffer FactionBuf    { uint factions[]; };
layout(std430, set=0, binding=3) readonly buffer FinalBonesBuf { mat4 final_bones_mat4[]; };
// TransformSoA::skin_ssbo_ (world/transform_soa.h/.cpp) — rgb=skin tint, a=strength.
layout(std430, set=0, binding=4) readonly buffer SkinBuf       { vec4 skin_rgba[]; };
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
layout(location = 5) out vec4 vSkinTint;   // rgb=skin tint, a=strength
// SkinVertex stride=52: pos(0) norm(12) uv(24) joints(32) weights(36)
// Locations 8/9 avoid conflict with vertex OUTPUTs at 0-4 (Intel ANV fix).
layout(location = 8) in uvec4 aJoints;
layout(location = 9) in vec4  aWeights;
layout(location = 5) in vec2  aTexCoord;
// Pre-baked body texture color (CPU-sampled at load time, uploaded as vertex attr).
// Avoids all descriptor-based resources (SDL3 3.4.8 Intel ANV bug).
layout(location =10) in vec4  aBodyColor;
#else
layout(std430, binding = 0) readonly buffer TransformBuf  { vec4 xzyr[]; };
layout(std430, binding = 1) readonly buffer VisibleBuf    { uint visible_indices[]; };
layout(std430, binding = 3) readonly buffer FactionBuf    { uint factions[]; };
layout(std430, binding = 4) readonly buffer FinalBonesBuf { mat4 final_bones_mat4[]; };
uniform mat4 viewProj;
uniform vec4 factionColors[4];
uniform float fogNear;
uniform float fogFar;
uniform vec3  cameraPos;
uniform vec3  fogColor;
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

    uint boneBase = (instanceIdx < 500u) ? instanceIdx * 64u : 0u;  // stride=MAX_BONES=64
#ifdef SPIRV_COMPILE
    vec4 lp = vec4(aPos, 1.0);
    mat4 bm0 = final_bones_mat4[boneBase + aJoints.x];
    mat4 bm1 = final_bones_mat4[boneBase + aJoints.y];
    mat4 bm2 = final_bones_mat4[boneBase + aJoints.z];
    mat4 bm3 = final_bones_mat4[boneBase + aJoints.w];
    // Linear blend skinning — position
    vec3 skinnedPos =
        aWeights.x * (bm0 * lp).xyz +
        aWeights.y * (bm1 * lp).xyz +
        aWeights.z * (bm2 * lp).xyz +
        aWeights.w * (bm3 * lp).xyz;
    // Linear blend skinning — normal (upper-left 3×3)
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
    vWorldPos   = worldPos4.xyz;
    vNormal     = normalize(mat3(model) * skinnedNorm);
    vTexCoord   = aTexCoord;

    uint fid = min(factions[instanceIdx], 3u);
    vColor = FACTION_COLORS[fid];

#ifdef SPIRV_COMPILE
    vBodySample = aBodyColor;
    vSkinTint   = skin_rgba[instanceIdx];
#else
    vBodySample = vec4(1.0);
    vSkinTint   = vec4(0.82, 0.65, 0.52, 0.0);
#endif
}
