#include "horizon_cull.h"
#include <monkey_dust/world/terrain_gen.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
namespace md {
namespace {
constexpr int kBins = 360, kSteps = 20, kBinsSpread = 3; constexpr float kD0 = 4.f, kD1 = 4300.f, kMargin = 0.02f, kMinDist = 8.f, kMaxDrift = 20.f, kRebuildDist = 2.f; constexpr int kRebuildFrames = 8;
constexpr float kTwoPi = 6.2831853f; constexpr float kMaxHeight = 80.f;
struct Table { float cam[3] = {}; float h[kBins][kSteps]; bool valid = false; };
Table g_tab[2]; int g_pub = -1;                      // published index (guarded by g_mu), -1 = none yet
std::mutex& g_mu = *new std::mutex; std::condition_variable& g_cv = *new std::condition_variable;   // never destroyed: the detached worker may still wait on them at process exit
bool g_req = false, g_quit = false; float g_req_cam[3] = {}; std::thread g_worker; bool g_started = false;
std::atomic<bool> g_on{getenv("MD_HORIZON_CULL") && atoi(getenv("MD_HORIZON_CULL")) == 1};   // default OFF: with it ON the perf gate refuses (unstable GPU frequency, max/min > 1.15: the lighter load downclocks the GT), see PR #164; MD_HORIZON_CULL=1 / md.set_horizon_cull(true) switch it on
HorizonCullStats g_stats; std::mutex& g_stats_mu = *new std::mutex; int g_frames_since = 1000; bool g_busy = false;
float g_edge[kSteps]; bool g_edge_init = false;
void InitEdges() { if (g_edge_init) return; for (int k = 0; k < kSteps; ++k) g_edge[k] = kD0 * std::pow(kD1 / kD0, float(k + 1) / kSteps); g_edge_init = true; }
void Build(Table& t, const float cam[3]) {
    t.cam[0] = cam[0]; t.cam[1] = cam[1]; t.cam[2] = cam[2];
    for (int b = 0; b < kBins; ++b) {
        const float a = (b + 0.5f) * kTwoPi / kBins, ux = std::cos(a), uz = std::sin(a);
        float mx = -1e9f; int k = 0;
        for (float d = 2.f; d < kD1 && k < kSteps; d += 1.5f + 0.02f * d) {
            const float h = TerrainAtlas_SampleWorld(cam[0] + ux * d, cam[2] + uz * d); mx = std::max(mx, (h - cam[1]) / d);
            while (k < kSteps && d >= g_edge[k]) t.h[b][k++] = mx;
        }
        while (k < kSteps) t.h[b][k++] = mx;
    }
    t.valid = true;
}
void Worker() {
    for (;;) {
        float cam[3];
        { std::unique_lock<std::mutex> lk(g_mu); g_cv.wait(lk, [] { return g_req || g_quit; }); if (g_quit) return; cam[0] = g_req_cam[0]; cam[1] = g_req_cam[1]; cam[2] = g_req_cam[2]; g_req = false; }
        const int back = (g_pub == 0) ? 1 : 0;   // g_pub only changes in this thread after the first build; the reader copies under the lock
        const auto t0 = std::chrono::steady_clock::now(); Build(g_tab[back], cam);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        { std::lock_guard<std::mutex> lk(g_mu); g_pub = back; g_busy = false; }
        { std::lock_guard<std::mutex> lk(g_stats_mu); g_stats.build_ms = ms; ++g_stats.builds; }
    }
}
}   // namespace
bool HorizonCullEnabled() { return g_on.load(); }
void HorizonCullSet(bool on) { g_on = on; }
HorizonCullStats HorizonCullGetStats() { std::lock_guard<std::mutex> lk(g_stats_mu); return g_stats; }
int HorizonCullFilter(const float cam[3], TerrainLodNode* nodes, int n) {
    if (!g_on.load()) return n;
    const auto t0 = std::chrono::steady_clock::now();
    InitEdges();
    if (cam[1] - TerrainAtlas_SampleWorld(cam[0], cam[2]) > kMaxHeight) return n;   // high above the ground the terrain hides almost nothing (high150: 2 % rejected): skip the filter and the table rebuild, they would only cost (p99 +2-4 ms measured)
    if (!g_started) { g_started = true; g_worker = std::thread(Worker); g_worker.detach(); }
    // snapshot of the published table (the worker writes only the other buffer; the reader uses its buffer without the lock for the duration of this call: the worker needs a new request to touch it, which only this function makes)
    int pub; { std::lock_guard<std::mutex> lk(g_mu); pub = g_pub; }
    ++g_frames_since;
    bool want = pub < 0;
    if (!want) { const Table& t = g_tab[pub]; const float dx = cam[0] - t.cam[0], dy = cam[1] - t.cam[1], dz = cam[2] - t.cam[2]; want = (dx * dx + dy * dy + dz * dz > kRebuildDist * kRebuildDist) || g_frames_since >= kRebuildFrames; }
    if (want) { std::lock_guard<std::mutex> lk(g_mu); if (!g_busy) { g_busy = true; g_req = true; g_req_cam[0] = cam[0]; g_req_cam[1] = cam[1]; g_req_cam[2] = cam[2]; g_frames_since = 0; g_cv.notify_one(); } }
    int out = n;
    if (pub >= 0) {
        const Table& t = g_tab[pub]; const float ddx = cam[0] - t.cam[0], ddy = cam[1] - t.cam[1], ddz = cam[2] - t.cam[2], drift = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
        if (drift <= kMaxDrift) {
            int w = 0;
            for (int i = 0; i < n; ++i) {
                const TerrainLodNode& nd = nodes[i]; bool rej = false;
                const float x0 = nd.origin_x - t.cam[0], x1 = x0 + nd.size, z0 = nd.origin_z - t.cam[2], z1 = z0 + nd.size;
                const float ndx = (x0 > 0 ? x0 : (x1 < 0 ? x1 : 0.f)), ndz = (z0 > 0 ? z0 : (z1 < 0 ? z1 : 0.f)), dmin = std::sqrt(ndx * ndx + ndz * ndz) - drift;
                if (dmin > kMinDist && g_edge[0] <= dmin * 0.985f) {
                    const float fdx = std::max(std::fabs(x0), std::fabs(x1)), fdz = std::max(std::fabs(z0), std::fabs(z1)), dmax = std::sqrt(fdx * fdx + fdz * fdz) + drift;
                    const float num = nd.y_max - t.cam[1] + drift, tmax = num >= 0 ? num / dmin : (nd.y_max - t.cam[1] - drift) / dmax;
                    const float cxs[4] = {x0, x1, x0, x1}, czs[4] = {z0, z0, z1, z1}; const float a0 = std::atan2(z0, x0); float amin = 1e9f, amax = -1e9f;
                    for (int c = 0; c < 4; ++c) { float da = std::atan2(czs[c], cxs[c]) - a0; while (da > 3.14159265f) da -= kTwoPi / 2; while (da < -3.14159265f) da += kTwoPi / 2; amin = std::min(amin, da); amax = std::max(amax, da); }
                    if (amax - amin < 3.0f) {
                        int k = 0; while (k < kSteps - 1 && g_edge[k + 1] <= dmin * 0.985f) ++k;
                        const int b0 = int(std::floor((a0 + amin) / kTwoPi * kBins)) - kBinsSpread, b1 = int(std::floor((a0 + amax) / kTwoPi * kBins)) + kBinsSpread;
                        bool all = true; for (int b = b0; b <= b1 && all; ++b) { const int bb = ((b % kBins) + kBins) % kBins; all = tmax < t.h[bb][k] - kMargin; }
                        rej = all;
                    }
                }
                if (!rej) nodes[w++] = nd;
            }
            out = w;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    { std::lock_guard<std::mutex> lk(g_stats_mu); g_stats.kept = out; g_stats.dropped = n - out; g_stats.filter_ms = ms; }
    return out;
}
}   // namespace md
