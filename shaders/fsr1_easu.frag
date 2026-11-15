#version 450 core
// fsr1_easu.frag -- #156: FSR 1 EASU (AMD FidelityFX FSR v1.0.2, MIT, shaders/fsr1/ffx_fsr1.h): edge-adaptive upsampling of the internal-resolution colour to the display size.
// SDL_GPU SPIR-V sets: fragment samplers = set 2, uniform buffers = set 3. u_gamma > 0.5: the source texture is an sRGB format (the sampler returns linear): EASU wants perceptual values, so the taps are
// square-rooted and the result squared (gamma 2.0, as AMD's own sample does for linear input).
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 2, binding = 0) uniform sampler2D u_src;
layout(std140, set = 3, binding = 0) uniform EasuUBO { uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; vec4 params; };   // params.x = gamma flag
AF4 EasuGamma(AF4 t) { return params.x > 0.5 ? sqrt(max(t, AF4(0.0))) : t; }
AF4 FsrEasuRF(AF2 p) { return EasuGamma(AF4(textureGather(u_src, p, 0))); }
AF4 FsrEasuGF(AF2 p) { return EasuGamma(AF4(textureGather(u_src, p, 1))); }
AF4 FsrEasuBF(AF2 p) { return EasuGamma(AF4(textureGather(u_src, p, 2))); }
#include "ffx_fsr1.h"
void main() {
    AF3 c;
    FsrEasuF(c, AU2(gl_FragCoord.xy), con0, con1, con2, con3);
    if (params.x > 0.5) c = c * c;
    out_color = vec4(c, 1.0);
}
