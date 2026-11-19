// SteerVelocity against kenshi_x64.exe character_steer_update ([KSDP]) run in the Unicorn emulator (tools_private/research/steer_oracle.py -> tools/tests/data/exe_steer.tsv).
// The exe's rsqrtps / rcpps are approximations refined once; ours uses sqrt, so rows agree within 1e-4 relative + 1e-5 absolute. Block rows hold with the two helpers stubbed (see the generator).
#include <gtest/gtest.h>
#include <monkey_dust/world/steer.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::vector<float> Floats(const std::string& s) {
    std::vector<float> v; std::istringstream ss(s); std::string t;
    while (std::getline(ss, t, ',')) v.push_back(std::stof(t));
    return v;
}
}  // namespace

TEST(Steer, MatchesExeEmulatorVectors) {
    std::ifstream in(MD_TOOLS_TEST_DATA "/exe_steer.tsv"); std::string line; int rows = 0, blocks = 0; double worst = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line.rfind("mode", 0) == 0) continue;
        std::istringstream ss(line); std::string f[9]; for (auto& x : f) std::getline(ss, x, '\t');
        const std::vector<float> vel = Floats(f[1]), dir = Floats(f[2]), want = Floats(f[8]);
        md::world::SteerBlock b;
        if (f[0] == "block") { b.blocked = true; const auto D = Floats(f[6]), C = Floats(f[7]); for (int i = 0; i < 4; ++i) { b.dir[i] = D[i]; b.clearance[i] = C[i]; } ++blocks; }
        float v[4] = {vel[0], vel[1], vel[2], vel[3]};
        md::world::SteerVelocity(v, dir.data(), std::stof(f[3]), std::stof(f[4]), std::stof(f[5]), b);
        for (int i = 0; i < 4; ++i) {
            const double err = std::fabs(v[i] - want[i]), tol = 1e-5 + 1e-4 * std::fabs(want[i]);
            worst = std::fmax(worst, err / tol);
            EXPECT_LE(err, tol) << "row " << rows << " lane " << i << " mode " << f[0];
        }
        ++rows;
    }
    std::printf("steer vs the exe: %d rows (%d blocked), worst error / tolerance %.3f\n", rows, blocks, worst);
    EXPECT_EQ(rows, 28);
    EXPECT_EQ(blocks, 12);
}
