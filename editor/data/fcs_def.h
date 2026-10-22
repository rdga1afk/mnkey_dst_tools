// Reader of the FCS definition files (`fcs_theme.def`, `fcs_settings.def`, `fcs_layout.def`, `fcs_enums.def`, `fcs.def`) of a Kenshi install: the field
// schema of every item type lives in those text files, not in the FCS program (docs/research/kenshi_specs/FCS_SCHEMA.md section 2 in the main repo). Our code
// written from that grammar; no Kenshi data is stored here. Load time only (std::string / std::vector on purpose).
#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

namespace md::fcsdef {

struct EnumValue { std::string name; int value = 0; std::string description; };
struct EnumDef { std::string name; bool shared = false; std::vector<EnumValue> values; };

enum class FieldKind { Bool, Float, Int, String, Color, Enum, Null, File, RefList, RefString };

struct Field {
    std::string name, category, description;
    FieldKind kind = FieldKind::String;
    std::string default_text;     // the default as written (`True`, `1.0`, `#ff8000`, `""`, `Enum.VALUE`, the file mask, the reference triple `(0, 24,0)`)
    std::string enum_name;        // Enum: the enum of `Enum.VALUE`
    std::string ref_type;         // RefList: the item type the references point to (`WEAPON`)
    int limit = 0;                // RefList: 1 = SINGLE, N = at most N, 0 = none
    bool required = false, bitset = false, looped = false, multiline = false;
    std::string condition;        // the text after `condition "<this field>" if`, empty when always shown
};

struct Section {
    std::vector<std::string> types;   // `[A,B]` -> {A, B}
    std::vector<Field> fields;
    std::vector<std::string> owned;   // `OWNED:` list
    std::string translate;            // `TRANSLATE:` text
};

struct LayoutNode {
    std::string name, type, filter;
    std::vector<LayoutNode> children;
};

struct Schema {
    std::vector<EnumDef> enums;
    std::vector<Section> sections;
    std::vector<LayoutNode> layout;                  // top level of `[FCS_LAYOUT]`
    std::map<std::string, std::string> settings;     // `[FCS_SETTINGS]`
    std::map<std::string, std::string> theme;        // `[FCS_THEME]`

    const EnumDef* FindEnum(const std::string& name) const;
    // The fields of one item type, in file order (several sections can list the same type: their fields add up).
    std::vector<const Field*> FieldsOf(const std::string& item_type) const;
};

// Parse one file's text into `out` (appends). Returns false and sets `err` on a malformed line.
bool ParseText(const std::string& text, Schema& out, std::string& err);
// The five files of `<dir>` in FCS order; a missing file is skipped, no file at all is an error.
bool LoadDir(const std::string& dir, Schema& out, std::string& err);

}  // namespace md::fcsdef
