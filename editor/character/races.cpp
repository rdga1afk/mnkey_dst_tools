#include "races.h"
#include <cstdio>
#include <sstream>

namespace md::character {
namespace {

bool BoolOf(const md::fcs::GameData& g, const char* k) { auto it = g.bools.find(k); return it != g.bools.end() && it->second; }
float FloatOf(const md::fcs::GameData& g, const char* k) { auto it = g.floats.find(k); return it == g.floats.end() ? 0.f : it->second; }
std::string Base(const std::string& p) { size_t s = p.find_last_of("/\\"); return s == std::string::npos ? p : p.substr(s + 1); }
std::string FileOf(const md::fcs::GameData& g, const char* k) { auto it = g.files.find(k); return it == g.files.end() ? std::string() : Base(it->second); }
std::vector<std::string> RefsOf(const md::fcs::GameData& g, const char* cat) {
    std::vector<std::string> v;
    if (const auto* r = g.Refs(cat)) for (const auto& t : *r) v.push_back(t.first);
    return v;
}
std::vector<int> StatsOf(const md::fcs::GameData& g, const std::string& prefix) {
    std::vector<int> v;   // std::map iterates the keys in order: "stats good0", "stats good1", ...
    for (const auto& kv : g.ints) if (kv.first.compare(0, prefix.size(), prefix) == 0) v.push_back(kv.second);
    return v;
}

} // namespace

const RaceInfo* RaceData::Find(const std::string& id) const {
    for (const auto& r : races) if (r.id == id) return &r;
    return nullptr;
}

std::vector<const RaceInfo*> RaceData::Subraces(const RaceGroup& g) const {
    std::vector<const RaceInfo*> out;
    for (const auto& id : g.race_ids) if (const RaceInfo* r = Find(id)) if (r->playable) out.push_back(r);
    return out;
}

RaceData LoadRaces(const md::fcs::GameDataContainer& data) {
    RaceData out;
    for (const md::fcs::GameData* g : data.OfType(kItemRace)) {
        RaceInfo r;
        r.id = g->string_id; r.name = g->name;
        auto d = g->strings.find("description"); if (d != g->strings.end()) r.description = d->second;
        r.playable = BoolOf(*g, "playable"); r.single_gender = BoolOf(*g, "single gender"); r.is_robot = BoolOf(*g, "is robot");
        r.no_hats = BoolOf(*g, "no hats"); r.no_shirts = BoolOf(*g, "no shirts"); r.no_shoes = BoolOf(*g, "no shoes");
        r.editor_limits = FileOf(*g, "editor limits"); r.male_mesh = FileOf(*g, "male mesh"); r.female_mesh = FileOf(*g, "female mesh");
        r.heads_male = RefsOf(*g, "heads male"); r.heads_female = RefsOf(*g, "heads female"); r.hairs = RefsOf(*g, "hairs"); r.hair_colors = RefsOf(*g, "hair colors");
        if (const auto* ca = g->Refs("combat anatomy")) if (!ca->empty() && (*ca)[0].second[1] >= 1) r.hit_points = float((*ca)[0].second[1]);
        if (auto it = g->floats.find("bleed rate"); it != g->floats.end()) r.bleed_rate = it->second;
        if (auto it = g->floats.find("strength"); it != g->floats.end()) r.strength = it->second;
        if (auto it = g->floats.find("dexterity"); it != g->floats.end()) r.dexterity = it->second;
        r.stats_good = StatsOf(*g, "stats good"); r.stats_bad = StatsOf(*g, "stats bad");
        r.portrait_distance = FloatOf(*g, "portrait distance");
        r.portrait_offset[0] = FloatOf(*g, "portrait offset x"); r.portrait_offset[1] = FloatOf(*g, "portrait offset y"); r.portrait_offset[2] = FloatOf(*g, "portrait offset z");
        out.races.push_back(std::move(r));
    }
    for (const md::fcs::GameData* g : data.OfType(kItemRaceGroup)) {
        RaceGroup gr; gr.id = g->string_id; gr.name = g->name; gr.race_ids = RefsOf(*g, "races");
        out.groups.push_back(std::move(gr));
    }
    return out;
}

std::string DumpRaces(const RaceData& rd) {
    std::ostringstream o;
    auto join = [](const std::vector<std::string>& v) { std::string s; for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + v[i]; return s; };
    auto joini = [](const std::vector<int>& v) { std::string s; for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]); return s; };
    for (const auto& g : rd.groups) o << "group|" << g.id << '|' << g.name << "|races=" << join(g.race_ids) << '\n';
    for (const auto& r : rd.races) {
        std::string d40 = r.description.substr(0, 40), esc;
        for (char c : d40) esc += c == '\n' ? std::string("\\n") : c == '\r' ? std::string("\\r") : std::string(1, c);
        char port[96]; std::snprintf(port, sizeof port, "%.9g,%.9g,%.9g,%.9g", r.portrait_distance, r.portrait_offset[0], r.portrait_offset[1], r.portrait_offset[2]);
        o << "race|" << r.id << '|' << r.name << "|playable=" << r.playable << "|single=" << r.single_gender << "|robot=" << r.is_robot << "|hats=" << r.no_hats
          << "|shirts=" << r.no_shirts << "|shoes=" << r.no_shoes << "|limits=" << r.editor_limits << "|male_mesh=" << r.male_mesh << "|female_mesh=" << r.female_mesh
          << "|heads_m=" << join(r.heads_male) << "|heads_f=" << join(r.heads_female) << "|hairs=" << join(r.hairs) << "|hair_colors=" << join(r.hair_colors)
          << "|good=" << joini(r.stats_good) << "|bad=" << joini(r.stats_bad) << "|portrait=" << port << "|desc=" << r.description.size() << ':' << esc << '\n';
    }
    return o.str();
}

} // namespace md::character
