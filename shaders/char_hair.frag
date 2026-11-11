#version 460 core
// Character preview hair / beard -- fragment stage. Install hair material (deferred/character.material Hair, hair_fs): the pixel is cut where the record's alpha channel is below its threshold,
// the colour is brightness (the record's diffuse channel) x the hair HSB colour x 0.7, written like any character surface into the lighting; no culling. mode.x = 1 (Kenshi's character-screen
// lighting, the same path and the same 0.7 as the body and the head layer): colour -> the shared light + environment cubes; 0: the old preview light and tone curve.
// set=2: binding 0 = the hair texture, 1 / 2 = the environment cubes; set=3 binding=0 = FU
#extension GL_GOOGLE_include_directive : require
#include "kenshi_rtt_light.glsl"
#include "kenshi_char_hair.glsl"
layout(location=0) in  vec3 vN;
layout(location=1) in  vec2 vUV;
layout(location=2) in  vec3 vVN;
layout(location=3) in  vec3 vVP;
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
    if (uf.mode.x > 0.5) {
        vec3 N = normalize(vVN), V = normalize(-vVP);
        float gloss = brightness * uf.mode.y;
        vec4 irr = textureLod(uIrrCube, N, 3.0);
        vec4 refl = textureLod(uSpecCube, KenshiReflectionDir(N, V, gloss), KenshiReflectionMip(gloss));
        fragColor = vec4(clamp(KenshiRttLight(N, V, KenshiHairAlbedo(uf.colour.rgb, brightness), gloss, 0.0, 0.0, vec3(-3.0, 2.0, -3.0), vec3(0.6), 0.5, irr, refl), 0.0, 1.0), 1.0);
        return;
    }
    vec3 L = normalize(vec3(0.6, 1.2, 0.8));
    float light = clamp(dot(normalize(vN), L), 0.0, 1.0) * 0.65 + 0.35;
    vec3 c = vec3(brightness) * uf.colour.rgb * light;
    c = clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14), 0.0, 1.0);
    fragColor = vec4(sqrt(c * sqrt(c)), 1.0);
}
