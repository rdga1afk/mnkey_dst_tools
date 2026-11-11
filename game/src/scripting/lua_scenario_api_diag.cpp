#include "lua_scenario_api_internal.h"

// ── md.scan_terrain_gaps() -> {triangles, degenerate, nan} ────────────────────
// Diagnostic tool (task #182, blue sky-gap slivers investigation) — kept
// as reusable infrastructure (same rationale as tools/md_terrain_erode.py:
// a working, verified diagnostic worth keeping for future re-checks, even
// though it did not confirm the theory it was written to test). Re-walks
// every LOADED chunk's own persistent chunk.heightmap.h[] using the SAME
// world-position formula and triangulation build_lod_ibo() (terrain_
// upload.cpp) uses — s_idx(col,row)=row*(GRID+1)+col, world pos =
// chunk_origin + col/row*TERRAIN_STEP, height=heightmap.h[] — at every
// LOD tier actually drawn (step 1/2/4/8, TERRAIN_LOD_STEPS). No GPU
// readback needed; this is exactly what got uploaded. For every triangle,
// checks (a) any corner height is NaN/Inf, (b) triangle area is near-zero
// (degenerate — GPU rasterizes zero pixels for it, exactly the "hole"
// symptom).
//
// RESULT (2026-07-19, task #182): run at a confirmed blue-gap repro
// location (9x9 window, all 4 LOD tiers, 3,525,120 triangles) — ZERO
// degenerate triangles, ZERO NaN/Inf. This rules out BOTH the height-
// magnitude theory (already disproven 2026-07-17 via md_terrain_erode.py)
// AND the "index-buffer/vertex-construction defect" theory from that same
// investigation — the actual uploaded geometry is 3D-clean at every LOD
// tier, world-space. Combined with backface culling being off for terrain
// (cull_back=false, terrain_renderer.cpp) and a raw-normal diagnostic
// shader showing the gaps are pure background colour (not a shading
// artifact), the most likely remaining explanation is a VIEW-DEPENDENT
// rasterization gap: on steep real Kenshi cliffs viewed from a grazing
// angle, an individual triangle's real 3D area can be substantial while
// its SCREEN-SPACE projected footprint collapses toward sub-pixel/zero
// (extreme foreshortening), so no pixel-center sample lands inside it —
// not a logic bug, a tessellation-density/sampling limitation inherent to
// heightfield terrain at grazing angles. Not yet verified directly (would
// need a live RenderDoc capture at the exact gap pixel to confirm the
// screen-space triangle is genuinely near-zero-area); logical next step
// if pursued further.
namespace {
struct DiagVec3 { float x, y, z; };
// -ffast-math (this project's release flags) makes std::isnan/isinf
// unreliable/UB — the compiler is allowed to assume they never occur and
// optimize the checks away. Bit-pattern test instead: NaN/Inf both have
// all-1 exponent bits (bits 23-30), safe regardless of fast-math.
static bool DiagBadFloat(float v) {
    uint32_t u; memcpy(&u, &v, sizeof(u));
    return ((u >> 23) & 0xFFu) == 0xFFu;
}
}

int l_md_scan_terrain_gaps(lua_State* L) {
    auto& sr = SceneRender::Get();
    long total = 0, degenerate = 0, nan_count = 0;
    int logged = 0;
    const int MAX_LOG = 40;
    const float AREA_EPS = 1e-4f;  // m^2; a real terrain triangle at 3.6m step has area ~6.5 m^2

    // step=1 is the plain full-resolution (LOD0) triangulation. Steps
    // 2/4/8 (TERRAIN_LOD_STEPS) now use BuildLodIboStitched (terrain_
    // chunk.h, task #182 T-junction fix) — the ACTUAL index buffer content
    // real chunks upload — instead of a hand-rolled reimplementation, so
    // this scan tests the real triangulation against real height data.
    static uint16_t s_stitch_idx[TERRAIN_LOD_IDX[0]];

    for (int cz = 0; cz < SceneRender::TNKN; ++cz) {
        for (int cx = 0; cx < SceneRender::TNKN; ++cx) {
            auto& ch = sr.terrain_chunks[cz][cx];
            if (!ch.loaded) continue;
            float ox = ch.center_x - CHUNK_SIZE * 0.5f;
            float oz = ch.center_z - CHUNK_SIZE * 0.5f;
            auto world = [&](int col, int row) -> DiagVec3 {
                return { ox + col * TERRAIN_STEP,
                         ch.heightmap.h[row * (TERRAIN_GRID + 1) + col],
                         oz + row * TERRAIN_STEP };
            };
            auto check_tri = [&](DiagVec3 a, DiagVec3 b, DiagVec3 c, int step, int tag) {
                total++;
                bool bad_nan = DiagBadFloat(a.x)||DiagBadFloat(a.y)||DiagBadFloat(a.z)
                             ||DiagBadFloat(b.x)||DiagBadFloat(b.y)||DiagBadFloat(b.z)
                             ||DiagBadFloat(c.x)||DiagBadFloat(c.y)||DiagBadFloat(c.z);
                if (bad_nan) {
                    nan_count++;
                    if (logged < MAX_LOG) {
                        printf("[scan_terrain_gaps] NaN/Inf chunk(%d,%d) step=%d tri#%d\n", cx, cz, step, tag);
                        ++logged;
                    }
                    return;
                }
                float ux=b.x-a.x, uy=b.y-a.y, uz=b.z-a.z;
                float vx=c.x-a.x, vy=c.y-a.y, vz=c.z-a.z;
                float crx = uy*vz - uz*vy, cry = uz*vx - ux*vz, crz = ux*vy - uy*vx;
                float area = 0.5f * sqrtf(crx*crx + cry*cry + crz*crz);
                if (area < AREA_EPS) {
                    degenerate++;
                    if (logged < MAX_LOG) {
                        printf("[scan_terrain_gaps] DEGENERATE chunk(%d,%d) step=%d tri#%d "
                               "area=%.6f world=(%.1f,%.1f,%.1f)\n",
                               cx, cz, step, tag, area, a.x, a.y, a.z);
                        ++logged;
                    }
                }
            };

            // step=1: plain full-resolution grid.
            for (int row = 0; row < TERRAIN_GRID; ++row) {
                for (int col = 0; col < TERRAIN_GRID; ++col) {
                    DiagVec3 bl = world(col, row), br = world(col+1, row);
                    DiagVec3 tl = world(col, row+1), tr = world(col+1, row+1);
                    check_tri(bl, br, tl, 1, row*TERRAIN_GRID+col);
                    check_tri(br, tr, tl, 1, row*TERRAIN_GRID+col);
                }
            }
            // steps 2/4/8: real stitched triangulation.
            for (int li = 0; li < TERRAIN_LOD_LEVELS; ++li) {
                int step = TERRAIN_LOD_STEPS[li];
                int count = BuildLodIboStitched(s_stitch_idx, step);
                for (int t = 0; t < count; t += 3) {
                    int c0 = s_stitch_idx[t]   % (TERRAIN_GRID+1), r0 = s_stitch_idx[t]   / (TERRAIN_GRID+1);
                    int c1 = s_stitch_idx[t+1] % (TERRAIN_GRID+1), r1 = s_stitch_idx[t+1] / (TERRAIN_GRID+1);
                    int c2 = s_stitch_idx[t+2] % (TERRAIN_GRID+1), r2 = s_stitch_idx[t+2] / (TERRAIN_GRID+1);
                    check_tri(world(c0,r0), world(c1,r1), world(c2,r2), step, t/3);
                }
            }
        }
    }
    printf("[scan_terrain_gaps] total=%ld degenerate=%ld nan=%ld\n", total, degenerate, nan_count);
    lua_newtable(L);
    lua_pushinteger(L, total);      lua_setfield(L, -2, "triangles");
    lua_pushinteger(L, degenerate); lua_setfield(L, -2, "degenerate");
    lua_pushinteger(L, nan_count);  lua_setfield(L, -2, "nan");
    return 1;
}

// ── md.scan_terrain_seam_tjunctions() -> {pairs_checked, max_gap_m, over_skirt} ─
// Diagnostic tool (task #182) — both md.scan_terrain_gaps() (0/3.5M
// degenerate, all LOD tiers) and md.scan_terrain_screen_gaps() (0/1.2M
// near-zero-screen-area) ruled out data corruption, shading, and single-
// triangle grazing-angle collapse as the cause of the blue sky-gap
// slivers. The remaining untested candidate: a classic adaptive-LOD
// T-junction. Two ADJACENT chunks drawn at DIFFERENT LOD tiers share a
// boundary in world space, but the denser side's edge visits every
// step_fine-th height sample (zigzagging with the real terrain) while the
// coarser side's edge only visits every step_coarse-th sample, connecting
// them with a STRAIGHT line — real local relief between those sparse
// points doesn't exist on the coarse side at all. Where the fine side's
// real height rises above the coarse side's straight-line interpolation,
// there is a real vertical gap the terrain_renderer.cpp skirt fix (2m
// drop, every LOD tier now) may not be deep enough to cover.
//
// For every pair of adjacent LOADED chunks whose CURRENT distance-based
// LOD tier differs (exact same formula npc_render.cpp's terrain draw loop
// uses: dist_h vs rqc.mesh_lod0/1/2_cr_m), walks the shared edge and
// computes, for each height sample that exists on the fine side but not
// the coarse side, the vertical gap between the fine side's real height
// and the coarse side's straight-line-interpolated height at that point.
// Reports the worst gap found and how many exceed SKIRT_DROP=2m (terrain_
// gen.cpp) — the threshold above which the current skirt fix cannot be
// covering it.
int l_md_scan_terrain_seam_tjunctions(lua_State* L) {
    auto& sr = SceneRender::Get();
    auto& rqc = RenderQualityConfig::Get();
    const float SKIRT_DROP = 2.0f;  // must match terrain_gen.cpp's own constant
    MdCamera cam{};
#ifdef MONKEY_DUST_EDITOR
    cam = EditorCore::Get().editor_cam;
#endif

    auto lod_step_for = [&](TerrainChunk& ch) -> int {
        float ddx = ch.center_x - cam.pos.x, ddz = ch.center_z - cam.pos.z;
        float dist_h = sqrtf(ddx*ddx + ddz*ddz);
        int lod = (dist_h < rqc.mesh_lod0_cr_m) ? 0
                : (dist_h < rqc.mesh_lod1_cr_m) ? 1
                : (dist_h < rqc.mesh_lod2_cr_m) ? 2 : 3;
        static const int steps[4] = {1, 2, 4, 8};
        return steps[lod];
    };

    // fine_h[i] = height at fine-side edge sample i (i in [0,TERRAIN_GRID]).
    // Returns the max |fine_h[i] - coarse straight-line interp| over all i
    // that don't land exactly on a coarse-side sample.
    auto worst_tjunction_gap = [&](const float* fine_h, int step_coarse, long& over_count) -> float {
        float worst = 0.f;
        for (int i = 0; i <= TERRAIN_GRID; i += step_coarse) {
            if (i + step_coarse > TERRAIN_GRID) break;
            float h0 = fine_h[i], h1 = fine_h[i + step_coarse];
            for (int k = 1; k < step_coarse; ++k) {
                float t = (float)k / (float)step_coarse;
                float interp = h0 * (1.f - t) + h1 * t;
                float gap = fabsf(fine_h[i + k] - interp);
                if (gap > worst) worst = gap;
                if (gap > SKIRT_DROP) over_count++;
            }
        }
        return worst;
    };

    long pairs_checked = 0, over_skirt = 0;
    float max_gap = 0.f;
    int worst_cx = -1, worst_cz = -1;

    for (int cz = 0; cz < SceneRender::TNKN; ++cz) {
        for (int cx = 0; cx < SceneRender::TNKN; ++cx) {
            auto& chA = sr.terrain_chunks[cz][cx];
            if (!chA.loaded) continue;
            int stepA = lod_step_for(chA);

            // Right neighbour (shared vertical edge: chA col=GRID vs chB col=0)
            if (cx + 1 < SceneRender::TNKN) {
                auto& chB = sr.terrain_chunks[cz][cx+1];
                if (chB.loaded) {
                    int stepB = lod_step_for(chB);
                    if (stepA != stepB) {
                        pairs_checked++;
                        static float edgeA[TERRAIN_GRID+1], edgeB[TERRAIN_GRID+1];
                        for (int r = 0; r <= TERRAIN_GRID; ++r) {
                            edgeA[r] = chA.heightmap.h[r*(TERRAIN_GRID+1) + TERRAIN_GRID];
                            edgeB[r] = chB.heightmap.h[r*(TERRAIN_GRID+1) + 0];
                        }
                        int fine_step = stepA < stepB ? stepA : stepB;
                        int coarse_step = stepA < stepB ? stepB : stepA;
                        const float* fine_edge = stepA < stepB ? edgeA : edgeB;
                        long over = 0;
                        float g = worst_tjunction_gap(fine_edge, coarse_step, over);
                        over_skirt += over;
                        if (g > max_gap) { max_gap = g; worst_cx = cx; worst_cz = cz; }
                        (void)fine_step;
                    }
                }
            }
            // Top neighbour (shared horizontal edge: chA row=GRID vs chC row=0)
            if (cz + 1 < SceneRender::TNKN) {
                auto& chC = sr.terrain_chunks[cz+1][cx];
                if (chC.loaded) {
                    int stepC = lod_step_for(chC);
                    if (stepA != stepC) {
                        pairs_checked++;
                        static float edgeA2[TERRAIN_GRID+1], edgeC[TERRAIN_GRID+1];
                        for (int c = 0; c <= TERRAIN_GRID; ++c) {
                            edgeA2[c] = chA.heightmap.h[TERRAIN_GRID*(TERRAIN_GRID+1) + c];
                            edgeC[c]  = chC.heightmap.h[0*(TERRAIN_GRID+1) + c];
                        }
                        int coarse_step = stepA < stepC ? stepC : stepA;
                        const float* fine_edge = stepA < stepC ? edgeA2 : edgeC;
                        long over = 0;
                        float g = worst_tjunction_gap(fine_edge, coarse_step, over);
                        over_skirt += over;
                        if (g > max_gap) { max_gap = g; worst_cx = cx; worst_cz = cz; }
                    }
                }
            }
        }
    }
    printf("[scan_terrain_seam_tjunctions] pairs_checked=%ld max_gap_m=%.3f (chunk %d,%d) over_skirt(>%.1fm)=%ld\n",
           pairs_checked, max_gap, worst_cx, worst_cz, SKIRT_DROP, over_skirt);
    lua_newtable(L);
    lua_pushinteger(L, pairs_checked); lua_setfield(L, -2, "pairs_checked");
    lua_pushnumber(L,  max_gap);       lua_setfield(L, -2, "max_gap_m");
    lua_pushinteger(L, over_skirt);    lua_setfield(L, -2, "over_skirt");
    return 1;
}
// ── Assertions — print EXPECTED/ACTUAL to stdout, then raise a Lua error ─────
// (principle 3: assert_* failure -> non-zero exit code; principle 10:
// failure-artifact auto-collection happens in the scenario driver's
// catch-all error path in main.cpp, not here — every Lua error, whatever
// its source, funnels through LuaSystem::ResumeScenario's Failed status.)
