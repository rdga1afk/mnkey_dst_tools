#version 460 core
// #154 phase 3: terrain patches with Kenshi-style LOD, instanced like terrain_quadtree_batched.vert. Per node (nodeDataTex, 2 texels, gl_InstanceIndex):
//   A = (origin x, origin z, vertex spacing, morph)   B = (level, skirt depth, 1 = heights from the fine window, 0)
// The patch is (cells+1)^2 vertices (cells = 16 or 64 from cam_pos_pad.w; id = row * (cells+1) + col) plus 4 x (cells+1) skirt vertices (ids (cells+1)^2 + side * (cells+1) + k: the border vertex dropped by the
// skirt depth, side 0 = z min, 1 = z max, 2 = x min, 3 = x max). Heights are point-fetched (no filtering) at the vertex's world position from the
// fine window (1.8 m, fullmap.tif) or the world heightmap (3.6 m); the node's vertices lie on that lattice. Each vertex's normal comes from its six
// neighbours (the plugin: ring (0,-1) (1,-1) (1,0) (0,1) (-1,1) (-1,0), cross products summed). Geomorph: an odd vertex's height / normal go to the mean
// of its two neighbours across as morph goes 1 -> 0 (the node's own = 1).
layout(set=0, binding=0) uniform sampler2D heightTex;   // world heightmap, R16_UNORM, (res - 1) cells over the world
layout(set=0, binding=1) uniform sampler2D fineTex;     // fine window, R16_UNORM, same normalisation
layout(set=0, binding=2) uniform sampler2D nodeDataTex;
layout(set=0, binding=3) uniform sampler2D fineNormTex;       // #207: RGBA32F, the plugin's 6-tap normal of every fine-window cell (terrain_lod_normals.comp: the same taps / formula as NormalAt below), point-fetched
layout(set=0, binding=4) uniform sampler2DArray worldNormTex; // #207: the same for the world heightmap, one 1024^2 layer per stride (1, 2, 4, 8, 16, 32 cells)

layout(std140, set=1, binding=0) uniform TerrainBatchUBO {
    mat4  vp;
    vec4  height_range; // x = heightMin, y = heightMax, z = worldExtent, w = resTexels
    vec4  cam_pos_pad;  // xyz = camera world position, w = patch quads per edge
    vec4  fine_win;     // x, y = window origin (cells), z = window side (cells), w = cell size (m)
    vec4  flags;        // x = 1: a vertex whose node a normal window covers reads its normal (and the geomorph targets' normals) there
    ivec4 wo[3];        // origin (stride cells) of the world normal window of stride level 2i (xy) and 2i+1 (zw)
} u;

layout(location=0) out vec3  vNormal;
layout(location=1) out vec2  vWorldPos;
layout(location=2) out float vWorldY;
layout(location=3) out float vDist;
layout(location=4) out float vIsSkirt;
#ifdef TS_SLOT
layout(location=5) flat out uint vSlot;   // ground live pass: the biome-list slot of the node (nodeB.z = fine + 2 * slot)
#endif

#ifndef TS_LOD_NORM
#define TS_LOD_NORM 1
#endif
#ifndef TS_LOD_MORPH
#define TS_LOD_MORPH 1
#endif
const int kNodeDataTexWidth = 256; // must match TerrainQuadtreeRenderer::kNodeDataTexWidth

vec4 FetchNodeTexel(int texelIndex) {
    return texelFetch(nodeDataTex, ivec2(texelIndex % kNodeDataTexWidth, texelIndex / kNodeDataTexWidth), 0);
}

bool  gUseFine;
float gSpacing;
int   gMode;      // 0 = fine window lattice, 1 = world heightmap lattice, 2 = bilinear fallback (a 1.8 m node outside the fine window)
int   gStep;      // vertex spacing in lattice cells (modes 0 / 1)
int   gMaxCell;

// height at a lattice cell (mode 0: relative to the fine window, mode 1: world heightmap cell)
float CellHeight(ivec2 c) {
    c = clamp(c, ivec2(0), ivec2(gMaxCell));
    float frac = (gMode == 0) ? texelFetch(fineTex, c, 0).r : texelFetch(heightTex, c, 0).r;
    return u.height_range.x + frac * (u.height_range.y - u.height_range.x);
}

// bilinear fallback on the 3.6 m map for a 1.8 m node outside the fine window (rare: the window follows the camera)
float HeightFallback(vec2 xz) {
    vec2 uv = (xz / u.height_range.z * (u.height_range.w - 1.0) + 0.5) / u.height_range.w;
    return u.height_range.x + textureLod(heightTex, uv, 0.0).r * (u.height_range.y - u.height_range.x);
}

// the lattice cell of a vertex of this node and the height there: lattice mode points the neighbours at +- gStep cells
struct Site { ivec2 c; vec2 xz; };
float SiteHeight(Site s, ivec2 d) {
    return (gMode == 2) ? HeightFallback(s.xz + vec2(d) * gSpacing) : CellHeight(s.c + d * gStep);
}

// the plugin's vertex normal from the six neighbours; r_k = (dx s, h(k) - h0, dz s), sum of r_k x r_{k+1}, negated
vec3 NormalAt(Site s, float h0) {
    const ivec2 ring[6] = ivec2[6](ivec2(0, -1), ivec2(1, -1), ivec2(1, 0), ivec2(0, 1), ivec2(-1, 1), ivec2(-1, 0));
    vec3 r[6];
    for (int k = 0; k < 6; ++k)
        r[k] = vec3(float(ring[k].x) * gSpacing, SiteHeight(s, ring[k]) - h0, float(ring[k].y) * gSpacing);
    vec3 sum = vec3(0.0);
    for (int k = 0; k < 6; ++k) sum += cross(r[k], r[(k + 1) % 6]);
    return -normalize(sum);
}

int  gWl = -1;      // world normal window layer of this node (-1 = none)
ivec2 gWo;          // its origin
// the normal of the lattice cell c (a lattice cell of this node), from the window texture
vec3 TexNormal(ivec2 c) {
    if (gMode == 0) return texelFetch(fineNormTex, clamp(c, ivec2(0), ivec2(gMaxCell)), 0).xyz;
    return texelFetch(worldNormTex, ivec3(c / gStep - gWo, gWl), 0).xyz;
}

void main() {
    vec4 nodeA = FetchNodeTexel(gl_InstanceIndex * 2);
    vec4 nodeB = FetchNodeTexel(gl_InstanceIndex * 2 + 1);
    vec2  origin = nodeA.xy;
    gSpacing = nodeA.z;
    float morph = nodeA.w;
    float skirtDepth = nodeB.y;
    gUseFine = (int(nodeB.z + 0.5) & 1) != 0;
#ifdef TS_SLOT
    vSlot = uint(int(nodeB.z + 0.5) >> 1);
#endif
    if (gUseFine)               { gMode = 0; gStep = 1; gMaxCell = int(u.fine_win.z) - 1; }
    else if (gSpacing > 3.5)    { gMode = 1; gStep = int(gSpacing / 3.6 + 0.5); gMaxCell = int(u.height_range.w) - 1; }
    else                        { gMode = 2; gStep = 1; gMaxCell = 0; }

    const int cells = int(u.cam_pos_pad.w + 0.5); // quads per patch edge: 16 or 64
    const int kGridSize = cells + 1;
    int id = gl_VertexIndex;
    bool skirt = id >= kGridSize * kGridSize;
    int col, row;
    if (!skirt) {
        col = id % kGridSize; row = id / kGridSize;
    } else {
        int s = id - kGridSize * kGridSize;
        int side = s / kGridSize, k = s % kGridSize;
        col = (side == 2) ? 0 : (side == 3) ? cells : k;
        row = (side == 0) ? 0 : (side == 1) ? cells : k;
    }

    vec2 xz = origin + vec2(float(col), float(row)) * gSpacing;
    Site site;
    site.xz = xz;
    if (gMode == 0)      site.c = ivec2(floor(xz / u.fine_win.w + 0.5)) - ivec2(u.fine_win.xy);
    else if (gMode == 1) site.c = ivec2(floor(xz / u.height_range.z * float(gMaxCell) + 0.5));
    else                 site.c = ivec2(0);
    float h = SiteHeight(site, ivec2(0));
    gWl = int(nodeB.w + 0.5) - 1;
    if (gWl >= 0) { ivec4 q = u.wo[gWl >> 1]; gWo = ((gWl & 1) == 0) ? q.xy : q.zw; }
    const bool normTex = u.flags.x > 0.5 && (gMode == 0 || (gMode == 1 && gWl >= 0));
#if TS_LOD_NORM
    vec3 N = normTex ? TexNormal(site.c) : NormalAt(site, h);
#else
    vec3 N = vec3(0.0, 1.0, 0.0);
#endif

    if ((TS_LOD_MORPH != 0) && morph < 0.999 && ((col | row) & 1) != 0) {
        // the two neighbours across (the plugin's morph targets): diagonal for (odd, odd), along the axis for the others
        vec2 da, db;
        if ((col & 1) != 0 && (row & 1) != 0) { da = vec2(-1.0, -1.0); db = vec2(1.0, 1.0); }
        else if ((col & 1) != 0)              { da = vec2(-1.0, 0.0);  db = vec2(1.0, 0.0); }
        else                                  { da = vec2(0.0, -1.0);  db = vec2(0.0, 1.0); }
        Site sa = site, sb = site;
        sa.xz = xz + da * gSpacing; sb.xz = xz + db * gSpacing;
        sa.c = site.c + ivec2(da) * gStep; sb.c = site.c + ivec2(db) * gStep;
        float ha = SiteHeight(site, ivec2(da)), hb = SiteHeight(site, ivec2(db));
        vec3 mn = normTex ? normalize(TexNormal(sa.c) + TexNormal(sb.c))
                          : normalize(NormalAt(sa, ha) + NormalAt(sb, hb));
        h = mix(0.5 * (ha + hb), h, morph);
        N = normalize(mix(mn, N, morph));
    }
    if (N.y < 0.0) N = -N;

    float y = h - (skirt ? skirtDepth : 0.0);
    vec3 posWS = vec3(xz.x, y, xz.y);
    vNormal   = N;
    vWorldPos = xz;
    vWorldY   = y;
    vIsSkirt  = skirt ? 1.0 : 0.0;
    vDist     = length(u.cam_pos_pad.xyz - posWS);
    gl_Position = u.vp * vec4(posWS, 1.0);
}
