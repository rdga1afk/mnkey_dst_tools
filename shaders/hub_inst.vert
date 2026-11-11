#version 460 core
// #119 spike: instanced props. Slot 0 = PropVertex (stride 36, as prop.vert), slot 1 = per instance { vec4 pos_scale (xyz world position, w uniform scale), vec4 quat (xyzw) }.
// Outputs match prop.vert so prop.frag is shared. set=1 binding=0: vp.
layout(location = 0) in vec3  aPos;
layout(location = 1) in vec3  aNormal;
layout(location = 2) in vec2  aUV;
layout(location = 3) in float aLayer;
layout(location = 4) in vec4  iPosScale;
layout(location = 5) in vec4  iQuat;

layout(std140, set=1, binding=0) uniform HubInstVert { mat4 vp; } u;

layout(location = 0) out vec3  vNormal;
layout(location = 1) out vec2  vUV;
layout(location = 2) out float vLayer;

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
    mat3 rot = quat_to_mat3(iQuat);
    vNormal = rot * aNormal;
    vUV = aUV;
    vLayer = aLayer;
    gl_Position = u.vp * vec4(rot * (aPos * iPosScale.w) + iPosScale.xyz, 1.0);
}
