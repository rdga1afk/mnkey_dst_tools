#version 460 core
// #176: map features with texture mode TERRAIN (Feature_Terrain_DX11 in the game: shaded with the biome's ground layers, no textures of their own) are drawn
// into the terrain G-buffer, so the screen-space ground resolve shades them like ground. Same varyings as terrain_quadtree.vert feeds terrain_gbuffer_mini.frag.
// Vertex layout as prop.vert (stride 36: pos, normal, uv, layer); set=1 binding=0: vp (64 B), model_pos_scale (xyz world position, w scale), model_quat (xyzw), model_slot (x: the biome-list slot at the feature's own position, FEATURE_SLOT variant).
layout(location = 0) in vec3  aPos;
layout(location = 1) in vec3  aNormal;
layout(location = 2) in vec2  aUV;
layout(location = 3) in float aLayer;

layout(std140, set=1, binding=0) uniform FeatureVert {
    mat4 vp;
    vec4 model_pos_scale;
    vec4 model_quat;
    vec4 model_slot;
} u;

layout(location = 0) out vec3  vNormal;
layout(location = 1) out vec2  vWorldPos;
layout(location = 2) out float vWorldY;
layout(location = 3) out float vDist;
layout(location = 4) out float vIsSkirt;
#ifdef FEATURE_SLOT
layout(location = 5) flat out uint vSlot;   // terrain_gbuffer_mini.frag TS_SLOT: the feature writes its own slot, also where no ground lies behind it
#endif

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
    mat3 rot = quat_to_mat3(u.model_quat);
    vec3 wp  = rot * (aPos * u.model_pos_scale.w) + u.model_pos_scale.xyz;
    vNormal   = rot * aNormal;
    vWorldPos = wp.xz;
    vWorldY   = wp.y;
    vDist     = 0.0;
    vIsSkirt  = 0.0;
#ifdef FEATURE_SLOT
    vSlot     = uint(u.model_slot.x + 0.5);
#endif
    gl_Position = u.vp * vec4(wp, 1.0);
}
