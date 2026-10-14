#include "race_stats.h"
#include <cmath>
#include <cstdio>

namespace md::character {

const char* StatName(int id) {
    static const char* const n[39] = {nullptr, "Strength", "Melee attack", "Labouring", "Science", "Engineer", "Robotics", "Weapon smith", "Armour smith", "Field medic", "Thievery", "Turrets", "Farming", "Cooking",
        nullptr, nullptr, "Stealth", "Athletics", "Dexterity", "Melee defence", nullptr, "Toughness", "Assassination", "Swimming", "Perception", "Katanas", "Sabres", "Hackers", "Heavy weapons", "Blunt",
        "Martial arts", nullptr, "Dodge", "Survival", "Polearms", "Crossbows", "Precision shooting", "Lockpicking", "Crossbow smith"};
    return id >= 1 && id <= 38 ? n[id] : nullptr;
}

std::vector<StatRow> RaceStatRows(const RaceInfo& r) {
    std::vector<StatRow> rows;
    char b[32];
    std::snprintf(b, sizeof b, "%d", int(r.hit_points + 0.5f));
    rows.push_back({"Hit Points:", b, false});
    if (r.bleed_rate < 0.1f) rows.push_back({"No bleeding", "", false});
    float mult[39];
    for (float& m : mult) m = 1.f;
    for (int id : r.stats_good) if (id > 0 && id <= 38) mult[id] = 1.2f;
    for (int id : r.stats_bad) if (id > 0 && id <= 38) mult[id] = 0.8f;
    mult[1] = r.strength; mult[18] = r.dexterity;
    auto add = [&](int id) {
        const char* nm = StatName(id);
        int pct = int(std::lround((mult[id] - 1.f) * 100.f));
        if (!nm || pct == 0) return;
        std::snprintf(b, sizeof b, "%+d%%", pct);
        rows.push_back({nm, b, pct < 0});
    };
    for (int id : r.stats_good) add(id);
    for (int id : r.stats_bad) add(id);
    for (int id : {1, 18}) {
        bool listed = false;
        for (int g : r.stats_good) listed |= g == id;
        for (int g : r.stats_bad) listed |= g == id;
        if (!listed) add(id);
    }
    return rows;
}

} // namespace md::character
