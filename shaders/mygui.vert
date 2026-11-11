#version 460 core
// MyGUI batch: MyGUI builds the vertices already in clip space (x, y in [-1, 1], top = +1, like its OpenGL platform); colour RGBA8, uv.

layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aColor;
layout(location=2) in vec2 aUV;
layout(location=0) out vec4 vColor;
layout(location=1) out vec2 vUV;

void main() {
    vColor = aColor;
    vUV = aUV;
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
}
