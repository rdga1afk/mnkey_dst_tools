#version 460 core
// Character editor scene geometry — flat shading, no lighting.
#ifdef SPIRV_COMPILE
layout(location=0) in  vec3 vColor;
layout(location=0) out vec4 fragColor;
void main() { fragColor = vec4(vColor, 1.0); }
#else
layout(location=0) out vec4 fragColor;
void main() { fragColor = vec4(0.4, 0.3, 0.2, 1.0); }
#endif
