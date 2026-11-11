#version 460 core
// Emissive material variant (VBfA-R9) — glowing NPC (fire, electricity, magic).
// Reuses animated.vert (same skeleton, bones, skinning); only FS differs.
// Simplified: Phong diffuse + configurable emissive glow.
// No CSM shadow sampling (emissive materials self-illuminate).
//
// Bindings:
//   set=2 b=0  sampler2D u_body     (body/diffuse texture)
//   set=3 b=0  EmissiveUBO (std140, 32B)

#ifdef SPIRV_COMPILE
layout(std140, set=3, binding=0) uniform EmissiveUBO {
    vec4  sun_dir;          //  0: xyz = sun→surface
    vec4  sun_color;        // 16: xyz = RGB
    vec3  emissive_color;   // 32: glow colour (e.g. orange for fire)
    float emissive_scale;   // 44: glow intensity multiplier
} u;
#else
uniform sampler2D u_body;
uniform vec4 sun_dir;
uniform vec4 sun_color;
uniform vec3 emissive_color;
uniform float emissive_scale;
#define u (struct{vec4 sun_dir=sun_dir;vec4 sun_color=sun_color;\
    vec3 emissive_color=emissive_color;float emissive_scale=emissive_scale;})
#endif

layout(location=0) in vec4  vColor;
layout(location=1) in vec3  vWorldPos;
layout(location=2) in vec3  vNormal;
layout(location=3) in vec2  vTexCoord;
#ifdef SPIRV_COMPILE
layout(location=4) in vec4  vBodySample;
#endif
layout(location=0) out vec4 fragColor;

void main() {
#ifdef SPIRV_COMPILE
    vec3  albedo = vBodySample.rgb;
#else
    vec3  albedo = texture(u_body, vTexCoord).rgb;
#endif
    vec3  N      = normalize(vNormal);
    vec3  toSun  = normalize(-u.sun_dir.xyz);
    float NdotL  = max(dot(N, toSun), 0.0);

    // Simplified Phong — no shadows (emissive materials self-illuminate)
    vec3 direct  = albedo * NdotL * u.sun_color.rgb;
    vec3 ambient = albedo * 0.25;
    vec3 lit     = direct + ambient;

    // VBfA-R9: emissive contribution — pulsing glow independent of shadow
    // emissive_color is the glow colour; emissive_scale drives intensity.
    vec3 emissive = u.emissive_color * u.emissive_scale;

    // Emissive overrides shadow/ambient: final = lit + glow
    vec3 color = lit + emissive;

    // Lightweight gamma (no tonemapping — emissive materials stay bright)
    color = sqrt(clamp(color, 0.0, 4.0));

    fragColor = vec4(color, 1.0);
}
