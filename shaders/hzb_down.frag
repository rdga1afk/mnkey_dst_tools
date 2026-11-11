#version 460 core
// #119 occlusion: one texel = the farthest linear eye depth (metres, sky = 1e6) over a cell x cell block of the scene depth (D32F, standard 0..1). Read back to the CPU for the culling of placed meshes (Dagor-style HZB of the previous frame).
layout(set=2, binding=0) uniform sampler2D depthTex;
layout(std140, set=3, binding=0) uniform HzbParams { vec4 p; } u;   // x = cell size in px, y = near, z = far
layout(location = 0) out float outFar;
void main() {
    // the map covers the whole depth texture whatever its size (dynamic resolution: 1440 x 810 is not a multiple of 320 x 180): texel (u, v) takes the farthest depth of its UV rectangle
    const ivec2 sz = textureSize(depthTex, 0), mapSz = ivec2(320, 180), t = ivec2(gl_FragCoord.xy);
    const ivec2 lo = (t * sz) / mapSz, hi = ((t + 1) * sz + mapSz - 1) / mapSz;   // [lo, hi) in depth texels
    vec2 invSz = 1.0 / vec2(sz);
    float d = 0.0;
    for (int j = lo.y; j < hi.y; j += 2)
        for (int i = lo.x; i < hi.x; i += 2) {   // textureGather: the 2x2 block at (i, j) in one fetch, so the whole rectangle is covered (a max, never a sample)
            vec4 g = textureGather(depthTex, (vec2(i, j) + 1.0) * invSz);
            d = max(d, max(max(g.x, g.y), max(g.z, g.w)));
        }
    outFar = d >= 1.0 ? 1.0e6 : u.p.y * u.p.z / (u.p.z - d * (u.p.z - u.p.y));
}
