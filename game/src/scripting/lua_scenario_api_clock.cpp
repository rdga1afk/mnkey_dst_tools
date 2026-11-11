// Measurement helpers for the frame-budget work (FRAME_AUDIT M2.1c): the cost of the GPU clock itself, GPU frequency, process memory,
// and a back-to-back render burst that measures the real frame interval. None of them changes a default.
#include "lua_scenario_api_internal.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// render burst (consumed by autonomy/scenario_driver.cpp): n back-to-back RenderFrame calls, per-call wall time in ms
int g_scenario_burst_n = 0;
bool g_scenario_burst_paced = false;
std::vector<float> g_scenario_burst_ms;
std::vector<int> g_scenario_burst_freq;
std::vector<float> g_scenario_burst_cpu_ms;
std::vector<float> g_scenario_path; int g_scenario_path_tick = 6; float g_scenario_path_dt = 1.f / 60.f;
std::vector<float> g_scenario_burst_dynres, g_scenario_burst_stream_ms; std::vector<int> g_scenario_burst_stream;

namespace {

double MsSince(uint64_t t0) {
    return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

// One submit -> fence measurement, the same clock GpuDevice::Submit() uses with SyncTiming on.
// kind 0: empty command buffer. kind 1: clear of a 1920x1080 RGBA8 render target. kind 2: acquire swapchain + clear it (present on submit).
// Returns the submit->fence ms; *acquire_ms gets the time spent in the swapchain acquire (kind 2), else 0.
double ProbeOnce(int kind, SDL_GPUTexture* rt, double* acquire_ms) {
    auto& gd = md::GpuDevice::Get();
    SDL_GPUDevice* dev = gd.SDLDevice();
    *acquire_ms = 0;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) return -1;
    SDL_GPUTexture* target = nullptr;
    if (kind == 1) {
        target = rt;
    } else if (kind == 2) {
        uint64_t ta = SDL_GetPerformanceCounter();
        uint32_t w = 0, h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, gd.Window(), &target, &w, &h)) target = nullptr;
        *acquire_ms = MsSince(ta);
    }
    if (target) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = target;
        ct.clear_color = SDL_FColor{0.1f, 0.2f, 0.3f, 1.0f};
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        SDL_EndGPURenderPass(rp);
    }
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (f) { SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f); }
    return MsSince(t0);
}

} // namespace

// ── md.clock_probe(kind, n) -> {submit_fence_ms...}, {acquire_ms...} ──────────
// Lower bound of what the submit->fence clock can show. kind: 0 empty submit, 1 clear one 1920x1080 RT, 2 acquire + clear + present.
int l_md_clock_probe(lua_State* L) {
    const int kind = (int)luaL_checkinteger(L, 1);
    const int n = (int)luaL_checkinteger(L, 2);
    lua_newtable(L);
    lua_newtable(L);
    auto& gd = md::GpuDevice::Get();
    SDL_GPUTexture* rt = nullptr;
    if (kind == 1) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ti.width = 1920; ti.height = 1080; ti.layer_count_or_depth = 1; ti.num_levels = 1;
        rt = SDL_CreateGPUTexture(gd.SDLDevice(), &ti);
    }
    gd.WaitForIdle();
    for (int i = 1; i <= n; ++i) {
        double acq = 0;
        double ms = ProbeOnce(kind, rt, &acq);
        lua_pushnumber(L, ms); lua_rawseti(L, -3, i);
        lua_pushnumber(L, acq); lua_rawseti(L, -2, i);
    }
    if (rt) SDL_ReleaseGPUTexture(gd.SDLDevice(), rt);
    return 2;
}

// ── md.place_window_on_largest_display([fullscreen]) -> w, h ──
// The game window opens on whichever monitor the pointer is on (laptop panel 1366x768 or the external 1920x1080), so a measurement
// run's resolution depended on desktop state. This moves the window to the display with the largest area, maximizes it (or, with
// fullscreen=true, makes it borderless fullscreen at the display's native size), waits for the WM, and refreshes the window-size cache
// that scenario rendering reads (window_begin_frame is not called in scenario mode).
int l_md_place_window_on_largest_display(lua_State* L) {
    SDL_Window* win = _wnd::ptr();
    const bool fullscreen = lua_toboolean(L, 1) != 0;
    const bool smallest = lua_toboolean(L, 2) != 0;   // measurement option (A4): the laptop panel (1366x768) instead of the external monitor
    int cnt = 0;
    SDL_DisplayID* ds = SDL_GetDisplays(&cnt);
    SDL_Rect best{0, 0, 0, 0};
    SDL_DisplayID best_id = 0;
    for (int i = 0; i < cnt; ++i) {
        SDL_Rect r{};
        const bool ok = fullscreen ? SDL_GetDisplayBounds(ds[i], &r) : SDL_GetDisplayUsableBounds(ds[i], &r);
        if (ok && (best.w == 0 || (smallest ? (int64_t)r.w * r.h < (int64_t)best.w * best.h : (int64_t)r.w * r.h > (int64_t)best.w * best.h))) { best = r; best_id = ds[i]; }
    }
    SDL_free(ds);
    int w = 0, h = 0;
    if (win && best.w > 0) {
        // Few, ordered window operations: the earlier sequence (fullscreen off, restore, position, size, fullscreen on) made SDL's X11 backend
        // recurse without end (SDL_SyncWindow <-> event handling, 1600+ frames, seen in gdb) and either hang or overflow the stack (#136).
        if (fullscreen) {
            if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) { // fullscreen on the wrong display: leave it first, waiting for the WM between steps
                SDL_SetWindowFullscreen(win, false);
                for (int i = 0; i < 50 && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN); ++i) { SDL_PumpEvents(); SDL_Delay(20); }
            }
            if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) { // a maximized window ignores position/size requests
                SDL_RestoreWindow(win);
                for (int i = 0; i < 50 && (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED); ++i) { SDL_PumpEvents(); SDL_Delay(20); }
            }
            if (SDL_GetDisplayForWindow(win) != best_id || !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)) {
                SDL_SetWindowPosition(win, best.x, best.y); // fullscreen goes to the display the window is on: put it there, at that size, first
                SDL_SetWindowSize(win, best.w, best.h);
                for (int i = 0; i < 25; ++i) { SDL_PumpEvents(); SDL_Delay(20); if (SDL_GetDisplayForWindow(win) == best_id) break; }
                SDL_SetWindowFullscreenMode(win, nullptr); // nullptr = borderless fullscreen at the desktop resolution
                SDL_SetWindowFullscreen(win, true);
            }
        } else {
            SDL_SetWindowFullscreen(win, false);
            SDL_SetWindowPosition(win, best.x, best.y);
            SDL_SetWindowSize(win, best.w, best.h);
            SDL_MaximizeWindow(win);
        }
        for (int i = 0; i < 100; ++i) { // up to ~2 s for the WM to apply it
            SDL_PumpEvents();
            SDL_Delay(20);
            SDL_GetWindowSize(win, &w, &h);
            if (w >= best.w && h >= (fullscreen ? best.h : best.h - 40)) break; // fullscreen: wait for the full display height, not the maximized one
        }
        SDL_GetWindowSize(win, &w, &h);
        _wnd::width() = w;
        _wnd::height() = h;
    }
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

// ── md.gpu_freq_mhz() -> current Intel GT frequency (sysfs gt_cur_freq_mhz), -1 if unreadable ──
int l_md_gpu_freq_mhz(lua_State* L) {
    int v = -1;
    if (FILE* f = fopen("/sys/class/drm/card1/gt_cur_freq_mhz", "r")) {
        if (fscanf(f, "%d", &v) != 1) v = -1;
        fclose(f);
    }
    lua_pushinteger(L, v);
    return 1;
}

// ── md.pkg_temp_c() -> x86_pkg_temp thermal zone in degrees C, -1 if unreadable ──
int l_md_pkg_temp_c(lua_State* L) {
    double c = -1;
    for (int z = 0; z < 16 && c < 0; ++z) {
        char path[96], type[32] = {0};
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(type, sizeof(type), f)) type[0] = 0;
        fclose(f);
        if (strncmp(type, "x86_pkg_temp", 12) != 0) continue;
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
        int v = 0;
        if ((f = fopen(path, "r"))) { if (fscanf(f, "%d", &v) == 1) c = v / 1000.0; fclose(f); }
    }
    lua_pushnumber(L, c);
    return 1;
}

// ── md.proc_mem_kb() -> VmRSS, VmHWM (kB) from /proc/self/status ──
int l_md_proc_mem_kb(lua_State* L) {
    long rss = -1, hwm = -1;
    if (FILE* f = fopen("/proc/self/status", "r")) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "VmRSS:", 6)) rss = atol(line + 6);
            else if (!strncmp(line, "VmHWM:", 6)) hwm = atol(line + 6);
        }
        fclose(f);
    }
    lua_pushinteger(L, rss);
    lua_pushinteger(L, hwm);
    return 2;
}

// ── md.render_burst(n) / md.render_burst_ms() ──
// render_burst(n) asks the scenario driver for n back-to-back frames (no logic ticks, no sleeping, no capture) during the next
// wait_ticks; render_burst_ms() returns the per-frame wall time (ms) of the last burst. With SyncTiming off, SubmitFrame pipelines
// frames, so the wall time per call in a long burst is the real frame interval (what "60 FPS" means).
int l_md_render_burst(lua_State* L) {
    g_scenario_burst_n = (int)luaL_checkinteger(L, 1);
    return 0;
}
// md.set_burst_paced(on): the bursts wait for the frame cap of the real game loop (power runs); off = back-to-back frames (measuring)
int l_md_set_burst_paced(lua_State* L) { g_scenario_burst_paced = lua_toboolean(L, 1) != 0; return 0; }
int l_md_render_burst_freq(lua_State* L) {
    lua_newtable(L);
    for (size_t i = 0; i < g_scenario_burst_freq.size(); ++i) { lua_pushinteger(L, g_scenario_burst_freq[i]); lua_rawseti(L, -2, (int)i + 1); }
    return 1;
}
// md.render_burst_path(P, tick_every, dt): P = array of {x, z, height_above_ground, yaw_deg, pitch_deg} (world metres), one entry per frame; the burst is run by the scenario driver
int l_md_render_burst_path(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    g_scenario_path.clear();
    const int n = (int)lua_rawlen(L, 1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        for (int k = 1; k <= 5; ++k) { lua_rawgeti(L, -k, k); }
        for (int k = 5; k >= 1; --k) g_scenario_path.push_back((float)lua_tonumber(L, -k));
        lua_pop(L, 6);
    }
    g_scenario_path_tick = (int)luaL_optinteger(L, 2, 6);
    g_scenario_path_dt = (float)luaL_optnumber(L, 3, 1.0 / 60.0);
    g_scenario_burst_n = n;
    return 0;
}
static void PushF(lua_State* L, const std::vector<float>& v) { lua_newtable(L); for (size_t i = 0; i < v.size(); ++i) { lua_pushnumber(L, v[i]); lua_rawseti(L, -2, (int)i + 1); } }
int l_md_render_burst_dynres(lua_State* L) { PushF(L, g_scenario_burst_dynres); return 1; }
int l_md_render_burst_stream_ms(lua_State* L) { PushF(L, g_scenario_burst_stream_ms); return 1; }
int l_md_render_burst_stream(lua_State* L) { lua_newtable(L); for (size_t i = 0; i < g_scenario_burst_stream.size(); ++i) { lua_pushinteger(L, g_scenario_burst_stream[i]); lua_rawseti(L, -2, (int)i + 1); } return 1; }
int l_md_render_burst_cpu_ms(lua_State* L) {
    lua_newtable(L);
    for (size_t i = 0; i < g_scenario_burst_cpu_ms.size(); ++i) { lua_pushnumber(L, g_scenario_burst_cpu_ms[i]); lua_rawseti(L, -2, (int)i + 1); }
    return 1;
}
int l_md_render_burst_ms(lua_State* L) {
    lua_newtable(L);
    for (size_t i = 0; i < g_scenario_burst_ms.size(); ++i) { lua_pushnumber(L, g_scenario_burst_ms[i]); lua_rawseti(L, -2, (int)i + 1); }
    return 1;
}
