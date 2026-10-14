// RACE STATS panel rows (exe 0x1405ee0c0): "Hit Points:", "No bleeding" (bleed rate under 0.1, exe 0x1405ee0c0 / constant 0x14167fad0), then each skill the race is better / worse at.
// Facts: race stat multipliers 1.2 (`stats good<N>`) / 0.8 (`stats bad<N>`), then RACE strength -> stat 1 and dexterity -> stat 18 overwrite (M:docs/game/character-stats.md "Race stat multipliers", exe FUN_14042efa0);
// Hit Points = base HP of the race's body parts, the second integer of a `combat anatomy` reference (M:docs/game/character-stats.md "What a body part is").
#pragma once
#include "races.h"
#include <string>
#include <vector>

namespace md::character {

struct StatRow { std::string label, value; bool bad = false; };

const char* StatName(int id);   // display name of StatsEnumerated id 1..38, nullptr otherwise
std::vector<StatRow> RaceStatRows(const RaceInfo& race);

} // namespace md::character
