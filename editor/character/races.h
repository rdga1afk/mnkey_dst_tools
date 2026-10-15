// Races and race groups of the character editor, from the merged FCS data (gamedata.base + the mods of the load order); nothing is listed in code.
// RACE records are FCS type 7, RACE_GROUP type 100 (`races` = the group's races). The editor shows the groups as RACE and the group's playable races as SUBRACE
// (exe 0x140079bb0 builds the group list and keeps the races whose `playable` is set; fcs.def:1497 / :1577).
// НЕВІДОМО: whether exe also requires a non-empty `editor limits` for the SUBRACE list (it reports a missing file at 0x141683da0 when a race is chosen).
#pragma once
#include <monkey_dust/io/fcs_gamedata.h>
#include <string>
#include <vector>

namespace md::character {

constexpr int kItemRace = 7;
constexpr int kItemRaceGroup = 100;

struct RaceInfo {
    std::string id, name, description;
    bool playable = false, single_gender = false, is_robot = false, no_hats = false, no_shirts = false, no_shoes = false;
    std::string editor_limits;                 // base name of the `editor limits` file, "" = none
    std::string male_mesh, female_mesh;        // base names
    std::vector<std::string> heads_male, heads_female, hairs, hair_colors;   // string_ids, in merge order
    std::vector<int> hair_weights, hair_color_weights;                       // the first integer of the reference, parallel to `hairs` / `hair_colors`
    std::vector<int> stats_good, stats_bad;    // `stats good0..` / `stats bad0..` (stat ids) in key order
    float bleed_rate = 1.f, hit_points = 100.f, strength = 1.f, dexterity = 1.f;   // hit_points: `combat anatomy` second value of the first part (100 when under 1); strength / dexterity: RACE floats
    float portrait_distance = 0.f, portrait_offset[3] = {0.f, 0.f, 0.f};
};

struct RaceGroup {
    std::string id, name;
    std::vector<std::string> race_ids;
};

struct RaceData {
    std::vector<RaceGroup> groups;             // file order
    std::vector<RaceInfo> races;               // file order
    const RaceInfo* Find(const std::string& id) const;
    // The SUBRACE list of a group: its races with `playable` set.
    std::vector<const RaceInfo*> Subraces(const RaceGroup& g) const;
};

RaceData LoadRaces(const md::fcs::GameDataContainer& data);
// One line per group and race, all fields (the oracle compares it with scripts/gen_char_editor_spec_tables.py --dump-races):
//   group|id|name|races=id,id
//   race|id|name|playable=0|single=0|robot=0|hats=0|shirts=0|shoes=0|limits=f|male_mesh=m|female_mesh=f|heads_m=a,b|heads_f=a|hairs=..|hair_colors=..|good=a,b|bad=a|portrait=d,x,y,z|desc=<len>:<first 40, \n escaped>
std::string DumpRaces(const RaceData& r);

} // namespace md::character
