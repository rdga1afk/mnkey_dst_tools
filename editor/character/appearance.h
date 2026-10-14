// Appearance state of the character editor (slider name -> value) and the RANDOMISE / RESET rules of the original editor.
// Sources (exe, ghidra MCP): RANDOMISE button handler 0x1405efa50 (calls per tab 0x14007b310 / 0x14007b7c0 / 0x14007b940 / 0x14007adc0);
// slider randomiser 0x14006fcf0, pose randomiser 0x140070010; RESET handler 0x1405ef870 -> 0x14007a490;
// random number 0x1409b1c10 (rand()/32768); XML field parser 0x140077090; colour blend 0x14006e340.
// НЕВІДОМО (#159): which tab number is Body / Face / Hair; Skin Tone randomisation (0x14007b940 picks an integer between two bounds, derivation not decoded);
// the two scale constants used by the parser (percent of range for `random_variation`, default `mid` position) are taken as 0.01 and 0.5, not read.
#pragma once
#include "editor_limits.h"
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace md::character {

// rand() of the original is 15 bit: Next() = n / 32768, n in [0, 32767].
struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed ? seed : 1u) {}
    float Next() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return float((state >> 8) & 0x7fff) / 32768.f; }
};

int RangeMid(const Range& r);          // `mid`, or round(min + (max - min) * 0.5) when absent (exe 0x140077e45)
int RangeVariation(const Range& r);    // round((max - min) * random_variation / 100); 0 when the attribute is absent (exe 0x1400781d6 .. 0x14007824a)

// ColourRange value -> colour: position = value * 0.1, linear blend of the neighbouring colours per channel, the last colour is not passed (exe 0x14006e340).
std::array<float, 3> ColourAt(const ColourRange& c, int value);

struct Appearance {
    std::map<std::string, float> values;   // Range: integer value; ColourRange: integer position
};

// Every slider of a gender configuration at `mid`.
Appearance MakeDefault(const Config& cfg);
// RESET (exe 0x14007a490, Range and ColourRange sliders of the Face and Body categories).
void Reset(const Config& cfg, Appearance& a);
// RANDOMISE of one category: a slider with no random_group gets round(mid + variation * (2 * r - 1)) with its own r; sliders of one random_group
// share one r (exe 0x14006fcf0). The original function does not clamp, neither does this (data: Fishman `Neck` has mid 100 outside min = max = 65);
// НЕВІДОМО (#159): whether the widget clamps when it shows the value.
void RandomiseCategory(const Config& cfg, const std::string& category, Appearance& a, Rng& rng);
// RANDOMISE ALL: every category of the configuration in file order.
void RandomiseAll(const Config& cfg, Appearance& a, Rng& rng);

} // namespace md::character
