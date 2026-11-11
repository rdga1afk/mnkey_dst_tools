// Appearance state of the character editor (slider name -> value) and the RANDOMISE / RESET rules of the original editor.
// Sources (exe, ghidra MCP): the RANDOMISE button handler and its per-tab calls;
// the slider randomiser, the pose randomiser; the RESET handler;
// random number (rand()/32768); XML field parser; colour blend.
// НЕВІДОМО (#159): which tab number is Body / Face / Hair; Skin Tone randomisation (picks an integer between two bounds, derivation not decoded);
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

int RangeMid(const Range& r);          // `mid`, or round(min + (max - min) * 0.5) when absent (exe)
int RangeVariation(const Range& r);    // round((max - min) * random_variation / 100); 0 when the attribute is absent (exe)

// ColourRange value -> colour: position = value * 0.1, linear blend of the neighbouring colours per channel, the last colour is not passed (exe).
std::array<float, 3> ColourAt(const ColourRange& c, int value);

struct Appearance {
    std::map<std::string, float> values;   // Range: integer value; ColourRange: integer position
};

// Every slider of a gender configuration at `mid`.
Appearance MakeDefault(const Config& cfg);
// RESET (exe, Range and ColourRange sliders of the Face and Body categories).
void Reset(const Config& cfg, Appearance& a);
// RANDOMISE of one category: a slider with no random_group gets round(mid + variation * (2 * r - 1)) with its own r; sliders of one random_group
// share one r (exe). The original function does not clamp, neither does this (data: Fishman `Neck` has mid 100 outside min = max = 65);
// НЕВІДОМО (#159): whether the widget clamps when it shows the value.
void RandomiseCategory(const Config& cfg, const std::string& category, Appearance& a, Rng& rng);
// RANDOMISE ALL: every category of the configuration in file order.
void RandomiseAll(const Config& cfg, Appearance& a, Rng& rng);

} // namespace md::character
