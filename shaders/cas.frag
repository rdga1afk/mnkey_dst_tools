#version 450 core
// cas.frag — Contrast Adaptive Sharpening (AMD FidelityFX CAS, simplified).
//
// Algorithm: 3×3 neighborhood, adaptive sharpening based on local contrast.
// Low-contrast regions get less sharpening; high-contrast edges preserved.
// Cost: 9 texture samples + ~20 ALU ops per pixel.
//
// Reference: AMD FidelityFX CAS (MIT), adapted to GLSL.

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

// set=0, binding=0: the scene texture (rendered frame before CAS).
layout(set = 0, binding = 0) uniform sampler2D u_scene;

// set=1, binding=0: per-pass constants.
layout(set = 1, binding = 0) uniform CasUBO {
    vec2  u_texel;      // 1.0 / vec2(width, height)
    float u_sharpness;  // [0..1], 0=no sharpening, 1=maximum
    float _pad;
};

void main() {
    vec2 ts = u_texel;

    // 3×3 neighborhood samples.
    vec3 a = texture(u_scene, v_uv + vec2(-1,-1)*ts).rgb;
    vec3 b = texture(u_scene, v_uv + vec2( 0,-1)*ts).rgb;
    vec3 c = texture(u_scene, v_uv + vec2( 1,-1)*ts).rgb;
    vec3 d = texture(u_scene, v_uv + vec2(-1, 0)*ts).rgb;
    vec3 e = texture(u_scene, v_uv).rgb;                   // center
    vec3 f = texture(u_scene, v_uv + vec2( 1, 0)*ts).rgb;
    vec3 g = texture(u_scene, v_uv + vec2(-1, 1)*ts).rgb;
    vec3 h = texture(u_scene, v_uv + vec2( 0, 1)*ts).rgb;
    vec3 i = texture(u_scene, v_uv + vec2( 1, 1)*ts).rgb;

    // Min/max over cross + corners (as in AMD CAS).
    vec3 mn  = min(min(min(d,e),min(f,b)),h);
    vec3 mn2 = min(mn, min(min(a,c),min(g,i)));
    mn += mn2;

    vec3 mx  = max(max(max(d,e),max(f,b)),h);
    vec3 mx2 = max(mx, max(max(a,c),max(g,i)));
    mx += mx2;

    // Adaptive sharpening weight.
    vec3 rcpMX = vec3(1.0) / (mx + 1e-5);
    vec3 amp   = clamp(min(mn, 2.0 - mx) * rcpMX, 0.0, 1.0);
    amp = sqrt(amp);

    float peak   = -3.0 * u_sharpness + 8.0;   // [5..8] as sharpness [1..0]
    vec3  w      = -amp / peak;
    vec3  rcpW   = vec3(1.0) / (1.0 + 4.0 * w);

    // 5-tap sharpening (center + 4-cross weighted sum).
    vec3 result = clamp(((b + d + f + h) * w + e) * rcpW, 0.0, 1.0);
    out_color   = vec4(result, 1.0);
}
