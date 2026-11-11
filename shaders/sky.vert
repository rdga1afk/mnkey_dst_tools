#version 460 core
// Sky gradient — fullscreen triangle at max depth.
// set=1 binding=0: SkyUBO (96 bytes)

#ifdef SPIRV_COMPILE
layout(std140, set=1, binding=0) uniform SkyUBO {
    vec4 cam_right;    // xyz = camera right  vector (world space)
    vec4 cam_up;       // xyz = camera up     vector (world space)
    vec4 cam_fwd;      // xyz = camera forward vector (world space)
    vec4 sun_dir;      // xyz = sun direction (world space, normalised)
    vec4 horizon_col;  // xyz = horizon/haze colour (biome-specific)
    float fov_tan;
    float aspect;
    float _pad0, _pad1;
} u;
layout(location=0) out vec2 vNDC;
#else
uniform vec4 cam_right, cam_up, cam_fwd, sun_dir;
uniform float fov_tan, aspect;
out vec2 vNDC;
#endif

void main() {
    // Fullscreen triangle (3 vertices cover entire clip space)
    const vec2 pts[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    vec2 p = pts[gl_VertexIndex];
    vNDC = p;
    // z = 1-ε so depth test passes where nothing else was drawn
    gl_Position = vec4(p, 0.9999, 1.0);
}
