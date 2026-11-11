#version 460 core
// Hair vertex shader — transforms hair mesh by head bone (bone 21).
// set=0 binding=0 = uBoneMats
// set=1 binding=0 = VU (mvp only, 64 bytes — fits Intel HD 520 push-constant limit)
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;

#ifdef SPIRV_COMPILE
layout(set=0, binding=0) uniform sampler2D uBoneMats;
layout(std140, set=1, binding=0) uniform VU { mat4 mvp; } u;
layout(location=0) out vec3 vN;
layout(location=1) out vec3 vModelPos;   // bone-deformed model-space position

mat4 getBoneMat(int bi) {
    return mat4(
        texelFetch(uBoneMats, ivec2(bi*4+0, 0), 0),
        texelFetch(uBoneMats, ivec2(bi*4+1, 0), 0),
        texelFetch(uBoneMats, ivec2(bi*4+2, 0), 0),
        texelFetch(uBoneMats, ivec2(bi*4+3, 0), 0)
    );
}

void main() {
    mat4 M = getBoneMat(21);
    vec4 bone_pos = M * vec4(aPos, 1.0);
    gl_Position = u.mvp * bone_pos;
    // Push hair mesh 0.0003 NDC units toward camera to prevent Z-fighting with head geometry.
    gl_Position.z -= 0.0003 * gl_Position.w;
    vN = normalize(mat3(transpose(inverse(mat3(M)))) * aNorm);
    vModelPos = bone_pos.xyz;
}
#else
uniform mat4 uMVP;
out vec3 vN;
out vec3 vModelPos;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vN = aNorm;
    vModelPos = aPos;
}
#endif
