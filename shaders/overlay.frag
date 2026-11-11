#version 460 core
// Overlay blit fragment shader — samples icon texture with alpha blend.

#ifdef SPIRV_COMPILE
layout(set=2, binding=0) uniform sampler2D uTex;
layout(location=0) in  vec2 vUV;
layout(location=0) out vec4 outColor;
#else
uniform sampler2D uTex;
in  vec2 vUV;
out vec4 outColor;
#endif

void main() {
    outColor = texture(uTex, vUV);
}
