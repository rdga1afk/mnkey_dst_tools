#version 460
// gl_BaseInstance + gl_InstanceIndex are core in GLSL 4.60 (no extension needed).
// gl_BaseInstance: base from indirect draw call
// gl_InstanceID:   0-based instance within this draw
#ifdef SPIRV_COMPILE
// Vulkan 460: gl_InstanceIndex = INSTANCE_IDX
#define INSTANCE_IDX gl_InstanceIndex
#else
#define INSTANCE_IDX (gl_BaseInstance + gl_InstanceID)
#endif


layout(location = 0) in vec3  aPos;
layout(location = 6) in vec4  aWeights;
layout(location = 7) in uvec4 aJoints;

#ifdef SPIRV_COMPILE
// SDL_GPU graphics vertex: set=0=storage bufs (contiguous), set=1=UBOs
layout(std430, set=0, binding=0) readonly buffer TransformBuf  { vec4 xzyr[]; };
layout(std430, set=0, binding=1) readonly buffer ShadowVisBuf  { uint shadow_visible[]; };
layout(std430, set=0, binding=2) readonly buffer FinalBonesBuf { mat4 final_bones[]; };
layout(std140, set=1, binding=0) uniform U { mat4 lightViewProj; } u;
#define LIGHT_VP u.lightViewProj
#else
layout(std430, binding = 0) readonly buffer TransformBuf  { vec4 xzyr[]; };
layout(std430, binding = 6) readonly buffer ShadowVisBuf  { uint shadow_visible[]; };
layout(std430, binding = 4) readonly buffer FinalBonesBuf { mat4 final_bones[]; };
uniform mat4 lightViewProj;
#define LIGHT_VP lightViewProj
#endif

void main() {
    uint instanceIdx = shadow_visible[INSTANCE_IDX];
    vec4 data = xzyr[instanceIdx];
    float x = data.x, z = data.y, y = data.z, rotY = data.w;

    float s = sin(rotY), c = cos(rotY);
    mat4 model = mat4(
         c,   0.0, -s,  0.0,
         0.0, 1.0,  0.0, 0.0,
         s,   0.0,  c,  0.0,
         x,   y + 0.9, z, 1.0
    );

    uint boneBase = (instanceIdx < 500u) ? instanceIdx * 64u : 0u;
#ifdef SPIRV_COMPILE
    // SDL_GPU rigid fallback: vertex buffer has no weights/joints; bone 0 applied.
    vec3 skinnedPos = (final_bones[boneBase] * vec4(aPos, 1.0)).xyz;
#else
    vec3 skinnedPos =
        aWeights.x * (final_bones[boneBase + aJoints.x] * vec4(aPos, 1.0)).xyz +
        aWeights.y * (final_bones[boneBase + aJoints.y] * vec4(aPos, 1.0)).xyz +
        aWeights.z * (final_bones[boneBase + aJoints.z] * vec4(aPos, 1.0)).xyz +
        aWeights.w * (final_bones[boneBase + aJoints.w] * vec4(aPos, 1.0)).xyz;
#endif

    gl_Position = LIGHT_VP * model * vec4(skinnedPos, 1.0);
}
