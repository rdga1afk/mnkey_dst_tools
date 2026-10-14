#include "race_assets.h"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace md::character {

bool LoadBodyMesh(const md::KenshiResources& res, const RaceInfo& race, Gender gender, md::ogre::Mesh& out, std::string& rel_path, std::string& error) {
    const std::string& name = (gender == Gender::Female && !race.female_mesh.empty()) ? race.female_mesh : race.male_mesh;
    if (name.empty()) { error = "race " + race.id + " names no mesh"; return false; }
    rel_path = res.Find(name);
    if (rel_path.empty()) { error = name + ": not found under data/"; return false; }
    md::ogre::Error e;
    if (!md::ogre::ReadMesh(res.Path(name), out, e)) { error = name + ": " + e.Message(); return false; }
    return true;
}

namespace {
const char* GenderName(Gender g) { return g == Gender::Male ? "Male" : g == Gender::Female ? "Female" : "-"; }
}

std::string DumpPoseCoverage(const RaceData& races, const std::vector<EditorLimits>& limits, const md::KenshiResources& res) {
    std::ostringstream o;
    std::map<std::string, const EditorLimits*> by_file;
    for (const EditorLimits& l : limits) by_file[l.file] = &l;
    std::map<std::string, std::set<std::string>> poses_cache;   // mesh file name + gender -> pose names; "<failed>" marker as a key
    for (const RaceInfo& r : races.races) {
        auto it = by_file.find(r.editor_limits);
        if (r.editor_limits.empty() || it == by_file.end()) continue;
        for (const Config& cfg : it->second->configs) {
            md::ogre::Mesh mesh; std::string rel, err;
            bool ok = LoadBodyMesh(res, r, cfg.gender, mesh, rel, err);
            if (!ok) { o << "mesh_missing|" << r.id << '|' << GenderName(cfg.gender) << '|' << (cfg.gender == Gender::Female && !r.female_mesh.empty() ? r.female_mesh : r.male_mesh) << '\n'; continue; }
            std::set<std::string> names;
            for (const md::ogre::Pose& p : mesh.poses) names.insert(p.name);
            for (const Category& c : cfg.categories) for (const Range& rg : c.ranges) {
                if (rg.target.empty() && rg.target_opposite.empty()) continue;
                auto part = [&](const std::string& t) { return t.empty() ? std::string("-") : t + ":" + (names.count(t) ? "1" : "0"); };
                o << r.id << '|' << GenderName(cfg.gender) << '|' << r.editor_limits << '|' << c.name << '|' << rg.name << "|target=" << part(rg.target) << "|opposite=" << part(rg.target_opposite) << '\n';
            }
        }
    }
    return o.str();
}

} // namespace md::character
