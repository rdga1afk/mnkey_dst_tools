#include "appearance_io.h"
#include "bod2.h"
#include "face_poses.h"
#include "race_assets.h"
#include "skinned_body.h"
#include <monkey_dust/io/ogre_skeleton.h>
#include <cmath>

namespace md::character {
namespace {
template <class V> const typename V::value_type::second_type* Find(const V& v, const std::string& k) {
    for (const auto& p : v) if (p.first == k) return &p.second;
    return nullptr;
}
}

bool ExportAppearance(const Session& s, const std::string& path, std::string& error) {
    if (!s.Loaded() || !s.Limits()) { error = "no race selected"; return false; }
    Bod2 b; b.file_version = 15;
    Bod2Record r; r.name = "0"; r.identifier = "1--INGAME"; r.item_id = 1;
    r.bools.push_back({"sex female", s.gender() == Gender::Female ? 1 : 0});
    r.refs.push_back({"race", {Bod2Ref{s.Current().id, 0, 0, 0}}});
    r.strings.push_back({"head", s.HeadCount() ? s.HeadId(s.head()) : ""});
    r.strings.push_back({"hair style", s.HairCount() ? s.Hair(s.hair_index()).id : ""});
    r.strings.push_back({"beard", s.BeardCount() ? s.Beard(s.beard_index()).id : ""});
    r.strings.push_back({"idle stance", s.StanceAnim()});
    std::map<std::string, float> values;
    for (const Category& c : s.Limits()->categories) {
        for (const Range& g : c.ranges) { float v = s.Value(g); values[g.name] = v; r.floats.push_back({g.name, v}); }
        for (const ColourRange& cr : c.colour_ranges) {
            auto it = s.Values().values.find(cr.name);
            int v = it == s.Values().values.end() ? cr.mid : int(it->second);
            if (cr.name == "Skin Tone") { auto col = ColourAt(cr, v); r.vec3s.push_back({cr.name, {col[0] / 255.f, col[1] / 255.f, col[2] / 255.f}}); }
            else r.floats.push_back({cr.name, float(v)});
        }
    }
    md::ogre::Mesh mesh; md::ogre::Skeleton sk; md::ogre::Error oe; SkinnedBody body; std::string rel, err;
    if (LoadBodyMesh(s.Resources(), s.Current(), s.gender(), mesh, rel, err) && md::ogre::ReadSkeleton(s.Resources().Path(mesh.skeleton), sk, oe) && BuildSkinnedBody(mesh, sk, body, err)) {
        std::vector<std::string> names; for (const auto& p : body.poses) names.push_back(p.name);
        std::vector<float> w = FacePoseWeights(*s.Limits(), names, values);
        for (size_t i = 0; i < w.size(); ++i) if (w[i] != 0.f) r.floats.push_back({names[i], w[i]});
    }
    b.records.push_back(r);
    std::vector<uint8_t> bytes = WriteBod2(b);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { error = "cannot write " + path; return false; }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!ok) error = "short write " + path;
    return ok;
}

bool ImportAppearance(Session& s, const std::string& path, std::string& error) {
    Bod2 b;
    if (!ReadBod2(path, b, error)) return false;
    if (b.records.empty()) { error = "no record in " + path; return false; }
    const Bod2Record& r = b.records[0];
    const std::vector<Bod2Ref>* race = Find(r.refs, "race");
    const md::fcs::GameData* rd = race && !race->empty() ? s.Data().Get((*race)[0].target) : nullptr;
    if (!rd) { error = "race of the file is not in the load order"; return false; }
    const uint8_t* fem = Find(r.bools, "sex female");
    if (!s.SelectSubraceByName(rd->name, fem && *fem)) { error = "race " + rd->name + " is not playable"; return false; }
    if (!s.Limits()) { error = "race " + rd->name + " has no editor limits"; return false; }
    for (const Category& c : s.Limits()->categories) {
        for (const Range& g : c.ranges) if (const float* v = Find(r.floats, g.name)) s.SetValue(g.name, *v);
        for (const ColourRange& cr : c.colour_ranges) {
            if (cr.name == "Skin Tone") {
                const std::array<float, 3>* t = Find(r.vec3s, cr.name);
                if (!t) continue;
                int best = cr.mid; float bd = 1e9f;
                for (int v = 0; v <= 1000; ++v) {
                    auto col = ColourAt(cr, v); float d = 0.f;
                    for (int k = 0; k < 3; ++k) d += std::fabs(col[k] / 255.f - (*t)[k]);
                    if (d < bd) { bd = d; best = v; }
                }
                s.SetValue(cr.name, float(best));
            } else if (const float* v = Find(r.floats, cr.name)) s.SetValue(cr.name, *v);
        }
    }
    if (const std::string* h = Find(r.strings, "head")) for (size_t i = 0; i < s.HeadCount(); ++i) if (s.HeadId(i) == *h) s.SelectHead(int(i));
    if (const std::string* h = Find(r.strings, "hair style")) for (size_t i = 0; i < s.HairCount(); ++i) if (s.Hair(i).id == *h) s.SelectHair(i);
    if (const std::string* h = Find(r.strings, "beard")) for (size_t i = 0; i < s.BeardCount(); ++i) if (s.Beard(i).id == *h) s.SelectBeard(i);
    if (const std::string* h = Find(r.strings, "idle stance")) for (size_t i = 0; i < s.StanceCount(); ++i) if (s.StanceName(i) == *h) s.SelectStance(int(i));
    return true;
}

} // namespace md::character
