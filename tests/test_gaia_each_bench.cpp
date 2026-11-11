// #187: query().each micro-benchmark (512 entities x 2-3 components, 1000 iterations x 2 queries). DISABLED_ so ctest skips it:
//   build/tests/md_tests --gtest_also_run_disabled_tests --gtest_filter='GaiaBench.*'
// md_tests is built with the gaia PCH codegen object (MD_GAIA_PCH=ON), which makes gaia inline functions external (+74 % here); configure with -DMD_GAIA_PCH=OFF for the inlined baseline.
#include <gtest/gtest.h>
#include <monkey_dust/ecs/md_registry.h>
#include <chrono>
#include <cstdio>

namespace {
struct BenchP { float x, y, z; };
struct BenchV { float x, y, z; };
struct BenchH { float hp; };
}

TEST(GaiaBench, DISABLED_QueryEach) {
    gaia::ecs::World w;
    for (int i = 0; i < 512; ++i) {
        auto e = w.add();
        w.add<BenchP>(e, {(float)i, 0.f, 0.f});
        w.add<BenchV>(e, {1.f, 2.f, 3.f});
        if (i & 1) w.add<BenchH>(e, {100.f});
    }
    auto q = w.query().all<BenchP&>().all<BenchV>();
    auto q2 = w.query().all<BenchH&>();
    double best = 1e9;
    for (int rep = 0; rep < 7; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (int it = 0; it < 1000; ++it) {
            q.each([&](BenchP& p, const BenchV& v) { p.x += v.x * 0.01f; p.y += v.y * 0.01f; p.z += v.z * 0.01f; });
            q2.each([&](BenchH& h) { h.hp -= 0.001f; });
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms < best) best = ms;
    }
    std::printf("[GaiaBench] best %.3f ms (1000 iterations x 2 queries, 512 entities)\n", best);
    EXPECT_GT(best, 0.0);
}
