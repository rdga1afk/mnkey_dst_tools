#include "stb_image_write.h"
#include <vector>
#include <cmath>
extern bool g_terrain_lod;
extern int g_terrain_lod_levels[11];
#include "lua_scenario_api_internal.h"

// task terrain-coord-bug (2026-09-04): TerrainQuery::GetHeight expects its
// wx/wz in SceneRender's LOCAL floating-window space (see that function's
// own doc comment -- it converts wx -> absolute via
// `wx - world_off_x_ + zone_ox_*chunk_size_`, the standard local->absolute
// formula this file's own l_md_world_to_local already documents). This
// binding's documented contract (md.terrain_height/md_ctl probe-height,
// used with real absolute Kenshi world metres everywhere in this project's
// history) is the OPPOSITE convention -- passing absolute coordinates
// straight through silently sampled a WRONG, unrelated world location
// (confirmed live: md.terrain_height(5960,9280) was actually reading
// absolute (12568.0,19574.4), ~7.4km away -- exact bit-for-bit match
// against a direct offline read of world_hmap.r16 at that wrong location).
// Convert absolute -> local first, same formula l_md_world_to_local uses,
// before handing off to the local-space GetHeight.
int l_md_terrain_height(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float z = (float)luaL_checknumber(L, 2);
    auto& sr = SceneRender::Get();
    float lx = x + sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE;
    float lz = z + sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE;
    float h = 0.f;
    if (TerrainQuery::Get().GetHeight(lx, lz, h)) {
        lua_pushnumber(L, (lua_Number)h);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// ── md.ray_ground(x, y, z, dx, dy, dz, tmax) -> distance of the first ground hit along the ray, or -1 ──
// #207 PVT simulation (tests/scenarios/pvt_sim.lua): a march against the same height query as md.terrain_height (absolute coordinates), step max(3 m, 4 % of the distance), no refinement.
int l_md_ray_ground(lua_State* L) {
    const float ox = (float)luaL_checknumber(L, 1), oy = (float)luaL_checknumber(L, 2), oz = (float)luaL_checknumber(L, 3);
    const float dx = (float)luaL_checknumber(L, 4), dy = (float)luaL_checknumber(L, 5), dz = (float)luaL_checknumber(L, 6);
    const float tmax = (float)luaL_checknumber(L, 7);
    auto& sr = SceneRender::Get();
    const float offx = sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE, offz = sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE;
    for (float t = 3.0f; t < tmax; t += std::max(3.0f, t * 0.04f)) {
        float h = 0.f;
        if (TerrainQuery::Get().GetHeight(ox + dx * t + offx, oz + dz * t + offz, h) && oy + dy * t < h) { lua_pushnumber(L, (lua_Number)t); return 1; }
    }
    lua_pushnumber(L, -1.0);
    return 1;
}

// ── md.pvt_sample(x, y, z, yaw_deg, pitch_deg, fov_deg, rx, ry, texel_m, max_mip) -> served_rays, near_rays, sky_rays, {unique tile keys} ──
// #207 PVT feasibility (tests/scenarios/pvt_sim.lua): rx x ry rays through the camera (yaw 0 = +z, 90 = +x, pitch > 0 looks down, fov = vertical, aspect 16:9) are marched like md.ray_ground.
// A hit at distance t has the pixel footprint f = t * 2 tan(fov/2) / 1080 (1080p); mip = round(log2(f / texel_m)); mip < 0 = nearer than the finest texel (stays live), else the 128^2-texel tile of size
// 128 * texel_m * 2^mip (mip capped at max_mip) that holds the hit; its key = mip * 2^44 + tx * 2^22 + tz (tx, tz = floor(world / tile size)).
int l_md_pvt_sample(lua_State* L) {
    const float ox = (float)luaL_checknumber(L, 1), oy = (float)luaL_checknumber(L, 2), oz = (float)luaL_checknumber(L, 3);
    const double yaw = luaL_checknumber(L, 4) * 3.14159265358979 / 180.0, pit = luaL_checknumber(L, 5) * 3.14159265358979 / 180.0;
    const double tanh_ = std::tan(luaL_checknumber(L, 6) * 3.14159265358979 / 360.0);
    const int rx = (int)luaL_checkinteger(L, 7), ry = (int)luaL_checkinteger(L, 8);
    const double texel = luaL_checknumber(L, 9); const int max_mip = (int)luaL_checkinteger(L, 10);
    const double fx = std::sin(yaw) * std::cos(pit), fy = -std::sin(pit), fz = std::cos(yaw) * std::cos(pit);
    const double rgx = std::cos(yaw), rgz = -std::sin(yaw);
    const double ux = -rgz * fy, uy = rgz * fx - rgx * fz, uz = rgx * fy;
    auto& sr = SceneRender::Get();
    const float offx = sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE, offz = sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE;
    int served = 0, nearr = 0, sky = 0;
    std::vector<double> keys; keys.reserve(1024);
    for (int iy = 0; iy < ry; ++iy) {
        const double ny = (1.0 - (iy + 0.5) / ry * 2.0) * tanh_;
        for (int ix = 0; ix < rx; ++ix) {
            const double nx = ((ix + 0.5) / rx * 2.0 - 1.0) * tanh_ * (16.0 / 9.0);
            double dx = fx + rgx * nx + ux * ny, dy = fy + uy * ny, dz = fz + rgz * nx + uz * ny;
            const double il = 1.0 / std::sqrt(dx * dx + dy * dy + dz * dz); dx *= il; dy *= il; dz *= il;
            float hit = -1.f;
            for (float t = 3.0f; t < 3000.0f; t += std::max(3.0f, t * 0.04f)) {
                float h = 0.f;
                if (TerrainQuery::Get().GetHeight((float)(ox + dx * t) + offx, (float)(oz + dz * t) + offz, h) && oy + dy * t < h) { hit = t; break; }
            }
            if (hit < 0.f) { ++sky; continue; }
            const double f = hit * 2.0 * tanh_ / 1080.0;
            int mip = (int)std::floor(std::log2(f / texel) + 0.5);
            if (mip < 0) { ++nearr; continue; }
            if (mip > max_mip) mip = max_mip;
            ++served;
            const double S = 128.0 * texel * std::ldexp(1.0, mip);
            const double wx = ox + dx * hit, wz = oz + dz * hit;
            keys.push_back((double)mip * 17592186044416.0 + std::floor(wx / S) * 4194304.0 + std::floor(wz / S));
        }
    }
    std::sort(keys.begin(), keys.end()); keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    lua_pushinteger(L, served); lua_pushinteger(L, nearr); lua_pushinteger(L, sky);
    lua_createtable(L, (int)keys.size(), 0);
    for (size_t i = 0; i < keys.size(); ++i) { lua_pushnumber(L, keys[i]); lua_rawseti(L, -2, (int)i + 1); }
    return 4;
}

// ── md.chunk_stats() -> {pending} ─────────────────────────────────────────────
// Used to report "loaded"/"dropped" from ChunkManager, the legacy entity/
// tree chunk-streaming system (own IO worker thread + LCG procedural
// generation) — removed (2026-08-07): its SetSpawnCallbacks was never
// called anywhere in the repo, so it only ever created/destroyed bare,
// componentless (invisible, non-functional) entities on every chunk-
// boundary crossing, superseded by the real Kenshi feature-scatter
// placement system. "pending" (TerrainStreamQueue's in-flight terrain
// build requests, the actual live terrain-mesh streaming system) is
// unrelated and still real.
int l_md_chunk_stats(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, TerrainStreamQueue::Get().PendingCount()); lua_setfield(L, -2, "pending");
    return 1;
}

// ── md.zone_info() -> {zone_ox, zone_oz, tnoff_x, tnoff_z} ────────────────────
// DIAG (bug-report cross-check, task terrain-patches): lets a --exec scenario
// convert a reported camera position (local floating-window coords, see
// SceneRender::tnoff_x's doc comment) into the absolute Kenshi zone + tif
// pixel that a screenshot's visible patch corresponds to, without guessing.
int l_md_zone_info(lua_State* L) {
    auto& sr = SceneRender::Get();
    lua_newtable(L);
    lua_pushinteger(L, sr.zone_ox); lua_setfield(L, -2, "zone_ox");
    lua_pushinteger(L, sr.zone_oz); lua_setfield(L, -2, "zone_oz");
    lua_pushnumber(L,  sr.tnoff_x); lua_setfield(L, -2, "tnoff_x");
    lua_pushnumber(L,  sr.tnoff_z); lua_setfield(L, -2, "tnoff_z");
    lua_pushstring(L,  WorldRegistry::Get().CurrentZone()); lua_setfield(L, -2, "current_zone");
    return 1;
}

// ── md.world_to_local(wx, wy, wz) -> local_x, local_y, local_z ────────────────
// Inverse of the abs=local-tnoff+zone_ox*CHUNK_SIZE relation documented on
// SceneRender::tnoff_x (see also l_md_set_camera_pose's range-check, added
// the same session this helper was added): scenarios naturally think in
// absolute Kenshi world metres ("teleport to settlement X"), so give them a
// direct conversion instead of expecting every script author to redo this
// algebra (and risk the silent-wrong-teleport failure mode) by hand.
int l_md_world_to_local(lua_State* L) {
    float wx = (float)luaL_checknumber(L, 1);
    float wy = (float)luaL_checknumber(L, 2);
    float wz = (float)luaL_checknumber(L, 3);
    auto& sr = SceneRender::Get();
    float lx = wx + sr.tnoff_x - (float)sr.zone_ox * CHUNK_SIZE;
    float lz = wz + sr.tnoff_z - (float)sr.zone_oz * CHUNK_SIZE;
    lua_pushnumber(L, lx);
    lua_pushnumber(L, wy);
    lua_pushnumber(L, lz);
    return 3;
}

// ── md.probe_world_heightmap() -> {ok, height_min, height_max, extent, res} ──
// Granite-migration Phase 1 verify probe (plan at /home/rdga1/.claude/plans/
// serene-pondering-teapot.md): builds ONE TerrainWorldHeightmap on demand
// (throwaway instance, not the production terrain_qt path — completely
// isolated from the shipping dual-run terrain system) and reports whether
// it built successfully plus its height range/extent, so a --exec scenario
// can confirm the R16_UNORM texture creation + upload + mip-generation
// actually completed on real hardware instead of trusting only that the
// C++ compiled.
int l_md_probe_world_heightmap(lua_State* L) {
    static TerrainWorldHeightmap* s_probe = nullptr;
    if (!s_probe) s_probe = new TerrainWorldHeightmap();
    bool ok = s_probe->Init(md::GpuDevice::Get().SDLDevice());
    lua_newtable(L);
    lua_pushboolean(L, ok);           lua_setfield(L, -2, "ok");
    lua_pushnumber(L, s_probe->HeightMin());   lua_setfield(L, -2, "height_min");
    lua_pushnumber(L, s_probe->HeightMax());   lua_setfield(L, -2, "height_max");
    lua_pushnumber(L, s_probe->WorldExtent()); lua_setfield(L, -2, "extent");
    lua_pushinteger(L, s_probe->Resolution()); lua_setfield(L, -2, "res");
    return 1;
}

// ── md.terrain_pixel_fraction() -> fraction (0..1) of G-buffer pixels with terrain ─
// M2.1 (PLAN_KENSHI_ENGINE): a pixel is terrain if the projected-mode G-buffer depth
// (cleared to 1.0, terrain writes < 1.0) is below 1.0. Reflects the LAST rendered frame,
// so call it after md.screenshot() + wait_ticks(). Returns -1 if unavailable.
int l_md_terrain_pixel_fraction(lua_State* L) {
    double frac = -1.0;
    auto& gd = SceneRender::Get().terrain_shading_projected_.GBufferDepth();
    SDL_GPUTexture* tex = gd.SDLTexture();
    const int w = gd.Width(), h = gd.Height();
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (tex && w > 0 && h > 0 && dev) {
        const Uint32 bytes = (Uint32)w * (Uint32)h * 4u;
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tbi.size  = bytes;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion reg{};
        reg.texture = tex; reg.w = (Uint32)w; reg.h = (Uint32)h; reg.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = tb; dst.pixels_per_row = (Uint32)w; dst.rows_per_layer = (Uint32)h;
        SDL_DownloadFromGPUTexture(cp, &reg, &dst);
        SDL_EndGPUCopyPass(cp);
        auto fence = md::GpuDevice::Get().SubmitAndAcquireFence(cmd);
        if (fence) { md::GpuDevice::Get().WaitForFence(fence); md::GpuDevice::Get().ReleaseFence(fence); }
        if (const float* d = (const float*)SDL_MapGPUTransferBuffer(dev, tb, false)) {
            size_t n = (size_t)w * (size_t)h, hit = 0;
            for (size_t i = 0; i < n; ++i) if (d[i] < 1.0f) ++hit;
            frac = (double)hit / (double)n;
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }
    lua_pushnumber(L, frac);
    return 1;
}

// ── md.dump_depth(path) -> ok: the last terrain G-buffer depth (D32F, 0..1, 1 = no terrain) as raw: int32 w, int32 h, float pr[16] (projection * rotation, camera at the origin,
// column-major), float cam[3] (absolute camera position), float depth[w*h]. For tests/scenarios/shimmer_metric.py (reprojection). Call after md.screenshot() + wait_ticks().
extern float g_dump_pr[16], g_dump_cam[3];
int l_md_dump_depth(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    bool ok = false;
    auto& gd = SceneRender::Get().terrain_shading_projected_.GBufferDepth();
    SDL_GPUTexture* tex = gd.SDLTexture();
    const int w = gd.Width(), h = gd.Height();
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (tex && w > 0 && h > 0 && dev) {
        SDL_GPUTransferBufferCreateInfo tbi{}; tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tbi.size = (Uint32)w * (Uint32)h * 4u;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion reg{}; reg.texture = tex; reg.w = (Uint32)w; reg.h = (Uint32)h; reg.d = 1;
        SDL_GPUTextureTransferInfo dst{}; dst.transfer_buffer = tb; dst.pixels_per_row = (Uint32)w; dst.rows_per_layer = (Uint32)h;
        SDL_DownloadFromGPUTexture(cp, &reg, &dst);
        SDL_EndGPUCopyPass(cp);
        auto fence = md::GpuDevice::Get().SubmitAndAcquireFence(cmd);
        if (fence) { md::GpuDevice::Get().WaitForFence(fence); md::GpuDevice::Get().ReleaseFence(fence); }
        if (const float* d = (const float*)SDL_MapGPUTransferBuffer(dev, tb, false)) {
            if (FILE* f = fopen(path, "wb")) {
                int32_t hd[2] = { w, h }; fwrite(hd, 4, 2, f); fwrite(g_dump_pr, 4, 16, f); fwrite(g_dump_cam, 4, 3, f); fwrite(d, 4, (size_t)w * (size_t)h, f); fclose(f); ok = true;
            }
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }
    lua_pushboolean(L, ok);
    return 1;
}


static bool DownloadTexture(SDL_GPUTexture* tex, int w, int h, size_t bpp, std::vector<uint8_t>& out) {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!tex || !dev) return false;
    SDL_GPUTransferBufferCreateInfo tbi{}; tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tbi.size = (Uint32)((size_t)w * h * bpp);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg{}; reg.texture = tex; reg.w = (Uint32)w; reg.h = (Uint32)h; reg.d = 1;
    SDL_GPUTextureTransferInfo dst{}; dst.transfer_buffer = tb; dst.pixels_per_row = (Uint32)w; dst.rows_per_layer = (Uint32)h;
    SDL_DownloadFromGPUTexture(cp, &reg, &dst);
    SDL_EndGPUCopyPass(cp);
    auto fence = md::GpuDevice::Get().SubmitAndAcquireFence(cmd);
    if (fence) { md::GpuDevice::Get().WaitForFence(fence); md::GpuDevice::Get().ReleaseFence(fence); }
    bool ok = false;
    if (const uint8_t* d = (const uint8_t*)SDL_MapGPUTransferBuffer(dev, tb, false)) { out.assign(d, d + tbi.size); SDL_UnmapGPUTransferBuffer(dev, tb); ok = true; }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return ok;
}

// ── md.ground_reuse_dump(path) -> bool ─
// #163 5b-2b: the last frame's ground target as a PNG, the pixels taken from the previous frame tinted green, pixels without ground black: shows that the history pixels are ground.
int l_md_ground_reuse_dump(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    bool ok = false;
    auto& sr = SceneRender::Get();
    SDL_GPUTexture* tex = sr.ground_split_pass_.Tex();
    const int w = sr.terrain_shading_projected_.GBufferDepth().Width(), h = sr.terrain_shading_projected_.GBufferDepth().Height();
    std::vector<uint8_t> px;
    if (tex && w > 0 && DownloadTexture(tex, w, h, 4, px)) {
        SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
        const bool bgra = SDL_GetGPUSwapchainTextureFormat(dev, md::GpuDevice::Get().Window()) == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                          SDL_GetGPUSwapchainTextureFormat(dev, md::GpuDevice::Get().Window()) == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
        std::vector<uint8_t> out((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const uint8_t a = px[i * 4 + 3];
            uint8_t r = px[i * 4 + (bgra ? 2 : 0)], g = px[i * 4 + 1], b = px[i * 4 + (bgra ? 0 : 2)];
            if (a < 128) { r = g = b = 0; }
            else if (a < 230) { r = (uint8_t)(r / 2); g = (uint8_t)(g / 2 + 127); b = (uint8_t)(b / 2); }
            out[i * 3] = r; out[i * 3 + 1] = g; out[i * 3 + 2] = b;
        }
        ok = stbi_write_png(path, w, h, 3, out.data(), w * 3) != 0;
    }
    lua_pushboolean(L, ok);
    return 1;
}

// md.splash_shot(path, w, h[, seed[, progress]]) -> bool: the loading splash rendered offscreen at w x h (seed 0 = the clock's pick, else a fixed picture and tip), PNG to `path`. #200 gate.
#include "../render/splash_screen.h"
int l_md_splash_shot(lua_State* L) {
    SplashScreen& sp = SplashScreen::Get();
    const bool was_ready = sp.IsReady();   // the game shuts the splash down after SceneRender::Init
    if (!was_ready) sp.Init(md::GpuDevice::Get().SDLDevice());
    if (lua_isnumber(L, 5)) sp.SetProgress((float)lua_tonumber(L, 5));
    const bool ok = sp.SaveFrame(luaL_checkstring(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), (uint32_t)luaL_optinteger(L, 4, 0));
    if (!was_ready) sp.Shutdown();
    lua_pushboolean(L, ok);
    return 1;
}

extern float g_ru_chain_pushed;   // npc_render_deferred.cpp (#207)
// ── md.ground_reuse_stats() -> {ok, w, h, ground, live, reused} ─
// #163 5b-2b: counts the pixels of the last frame's ground target by alpha: 255 = shaded live, 128..253 (0.75 .. 0.98 with a chain) = taken from the previous frame, 0 = no ground written.
int l_md_ground_reuse_stats(lua_State* L) {
    lua_newtable(L);
    auto& sr = SceneRender::Get();
    SDL_GPUTexture* tex = sr.ground_split_pass_.Tex();
    const int w = sr.terrain_shading_projected_.GBufferDepth().Width(), h = sr.terrain_shading_projected_.GBufferDepth().Height();
    std::vector<uint8_t> px;
    const bool ok = tex && w > 0 && DownloadTexture(tex, w, h, 4, px);
    lua_pushboolean(L, ok); lua_setfield(L, -2, "ok");
    if (ok) {
        size_t live = 0, reused = 0;
        for (size_t i = 0; i < (size_t)w * h; ++i) { const uint8_t a = px[i * 4 + 3]; if (a >= 254) ++live; else if (a >= 128) ++reused; }   // #207: a chain pixel (alpha 0.98 .. 0.75) is a reused one, only a live pixel has alpha 1.0
        // #207 (coordinator 6523976264 item 1c): the 4 x 2 blocks (the SIMD8 dispatch unit of pass B) with >= 1 live pixel, and those whose 8 pixels are all live
        size_t blk_ground = 0, blk_any = 0, blk_full = 0;
        for (int by = 0; by + 1 < h; by += 2) for (int bx = 0; bx + 3 < w; bx += 4) {
            int nl = 0, ng = 0;
            for (int j = 0; j < 2; ++j) for (int i = 0; i < 4; ++i) { const uint8_t a = px[((size_t)(by + j) * w + bx + i) * 4 + 3]; if (a >= 254) ++nl; if (a >= 128) ++ng; }
            if (ng) ++blk_ground;
            if (nl) ++blk_any;
            if (nl == 8) ++blk_full;
        }
        lua_pushinteger(L, (lua_Integer)blk_ground); lua_setfield(L, -2, "blk_ground");
        lua_pushinteger(L, (lua_Integer)blk_any); lua_setfield(L, -2, "blk_any_live");
        lua_pushinteger(L, (lua_Integer)blk_full); lua_setfield(L, -2, "blk_full_live");
        lua_pushinteger(L, w); lua_setfield(L, -2, "w");
        lua_pushinteger(L, h); lua_setfield(L, -2, "h");
        lua_pushinteger(L, (lua_Integer)(live + reused)); lua_setfield(L, -2, "ground");
        lua_pushinteger(L, (lua_Integer)live); lua_setfield(L, -2, "live");
        lua_pushinteger(L, (lua_Integer)reused); lua_setfield(L, -2, "reused");
        lua_pushnumber(L, g_ru_chain_pushed); lua_setfield(L, -2, "chain_pushed");   // #207: ru_chain.x in force
    }
    return 1;
}

// ── md.granite_terrain_stats() -> {ready, tier_counts[kQuadtreeDepthLevels]} ─
// Ogre-quadtree cutover: no persistent per-tier instance count exists (no
// cache/tree state, TerrainQuadtree::SelectVisible recomputes purely from
// camera+frustum every frame -- see that class's own doc comment), so this
// keeps the SAME {ready, tier_counts[]} shape the GEOCLIPMAP-era API had
// (one entry per depth level, 1 if ready else 0) so existing scenario
// scripts (verify_terrain_repro.lua) don't need their own check rewritten.
// md.gpu_act_freq() -> the Intel GPU's actual clock in MHz (sysfs gt_act_freq_mhz, 0 when it cannot be read): the governor moves it with the load, so a pass's cost in cycles = ms x MHz (#207 H12 / coordinator 6511566625)
int l_md_gpu_act_freq(lua_State* L) {
    int mhz = 0;
    if (FILE* f = fopen("/sys/class/drm/card1/gt_act_freq_mhz", "r")) { if (fscanf(f, "%d", &mhz) != 1) mhz = 0; fclose(f); }
    lua_pushinteger(L, mhz);
    return 1;
}

int l_md_pvt_flush(lua_State*) { SceneRender::Get().terrain_shading_projected_.PvtFlush(); return 0; }   // measurement (#207 P4): forget every baked PVT tile

int l_md_granite_terrain_stats(lua_State* L) {
    lua_newtable(L);
    auto& sr = SceneRender::Get();
    lua_pushboolean(L, sr.terrain_granite_ready); lua_setfield(L, -2, "ready");
    lua_newtable(L);
    constexpr int kQuadtreeDepthLevels = 4; // max_depth(3) + 1, terrain_quadtree_'s own Init() arg
    for (int lvl = 0; lvl < kQuadtreeDepthLevels; ++lvl) {
        lua_pushinteger(L, sr.terrain_granite_ready ? 1 : 0);
        lua_rawseti(L, -2, lvl + 1);
    }
    lua_setfield(L, -2, "tier_counts");
    // terrain-perf-measure (2026-08-12): resolution-scaling test trap check
    // -- confirm the Variant A G-buffer actually resizes with the window
    // instead of silently staying at its init-time size (would flatten the
    // frame-time-vs-pixel-count slope for the shading resolve pass without
    // any error).
    lua_pushinteger(L, sr.terrain_shading_projected_.GBufferColor().Width());
    lua_setfield(L, -2, "gbuf_w");
    lua_pushinteger(L, sr.terrain_shading_projected_.GBufferColor().Height());
    lua_setfield(L, -2, "gbuf_h");
    // real per-frame draw-call count (set in npc_render_frame_prep.cpp
    // right after SelectVisible).
    lua_pushinteger(L, sr.terrain_quadtree_last_visible_count_);
    lua_setfield(L, -2, "node_count");
    lua_pushinteger(L, g_terrain_lod && sr.terrain_quadtree_renderer_.IsLodReady() ? sr.terrain_quadtree_renderer_.LodTrianglesPerNode() : sr.terrain_quadtree_renderer_.FilledTrianglesPerNode());
    lua_setfield(L, -2, "tris_per_node");
    lua_pushboolean(L, sr.terrain_quadtree_renderer_.LodFineCovers(sr.granite_abs_cam_[0], sr.granite_abs_cam_[2], 100.f));
    lua_setfield(L, -2, "lod_fine_covers_cam");
    lua_pushboolean(L, sr.terrain_quadtree_renderer_.LodWindowsIdle(sr.granite_abs_cam_[0], sr.granite_abs_cam_[2]));
    lua_setfield(L, -2, "lod_windows_idle");   // #207: no LOD window job running / pending and the camera inside the middle of the fine window (the settled state) // the LOD fine height window (read on a worker) holds the heights around the camera
    lua_newtable(L);
    for (int lv = 0; lv < 11; ++lv) { lua_pushinteger(L, g_terrain_lod_levels[lv]); lua_rawseti(L, -2, lv + 1); }
    lua_setfield(L, -2, "lod_levels");
    lua_pushinteger(L, sr.terrain_resolve_repeat_debug);
    lua_setfield(L, -2, "resolve_repeat_debug");
    lua_pushboolean(L, sr.terrain_shading_projected_.IsReady());
    lua_setfield(L, -2, "shading_ready");
    return 1;
}

// ── md.debug_open_map(zoom, pan_u, pan_v) ─────────────────────────────────────
// TEMPORARY (world map zoom/pan pin-clipping + player-marker fix
// verification) -- opens the map and sets its zoom/pan state directly so
// a --exec scenario can screenshot the SAME zoomed/panned view the user
// reported the pin/marker bugs in, without needing real mouse-scroll
// input. Remove after verification, same practice as this session's
// other one-off MD_*_DEBUG hooks.
int l_md_debug_open_map(lua_State* L) {
    float zoom  = (float)luaL_optnumber(L, 1, 1.0);
    float pan_u = (float)luaL_optnumber(L, 2, 0.5);
    float pan_v = (float)luaL_optnumber(L, 3, 0.5);
    auto& wm = WorldMapUI::Get();
    if (!wm.IsOpen()) wm.Toggle();
    wm.SetZoomPan(zoom, pan_u, pan_v);
    return 0;
}

