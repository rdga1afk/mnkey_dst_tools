#version 460 core
// Character preview hair / beard, GL-style fallback when the install programs (editor_char_body_live) are not available: the hair texture cut by the record's alpha channel, colour = brightness x hair colour with a simple light. The install light path is editor_char_body_live.cpp (rtt.hlsl).
layout(location=0) in  vec3 vN;
layout(location=1) in  vec2 vUV;
layout(location=2) in  vec3 vVN;
layout(location=3) in  vec3 vVP;   // kept: the vertex stage writes them
layout(set=2, binding=0) uniform sampler2D uHair;
layout(set=2, binding=1) uniform samplerCube uIrrCube;
layout(set=2, binding=2) uniform samplerCube uSpecCube;
layout(std140, set=3, binding=0) uniform FU {
    vec4 colour;
    vec4 diffuse_ch;
    vec4 alpha_ch;
    vec4 params;   // x = alpha threshold
    vec4 mode;     // x = 1: Kenshi's character-screen lighting, y = the record's specular (the hair program's specularMult)
} uf;
layout(location=0) out vec4 fragColor;

void main() {
    vec4 t = texture(uHair, vUV);
    if (dot(t, uf.alpha_ch) < uf.params.x) discard;
    float brightness = dot(t, uf.diffuse_ch);
    vec3 L = normalize(vec3(0.6, 1.2, 0.8));
    float light = clamp(dot(normalize(vN), L), 0.0, 1.0) * 0.65 + 0.35;
    vec3 c = vec3(brightness) * uf.colour.rgb * light;
    c = clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14), 0.0, 1.0);
    fragColor = vec4(sqrt(c * sqrt(c)), 1.0);
}
