// #159 phase 2 data check: bone scales (md::character::ComputeBoneScales) for every .bod2 of the install: finite and > 0, left / right equal (any value the game data holds is valid input).
// usage: md_character_scales_test <data/character/bodies dir>
#include "bod2.h"
#include "bone_scales.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    namespace fs = std::filesystem; std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(argv[1])) if (e.is_regular_file() && e.path().extension() == ".bod2") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    int bad = 0, checked = 0; float lo = 1e9f, hi = -1e9f;
    for (const auto& p : files) {
        md::character::Bod2 b; std::string err;
        if (!md::character::ReadBod2(p.string(), b, err) || b.records.empty()) { std::printf("FAIL %s: %s\n", p.string().c_str(), err.c_str()); return 1; }
        std::map<std::string, float> v; bool female = false;
        for (const auto& kv : b.records[0].floats) v[kv.first] = kv.second;
        for (const auto& kv : b.records[0].bools) if (kv.first == "sex female") female = kv.second != 0;
        md::character::BodyScaleInputs in; in.female = female; in.has_breast_bones = female; in.has_jaw_bone = !female;
        auto scales = md::character::ComputeBoneScales(v, in);
        for (const auto& s : scales) {
            for (int k = 0; k < 3; ++k) {
                float xs[2] = {s.size[k], s.has_positional ? s.positional[k] : 1.f};
                for (float x : xs) {
                    ++checked; lo = std::min(lo, x); hi = std::max(hi, x);
                    if (!std::isfinite(x) || !(x > 0.f)) { if (bad++ < 12) std::printf("OUT %s %s[%d]=%g\n", fs::relative(p, argv[1]).c_str(), s.bone.c_str(), k, x); }
                }
            }
        }
        for (size_t i = 0; i + 1 < scales.size(); ++i) {   // pairs are emitted adjacently as (L, R) or (R, L)
            const auto &a = scales[i], &c = scales[i + 1];
            auto strip = [](std::string n) { for (const char* t : {" L ", " R "}) { auto q = n.find(t); if (q != std::string::npos) n.erase(q, 3); } if (n.rfind("L Boob", 0) == 0 || n.rfind("R Boob", 0) == 0) n = "Boob"; return n; };
            if (strip(a.bone) == strip(c.bone) && a.bone != c.bone) {
                for (int k = 0; k < 3; ++k) if (a.size[k] != c.size[k] || a.positional[k] != c.positional[k]) { if (bad++ < 12) std::printf("ASYM %s %s\n", fs::relative(p, argv[1]).c_str(), a.bone.c_str()); }
            }
        }
    }
    if (bad) { std::printf("FAIL scales: %d problems in %zu files\n", bad, files.size()); return 1; }
    std::printf("PASS data scales: %zu bod2 files, %d values finite and > 0 (range %.3f .. %.3f), left / right equal\n", files.size(), checked, lo, hi);
    return 0;
}
