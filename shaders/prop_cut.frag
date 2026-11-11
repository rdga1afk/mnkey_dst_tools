#version 460 core
// Town building batches with a TRANSPARENCY material (spec `material type` 1 / 2, D-009 stage 2c): a pixel is cut where the alpha of the NORMAL map is below the spec's
// alpha threshold / 255. Same inputs and lighting as prop.frag, plus tex_nrm (set=2 binding=3). The vertex layer carries 2 + threshold (custom texture; the threshold is the
// fraction), so one pipeline serves every threshold. Drawn in the scene pass with the other props (no stencil writes there). Spec: docs/spec/building_textures.md.
layout(std140, set=3, binding=0) uniform PropFrag {
    vec4 sun_dir_str;   // xyz=direction (normalised), w=strength
    vec4 ambient;       // xyz=ambient colour
} uf;
layout(set=2, binding=0) uniform sampler2DArray tex_rock;
layout(set=2, binding=1) uniform sampler2DArray tex_veg;
layout(set=2, binding=2) uniform sampler2D      tex_custom;
layout(set=2, binding=3) uniform sampler2D      tex_nrm;
layout(location = 0) in  vec3  vNormal;
layout(location = 1) in  vec2  vUV;
layout(location = 2) in  float vLayer;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 N = normalize(vNormal);
    const float thr = clamp(vLayer - 2.0, 0.0, 1.0);
    if (texture(tex_nrm, vUV).a < thr) discard;
    vec3 albedo = texture(tex_custom, vUV).rgb;
    // keep the rock / veg samplers referenced so the pipeline layout matches prop.frag's three bindings
    albedo += (texture(tex_rock, vec3(vUV, 0.0)).rgb + texture(tex_veg, vec3(vUV, 0.0)).rgb) * 0.0;
    float ndl  = max(dot(N, uf.sun_dir_str.xyz), 0.0);
    vec3  amb   = max(uf.ambient.xyz, vec3(0.30));
    vec3  light = amb + ndl * uf.sun_dir_str.w;
    fragColor = vec4(albedo * light, 1.0);
}
