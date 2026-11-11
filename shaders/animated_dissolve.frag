#version 460 core
// Dissolve material variant (VBfA-R9) — death/despawn effect.
// Reuses animated.vert; drives dissolve via dissolve_t [0..1].
// dissolve_t = 0: fully visible; dissolve_t = 1: fully gone.
// Edge ring: orange glow at the dissolve boundary.
//
// Bindings:
//   set=2 b=0  sampler2D u_body     (body/diffuse texture)
//   set=3 b=0  DissolveUBO (std140, 16B)

#ifdef SPIRV_COMPILE
layout(std140, set=3, binding=0) uniform DissolveUBO {
    float dissolve_t;   //  0: [0=solid, 1=gone]
    float edge_width;   //  4: edge ring width in noise-space (~0.05)
    float _pad[2];      //  8
} u;
#else
uniform sampler2D u_body;
uniform float dissolve_t;
uniform float edge_width;
#endif

layout(location=0) in vec4  vColor;
layout(location=1) in vec3  vWorldPos;
layout(location=2) in vec3  vNormal;
layout(location=3) in vec2  vTexCoord;
#ifdef SPIRV_COMPILE
layout(location=4) in vec4  vBodySample;
#endif
layout(location=0) out vec4 fragColor;

// Smooth value noise (cheap, no texture needed)
float hash21(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 17.5);
    return fract(p.x * p.y);
}
float noise(vec2 uv) {
    vec2  i = floor(uv);
    vec2  f = fract(uv);
    f = f * f * (3.0 - 2.0 * f);  // smoothstep
    return mix(mix(hash21(i),             hash21(i + vec2(1,0)), f.x),
               mix(hash21(i + vec2(0,1)), hash21(i + vec2(1,1)), f.x), f.y);
}

void main() {
#ifdef SPIRV_COMPILE
    float dt = u.dissolve_t;
    float ew = u.edge_width;
#else
    float dt = dissolve_t;
    float ew = edge_width;
#endif

    // Multi-octave noise for organic dissolve pattern
    vec2  uv  = vTexCoord * 3.0;
    float n   = noise(uv) * 0.6 + noise(uv * 2.0) * 0.3 + noise(uv * 4.0) * 0.1;

    // Discard fully dissolved pixels
    if (n < dt) discard;

    // Edge glow ring: [dt, dt+ew] → orange→base blend
    float edge = smoothstep(0.0, ew, n - dt);

#ifdef SPIRV_COMPILE
    vec3  albedo     = vBodySample.rgb;
#else
    vec3  albedo     = texture(u_body, vTexCoord).rgb;
#endif
    vec3  N          = normalize(vNormal);
    // Simple ambient-only lighting for dissolving character (no shadow needed)
    vec3  base_lit   = albedo * (max(N.y, 0.0) * 0.4 + 0.3);

    // VBfA-R9: orange edge ring dissolve (matches VBfA Dead_Body_Flexi.fx glow)
    vec3  edge_color = vec3(1.0, 0.35, 0.0);
    vec3  color      = mix(edge_color, base_lit, edge);

    // Alpha: opaque at base, slight edge fade (not transparent to avoid OIT cost)
    fragColor = vec4(color, 1.0);
}
