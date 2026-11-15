#version 450 core
// fsr1_rcas.frag -- #156: FSR 1 RCAS (robust contrast-adaptive sharpening, AMD FidelityFX FSR v1.0.2, MIT, shaders/fsr1/ffx_fsr1.h) of the EASU output, onto the swapchain.
// SDL_GPU SPIR-V sets: fragment samplers = set 2, uniform buffers = set 3. u_gamma as in fsr1_easu.frag.
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_src;
layout(std140, set = 3, binding = 0) uniform RcasUBO { uvec4 con; vec4 params; };   // params.x = gamma flag
AF4 FsrRcasLoadF(ASU2 p) { AF4 t = AF4(texelFetch(u_src, p, 0)); return params.x > 0.5 ? sqrt(max(t, AF4(0.0))) : t; }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"
void main() {
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), con);
    if (params.x > 0.5) c = c * c;
    out_color = vec4(c, 1.0);
}
