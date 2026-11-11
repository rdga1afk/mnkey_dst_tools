#version 460 core
// Character preview — vertex stage (proper LBS matrix skinning).
// Vertex layout: pos(12) + norm(12) + uv(8) + joints_u8x4(4) + weights_f4(16) = stride 52.
// set=1 binding=0 = VU (64 bytes): mvp only
// set=1 binding=1 = BoneMats: 30 mat4 — Ws[i] = world-space scale-around-pivot matrix
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNorm;
layout(location=2) in vec2  aUV;
layout(location=3) in uvec4 aJoints;
layout(location=4) in vec4  aWeights;
layout(location=5) in vec3  aTan;
layout(location=6) in vec3  aBin;

#ifdef SPIRV_COMPILE
layout(std140, set=1, binding=0) uniform VU {
    mat4 mvp;
} u;
layout(std140, set=1, binding=1) uniform BoneMats {
    mat4 Ws[30];
} bones;

layout(std140, set=1, binding=2) uniform VU2 {
    mat4 mv;   // view * the body's model matrix: the character screen lights in view space (#192)
} u2;

layout(location=0) out vec3 vN;
layout(location=1) out vec2 vUV;
layout(location=2) out vec3 vVN;   // view-space normal
layout(location=3) out vec3 vVP;   // view-space position
layout(location=4) out vec3 vVT;   // view-space tangent / binormal of the mesh (Character_FP builds its tbn from them)
layout(location=5) out vec3 vVB;

mat4 getBoneMat(int bi) { return bones.Ws[bi]; }

void main() {
    // Standard LBS: blend 4 bone matrices by skin weights.
    // Our Ws matrices are pure world-space scale+pivot — no rotation component.
    float tw = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
    mat4 M;
    if (tw > 0.001) {
        float iw = 1.0 / tw;
        M = (aWeights.x * getBoneMat(int(aJoints.x))
           + aWeights.y * getBoneMat(int(aJoints.y))
           + aWeights.z * getBoneMat(int(aJoints.z))
           + aWeights.w * getBoneMat(int(aJoints.w))) * iw;
    } else {
        M = mat4(1.0);  // identity for unskinned vertices
    }

    gl_Position = u.mvp * (M * vec4(aPos, 1.0));

    // Correct skinning normal transform: inverse-transpose of blended M.
    // M contains full rotation+scale (idle pose + bone scales) — diagonal
    // extraction is wrong when rotation is present, causing dark shading artifacts.
    vN  = normalize(transpose(inverse(mat3(M))) * aNorm);
    vUV = aUV;
    vVN = normalize(mat3(u2.mv) * vN);
    vVT = mat3(u2.mv) * normalize(mat3(M) * aTan);
    vVB = mat3(u2.mv) * normalize(mat3(M) * aBin);
    vVP = (u2.mv * (M * vec4(aPos, 1.0))).xyz;
}
#else
uniform mat4 uMVP;
out vec3 vN;
out vec2 vUV;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); vN = aNorm; vUV = aUV; }
#endif
