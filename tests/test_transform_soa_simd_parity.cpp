#include <gtest/gtest.h>
#include <monkey_dust/world/transform_soa_simd.h>
#include <cmath>
#include <cstdint>

// Phase 2.2 (audit): TransformSoA::BulkComputeDistSq's AVX2 and scalar
// bodies used to live inline behind #ifdef __AVX2__ — only one was ever
// compiled per build, so nothing could catch the two silently diverging
// (e.g. someone "optimizes" the AVX2 path and forgets the scalar one, or
// vice versa). Now extracted to transform_soa_simd.h's two always-callable
// functions (see that header's doc comment + engine/CMakeLists.txt's
// transform_soa_simd_avx2.cpp per-TU -mavx2/-mfma override) — this test
// calls BOTH on the SAME input and asserts they agree within float epsilon.
// Must fail if a future edit desyncs them.

namespace {
// alignas(32) — minimum for _mm256_load_ps/_mm256_store_ps (AVX2 requires
// 32-byte alignment for the aligned load/store intrinsics used here).
constexpr int kCount = 137;  // deliberately NOT a multiple of 8 — exercises
                             // both the vectorized main loop AND the scalar
                             // tail loop inside ComputeDistSqAVX2.

struct Fixture {
    alignas(32) float px[kCount];
    alignas(32) float pz[kCount];
    alignas(32) float out_scalar[kCount];
    alignas(32) float out_avx2[kCount];

    Fixture(uint32_t seed) {
        uint32_t rng = seed;
        auto next = [&]() {
            rng = rng * 1664525u + 1013904223u;
            return (float)(rng >> 8) / (float)(1u << 24);  // [0,1)
        };
        for (int i = 0; i < kCount; ++i) {
            px[i] = (next() - 0.5f) * 20000.f;  // world-scale coordinates
            pz[i] = (next() - 0.5f) * 20000.f;
        }
    }
};
}  // namespace

TEST(TransformSoaSimdParity, AVX2MatchesScalarOnRandomInputs) {
    Fixture f(0xC0FFEEu);
    const float cam_x = 123.456f, cam_z = -789.012f;

    md::simd::ComputeDistSqScalar(f.px, f.pz, cam_x, cam_z, f.out_scalar, kCount);
    md::simd::ComputeDistSqAVX2(f.px, f.pz, cam_x, cam_z, f.out_avx2, kCount);

    for (int i = 0; i < kCount; ++i) {
        // FMA (AVX2 path) vs separate mul+add (scalar) can differ in the
        // last ULP or two — relative epsilon, not bit-exact equality.
        float ref = f.out_scalar[i];
        float rel_eps = std::fabs(ref) * 1e-5f + 1e-4f;
        EXPECT_NEAR(f.out_avx2[i], ref, rel_eps) << "mismatch at index " << i;
    }
}

TEST(TransformSoaSimdParity, AVX2MatchesScalarOnZeroCount) {
    Fixture f(1u);
    md::simd::ComputeDistSqScalar(f.px, f.pz, 0.f, 0.f, f.out_scalar, 0);
    md::simd::ComputeDistSqAVX2(f.px, f.pz, 0.f, 0.f, f.out_avx2, 0);
    SUCCEED();  // must not crash on count=0 (both loops should no-op cleanly)
}

TEST(TransformSoaSimdParity, AVX2MatchesScalarOnExactMultipleOf8) {
    Fixture f(42u);
    constexpr int n = 64;  // exact multiple of 8 — no scalar tail loop taken
    md::simd::ComputeDistSqScalar(f.px, f.pz, 5.f, -5.f, f.out_scalar, n);
    md::simd::ComputeDistSqAVX2(f.px, f.pz, 5.f, -5.f, f.out_avx2, n);
    for (int i = 0; i < n; ++i) {
        float ref = f.out_scalar[i];
        float rel_eps = std::fabs(ref) * 1e-5f + 1e-4f;
        EXPECT_NEAR(f.out_avx2[i], ref, rel_eps) << "mismatch at index " << i;
    }
}
