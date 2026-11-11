#version 460 core
// Prop vertex shader — static props + simple procedural animation + terrain tilt (G-2)
// + real per-instance quaternion rotation (FeatureScatter, real Kenshi placement).
// Vertex layout (stride=36): pos(12) + normal(12) + uv(8) + layer(4)
// set=1 binding=0: PropVert UBO (128 bytes)
//   mat4  vp               — 64 B
//   vec4  model_pos_scale  — 16 B  (xyz=world pos, w=scale)
//   vec4  anim_params      — 16 B  (x=time, y=mode, z=mesh_height, w=phase)
//   vec4  model_normal     — 16 B  (xyz=terrain surface normal; (0,1,0) = no tilt)
//   vec4  model_quat       — 16 B  (xyzw quaternion; w==0 = unused, use tilt instead —
//                                   real FeatureScatter placements always have w!=0)
//     mode 0 = static
//     mode 1 = wind sway (vegetation)
//     mode 2 = character bob

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec3  aNormal;
layout(location = 2) in vec2  aUV;
layout(location = 3) in float aLayer;

#ifdef SPIRV_COMPILE
layout(std140, set=1, binding=0) uniform PropVert {
    mat4 vp;
    vec4 model_pos_scale;
    vec4 anim_params;   // x=time, y=mode, z=mesh_height, w=phase
    vec4 model_normal;  // xyz=terrain surface normal; (0,1,0) = no tilt
    vec4 model_quat;    // xyzw quaternion; w==0 = unused, fall back to tilt-only
} u;
layout(location = 0) out vec3  vNormal;
layout(location = 1) out vec2  vUV;
layout(location = 2) out float vLayer;
layout(location = 3) out vec3  vWorld;   // triplanar features (layer >= 4)
#else
uniform mat4 vp;
uniform vec4 model_pos_scale;
uniform vec4 anim_params;
uniform vec4 model_normal;
uniform vec4 model_quat;
out vec3 vNormal;
#endif

// Standard quaternion (x,y,z,w) -> 3x3 rotation matrix.
mat3 quat_to_mat3(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;
    return mat3(
        1.0 - (yy + zz), xy + wz,         xz - wy,
        xy - wz,         1.0 - (xx + zz), yz + wx,
        xz + wy,         yz - wx,         1.0 - (xx + yy));
}

void main() {
#ifdef SPIRV_COMPILE
    float sc   = u.model_pos_scale.w;
    vec3 wpos  = u.model_pos_scale.xyz;
    float time = u.anim_params.x;
    float mode = u.anim_params.y;
    float mh   = u.anim_params.z;   // mesh height in model space
    float ph   = u.anim_params.w;   // per-instance phase
    vec3 pos   = aPos * sc;
#else
    float sc   = model_pos_scale.w;
    vec3 wpos  = model_pos_scale.xyz;
    float time = anim_params.x;
    float mode = anim_params.y;
    float mh   = anim_params.z;
    float ph   = anim_params.w;
    vec3 pos   = aPos * sc;
#endif

    if (mode > 0.5 && mode < 1.5) {
        // Wind sway: top of plant sways more than base
        float t     = max(0.0, pos.y / max(mh * sc, 0.001));
        float sway  = sin(time * 1.8 + ph) * 0.04 * t * t;
        float rustle= sin(time * 3.7 + ph * 1.6) * 0.015 * t;
        pos.x += sway;
        pos.z += rustle;
    } else if (mode > 1.5) {
        // Character bob: whole mesh oscillates up/down at walk frequency
        float bob = sin(time * 5.0 + ph) * 0.06;
        pos.y    += bob;
    }

#ifdef SPIRV_COMPILE
    mat3 rot;
    if (abs(u.model_quat.w) > 0.0001) {
        // Real per-instance placement rotation (FeatureScatter) — replaces
        // the tilt-only mechanism below, which can't express full 3-DOF
        // rotation from a single surface normal.
        rot = quat_to_mat3(u.model_quat);
    } else {
        // G-2: terrain-normal tilt via TBN rotation.
        // N = terrain normal; T = tangent (Gram-Schmidt from X); B = cross(T,N).
        // mat3(T,N,B): model X→T, model Y→N, model Z→B.
        // When model_normal=(0,1,0): T=(1,0,0), B=(0,0,1) → identity.
        vec3 N  = normalize(u.model_normal.xyz);
        vec3 Tx = (abs(N.x) < 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0);
        vec3 T  = normalize(Tx - N * dot(Tx, N));
        vec3 B  = cross(T, N);
        rot = mat3(T, N, B);
    }

    vNormal     = rot * aNormal;
    vUV         = aUV;
    vLayer      = aLayer;
    vWorld      = rot * pos + wpos;
    gl_Position = u.vp * vec4(vWorld, 1.0);
#else
    vec3 N  = normalize(model_normal.xyz);
    vec3 Tx = (abs(N.x) < 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 T  = normalize(Tx - N * dot(Tx, N));
    vec3 B  = cross(T, N);
    mat3 rot = mat3(T, N, B);

    vNormal     = rot * aNormal;
    gl_Position = vp * vec4(rot * pos + wpos, 1.0);
#endif
}
