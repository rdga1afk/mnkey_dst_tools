#version 460
// Player hair vertex shader — rigid single-bone attach (head, bone idx=21)
// to the shared NPC FinalBonesBuf SSBO. PropVertex layout: pos(0,F3) norm(12,F3)
// uv(24,F2) layer(32,F1) stride=36 — only pos/norm declared here, uv/layer unused.
// set=0 binding=0 = FinalBonesBuf (all NPC bone matrices, stride=64 per slot)
// set=1 binding=0 = CU (viewProj + playerSlot)
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;

#ifdef SPIRV_COMPILE
layout(std430, set=0, binding=0) readonly buffer FinalBonesBuf { mat4 final_bones[]; };
layout(std140, set=1, binding=0) uniform CU {
    mat4  viewProj;
    uint  playerSlot;
    float _pad[3];
} u;
layout(location=0) out vec3 vN;

const uint HAIR_BONE = 21u; // head — matches editor's char_hair.vert

// FinalBonesBuf stores SKINNING matrices (world * inv_bind — see
// ozz_animator.h's Sample() doc comment), meant to transform vertices
// authored in bind-pose-local space. The hair GLB is a rigid prop authored
// directly in bone-21's rest-local space (like the editor's uBoneMats,
// which are world-only — no inv_bind baked in), so using final_bones[]
// directly here collapses the mesh to a degenerate transform (invisible,
// confirmed via magenta-fragcolor draw-call trace — task #233). Right-
// multiplying by bone 21's bind pose (inverse of its inv_bind, extracted
// once via a live debug dump and hardcoded — constant per skeleton, all
// humans share md_human.glb) cancels the inv_bind term and recovers the
// bone's current WORLD matrix: world = skinning_mat * bind_pose.
const mat4 kBindPose21 = mat4(
    -0.000001,  1.000000,  0.000002,  0.000000,
     1.000000,  0.000001, -0.000005,  0.000000,
    -0.000005,  0.000002, -1.000000,  0.000000,
    -0.004463,  1.741914,  0.013705,  1.000000
);

void main() {
    mat4 boneMat = final_bones[u.playerSlot * 64u + HAIR_BONE] * kBindPose21;
    vec3 sp = (boneMat * vec4(aPos, 1.0)).xyz;
    vec3 sn = normalize(mat3(boneMat) * aNorm);
    gl_Position = u.viewProj * vec4(sp, 1.0);
    // Small forward Z-bias to avoid z-fighting against head geometry
    // (matches editor's char_hair.vert reference).
    gl_Position.z -= 0.0003 * gl_Position.w;
    vN = sn;
}
#else
void main() { gl_Position = vec4(0.0); }
#endif
