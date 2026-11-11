#version 460 core
// #173: the Kenshi sky of the frame: a sample of the SkyX panorama (sky_pano.frag, redrawn every few frames) times the exposure. Same UBO as sky.frag, horizon_col.x = exposure.
layout(std140, set=3, binding=0) uniform SkyUBO {
    vec4 cam_right;
    vec4 cam_up;
    vec4 cam_fwd;
    vec4 sun_dir;
    vec4 horizon_col;
    float fov_tan;
    float aspect;
    float _pad0, _pad1;
} u;
layout(set=2, binding=0) uniform sampler2D pano;
layout(location=0) in  vec2 vNDC;
layout(location=0) out vec4 outColor;
const float PI = 3.14159265358979;
void main() {
    vec3 dir = normalize(u.cam_fwd.xyz + u.cam_right.xyz * (vNDC.x * u.fov_tan * u.aspect) + u.cam_up.xyz * (vNDC.y * u.fov_tan));
    float el = asin(clamp(dir.y, 0.0, 1.0));
    vec2 uv = vec2(atan(dir.z, dir.x) / (2.0 * PI) + 0.5, sqrt(el / (0.5 * PI)));
    outColor = vec4(texture(pano, uv).rgb * u.horizon_col.x, 1.0);
}
