#include "bugcap.h"
#include "game_common.h"
#include "render/scene_render.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>

extern bool g_opt_fxaa, g_opt_ground_reuse, g_opt_force_upscale, g_dynres_on;
extern int g_opt_ground_reuse_period, g_opt_ground_reuse_cr, g_opt_ground_split, g_dynres_down, g_dynres_up;
extern float g_dynres_scale, g_dynres_cur;
extern int g_kenshi_lighting_mode;
extern const char* const md_build_title;
float md_settings_get_resolution_scale();

namespace bugcap {
namespace {

constexpr int kSlots = 3;           // frames in flight on the GPU side
constexpr int kPool = 12;           // frames waiting for the encoder
constexpr double kMaxSeconds = 60.0;
constexpr size_t kMaxPoses = 60 * 60 + 600;

struct Row { unsigned frame; double t; float dt, x, y, z, wx, wz, yaw, pitch, speed, dynres; int period, phase, rw, rh; };

struct Slot { SDL_GPUTransferBuffer* tb = nullptr; SDL_GPUFence* fence = nullptr; Row row; bool used = false; };

struct Rec {
    bool active = false, started = false;
    std::string dir;
    uint32_t w = 0, h = 0;
    SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_INVALID;
    Slot slot[kSlots];
    unsigned submitted = 0;          // frames handed to the GPU copy
    unsigned written = 0;            // frames the encoder took (= rows of frames.csv)
    unsigned dropped = 0;
    double t0 = 0;
    FILE* ff = nullptr;              // pipe to ffmpeg
    FILE* csv = nullptr;
    FILE* drop = nullptr;
    // encoder side
    uint8_t* pool[kPool] = {};
    Row pool_row[kPool];
    int q_head = 0, q_tail = 0, q_count = 0;
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
    bool quit = false;
    // the path for replay.lua
    Row* poses = nullptr;
    size_t pose_count = 0;
    float px = 0, py = 0, pz = 0;
} g;

double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void WriterThread() {
    for (;;) {
        int i;
        {
            std::unique_lock<std::mutex> lk(g.m);
            g.cv.wait(lk, [] { return g.q_count > 0 || g.quit; });
            if (g.q_count == 0 && g.quit) return;
            i = g.q_head; g.q_head = (g.q_head + 1) % kPool;
        }
        const Row& r = g.pool_row[i];
        fwrite(g.pool[i], 1, (size_t)g.w * g.h * 4, g.ff);
        fprintf(g.csv, "%u,%.4f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%d,%d,%dx%d\n", g.written, r.t, r.dt, r.x, r.y, r.z, r.wx, r.wz, r.yaw, r.pitch, r.speed, r.dynres, r.period, r.phase, r.rw, r.rh);   // frame number = position in the video
        {
            std::lock_guard<std::mutex> lk(g.m);
            --g.q_count; ++g.written;
        }
        g.cv.notify_all();
    }
}

bool BeginEncoder(uint32_t w, uint32_t h, SDL_GPUTextureFormat fmt) {
    g.w = w; g.h = h; g.fmt = fmt;
    const char* pix = fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB ? "bgra" : "rgba";
    char cmd[768];
    snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt %s -s %ux%u -framerate 60 -i - -c:v libx264 -qp 0 -preset ultrafast -pix_fmt yuv444p '%s/video.mkv' 2>'%s/ffmpeg.log'", pix, w, h, g.dir.c_str(), g.dir.c_str());
    g.ff = popen(cmd, "w");
    if (!g.ff) return false;
    g.csv = fopen((g.dir + "/frames.csv").c_str(), "w");
    g.drop = fopen((g.dir + "/dropped.txt").c_str(), "w");
    if (!g.csv || !g.drop) return false;
    fprintf(g.csv, "frame,time_s,dt,x_local,y,z_local,x_world,z_world,yaw,pitch,speed_mps,dynres_scale,reuse_period,reuse_phase,render_size\n");
    for (int i = 0; i < kPool; ++i) g.pool[i] = (uint8_t*)malloc((size_t)w * h * 4);   // once per recording, not per frame
    g.quit = false; g.q_head = g.q_tail = g.q_count = 0;
    g.th = std::thread(WriterThread);
    return true;
}

void ToWorld(float lx, float lz, float& wx, float& wz) {
    auto& sr = SceneRender::Get();
    wx = lx - sr.tnoff_x + (float)sr.zone_ox * CHUNK_SIZE;
    wz = lz - sr.tnoff_z + (float)sr.zone_oz * CHUNK_SIZE;
}

// a slot whose copy has finished: its pixels go to the encoder (or are dropped when the encoder is behind)
void Harvest(Slot& s) {
    if (!s.used) return;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    void* m = SDL_MapGPUTransferBuffer(dev, s.tb, false);
    if (m) {
        std::unique_lock<std::mutex> lk(g.m);
        if (g.q_count < kPool) {
            const int i = g.q_tail; g.q_tail = (g.q_tail + 1) % kPool; ++g.q_count;
            lk.unlock();
            memcpy(g.pool[i], m, (size_t)g.w * g.h * 4);
            g.pool_row[i] = s.row;
            if (g.pose_count < kMaxPoses) g.poses[g.pose_count++] = s.row;   // pose N = video frame N
            lk.lock();
            g.cv.notify_all();
        } else {
            fprintf(g.drop, "%u t=%.4f\n", s.row.frame, s.row.t);
            ++g.dropped;
        }
        SDL_UnmapGPUTransferBuffer(dev, s.tb);
    }
    s.used = false;
}

} // namespace

bool Start() {
    if (g.active) return true;
    mkdir("temp_", 0755); mkdir("temp_/bugcap", 0755);
    time_t now = time(nullptr); struct tm* lt = localtime(&now);
    char d[256];
    snprintf(d, sizeof d, "temp_/bugcap/%04d%02d%02d_%02d%02d%02d", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
    if (mkdir(d, 0755) != 0) { fprintf(stderr, "[bugcap] cannot create %s\n", d); return false; }
    g.dir = d; g.started = false; g.submitted = g.written = g.dropped = 0; g.pose_count = 0;
    if (!g.poses) g.poses = (Row*)malloc(sizeof(Row) * kMaxPoses);
    if (FILE* f = fopen((g.dir + "/settings.txt").c_str(), "w")) {
        fprintf(f, "build=%s\n", md_build_title);
        fprintf(f, "ground_reuse_period=%d\nground_reuse=%d\nground_reuse_cr=%d\nground_split=%d\nfxaa=%d\nforce_upscale=%d\n", g_opt_ground_reuse_period, (int)g_opt_ground_reuse, g_opt_ground_reuse_cr, g_opt_ground_split, (int)g_opt_fxaa, (int)g_opt_force_upscale);
        fprintf(f, "dynres_on=%d\ndynres_scale=%.3f\ndynres_down_frames=%d\ndynres_up_frames=%d\nresolution_scale=%.3f\nlighting_mode=%d\ntime_override=%d\ntime_hours=%.3f\nwindow=%dx%d\n",
                (int)g_dynres_on, g_dynres_scale, g_dynres_down, g_dynres_up, md_settings_get_resolution_scale(), g_kenshi_lighting_mode, (int)LightSystem::Get().time_override_enabled, LightSystem::Get().time_override_hours, window_get_width(), window_get_height());
        fclose(f);
    }
    g.t0 = NowS();
    g.active = true;
    fprintf(stdout, "[bugcap] recording -> %s (stops at %.0f s)\n", d, kMaxSeconds);
    return true;
}

bool Active() { return g.active; }

void Stop() {
    if (!g.active) return;
    g.active = false;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    for (Slot& s : g.slot) {
        if (s.used) { if (s.fence) { SDL_WaitForGPUFences(dev, true, &s.fence, 1); SDL_ReleaseGPUFence(dev, s.fence); s.fence = nullptr; } Harvest(s); }
        if (s.tb) { SDL_ReleaseGPUTransferBuffer(dev, s.tb); s.tb = nullptr; }
    }
    if (g.started) {
        { std::lock_guard<std::mutex> lk(g.m); g.quit = true; }
        g.cv.notify_all();
        if (g.th.joinable()) g.th.join();
        if (g.ff) { pclose(g.ff); g.ff = nullptr; }
        if (g.csv) { fclose(g.csv); g.csv = nullptr; }
        if (g.drop) { fclose(g.drop); g.drop = nullptr; }
        for (uint8_t*& p : g.pool) { free(p); p = nullptr; }
    }
    // replay.lua: the camera path of the frames that reached the video (row N = video frame N), the settings of settings.txt
    if (FILE* f = fopen((g.dir + "/replay.lua").c_str(), "w")) {
        fprintf(f, "-- bug video %s: replays the camera path of its %u frames with the settings of settings.txt; run: tools/md_ctl run %s/replay.lua  (KENSHI_DIR set)\n", g.dir.c_str(), g.written, g.dir.c_str());
        fprintf(f, "-- frame N of the video = pose N below; the poses are the ones the game rendered (world coordinates)\n");
        fprintf(f, "md.wait_ticks(100); md.set_time_of_day(%.3f); md.set_resolution_scale(%.3f)\n", LightSystem::Get().time_override_enabled ? LightSystem::Get().time_override_hours : 12.0f, md_settings_get_resolution_scale());
        fprintf(f, "for a = 1, 4 do md.place_window_on_largest_display(true); md.wait_ticks(100); md.render_frame(); md.wait_ticks(10) end\n");
        fprintf(f, "md.set_terrain_lod(true)\n");
        fprintf(f, "md.set_opt_ground_split(%d); md.set_opt_ground_reuse(%s); md.set_ground_reuse_period(%d); md.set_dyn_res(%s, %.3f)\n", g_opt_ground_split, g_opt_ground_reuse ? "true" : "false", g_opt_ground_reuse_period, g_dynres_on ? "true" : "false", g_dynres_scale);
        fprintf(f, "for i = 1, 1500 do md.wait_ticks(50); md.render_frame(); md.wait_ticks(5); if (md.granite_terrain_stats().tris_per_node or 0) ~= 512 then break end end\n");
        if (g.pose_count) fprintf(f, "md.teleport_player(%.3f, %.3f); md.wait_ticks(300)\n", g.poses[0].wx, g.poses[0].wz);
        fprintf(f, "local P = {\n");
        for (size_t i = 0; i < g.pose_count; ++i) fprintf(f, "  {%.4f, %.4f, %.4f, %.4f, %.4f},\n", g.poses[i].wx, g.poses[i].y, g.poses[i].wz, g.poses[i].yaw, -g.poses[i].pitch);   // md.set_camera_pose_world takes pitch positive = looking down, the csv has elevation (down = negative)
        fprintf(f, "}\nif #P == 0 then md.log(\"replay: no frames\") md.quit(1) end\n");
        fprintf(f, "md.set_camera_pose_world(P[1][1], P[1][2], P[1][3], P[1][4], P[1][5]); md.wait_ticks(120)\n");
        fprintf(f, "for i = 1, 200 do md.render_frame(); md.wait_ticks(10); if md.granite_terrain_stats().lod_fine_covers_cam then break end end\n");
        fprintf(f, "for i = 1, #P do md.set_camera_pose_world(P[i][1], P[i][2], P[i][3], P[i][4], P[i][5]); md.render_frame(); md.wait_ticks(1); if i %% 10 == 0 or i == #P then md.screenshot(string.format(\"temp_/bugcap_replay/%%05d.png\", i)); md.render_frame(); md.wait_ticks(1) end end\n");
        fprintf(f, "md.log(\"bugcap replay: done\") md.quit(0)\n");
        fclose(f);
    }
    fprintf(stdout, "[bugcap] stopped: %u frames in video.mkv, %u dropped, %.1f s -> %s\n", g.written, g.dropped, NowS() - g.t0, g.dir.c_str());
}

bool SubmitFrame(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle sc, uint32_t w, uint32_t h, SDL_GPUTextureFormat fmt, const MdCamera& cam, float dt, int rw, int rh) {
    if (!g.active || !sc || !w || !h) return false;
    if (NowS() - g.t0 > kMaxSeconds) { Stop(); return false; }
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (!g.started) {
        if (!BeginEncoder(w, h, fmt)) { fprintf(stderr, "[bugcap] ffmpeg not available: no video\n"); g.active = false; return false; }
        g.started = true;
    }
    if (w != g.w || h != g.h) return false;      // window resized during the recording: those frames are skipped
    // harvest the copies that finished
    for (Slot& s : g.slot) if (s.used && s.fence && SDL_QueryGPUFence(dev, s.fence)) { SDL_ReleaseGPUFence(dev, s.fence); s.fence = nullptr; Harvest(s); }
    Slot* free_slot = nullptr;
    for (Slot& s : g.slot) if (!s.used) { free_slot = &s; break; }
    if (!free_slot) { Slot& s = g.slot[g.submitted % kSlots]; SDL_WaitForGPUFences(dev, true, &s.fence, 1); SDL_ReleaseGPUFence(dev, s.fence); s.fence = nullptr; Harvest(s); free_slot = &s; }
    Slot& s = *free_slot;
    if (!s.tb) { SDL_GPUTransferBufferCreateInfo tci{}; tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tci.size = w * h * 4; s.tb = SDL_CreateGPUTransferBuffer(dev, &tci); }
    if (!s.tb) return false;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{}; src.texture = sc; src.w = w; src.h = h; src.d = 1;
    SDL_GPUTextureTransferInfo dst{}; dst.transfer_buffer = s.tb; dst.pixels_per_row = w; dst.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    s.fence = md::GpuDevice::Get().SubmitAndAcquireFence(cmd);
    md::GpuDevice::Get().NotifyCommandBufferSubmitted();
    // the row of this frame (the camera is the one this frame was drawn with)
    Row r{};
    Vec3 d = vec3_sub(cam.target, cam.pos);
    r.yaw = atan2f(d.x, d.z) * (180.f / 3.14159265f); r.pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z)) * (180.f / 3.14159265f);
    r.x = cam.pos.x; r.y = cam.pos.y; r.z = cam.pos.z; ToWorld(r.x, r.z, r.wx, r.wz);
    r.t = NowS() - g.t0; r.dt = dt; r.rw = rw; r.rh = rh; r.dynres = g_dynres_cur; r.period = g_opt_ground_reuse_period;
    r.phase = (int)(SceneRender::Get().gr_frame_ & (uint32_t)(g_opt_ground_reuse_period - 1));
    if (g.submitted) r.speed = dt > 0.f ? sqrtf((r.x - g.px) * (r.x - g.px) + (r.y - g.py) * (r.y - g.py) + (r.z - g.pz) * (r.z - g.pz)) / dt : 0.f;
    g.px = r.x; g.py = r.y; g.pz = r.z;
    r.frame = g.submitted;
    s.row = r; s.used = true; ++g.submitted;
    return s.fence != nullptr;
}

}
