// #192 head check (owner 28.10): for every race / subrace / gender of the install, with the editor's default slider values, the posed body of the Characters preview
// has a head: the bone scales of the head are finite and not collapsed, the vertices that belong to the head bone are skinned to a block of sensible size, and the highest
// vertices of the whole body are head vertices (a head that vanished leaves a neck stump: the highest vertices would be the shoulders). Same pipeline as
// tools/editor/editor_char_preview.cpp (session defaults -> bone scales -> idle stance + slider layers -> skin matrices -> linear blend skinning), on the CPU.
// usage: md_character_head_test <KENSHI_DIR> [race-name ...]   (no names: all races)
#include "session.h"
#include "skinned_body.h"
#include "pose_solver.h"
#include "bone_scales.h"
#include "race_assets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

using namespace md::character;

struct Result { bool ok = true; char why[200] = {}; float head_h = 0, body_h = 0, head_w = 0; size_t head_verts = 0; };

static Result Check(Session& s) {
    Result r; const RaceInfo& race = s.Current(); const bool female = s.gender() == Gender::Female;
    md::ogre::Mesh m; std::string rel, err;
    if (!LoadBodyMesh(s.Resources(), race, s.gender(), m, rel, err)) { r.ok = false; std::snprintf(r.why, sizeof r.why, "body mesh: %s", err.c_str()); return r; }
    md::ogre::Skeleton sk; md::ogre::Error oe;
    if (!md::ogre::ReadSkeleton(s.Resources().Path(m.skeleton), sk, oe)) { r.ok = false; std::snprintf(r.why, sizeof r.why, "skeleton %s", m.skeleton.c_str()); return r; }
    SkinnedBody b;
    if (!BuildSkinnedBody(m, sk, b, err)) { r.ok = false; std::snprintf(r.why, sizeof r.why, "skinned body: %s", err.c_str()); return r; }
    int head = -1;
    for (size_t i = 0; i < b.bone_name.size(); ++i) if (b.bone_name[i] == "Bip01 Head") head = int(i);
    if (head < 0) for (size_t i = 0; i < b.bone_name.size() && head < 0; ++i) if (b.bone_name[i].find("Head") != std::string::npos) head = int(i);
    if (head < 0) { std::snprintf(r.why, sizeof r.why, "no head bone in this skeleton (skipped)"); r.head_verts = 0; return r; }
    BodyScaleInputs in; in.female = female;
    for (const std::string& n : b.bone_name) { if (n == "L Boob") in.has_breast_bones = true; if (n == "Bip01 Jaw") in.has_jaw_bone = true; }
    std::map<std::string, float> vals(s.Values().values.begin(), s.Values().values.end());
    auto scales = ComputeBoneScales(vals, in);
    for (const BoneScale& bs : scales)
        if (bs.bone == "Bip01 Head")
            for (int k = 0; k < 3; ++k)
                if (!std::isfinite(bs.size[k]) || bs.size[k] < 0.3f || bs.size[k] > 4.f) { r.ok = false; std::snprintf(r.why, sizeof r.why, "Bip01 Head scale[%d] = %g", k, double(bs.size[k])); return r; }
    std::vector<AnimLayer> layers = {{FindAnimation(sk, s.StanceAnim()), 0.f}};
    for (const auto& pr : {std::make_pair("Posture", "postures"), std::make_pair("Neck position", "shoulder set"), std::make_pair("Shoulder set", "neck set")}) {
        const md::ogre::Animation* a = FindAnimation(sk, pr.second); auto it = vals.find(pr.first);
        if (a && it != vals.end()) layers.push_back({a, a->length * it->second * 0.01f});
    }
    std::vector<M4> skin; SolveSkinMatrices(b, sk, layers, scales, skin);
    std::vector<float> y(b.vertices.size()); std::vector<std::array<float, 3>> p(b.vertices.size());
    for (size_t i = 0; i < b.vertices.size(); ++i) {
        const SkinnedVertex& v = b.vertices[i]; float o[3] = {0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            if (v.weight[k] <= 0.f) continue; const M4& M = skin[v.joint[k]];
            for (int c = 0; c < 3; ++c) o[c] += v.weight[k] * (M[c] * v.pos[0] + M[4 + c] * v.pos[1] + M[8 + c] * v.pos[2] + M[12 + c]);
        }
        p[i] = {o[0], o[1], o[2]}; y[i] = o[1];
        if (!std::isfinite(o[0] + o[1] + o[2])) { r.ok = false; std::snprintf(r.why, sizeof r.why, "vertex %zu not finite after skinning", i); return r; }
    }
    float ymin = *std::min_element(y.begin(), y.end()), ymax = *std::max_element(y.begin(), y.end()); r.body_h = ymax - ymin;
    std::vector<size_t> idx(b.vertices.size()); for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t c) { return y[a] > y[c]; });
    size_t top = std::max<size_t>(20, idx.size() / 50), hv = 0;
    for (size_t k = 0; k < top; ++k) { const SkinnedVertex& v = b.vertices[idx[k]]; int dom = 0; for (int j = 1; j < 4; ++j) if (v.weight[j] > v.weight[dom]) dom = j; if (v.joint[dom] == head) ++hv; }
    float hy0 = 1e9f, hy1 = -1e9f, hx0 = 1e9f, hx1 = -1e9f;
    for (size_t i = 0; i < b.vertices.size(); ++i) { const SkinnedVertex& v = b.vertices[i]; int dom = 0; for (int j = 1; j < 4; ++j) if (v.weight[j] > v.weight[dom]) dom = j;
        if (v.joint[dom] != head) continue; ++r.head_verts; hy0 = std::min(hy0, p[i][1]); hy1 = std::max(hy1, p[i][1]); hx0 = std::min(hx0, p[i][0]); hx1 = std::max(hx1, p[i][0]); }
    r.head_h = r.head_verts ? hy1 - hy0 : 0; r.head_w = r.head_verts ? hx1 - hx0 : 0;
    if (r.head_verts < 100) { r.ok = false; std::snprintf(r.why, sizeof r.why, "only %zu vertices belong to the head bone", r.head_verts); }
    else if (r.head_h < 0.08f * r.body_h || r.head_w < 0.04f * r.body_h) { r.ok = false; std::snprintf(r.why, sizeof r.why, "head block %.2f x %.2f of a %.2f body: collapsed", double(r.head_h), double(r.head_w), double(r.body_h)); }
    else if (hv * 10 < top * 8) { r.ok = false; std::snprintf(r.why, sizeof r.why, "only %zu of the %zu highest vertices are head vertices (neck stump?)", hv, top); }
    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    Session s; std::string err;
    if (!s.Load(argv[1], err)) { std::printf("FAIL head: load: %s\n", err.c_str()); return 1; }
    std::vector<std::string> only; for (int i = 2; i < argc; ++i) only.push_back(argv[i]);
    int checked = 0, skipped = 0, bad = 0; bool human_green_m = false, human_green_f = false;
    for (size_t ri = 0; ri < s.RaceCount(); ++ri) {
        s.SelectRace(ri);
        if (!only.empty() && std::find(only.begin(), only.end(), s.Race(ri).name) == only.end()) continue;
        for (size_t si = 0; si < s.SubraceCount(); ++si) {
            s.SelectSubrace(si);
            for (int g = 0; g < 2; ++g) {
                if (g == 1) { if (s.SingleGender()) continue; s.SetGender(Gender::Female); } else if (!s.SingleGender()) s.SetGender(Gender::Male);
                Result r = Check(s);
                const bool skip = r.ok && r.head_verts == 0 && r.why[0];
                std::printf("%s %-10s %-14s %-6s head verts %5zu head %.2f x %.2f of body %.2f %s\n", skip ? "SKIP" : (r.ok ? "ok  " : "FAIL"), s.Race(ri).name.c_str(), s.Current().name.c_str(),
                            s.SingleGender() ? "-" : (g ? "Female" : "Male"), r.head_verts, double(r.head_h), double(r.head_w), double(r.body_h), r.why);
                if (skip) { ++skipped; continue; }
                ++checked; if (!r.ok) ++bad;
                if (s.Race(ri).name == "Human" && s.Current().name == "Greenlander") (g ? human_green_f : human_green_m) = r.ok;
            }
        }
    }
    // #192 (coordinator 6511465033): the owner's headless frame had non-default sliders (Idle stance 6, Posture 35, Shoulder set 68, Neck position 53): every stance and every slider of the editor at its real min / max, one at a time, Human Greenlander M/F
    if (only.empty()) for (size_t ri = 0; ri < s.RaceCount(); ++ri) {
        s.SelectRace(ri); if (s.Race(ri).name != "Human") continue;
        for (size_t si = 0; si < s.SubraceCount(); ++si) {
            s.SelectSubrace(si); if (s.Current().name != "Greenlander") continue;
            for (int g = 0; g < 2; ++g) {
                if (g == 1) s.SetGender(Gender::Female); else s.SetGender(Gender::Male);
                const char* nm = g ? "Female" : "Male"; int n = 0, nb = 0;
                auto run = [&](const char* what, float v) { Result r = Check(s); ++n; if (!r.ok) { ++bad; ++nb; std::printf("FAIL Greenlander %s %s = %g: %s\n", nm, what, double(v), r.why); } };
                for (size_t st = 0; st < s.StanceCount(); ++st) { s.Reset(); s.SelectStance(int(st)); run(("stance " + s.StanceName(st)).c_str(), float(st)); }
                if (const Config* cfg = s.Limits()) for (const Category& cat : cfg->categories) for (const Range& rg : cat.ranges)   // every slider of the race / gender at its real minimum and maximum, one at a time
                    for (float v : { float(rg.min), float(rg.max) }) { s.Reset(); s.SetValue(rg.name, v); run(rg.name.c_str(), v); }
                s.Reset(); s.SetValue("Neck position", 53); s.SetValue("Shoulder set", 68); s.SetValue("Posture", 35); s.SelectStance(6); run("owner combo (Idle 6, Posture 35, Shoulder 68, Neck 53)", 6);
                std::printf("slider / stance sweep Greenlander %s: %d checks, %d bad\n", nm, n, nb);
            }
        }
    }
    if (only.empty() && !(human_green_m && human_green_f)) { std::printf("FAIL head: Human Greenlander male / female were not checked or failed\n"); return 1; }
    if (bad || checked == 0) { std::printf("FAIL head: %d of %d race / gender combinations have no head\n", bad, checked); return 1; }
    std::printf("PASS head: %d combinations (%d skipped: no head bone) have a head of sensible size on top of the body\n", checked, skipped);
    return 0;
}
