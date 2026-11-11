#version 460 core
// #119 spike: alpha cut-out only (custom texture alpha < 0.5 -> discard), no colour output used (the pipeline masks colour writes). Same inputs and sampler layout as prop.frag.
layout(set=2, binding=0) uniform sampler2DArray tex_rock;
layout(set=2, binding=1) uniform sampler2DArray tex_veg;
layout(set=2, binding=2) uniform sampler2D      tex_custom;
layout(location = 0) in  vec3  vNormal;
layout(location = 1) in  vec2  vUV;
layout(location = 2) in  float vLayer;
layout(location = 0) out vec4 fragColor;
void main() {
    float a = texture(tex_custom, vUV).a;
    if (a < 0.5) discard;
    fragColor = vec4(texture(tex_rock, vec3(vUV, 0.0)).rgb * 0.0 + texture(tex_veg, vec3(vUV, 0.0)).rgb * 0.0 + vNormal * 0.0 + vec3(vLayer * 0.0), 1.0);
}
