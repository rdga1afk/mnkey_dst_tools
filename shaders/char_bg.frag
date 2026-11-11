#version 460 core
// Character preview background — sky + textured desert ground via ray-ground intersection.
// set=3 binding=0 = BgUU (64 bytes)
// set=2 binding=0 = sand texture (desert_sand)
// set=2 binding=1 = sand detail texture (desert_dune)
#ifdef SPIRV_COMPILE
layout(location=0) in  vec2 vNDC;
layout(location=0) out vec4 fragColor;

layout(std140, set=3, binding=0) uniform BgUU {
    vec4 right;    // xyz=cam_right, w=tan_hfov
    vec4 up;       // xyz=cam_up,    w=tan_vfov
    vec4 fwd;      // xyz=cam_fwd (into scene), w=ground_y
    vec4 eye;      // xyz=cam_world_pos, w=unused
} u;

layout(set=2, binding=0) uniform sampler2D uSand;
layout(set=2, binding=1) uniform sampler2D uDune;
layout(set=2, binding=2) uniform sampler2D uSandy;   // gui/gfx/SandyGround.dds, the original CharacterEditorBackground

void main() {
    if (u.eye.w > 0.5) {   // charedit.compositor: a full-viewport quad textured with SandyGround.dds (Basic_Texture_VP / FP: uv = the quad's uv, colour 1)
        fragColor = vec4(texture(uSandy, vec2(vNDC.x * 0.5 + 0.5, 0.5 - vNDC.y * 0.5)).rgb, 1.0);
        return;
    }
    vec3 dir = normalize(u.fwd.xyz
                       + vNDC.x * u.right.w * u.right.xyz
                       + vNDC.y * u.up.w    * u.up.xyz);
    vec3 ori = u.eye.xyz;

    float sky_t = clamp(vNDC.y * 0.5 + 0.5, 0.0, 1.0);

    // Sky colours — Kenshi overcast (flat warm grey, dust haze)
    vec3 sky_top = vec3(0.48, 0.45, 0.40);
    vec3 horizon = vec3(0.70, 0.62, 0.50);
    vec3 col;

    float t_ground = (u.fwd.w - ori.y) / (dir.y - 0.0001);
    if (t_ground > 0.05 && dir.y < -0.001) {
        vec3 hit = ori + dir * t_ground;

        // Two-scale texture tiling
        vec2 uv_near = hit.xz * 0.55;    // close detail
        vec2 uv_far  = hit.xz * 0.12;    // large dune pattern

        vec3 sand  = texture(uSand, uv_near).rgb;
        vec3 dune  = texture(uDune, uv_far ).rgb;

        // Blend: close = sharp sand, far = dune pattern fades in
        float fog   = exp(-t_ground * 0.10);
        float blend = clamp(1.0 - fog * 1.4, 0.0, 1.0);
        vec3 ground = mix(sand, dune, blend * 0.55);

        // Slight warm tint to unify colour temperature
        ground *= vec3(1.05, 0.98, 0.85);

        col = mix(horizon, ground, fog);
    } else {
        // Sky — horizon → zenith gradient
        float st = clamp((sky_t - 0.30) / 0.70, 0.0, 1.0);
        col = mix(horizon, sky_top, st * st);
        // Warm glow near horizon
        float glow = clamp(1.0 - sky_t * 2.2, 0.0, 1.0);
        col = mix(col, horizon * 1.10, glow * 0.45);
        // Heat haze band right at horizon
        float haze = clamp(1.0 - abs(sky_t - 0.36) * 14.0, 0.0, 1.0);
        col = mix(col, vec3(0.92, 0.80, 0.58), haze * 0.25);
    }
    fragColor = vec4(col, 1.0);
}
#else
layout(location=0) out vec4 fragColor;
void main() { fragColor = vec4(0.45, 0.50, 0.60, 1.0); }
#endif
