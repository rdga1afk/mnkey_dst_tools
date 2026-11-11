#version 460 core
#extension GL_GOOGLE_include_directive : require
#include "terrain_gbuf_pack.glsl"
// TERRAIN_CA_REBUILD_PROMPT.md Phase 4 -- Variant A (screen-space
// decoupled shading, CA §1.2 adapted to this project's data model, see
// terrain_research/perf/PHASE4_PLAN.md). This is the geometry-pass half:
// writes world-position + normal to a dedicated mini-G-buffer instead of
// doing any ground-layer shading inline. The actual shading happens in a
// SEPARATE screen-space pass (terrain_shading_screenspace.frag) that reads
// this back -- the CA idea of decoupling geometry cost from shading cost,
// done honestly rather than corner-cut.
//
// Single RGBA32F target (not true multi-render-target output) -- this
// project's GpuPipeline HAL wrapper only supports one color attachment per
// pipeline (confirmed: the one existing 2-target G-buffer pipeline in this
// codebase, npc_render.cpp's NPC gbuffer_pipeline, is dead/unused code that
// never actually draws -- not a working precedent to copy). Extending the
// shared HAL for real MRT would touch code every pipeline in the engine
// depends on, for an experimental path the reference material itself
// predicts will lose on this hardware's bandwidth -- not a good trade.
// Packing avoids that entirely: xyz = world position, w = normal.xz packed
// as two 12-bit quantized values via PLAIN FLOAT ARITHMETIC (normal.y is
// reconstructed in the shading pass -- always non-negative for a true
// heightfield terrain, which cannot represent overhangs by construction,
// so sqrt(max(0, 1-x*x-z*z)) is a safe, lossless-enough reconstruction).
// Diagnosed live (2026-08-02, real crosshatch-streak artifact across the
// whole visible ground, reproduced with a real screenshot): an EARLIER
// version of this packing used uintBitsToFloat(nx16<<16 | nz16) to cram
// both 16-bit values into one bit-exact float. That is unsafe -- the
// packed integer's top bits land in the float's sign+exponent field, so
// almost any (nx,nz) pair produces a NaN/Inf/denormal bit pattern, and
// this GPU's render-target write/read path does not guarantee those
// survive a round trip through an RGBA32F attachment (flush-to-zero /
// NaN-canonicalization are both permitted, unlike a plain SSBO). The
// arithmetic packing below only ever produces small, exactly-representable
// finite floats (0..~16.7M) -- no bit-pattern ever crosses into NaN/Inf/
// denormal territory, so it round-trips exactly through the render target.
//
// Reuses the SAME vertex inputs (vNormal/vWorldPos/vWorldY/vDist)
// terrain_patch.vert produces -- no new vertex shader needed, this only
// replaces the FRAGMENT stage.
//
// Normal: uses the vertex-interpolated `vNormal` varying directly (NOT a
// dFdx/dFdy screen-space reconstruction). History, for anyone re-deriving
// this: an earlier version of this file reconstructed the normal
// geometrically from dFdx/dFdy of world position, because a same-day
// diagnostic against BAKED geometry (terrain_baked.vert) found vNormal
// decoding to zero there. That workaround was applied unconditionally to
// this shared fragment shader, even though it was only ever verified
// against the baked vertex shader -- and it broke VTF geometry
// (terrain_patch.vert) the moment VTF+Variant A was actually exercised:
// dFdx/dFdy of an interpolated position is the FLAT per-triangle normal,
// not a smooth per-vertex one, and VTF's world-space triangles are large
// enough (unlike baked's finer mesh) that the facets became a visible
// "shard"/moire artifact covering the whole terrain (diagnosed live
// 2026-08-02 via the project's standard N*0.5+0.5 diagnostic: the
// dFdx/dFdy version showed sharp triangular facets exactly matching the
// reported bug; switching to plain `normalize(vNormal)` produced a
// perfectly smooth gradient with the artifact gone). Since VTF is now the
// ONLY geometry path this codebase runs (terrain_path_baked locked false,
// scene_render.h) -- baked's separate vNormal issue is moot, it never
// executes -- so there is no longer a reason to carry the inferior flat
// fallback at all.

// RESOLVE_OPT spatial-split plan, Крок 4/6 (docs/RESOLVE_OPT.md,
// 2026-09-19): boundary-mask, bit-stolen into this G-buffer's existing
// packed .w channel instead of true MRT. The spatial split itself was
// reverted (engine 81a091f, 2026-09-21) -- block-sampled measurement
// found the 4-draw resolve variant ~4x slower in real GPU-ms than a
// single full-shader draw (no hardware discard, each of the 4 pipelines
// rasterized the whole screen and re-fetched the G-buffer). The category
// computation this comment used to justify (TS_CornerBlendCategory, plus
// the 3 samplers it needed) is dead now that the resolve shader never
// reads it back (terrain_shading_screenspace.frag decodes the 2-bit
// slot only to shift past it, never uses the value) -- surgical-
// simplicity audit follow-up, Phase-2-adjacent perf work, 2026-09-26.
// The packed layout keeps its category slot (always 0 now) so the
// decode side needs no change.

layout(location=0) in vec3  vNormal;
layout(location=1) in vec2  vWorldPos;
layout(location=2) in float vWorldY;
layout(location=3) in float vDist;
layout(location=4) in float vIsSkirt;

#ifdef TS_SLOT
layout(location=5) flat in uint vSlot;
layout(location=1) out uint outSlot;    // R8_UINT: the biome-list slot of the patch (ground live pass)
#elif defined(TS_SLOT0)
layout(location=1) out uint outSlot;    // geometry without a patch list (features, props): slot 0
#endif
layout(location=0) out uvec4 outGbuffer; // R16G16B16A16_UINT, see terrain_gbuf_pack.glsl

void main() {
    vec3 N = normalize(vNormal);
    // 2026-08-24: nzQ dropped from 12-bit to 11-bit for isSkirt. Крок 4
    // (2026-09-19): nzQ dropped 11-bit->10-bit for a 1-bit boundary
    // mask. Крок 6 (2026-09-19, same session): nzQ dropped once more,
    // 10-bit->9-bit (1023->511 levels, still far finer than visually
    // perceptible -- same judgment call as the two earlier drops), to
    // widen the mask from 1 bit to 2 -- the §"3-way розподіл" measurement
    // found cliff-only and zone-only boundary populations are each large
    // enough on their own (not just "both") to justify their own cheap
    // SPIR-V. Layout: nxQ(12b, mul=1) + nzQ(9b, mul=4096) + skirtBit(1b,
    // mul=2097152) + category(2b, mul=4194304). Max packed value stays
    // exactly 2^24-1 (4095 + 511*4096 + 2097152 + 3*4194304), still
    // bit-exact through RGBA32F.
    outGbuffer = TS_GbufEncode(vec3(vWorldPos.x, vWorldY, vWorldPos.y), N, vIsSkirt > 0.5);
#ifdef TS_SLOT
    outSlot = vSlot;
#elif defined(TS_SLOT0)
    outSlot = 0u;
#endif
}
