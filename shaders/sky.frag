#version 460 core
// Sky gradient with sun disc.
// World-space ray reconstructed from NDC + camera basis vectors.

#ifdef SPIRV_COMPILE
layout(std140, set=3, binding=0) uniform SkyUBO {
    vec4 cam_right;
    vec4 cam_up;
    vec4 cam_fwd;
    vec4 sun_dir;
    vec4 horizon_col;  // biome-specific horizon haze
    float fov_tan;
    float aspect;
    float _pad0, _pad1;
} u;
layout(location=0) in  vec2 vNDC;
layout(location=0) out vec4 outColor;
#else
uniform vec4 cam_right, cam_up, cam_fwd, sun_dir;
uniform float fov_tan, aspect;
in  vec2 vNDC;
out vec4 outColor;
#endif

void main() {
#ifdef SPIRV_COMPILE
    vec3 right = u.cam_right.xyz;
    vec3 up    = u.cam_up.xyz;
    vec3 fwd   = u.cam_fwd.xyz;
    vec3 sdir  = u.sun_dir.xyz;
    float ft   = u.fov_tan;
    float asp  = u.aspect;
#else
    vec3 right = cam_right.xyz;
    vec3 up    = cam_up.xyz;
    vec3 fwd   = cam_fwd.xyz;
    vec3 sdir  = sun_dir.xyz;
    float ft   = fov_tan;
    float asp  = aspect;
#endif

    // Reconstruct world-space ray for this pixel
    vec3 ray = normalize(fwd + right * (vNDC.x * ft * asp)
                              + up    * (vNDC.y * ft));

    // RTS camera at ~49° elevation, FoV 45°: top-of-screen ray.y ≈ -0.45.
    // Shift so that the top of the screen maps to mid-sky blue, not earth.
    float h = ray.y;

    float h_shifted = h + 0.80;   // 0 = apparent horizon for this camera angle

    float t_up  = smoothstep(0.0, 0.55, h_shifted);
    float t_low = smoothstep(-0.15, 0.0, h_shifted);  // 0=below horizon→earth, 1=above→sky

    // Colour palette — horizon from biome, sky always blue
    vec3 zenith  = vec3(0.10, 0.22, 0.52);
    vec3 mid_sky = vec3(0.30, 0.52, 0.78);
#ifdef SPIRV_COMPILE
    vec3 horizon = u.horizon_col.rgb;
#else
    vec3 horizon = horizon_col.rgb;
#endif
    vec3 earth   = horizon * 0.35;

    vec3 sky_col = mix(horizon, mid_sky, t_up);
    sky_col      = mix(sky_col, zenith,  t_up * t_up);
    sky_col      = mix(earth,   sky_col, t_low);

    // Sun disc + corona
    float sun_dot = dot(ray, sdir);
    float sun_disc   = smoothstep(0.9990, 0.9996, sun_dot);
    float sun_corona = smoothstep(0.994,  0.9990, sun_dot) * 0.4;
    float sun_glow   = smoothstep(0.95,   0.994,  sun_dot) * 0.12;

    vec3 sun_color = vec3(1.0, 0.95, 0.75);
    sky_col += sun_color * (sun_disc + sun_corona + sun_glow);

    outColor = vec4(sky_col, 1.0);
}
