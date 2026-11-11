#version 460 core
// MyGUI batch: texture * vertex colour (MyGUI white texture for untextured quads).

layout(set=2, binding=0) uniform sampler2D uTex;
layout(location=0) in vec4 vColor;
layout(location=1) in vec2 vUV;
layout(location=0) out vec4 outColor;

void main() {
    outColor = texture(uTex, vUV) * vColor;
}
