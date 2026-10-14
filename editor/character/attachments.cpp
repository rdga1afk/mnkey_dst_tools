#include "attachments.h"
#include <algorithm>
#include <cmath>

namespace md::character {
namespace {
std::string FileOf(const md::fcs::GameData& g, const char* k) { auto it = g.files.find(k); return it == g.files.end() ? std::string() : it->second; }
int IntOf(const md::fcs::GameData& g, const char* k, int d) { auto it = g.ints.find(k); return it == g.ints.end() ? d : it->second; }
}

bool LoadAttachment(const md::fcs::GameDataContainer& data, const std::string& id, AttachmentInfo& out) {
    const md::fcs::GameData* g = data.Get(id);
    if (!g || g->kind != 6) return false;
    out = AttachmentInfo();
    out.id = id; out.name = g->name;
    out.mesh = FileOf(*g, "mesh"); out.mesh_female = FileOf(*g, "mesh female");
    out.texture = FileOf(*g, "texture map"); out.texture_female = FileOf(*g, "texture map female");
    out.slot = IntOf(*g, "attach slot", 0); out.alpha_rejection = IntOf(*g, "alpha rejection", 128);
    out.diffuse_channel = IntOf(*g, "hair diffuse channel", 0); out.alpha_channel = IntOf(*g, "hair alpha channel", 0);
    return true;
}

bool LoadColour(const md::fcs::GameDataContainer& data, const std::string& id, std::array<int, 3>& rgb) {
    const md::fcs::GameData* g = data.Get(id);
    if (!g) return false;
    auto it = g->ints.find("color 1");
    if (it == g->ints.end()) return false;
    rgb = {(it->second >> 16) & 255, (it->second >> 8) & 255, it->second & 255};
    return true;
}

std::array<float, 3> HsbToRgb(float h, float s, float b) {
    h -= std::floor(h); s = std::max(0.f, std::min(1.f, s)); b = std::max(0.f, std::min(1.f, b));
    float hh = h * 6.f; int i = int(std::floor(hh)) % 6; float f = hh - std::floor(hh);
    float p = b * (1 - s), q = b * (1 - s * f), t = b * (1 - s * (1 - f));
    switch (i) { case 0: return {b, t, p}; case 1: return {q, b, p}; case 2: return {p, b, t}; case 3: return {p, q, b}; case 4: return {t, p, b}; default: return {b, p, q}; }
}

std::array<float, 3> RgbToHsb(float r, float g, float b) {
    float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn, h = 0.f;
    if (d > 0.f) {
        if (mx == r) h = std::fmod((g - b) / d, 6.f); else if (mx == g) h = (b - r) / d + 2.f; else h = (r - g) / d + 4.f;
        h /= 6.f; if (h < 0.f) h += 1.f;
    }
    return {h, mx > 0.f ? d / mx : 0.f, mx};
}

} // namespace md::character
