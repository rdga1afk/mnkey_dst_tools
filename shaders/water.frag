#version 460 core
// water.frag — translucent sea surface (G-3).
// Diffuse + Blinn-Phong specular + animated ripple normals.
#ifdef SPIRV_COMPILE

layout(std140, set=3, binding=0) uniform WaterFU {
    vec4 sun_dir;   //  0: xyz=sun direction (to-sun, normalised), w unused
    vec4 cam_pos;   // 16: xyz=camera world pos, w unused
    float time;     // 32
    float opacity;  // 36
    float pad0, pad1;
} u;

// G-3: real per-pixel water/land mask (task #158i-4/9). CPU fills this each
// frame with TerrainQuery::GetHeight samples across the same square the
// quad covers (see SceneRender::kWaterMaskN / water_mask_ssbo) -- 1.0 =
// real terrain-below-water-level, 0.0 = dry. Replaces the old all-or-
// nothing CPU gate (checked only at the camera's own XZ position) that let
// the flat plane draw over dry terrain far outside the loaded chunk grid.
// Must match SceneRender::kWaterMaskN (scene_render.h) exactly.
const int WATER_MASK_N = 64;
layout(std430, set=2, binding=0) readonly buffer WaterMaskSSBO { float waterMask[]; };

layout(location=0) in  vec2  vWorldXZ;
layout(location=1) in  float vFade;
layout(location=2) in  vec2  vMaskUV;
layout(location=0) out vec4  fragColor;

float SampleWaterMask(vec2 uv) {
    vec2 g   = clamp(uv, 0.0, 1.0) * float(WATER_MASK_N - 1);
    ivec2 gi = ivec2(floor(g));
    vec2  gf = fract(g);
    ivec2 gi1 = min(gi + ivec2(1, 1), ivec2(WATER_MASK_N - 1, WATER_MASK_N - 1));
    float m00 = waterMask[gi.y  * WATER_MASK_N + gi.x];
    float m10 = waterMask[gi.y  * WATER_MASK_N + gi1.x];
    float m01 = waterMask[gi1.y * WATER_MASK_N + gi.x];
    float m11 = waterMask[gi1.y * WATER_MASK_N + gi1.x];
    return mix(mix(m00, m10, gf.x), mix(m01, m11, gf.x), gf.y);
}

void main() {
    float mask = SampleWaterMask(vMaskUV);
    // task water-mask-false-positive-bleed (2026-07-31, round 2): the earlier
    // fix (kWaterMaskN 32->64, kWaterWorldSz 3000->2200, see SceneRender's
    // own doc comment) cut the false-positive bleed radius but did not
    // eliminate it -- user screenshots 2026-07-31 19:21 showed long, vivid,
    // fully-saturated blue ribbons/wedges running tens of metres along dry
    // ridge crests, far worse than the earlier thin slivers. Root cause of
    // the REMAINING bleed: for a mask cell with exactly one wet corner
    // (m=1) and three dry corners (m=0), bilinear interpolation gives
    // f(x,y)=(1-x)(1-y) -- solving f=0.02 along the cell diagonal gives
    // x=0.859, i.e. the OLD discard<0.02 threshold let a single isolated wet
    // sample paint water colour across ~86% of the cell's ~70m diagonal
    // (~85m reach) before finally cutting off. Raising the threshold to 0.5
    // drops that reach to x=0.293 (~29m) -- a ~3x tighter bleed radius for
    // the exact same isolated-sample case, at the cost of a sharper (not
    // gradual) edge on real multi-cell lakes' shoreline, which is an
    // acceptable trade since the interior of a real lake stays mask=1
    // regardless of this threshold.
    if (mask < 0.5) discard;

    vec2 uv = vWorldXZ * 0.012;
    float t = u.time;

    // Three-octave animated normal
    float w1 = sin(uv.x * 5.1 + t * 0.9) * 0.06;
    float w2 = sin(uv.y * 4.3 + t * 0.7) * 0.05;
    float w3 = cos(uv.x * 2.8 - uv.y * 3.5 + t * 0.5) * 0.04;
    vec3 N = normalize(vec3(w1 + w3, 1.0, w2 + w3));

    // Diffuse
    vec3 L      = normalize(u.sun_dir.xyz);
    float ndl   = max(dot(N, L), 0.0);
    vec3 sun_col = vec3(0.90, 0.82, 0.65);

    // Blinn-Phong specular
    vec3 camDir = normalize(u.cam_pos.xyz - vec3(vWorldXZ.x, 0.2, vWorldXZ.y));
    vec3 H      = normalize(L + camDir);
    float spec  = pow(max(dot(N, H), 0.0), 80.0) * 0.65;

    // Water color: deep teal → shallow azure
    vec3 deep    = vec3(0.02, 0.10, 0.28);
    vec3 shallow = vec3(0.10, 0.36, 0.54);
    vec3 col     = mix(deep, shallow, clamp(ndl, 0.0, 1.0));
    col += sun_col * spec;

    float alpha = u.opacity * vFade * smoothstep(0.5, 0.85, mask);
    fragColor = vec4(col, alpha);
}
#else
out vec4 fragColor;
void main() { fragColor = vec4(0.05, 0.2, 0.4, 0.6); }
#endif
