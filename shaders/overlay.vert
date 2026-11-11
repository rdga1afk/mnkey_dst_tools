#version 460 core
// Overlay blit: renders a textured quad at a given screen rect.
// No VBO — positions derived from gl_VertexIndex.
// set=1 binding=0: vec4 rect (x,y,w,h) in normalised screen coords [0..1].

#ifdef SPIRV_COMPILE
layout(std140, set=1, binding=0) uniform OverlayRect {
    float x, y, w, h;
} u;
layout(location=0) out vec2 vUV;
#else
uniform vec4 overlay_rect;
out vec2 vUV;
#endif

void main() {
    const vec2 corner[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
    );
    vec2 p = corner[gl_VertexIndex];
    vUV = p;

#ifdef SPIRV_COMPILE
    vec2 sc = vec2(u.x + p.x * u.w, u.y + p.y * u.h);
#else
    vec2 sc = vec2(overlay_rect.x + p.x * overlay_rect.z,
                   overlay_rect.y + p.y * overlay_rect.w);
#endif
    gl_Position = vec4(sc.x * 2.0 - 1.0, -(sc.y * 2.0 - 1.0), 0.0, 1.0);
}
