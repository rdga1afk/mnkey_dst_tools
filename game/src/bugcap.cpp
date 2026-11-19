#include "bugcap.h"
#include "game_common.h"
#include "stb_image_write.h"
#include "render/dynres3.h"
#include <monkey_dust/render/ground_slot.h>
#include "render/scene_render.h"
#include <atomic>
#include <csignal>
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
#include <vector>

extern bool g_opt_cmaa2, g_opt_ground_reuse, g_opt_force_upscale, g_dynres_on;
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
constexpr int kRing = 300;           // snapshot: the camera path of the last 5 s at 60 frames / s
constexpr const char* kPoseLog = "temp_/pose_log.tsv";
constexpr long kPoseLogMax = 10 * 1024 * 1024;
constexpr int kViews = 5;            // snapshot frames: 0 = the picture, 1..4 = md.set_debug_view modes
constexpr int kViewSettle = 20;      // full frames after a view change (ground reuse period 16 covers the screen)

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

void WriteSettings(const std::string& dir) {
    if (FILE* f = fopen((dir + "/settings.txt").c_str(), "w")) {
        fprintf(f, "build=%s\n", md_build_title);
        fprintf(f, "ground_reuse_period=%d\nground_reuse=%d\nground_reuse_cr=%d\nground_split=%d\ncmaa2=%d\nforce_upscale=%d\n", g_opt_ground_reuse_period, (int)g_opt_ground_reuse, g_opt_ground_reuse_cr, g_opt_ground_split, (int)g_opt_cmaa2, (int)g_opt_force_upscale);
        fprintf(f, "dynres_on=%d\ndynres_scale=%.3f\ndynres_down_frames=%d\ndynres_up_frames=%d\nresolution_scale=%.3f\nlighting_mode=%d\ntime_override=%d\ntime_hours=%.3f\nwindow=%dx%d\n",
                (int)g_dynres_on, g_dynres_scale, g_dynres_down, g_dynres_up, md_settings_get_resolution_scale(), g_kenshi_lighting_mode, (int)LightSystem::Get().time_override_enabled, LightSystem::Get().time_override_hours, window_get_width(), window_get_height());
        fclose(f);
    }
}

void WriteReplay(const std::string& dir, const Row* p, size_t n, unsigned frames) {
    // replay.lua: the camera path of the frames that reached the video (row N = video frame N), the settings of settings.txt
    if (FILE* f = fopen((dir + "/replay.lua").c_str(), "w")) {
        fprintf(f, "-- bug video %s: replays the camera path of its %u frames with the settings of settings.txt; run: tools/md_ctl run %s/replay.lua  (KENSHI_DIR set)\n", dir.c_str(), frames, dir.c_str());
        fprintf(f, "-- frame N of the video = pose N below; the poses are the ones the game rendered (world coordinates)\n");
        fprintf(f, "md.wait_ticks(100); md.set_time_of_day(%.3f); md.set_resolution_scale(%.3f)\n", LightSystem::Get().time_override_enabled ? LightSystem::Get().time_override_hours : 12.0f, md_settings_get_resolution_scale());
        fprintf(f, "for a = 1, 4 do md.place_window_on_largest_display(true); md.wait_ticks(100); md.render_frame(); md.wait_ticks(10) end\n");
        fprintf(f, "md.set_terrain_lod(true)\n");
        fprintf(f, "md.set_opt_ground_split(%d); md.set_opt_ground_reuse(%s); md.set_ground_reuse_period(%d); md.set_dyn_res(%s, %.3f)\n", g_opt_ground_split, g_opt_ground_reuse ? "true" : "false", g_opt_ground_reuse_period, g_dynres_on ? "true" : "false", g_dynres_scale);
        fprintf(f, "for i = 1, 1500 do md.wait_ticks(50); md.render_frame(); md.wait_ticks(5); if (md.granite_terrain_stats().tris_per_node or 0) ~= 512 then break end end\n");
        if (n) fprintf(f, "md.teleport_player(%.3f, %.3f); md.wait_ticks(300)\n", p[0].wx, p[0].wz);
        fprintf(f, "local P = {\n");
        for (size_t i = 0; i < n; ++i) fprintf(f, "  {%.4f, %.4f, %.4f, %.4f, %.4f},\n", p[i].wx, p[i].y, p[i].wz, p[i].yaw, -p[i].pitch);   // md.set_camera_pose_world takes pitch positive = looking down, the csv has elevation (down = negative)
        fprintf(f, "}\nif #P == 0 then md.log(\"replay: no frames\") md.quit(1) end\n");
        fprintf(f, "md.set_camera_pose_world(P[1][1], P[1][2], P[1][3], P[1][4], P[1][5]); md.wait_ticks(120)\n");
        fprintf(f, "for i = 1, 200 do md.render_frame(); md.wait_ticks(10); if md.granite_terrain_stats().lod_fine_covers_cam then break end end\n");
        fprintf(f, "for i = 1, #P do md.set_camera_pose_world(P[i][1], P[i][2], P[i][3], P[i][4], P[i][5]); md.render_frame(); md.wait_ticks(1); if i %% 10 == 0 or i == #P then md.screenshot(string.format(\"temp_/bugcap_replay/%%05d.png\", i)); md.render_frame(); md.wait_ticks(1) end end\n");
        fprintf(f, "md.log(\"bugcap replay: done\") md.quit(0)\n");
        fclose(f);
    }
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
    WriteSettings(g.dir);
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
    WriteReplay(g.dir, g.poses, g.pose_count, g.written);
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


// ── one-key snapshot (F9) and the pose log ───────────────────────────────────
namespace {
struct Snap {
    Row ring[kRing]; int head = 0, count = 0; unsigned frame = 0;
    double last_log = -1e9;
    int state = 0;                  // 0 idle, 1 running
    int view = 0, wait = 0; bool want = false;
    std::string dir;
} sn;

void LogPose(const Row& r) {
    struct stat st{}; if (stat(kPoseLog, &st) == 0 && st.st_size > kPoseLogMax) rename(kPoseLog, "temp_/pose_log.tsv.1");
    mkdir("temp_", 0755);
    FILE* f = fopen(kPoseLog, "a"); if (!f) return;
    if (ftell(f) == 0) fprintf(f, "wall_clock\tx_world\ty\tz_world\tyaw\tpitch\tdynres_scale\trender_size\tdynres_version\tbuild\n");
    time_t now = time(nullptr); struct tm* lt = localtime(&now); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", lt);
    fprintf(f, "%s\t%.3f\t%.3f\t%.3f\t%.2f\t%.2f\t%.3f\t%dx%d\t%s\t%s\n", ts, r.wx, r.y, r.wz, r.yaw, r.pitch, r.dynres, r.rw, r.rh, kDynresVersion, md_build_title);
    fclose(f);
}

void WriteLogTail(const std::string& dir) {   // md.sh tees the game output into temp_/game_last.log
    FILE* in = fopen("temp_/game_last.log", "rb"); FILE* out = fopen((dir + "/log_tail.txt").c_str(), "w");
    if (in && out) {
        fseek(in, 0, SEEK_END); long sz = ftell(in); const long take = sz > 65536 ? 65536 : sz;
        fseek(in, sz - take, SEEK_SET); std::string b((size_t)take, '\0'); const size_t n = fread(&b[0], 1, (size_t)take, in); b.resize(n);
        fwrite(b.data(), 1, b.size(), out);
    } else if (out) fprintf(out, "no temp_/game_last.log: start the game with ./md.sh\n");
    if (in) fclose(in); if (out) fclose(out);
}
}  // namespace

static std::atomic<bool> g_snap_signal{false};
static std::atomic<bool> g_video_signal{false};
static void OnUsr1(int) { g_snap_signal.store(true); }
static void OnUsr2(int) { g_video_signal.store(true); }   // SIGUSR2: the camera-button video, start / stop   // only sets the flag: the dump runs on the next frame (Note)

void Note(const MdCamera& cam, float dt, int rw, int rh) {
    static bool hooked = false; if (!hooked) { hooked = true; struct sigaction sa{}; sa.sa_handler = OnUsr1; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART; sigaction(SIGUSR1, &sa, nullptr); sa.sa_handler = OnUsr2; sigaction(SIGUSR2, &sa, nullptr); }
    if (g_no_dialogs) {   // scenario runs bypass the interactive loop (main.cpp): the camera button and F9 are polled here; they only start / stop a capture, never touch the camera or the scenario
        static bool prev_lmb = false, prev_f9 = false;
        float mx = 0.f, my = 0.f; const bool lmb = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK) != 0;
        const int sw = window_get_width(), sh = window_get_height();
        const int bx = sw - SceneRender::CAM_BTN_SZ - SceneRender::CAM_BTN_MARGIN, by = sh - SceneRender::CAM_BTN_SZ - SceneRender::CAM_BTN_MARGIN;
        if (lmb && !prev_lmb && (int)mx >= bx && (int)mx < bx + SceneRender::CAM_BTN_SZ && (int)my >= by && (int)my < by + SceneRender::CAM_BTN_SZ) g_video_signal.store(true);
        prev_lmb = lmb;
        const bool f9 = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F9];
        if (f9 && !prev_f9) g_snap_signal.store(true);
        prev_f9 = f9;
    }
    if (g_video_signal.exchange(false)) { if (Active()) Stop(); else Start(); }
    if (g_snap_signal.exchange(false)) { if (const char* d = SnapKey()) fprintf(stdout, "[bugcap] SIGUSR1 -> %s\n", d); }
    Row r{};
    Vec3 d = vec3_sub(cam.target, cam.pos);
    r.yaw = atan2f(d.x, d.z) * (180.f / 3.14159265f); r.pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z)) * (180.f / 3.14159265f);
    r.x = cam.pos.x; r.y = cam.pos.y; r.z = cam.pos.z; ToWorld(r.x, r.z, r.wx, r.wz);
    r.t = NowS(); r.dt = dt; r.rw = rw; r.rh = rh; r.dynres = g_dynres_cur; r.period = g_opt_ground_reuse_period; r.frame = sn.frame++;
    sn.ring[sn.head] = r; sn.head = (sn.head + 1) % kRing; if (sn.count < kRing) ++sn.count;
    if (r.t - sn.last_log >= 1.0) { sn.last_log = r.t; LogPose(r); }
    if (sn.state == 1) {   // the views: set the mode, wait for full frames, then one frame is read back
        if (sn.wait > 0) --sn.wait; else sn.want = true;
    }
}

const char* SnapKey() {
    if (sn.state == 1) return nullptr;
    mkdir("temp_", 0755); mkdir("temp_/bugcap", 0755);
    time_t now = time(nullptr); struct tm* lt = localtime(&now); char d[256];
    snprintf(d, sizeof d, "temp_/bugcap/snap_%04d%02d%02d_%02d%02d%02d", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
    if (mkdir(d, 0755) != 0) return nullptr;
    sn.dir = d;
    Row path[kRing]; int n = sn.count; for (int i = 0; i < n; ++i) path[i] = sn.ring[(sn.head - n + i + kRing) % kRing];
    for (int i = 0; i < n; ++i) path[i].t -= path[0].t;
    WriteSettings(sn.dir); WriteReplay(sn.dir, path, (size_t)n, (unsigned)n); WriteLogTail(sn.dir);
    if (FILE* f = fopen((sn.dir + "/pose.txt").c_str(), "w")) {
        if (n) { const Row& r = path[n - 1]; time_t t2 = time(nullptr); fprintf(f, "wall_clock=%ld\nx_world=%.3f y=%.3f z_world=%.3f yaw=%.2f pitch=%.2f\ndynres_scale=%.3f render=%dx%d %s\nbuild=%s\npath_frames=%d (replay.lua)\n", (long)t2, r.wx, r.y, r.wz, r.yaw, r.pitch, r.dynres, r.rw, r.rh, kDynresVersion, md_build_title, n); }
        fclose(f);
    }
    sn.state = 1; sn.view = 0; sn.wait = 2; sn.want = false; md::GroundLiveRtState().debug_view = 0;
    fprintf(stdout, "[bugcap] snapshot -> %s (pose, settings, log tail, replay.lua of the last %d frames; view PNGs follow)\n", d, n);
    return sn.dir.c_str();
}

bool SnapWants() { return sn.state == 1 && sn.want; }

bool SnapSubmit(md::GpuCommandBufferHandle cmd, md::GpuTextureHandle sc, uint32_t w, uint32_t h, SDL_GPUTextureFormat fmt) {
    sn.want = false;
    if (!sc || !w || !h) return false;
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTransferBufferCreateInfo tci{}; tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tci.size = w * h * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tci); if (!tb) return false;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{}; src.texture = sc; src.w = w; src.h = h; src.d = 1;
    SDL_GPUTextureTransferInfo dst{}; dst.transfer_buffer = tb; dst.pixels_per_row = w; dst.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = md::GpuDevice::Get().SubmitAndAcquireFence(cmd);
    md::GpuDevice::Get().NotifyCommandBufferSubmitted();
    if (fence) { SDL_WaitForGPUFences(dev, true, &fence, 1); SDL_ReleaseGPUFence(dev, fence); }
    if (void* m = SDL_MapGPUTransferBuffer(dev, tb, false)) {
        const bool bgra = fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
        std::vector<uint8_t> rgb((size_t)w * h * 3); const uint8_t* p = (const uint8_t*)m;
        for (size_t i = 0; i < (size_t)w * h; ++i) { rgb[i * 3] = p[i * 4 + (bgra ? 2 : 0)]; rgb[i * 3 + 1] = p[i * 4 + 1]; rgb[i * 3 + 2] = p[i * 4 + (bgra ? 0 : 2)]; }
        char path[320]; snprintf(path, sizeof path, "%s/view%d.png", sn.dir.c_str(), sn.view);
        stbi_write_png(path, (int)w, (int)h, 3, rgb.data(), (int)w * 3);
        SDL_UnmapGPUTransferBuffer(dev, tb);
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    if (++sn.view >= kViews) { sn.state = 0; md::GroundLiveRtState().debug_view = 0; fprintf(stdout, "[bugcap] snapshot done: %s\n", sn.dir.c_str()); }
    else { md::GroundLiveRtState().debug_view = sn.view; sn.wait = kViewSettle; }
    return true;
}

}
