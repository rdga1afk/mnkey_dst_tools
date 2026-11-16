// A2 analysis (game/src/render/gbuf_check.cpp Analyze), CPU only: a plausible ground passes; NaN, unwritten, normals pointing down, albedo equal to the normal, and an out-of-range normal each FAIL.
#include "../game/src/render/gbuf_analyze.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
static uint16_t H(float f) {   // float -> half (round to nearest, normals only)
    uint32_t x; memcpy(&x, &f, 4); const uint32_t s = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 255) - 127 + 15; uint32_t m = (x >> 13) & 1023;
    if (f == 0.f) return (uint16_t)s; if (e <= 0) return (uint16_t)s; if (e >= 31) return (uint16_t)(s | 0x7c00); return (uint16_t)(s | (e << 10) | m);
}
static const size_t N = 4096;
static void Fill(std::vector<uint16_t>& A, std::vector<uint16_t>& B) {   // a plausible ground: albedo ~0.3..0.6, normal up with a tilt, distance 100
    A.assign(N * 4, 0); B.assign(N * 4, 0);
    for (size_t i = 0; i < N; ++i) {
        const float t = (float)(i % 64) / 64.f, ar = 0.30f + 0.3f * t, ag = 0.25f + 0.2f * t, ab = 0.15f + 0.1f * t;
        A[i * 4] = H(ar); A[i * 4 + 1] = H(ag); A[i * 4 + 2] = H(ab); A[i * 4 + 3] = H(0.3f);
        const float nx = 0.2f * (t - 0.5f), nz = 0.15f, ny = std::sqrt(1.f - nx * nx - nz * nz);
        B[i * 4] = H(0.5f * nx + 0.5f); B[i * 4 + 1] = H(0.5f * ny + 0.5f); B[i * 4 + 2] = H(0.5f * nz + 0.5f); B[i * 4 + 3] = H(100.f);
    }
}
int main() {
    int fails = 0; char txt[900]; std::vector<uint16_t> A, B;
    auto expect = [&](const char* what, bool want_pass) { const bool ok = gbuf_check::Analyze(A.data(), B.data(), N, txt, sizeof txt); const bool good = ok == want_pass; printf("%s: %s (%s)\n", good ? "PASS" : "FAIL", what, ok ? "check passed" : "check failed"); if (!good) { ++fails; puts(txt); } };
    Fill(A, B); expect("plausible ground passes", true);
    Fill(A, B); for (size_t i = 0; i < N; i += 7) A[i * 4] = 0x7e00; expect("NaN albedo fails", false);
    Fill(A, B); for (size_t i = 0; i < N; ++i) B[i * 4 + 3] = 0; expect("nothing written fails", false);
    Fill(A, B); for (size_t i = 0; i < N; ++i) B[i * 4 + 1] = H(0.2f); expect("normals pointing down fail", false);
    Fill(A, B); for (size_t i = 0; i < N; ++i) { B[i * 4] = A[i * 4]; B[i * 4 + 1] = A[i * 4 + 1]; B[i * 4 + 2] = A[i * 4 + 2]; } expect("albedo written into the normal target fails", false);
    Fill(A, B); for (size_t i = 0; i < N; ++i) { B[i * 4] = H(1.0f); B[i * 4 + 1] = H(1.0f); B[i * 4 + 2] = H(1.0f); } expect("normal of length 1.7 fails", false);
    Fill(A, B); for (size_t i = 0; i < N; i += 9) A[i * 4 + 1] = H(-0.3f); expect("negative albedo fails", false);
    printf(fails ? "gbuf_check test: FAIL (%d)\n" : "gbuf_check test: PASS\n", fails); return fails ? 1 : 0;
}
