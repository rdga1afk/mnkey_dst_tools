#version 460 core
// Background gradient for character preview — fullscreen triangle, no VBO.
// Used as first draw call in CharPreviewSDLGPU::RenderFrame; depth_test=false.
#ifdef SPIRV_COMPILE
layout(location=0) out vec2 vNDC;
void main() {
    // Fullscreen triangle covering [-1,1]×[-1,1]:
    //   vtx 0: (-1, -3)   vtx 1: (-1, +1)   vtx 2: (+3, +1)
    vec2 p = vec2(gl_VertexIndex == 2 ? 3.0 : -1.0,
                  gl_VertexIndex == 0 ? -3.0 : 1.0);
    vNDC = p;
    gl_Position = vec4(p, 0.9999, 1.0);  // far-plane z — character draws in front
}
#else
out vec2 vNDC;
void main() { vNDC = vec2(0.0); gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
#endif
