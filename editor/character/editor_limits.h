// Per-race slider limits of Kenshi's character editor: `data/editor/editor_data_*.xml` (RACE field `editor limits`).
// Structure: Editor > Config(gender) > Category(Face|Body|Hair|Personality) > Range / ColourRange. C++17 + stdlib only.
#pragma once
#include <array>
#include <string>
#include <vector>

namespace md::character {

struct Range {
    std::string name;
    int min = 0, max = 0;
    bool has_mid = false;  int mid = 0;
    std::string target, target_opposite;       // face pose names (weight = value * 0.01, a negative value uses target_opposite)
    bool has_random_group = false;  int random_group = 0;
    bool has_random_variation = false;  int random_variation = 0;
};

struct ColourRange {
    std::string name;
    bool has_mid = false;  int mid = 0;
    std::vector<std::array<int, 3>> colours;   // r g b
};

struct Category {
    std::string name;                          // Face, Body, Hair, Personality
    std::vector<Range> ranges;
    std::vector<ColourRange> colour_ranges;
};

enum class Gender { Single, Male, Female };

struct Config {
    Gender gender = Gender::Single;            // no `gender` attribute: a single-gender race
    std::vector<Category> categories;
    const Category* Find(const std::string& category) const;
};

struct EditorLimits {
    std::string file;                          // base name, e.g. editor_data_human.xml
    std::vector<Config> configs;
    const Config* For(Gender g) const;         // Single-gender files answer for any gender
};

// One file. False + `error` on a missing / malformed file or an attribute that is not an integer.
bool LoadEditorLimits(const std::string& path, EditorLimits& out, std::string& error);
// Every editor_data_*.xml of a directory, sorted by file name.
bool LoadEditorLimitsDir(const std::string& dir, std::vector<EditorLimits>& out, std::string& error);
// Text dump, one line per Range / ColourRange with all attributes (the oracle compares it with scripts/gen_char_editor_spec_tables.py --dump-limits):
//   file|gender|category|range|name|min|max|mid|target|target_opposite|random_group|random_variation
//   file|gender|category|colour_range|name|mid|r,g,b;r,g,b;...
std::string DumpEditorLimits(const std::vector<EditorLimits>& all);

} // namespace md::character
