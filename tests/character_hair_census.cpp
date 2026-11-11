// Hair / beard census (diagnosis of the overlapping hairstyles in the Characters tab, coordinator 6804920270, step 1 «data first»):
// for every race x subrace x gender the attachment records of the hair and beard slots, and for each distinct mesh file how many submeshes it has (each with its material and index count).
// A style whose mesh has several submeshes is drawn as ONE attachment made of several parts: that is legal data, not a leak. usage: md_character_hair_census <KENSHI_DIR>
#include "session.h"
#include <monkey_dust/io/ogre_mesh.h>
#include <cstdio>
#include <map>
#include <set>

using namespace md::character;

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    Session s; std::string err;
    if (!s.Load(argv[1], err)) { std::printf("load: %s\n", err.c_str()); return 1; }
    std::map<std::string, std::string> seen;   // mesh file -> "slot name: N submeshes [material:indices ...]"
    size_t styles = 0, multi = 0, nomesh = 0;
    std::set<std::string> counted, diffs;
    for (size_t r = 0; r < s.RaceCount(); ++r) {
        s.SelectRace(r);
        for (size_t sr = 0; sr < std::max<size_t>(1, s.SubraceCount()); ++sr) {
            if (s.SubraceCount()) s.SelectSubrace(sr);
            const bool single = s.SingleGender();
            for (int g = 0; g < (single ? 1 : 2); ++g) {
                if (!single) s.SetGender(g ? Gender::Female : Gender::Male);
                const bool fem = s.gender() == Gender::Female;
                for (int slot = 0; slot < 2; ++slot) {
                    const size_t n = slot ? s.BeardCount() : s.HairCount();
                    for (size_t i = 0; i < n; ++i) {
                        const AttachmentInfo& a = slot ? s.Beard(i) : s.Hair(i);
                        const std::string key = std::to_string(slot) + a.id + (fem ? "f" : "m");
                        if (!counted.insert(key).second) continue;
                        if (!slot && a.name.find("hawk") != std::string::npos) std::printf("HIDX race %s sub %zu %s hair index %zu id %s\n", s.Race(r).name.c_str(), sr, fem ? "F" : "M", i, a.id.c_str());
                        ++styles;
                        if (!diffs.count(a.id)) { diffs.insert(a.id); std::printf("REC %s | %s | %s | diffuse ch %d alpha ch %d rej %d spec %.2f | tex %s\n", a.id.c_str(), a.name.c_str(), slot ? "beard" : "hair", a.diffuse_channel, a.alpha_channel, a.alpha_rejection, a.specular, a.Texture(fem).c_str()); if (const auto* g = s.Data().Get(a.id)) { static int shown = 0; if (shown++ < 3) std::printf("RAW %s specular: float key %s, int key %s\n", a.id.c_str(), g->floats.count("specular") ? std::to_string(g->floats.at("specular")).c_str() : "absent", g->ints.count("specular") ? std::to_string(g->ints.at("specular")).c_str() : "absent"); } }
                        if (!a.HasMesh(fem)) { ++nomesh; continue; }
                        const std::string path = s.Resources().Path(a.Mesh(fem));
                        if (seen.count(path)) continue;
                        md::ogre::Mesh m; md::ogre::Error oe;
                        if (!md::ogre::ReadMesh(path, m, oe)) { seen[path] = std::string(slot ? "beard " : "hair ") + a.name + ": UNREADABLE " + oe.Message(); continue; }
                        std::string d = std::string(slot ? "beard " : "hair ") + a.name + ": " + std::to_string(m.submeshes.size()) + " submeshes [";
                        for (const auto& sub : m.submeshes) d += sub.material + ":" + std::to_string(sub.indices.size()) + " ";
                        seen[path] = d + "]";
                        if (m.submeshes.size() > 1) ++multi;
                    }
                }
            }
        }
    }
    for (const auto& kv : seen) std::printf("%s | %s\n", kv.first.c_str(), kv.second.c_str());
    std::printf("CENSUS: %zu hair / beard records over all race x subrace x gender, %zu without a mesh, %zu distinct mesh files, %zu with more than one submesh\n", styles, nomesh, seen.size(), multi);
    return 0;
}
