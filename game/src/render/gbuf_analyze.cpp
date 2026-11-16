#include "gbuf_analyze.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace gbuf_check {
namespace {
float Half(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m + 1024), (int)e - 25);
    return s ? -v : v;
}

}

bool Analyze(const uint16_t* A, const uint16_t* B, size_t n, char* text, size_t text_size) {
    size_t written = 0, nan_px = 0, neg_px = 0, nrm_bad = 0, same = 0, up = 0;
    double dsum = 0, nmin = 1e9, nmax = -1e9;
    for (size_t i = 0; i < n; ++i) {
        const float ar = Half(A[i * 4]), ag = Half(A[i * 4 + 1]), ab = Half(A[i * 4 + 2]), aa = Half(A[i * 4 + 3]);
        const float br = Half(B[i * 4]), bg = Half(B[i * 4 + 1]), bbv = Half(B[i * 4 + 2]), bd = Half(B[i * 4 + 3]);
        const float all[8] = { ar, ag, ab, aa, br, bg, bbv, bd };
        bool bad = false; for (float v : all) if (!std::isfinite(v)) bad = true;
        if (bad) { ++nan_px; continue; }
        if (!(bd > 0.f)) { if (bd < 0.f) ++neg_px; continue; }   // 0 = not written
        ++written;
        if (ar < 0.f || ag < 0.f || ab < 0.f) ++neg_px;
        const double nx = 2.0 * br - 1.0, ny = 2.0 * bg - 1.0, nz = 2.0 * bbv - 1.0, len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < nmin) nmin = len;
        if (len > nmax) nmax = len;
        if (len < 0.7 || len > 1.6) ++nrm_bad;
        if (ny > 0.0) ++up;
        const double d = std::fabs(ar - br) + std::fabs(ag - bg) + std::fabs(ab - bbv);
        dsum += d;
        if (d < 0.01) ++same;
    }
    const double mean_diff = written ? dsum / (3.0 * written) : 0.0, up_frac = written ? (double)up / written : 0.0;
    std::string why;
    if (written < n / 100) why += "fewer than 1 % of the pixels written; ";
    if (nan_px) why += "NaN / Inf pixels; ";
    if (neg_px > written / 50000) why += "negative albedo or distance; ";
    if (written && nrm_bad > written / 50) why += "|normal| outside 0.7..1.6 on more than 2 % of the written pixels; ";
    if (written && up_frac < 0.9) why += "the normal points up on fewer than 90 % of the written pixels; ";
    if (written && (mean_diff < 0.02 || same > written / 2)) why += "albedo equals the normal; ";
    snprintf(text, text_size, "gbuf_check: %s written=%zu/%zu nan=%zu neg=%zu normal_len=[%.3f,%.3f] bad_normal=%zu up=%.3f albedo_vs_normal_mean_abs=%.3f same_px=%zu %s",
             why.empty() ? "PASS" : "FAIL", written, n, nan_px, neg_px, written ? nmin : 0.0, written ? nmax : 0.0, nrm_bad, up_frac, mean_diff, same, why.c_str());
    return why.empty();
}

}
