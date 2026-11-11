#version 430 core
layout(location = 0) in vec3  aPos;
layout(location = 1) in float aSize;
layout(location = 2) in vec4  aColor;

// SPIRV_COMPILE: group uniforms into UBO (Vulkan requires it).
// OpenGL path keeps individual uniforms unchanged.
#ifdef SPIRV_COMPILE
layout(std140, set=1, binding=0) uniform U {
    mat4 viewProj;
    vec3 cameraPos; float _pad;
} u;
#define VIEWPROJ u.viewProj
#define CAM_POS  u.cameraPos
layout(location = 0) out vec4 vColor;
#else
uniform mat4 viewProj;
uniform vec3 cameraPos;
#define VIEWPROJ viewProj
#define CAM_POS  cameraPos
out vec4 vColor;
#endif

void main() {
    gl_Position = VIEWPROJ * vec4(aPos, 1.0);
    float dist = length(aPos - CAM_POS);
    gl_PointSize = clamp(aSize * 300.0 / max(dist, 1.0), 1.0, 64.0);
    vColor = aColor;
}
