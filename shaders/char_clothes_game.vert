#version 460
// Player clothing vertex shader — reads from the shared NPC FinalBonesBuf SSBO.
// SkinVertex layout: pos(0,F3) norm(12,F3) uv(24,F2) joints(32,U8x4) weights(36,F4) stride=52
// set=0 binding=0 = FinalBonesBuf (all NPC bone matrices, stride=64 per slot)
// set=1 binding=0 = CU (viewProj + playerSlot)
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNorm;
layout(location=2) in vec2  aUV;      // unused in frag but keeps stride intact
layout(location=3) in uvec4 aJoints;
layout(location=4) in vec4  aWeights;

#ifdef SPIRV_COMPILE
layout(std430, set=0, binding=0) readonly buffer FinalBonesBuf { mat4 final_bones[]; };
layout(std140, set=1, binding=0) uniform CU {
    mat4  viewProj;
    uint  playerSlot;
    float _pad[3];
} u;
layout(location=0) out vec3 vN;

void main() {
    uint boneBase = u.playerSlot * 64u;
    vec4 lp = vec4(aPos, 1.0);
    vec3 sp =
        aWeights.x * (final_bones[boneBase + aJoints.x] * lp).xyz +
        aWeights.y * (final_bones[boneBase + aJoints.y] * lp).xyz +
        aWeights.z * (final_bones[boneBase + aJoints.z] * lp).xyz +
        aWeights.w * (final_bones[boneBase + aJoints.w] * lp).xyz;
    vec3 sn =
        aWeights.x * (mat3(final_bones[boneBase + aJoints.x]) * aNorm) +
        aWeights.y * (mat3(final_bones[boneBase + aJoints.y]) * aNorm) +
        aWeights.z * (mat3(final_bones[boneBase + aJoints.z]) * aNorm) +
        aWeights.w * (mat3(final_bones[boneBase + aJoints.w]) * aNorm);
    gl_Position = u.viewProj * vec4(sp, 1.0);
    vN = normalize(sn);
}
#else
void main() { gl_Position = vec4(0.0); }
#endif
