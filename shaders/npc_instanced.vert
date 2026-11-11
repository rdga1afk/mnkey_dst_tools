#version 460
// gl_BaseInstance + gl_InstanceIndex are core in GLSL 4.60 (no extension needed).
// gl_BaseInstance: base from indirect draw call
// gl_InstanceID:   0-based instance within this draw
#ifdef SPIRV_COMPILE
// Vulkan 460: gl_InstanceIndex = INSTANCE_IDX
#define INSTANCE_IDX gl_InstanceIndex
#else
#define INSTANCE_IDX (gl_BaseInstance + gl_InstanceID)
#endif


layout(location = 0) in vec3 aPos;

#ifdef SPIRV_COMPILE
// SDL_GPU graphics vertex: set=0=storage bufs, set=1=UBOs
layout(std430, set=0, binding=0) readonly buffer TransformBuf { vec4 xzyr[]; };
layout(std430, set=0, binding=1) readonly buffer VisibleBuf   { uint visible_indices[]; };
layout(std430, set=0, binding=2) readonly buffer FactionBuf   { uint factions[]; };
layout(std140, set=1, binding=0) uniform U {
    mat4 viewProj;
    vec4 factionColors[4];
} u;
#define VIEWPROJ       u.viewProj
#define FACTION_COLORS u.factionColors
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vWorldPos;
#else
layout(std430, binding = 0) readonly buffer TransformBuf { vec4 xzyr[]; };
layout(std430, binding = 1) readonly buffer VisibleBuf   { uint visible_indices[]; };
layout(std430, binding = 3) readonly buffer FactionBuf   { uint factions[]; };
uniform mat4 viewProj;
uniform vec4 factionColors[4];
#define VIEWPROJ       viewProj
#define FACTION_COLORS factionColors
out vec4 vColor;
out vec3 vWorldPos;
#endif

void main() {
    uint instanceIdx = visible_indices[INSTANCE_IDX];
    vec4 data  = xzyr[instanceIdx];
    float x    = data.x, z = data.y, y = data.z, rotY = data.w;

    float s = sin(rotY), c = cos(rotY);
    mat4 model = mat4(
        c,   0.0, -s,  0.0,
        0.0, 1.0,  0.0, 0.0,
        s,   0.0,  c,  0.0,
        x,   y + 0.9, z, 1.0
    );
    gl_Position = VIEWPROJ * model * vec4(aPos, 1.0);
    vWorldPos   = (model * vec4(aPos, 1.0)).xyz;

    uint fid = min(factions[instanceIdx], 3u);
    vColor = FACTION_COLORS[fid];
}
