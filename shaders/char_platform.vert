#version 460 core
// Character editor platform (character_editor_platform.mesh): static, textured. Vertex: pos(12) + normal(12) + uv(8) + colour(16), stride 48. set=1 binding=0 = mvp.
#ifdef SPIRV_COMPILE
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aCol;
layout(location=0) out vec3 vN;
layout(location=1) out vec2 vUV;
layout(location=2) out vec4 vCol;
layout(std140, set=1, binding=0) uniform VU { mat4 mvp; };
void main() {
    gl_Position = mvp * vec4(aPos, 1.0);
    vN = aNorm;
    vUV = aUV;
    vCol = aCol;
}
#else
void main() { gl_Position = vec4(0.0); }
#endif
