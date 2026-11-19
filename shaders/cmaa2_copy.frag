#version 450 core
// cmaa2_copy.frag -- texel-exact copy of the scene colour for the CMAA 2 pass (DE-358): one texelFetch at the pixel, no filtering, no sharpening (a filtered copy would change the input of the AA).
// SDL_GPU SPIR-V sets: fragment samplers = set 2.
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_scene;
void main() {
    out_color = vec4(texelFetch(u_scene, ivec2(gl_FragCoord.xy), 0).rgb, 1.0);
}
