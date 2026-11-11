#version 460 core
// Character editor scene geometry — platform planks + anthropometer pole.
// Vertex: pos(xyz) + color(rgb), stride=24.  MVP via set=1 binding=0.
#ifdef SPIRV_COMPILE
layout(location=0) in  vec3 aPos;
layout(location=1) in  vec3 aColor;
layout(location=0) out vec3 vColor;
layout(std140, set=1, binding=0) uniform VU { mat4 mvp; };
void main() {
    gl_Position = mvp * vec4(aPos, 1.0);
    vColor = aColor;
}
#else
void main() { gl_Position = vec4(0.0); }
#endif
