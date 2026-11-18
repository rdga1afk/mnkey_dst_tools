// NeighbourAvoidance against kenshi_x64.exe character_neighbour_avoidance_query (0x1401476f0) run in the Unicorn emulator (tools_private/research/query_oracle.py -> tools/tests/data/exe_neighbour.tsv).
// The exe's rsqrtps / rcpps are approximations refined once, ours is exact: rows agree within 1e-4 relative + 1e-5 absolute. The coincident case (random direction) has no row.
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

TEST(NeighbourAvoid, MatchesExeEmulatorVectors) {
    std::ifstream in("tools/tests/data/exe_neighbour.tsv"); std::string line; int rows = 0, pushed = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line.rfind("char_pos", 0) == 0) continue;
        std::istringstream ss(line); std::string f[11]; for (auto& x : f) std::getline(ss, x, '\t');
        const std::vector<float> c = Floats(f[0]), tg = Floats(f[5]), dir = Floats(f[7]), st = Floats(f[8]);
        std::vector<md::world::Neighbour> nb;
        std::istringstream ns(f[4]); std::string e;
        while (std::getline(ns, e, ';')) {
            const size_t a = e.find('/'), b = e.rfind('/'); const std::vector<float> p = Floats(e.substr(0, a));
            nb.push_back({{p[0], p[1], p[2]}, std::stof(e.substr(a + 1, b - a - 1)), e.substr(b + 1) == "1"});
        }
        const md::world::AvoidResult r = md::world::NeighbourAvoidance(c.data(), std::stof(f[1]), std::stoi(f[2]), tg.data(), nb.data(), (int)nb.size(), f[3] == "1");
        ++rows;
        const int want = std::stoi(f[6]);
        ASSERT_EQ(r.status, want) << line;
        ASSERT_EQ(r.state, std::stoi(f[10])) << line;
        if (!want) continue;
        ++pushed;
        for (int i = 0; i < 4; ++i) EXPECT_NEAR(r.dir[i], dir[i], 1e-4f * std::fabs(dir[i]) + 1e-5f) << line;
        EXPECT_NEAR(r.strength, st[0], 1e-4f * std::fabs(st[0]) + 1e-5f) << line;
    }
    std::printf("neighbour avoidance vs exe: %d rows, %d with a push\n", rows, pushed);
    EXPECT_EQ(rows, 22);
}
