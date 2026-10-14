#include "appearance.h"
#include <algorithm>
#include <cmath>

namespace md::character {
namespace {
int RoundAway(float v) { return int(v < 0.f ? std::ceil(v - 0.5f) : std::floor(v + 0.5f)); }   // exe 0x1409b32b0
}

int RangeMid(const Range& r) { return r.has_mid ? r.mid : RoundAway(float(r.max - r.min) * 0.5f + float(r.min)); }
int RangeVariation(const Range& r) { return r.has_random_variation ? RoundAway(float(r.max - r.min) * (float(r.random_variation) * 0.01f)) : 0; }

std::array<float, 3> ColourAt(const ColourRange& c, int value) {
    if (c.colours.empty()) return {0.f, 0.f, 0.f};
    if (c.colours.size() == 1) return {float(c.colours[0][0]), float(c.colours[0][1]), float(c.colours[0][2])};
    float p = float(value) * 0.1f;
    int i = std::max(0, std::min(int(std::floor(p)), int(c.colours.size()) - 1));
    int j = i == int(c.colours.size()) - 1 ? i : i + 1;
    float t = p - float(i);
    std::array<float, 3> o;
    for (int k = 0; k < 3; ++k) o[k] = float(c.colours[i][k]) + (float(c.colours[j][k]) - float(c.colours[i][k])) * t;
    return o;
}

Appearance MakeDefault(const Config& cfg) { Appearance a; Reset(cfg, a); return a; }

void Reset(const Config& cfg, Appearance& a) {
    for (const Category& c : cfg.categories) {
        for (const Range& r : c.ranges) a.values[r.name] = float(RangeMid(r));
        for (const ColourRange& r : c.colour_ranges) a.values[r.name] = float(r.has_mid ? r.mid : 0);
    }
}

void RandomiseCategory(const Config& cfg, const std::string& category, Appearance& a, Rng& rng) {
    const Category* c = cfg.Find(category);
    if (!c) return;
    auto set = [&](const Range& r, float rnd) {
        int v = RoundAway(float(RangeVariation(r)) * (rnd * 2.f - 1.f) + float(RangeMid(r)));
        a.values[r.name] = float(v);
    };
    std::map<int, float> group_r;   // one random number per random_group
    std::vector<int> group_order;
    for (const Range& r : c->ranges) if (r.has_random_group && r.random_group >= 0 && !group_r.count(r.random_group)) group_order.push_back(r.random_group), group_r[r.random_group] = 0.f;
    for (const Range& r : c->ranges) if (!(r.has_random_group && r.random_group >= 0)) set(r, rng.Next());
    for (int g : group_order) {
        float rnd = rng.Next();
        for (const Range& r : c->ranges) if (r.has_random_group && r.random_group == g) set(r, rnd);
    }
}

void RandomiseAll(const Config& cfg, Appearance& a, Rng& rng) {
    for (const Category& c : cfg.categories) RandomiseCategory(cfg, c.name, a, rng);
}

} // namespace md::character
