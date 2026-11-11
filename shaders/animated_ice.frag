#version 460 core
// Frozen/ice NPC material variant (SF_Ice = 1u<<5).
// Reuses animated.vert skinning; only FS differs.
// Effect: icy blue tint + crystalline specular + vertex-normal frost overlay.
// VBfA MD_VBfA_IMPL.md §VBfA-R9: SF_Ice flag for frozen NPC effect.
//
// Bindings (same as animated.frag):
//   set=2 b=0  sampler2D u_body     (body/diffuse texture)
//   set=3 b=0  IceUBO (std140, 32B)

#ifdef SPIRV_COMPILE
layout(std140, set=3, binding=0) uniform IceUBO {
    vec4  sun_dir;        //  0: xyz = sun→surface direction
    vec4  sun_color;      // 16: xyz = RGB sunlight
    float ice_factor;     // 32: 0=no ice, 1=fully frozen
    float _pad[3];        // 36: padding to 48B
} u;

layout(set=2, binding=0) uniform sampler2D u_body;

layout(location=0) in  vec3 v_normal;
layout(location=1) in  vec2 v_uv;
layout(location=2) in  vec3 v_world_pos;
layout(location=0) out vec4 frag_color;

void main() {
    vec4 base = texture(u_body, v_uv);

    // Diffuse lighting
    float ndotl = max(dot(normalize(v_normal), -u.sun_dir.xyz), 0.0);
    vec3 diffuse = base.rgb * (u.sun_color.rgb * ndotl + vec3(0.12, 0.15, 0.25)); // cool ambient

    // Frost overlay: blend toward icy blue-white based on ice_factor + normal.y
    float frost = clamp((v_normal.y * 0.5 + 0.5) * u.ice_factor, 0.0, 1.0);
    vec3 frost_color = vec3(0.82, 0.92, 1.0);  // pale blue-white
    diffuse = mix(diffuse, frost_color, frost * 0.65);

    // Crystalline specular (Blinn-Phong, high shininess for ice facets)
    vec3 view_dir = normalize(vec3(0.0, 2.5, 3.0) - v_world_pos);
    vec3 half_dir = normalize(-u.sun_dir.xyz + view_dir);
    float spec = pow(max(dot(normalize(v_normal), half_dir), 0.0), 64.0);
    diffuse += vec3(0.9, 0.96, 1.0) * spec * u.ice_factor * 0.5;

    frag_color = vec4(diffuse, base.a);
}
#else
// OpenGL fallback (non-SPIR-V path) — simplified ice tint only
uniform sampler2D u_body;
uniform float ice_factor = 1.0;

in  vec3 v_normal;
in  vec2 v_uv;
out vec4 frag_color;

void main() {
    vec4 base = texture(u_body, v_uv);
    float frost = clamp((v_normal.y * 0.5 + 0.5) * ice_factor, 0.0, 1.0);
    vec3 frost_color = vec3(0.82, 0.92, 1.0);
    frag_color = vec4(mix(base.rgb, frost_color, frost * 0.6), base.a);
}
#endif
