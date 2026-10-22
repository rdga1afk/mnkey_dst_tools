// Synthetic mini .def files (not Kenshi data) through the schema reader. With KENSHI_DIR set, also loads the real files and checks only that they parse.
#include "../editor/data/data_browser.h"
#include <monkey_dust/io/fcs.h>
#include <cstdint>
#include <cstring>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace md::fcsdef;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static const char* kEnums = R"(
enum Color { RED, GREEN = 5, BLUE, }
enum shared Fill {
	EMPTY, // nothing inside
	FULL
}
)";

static const char* kDef = R"(// a comment
[WIDGET,GADGET]
details:
name2:  "" "the name // not a comment"
count:  3 "how many"
ratio:  1.5
on:     False "a flag"
tint:   #ff8000
kind:   Color.GREEN bitset "which"
mesh:   "Ogre mesh|*.mesh" "the mesh"
tex:    TEXTURE_DDS
text:   "" multiline "long\ntext"
parts0: Fill.EMPTY looped
link:   WIDGET SINGLE REQUIRED "one widget"
slots:  GADGET 4 (0, 24,0) "many gadgets"
cond:   true
condition "cond" if "count" is 3 or 4 and "on" not True
OWNED: link, slots
TRANSLATE: all
Second:
other:  0.25

[FCS_LAYOUT]
Top :   WIDGET "count!=0"
	Child:   GADGET
		Deep:  GADGET
Other:  WIDGET

[FCS_SETTINGS]
data folder: data/
)";


// ---- the browser on synthetic records (v16 file written here) ----
namespace {
struct W {
    std::vector<uint8_t> b;
    W& i32(int32_t v) { for (int k = 0; k < 4; ++k) b.push_back((uint8_t)((uint32_t)v >> (8 * k))); return *this; }
    W& f32(float v) { uint8_t t[4]; std::memcpy(t, &v, 4); b.insert(b.end(), t, t + 4); return *this; }
    W& str(const std::string& x) { i32((int32_t)x.size()); b.insert(b.end(), x.begin(), x.end()); return *this; }
    W& u8(uint8_t v) { b.push_back(v); return *this; }
};
// kind 2 = WEAPON: "kind" enum int, "weight" float, "label" string, one reference list "parts"
void Rec(W& w, int id, const std::string& name, const std::string& sid, int kind_val, float weight) {
    w.i32(0).i32(2).i32(id).str(name).str(sid).i32(0);
    w.i32(0);                                                          // bools
    w.i32(1).str("weight").f32(weight);                                // floats
    w.i32(1).str("kind").i32(kind_val);                                // ints
    w.i32(0).i32(0);                                                   // vec3, vec4
    w.i32(1).str("label").str(name + "!");                             // strings
    w.i32(0);                                                          // files
    w.i32(1).str("parts").i32(1).str("p-1").i32(0).i32(0).i32(0);      // references
    w.i32(0);                                                          // instances
}
}  // namespace

// F2 oracle: 20 random records of the installed gamedata.base (fixed seed): every raw field of the record that the schema describes must show the same value on the card
static void RealCardTest(const std::string& kd, const Schema& schema) {
    md::fcs::File file; md::fcs::Error e;
    if (!md::fcs::ReadFile(kd + "/data/gamedata.base", file, e)) { std::printf("FAIL cannot read gamedata.base: %s\n", e.Message().c_str()); ++fails; return; }
    md::fcs::GameDataContainer data; data.ApplyFile("gamedata.base", file);
    DataBrowser br(schema, data);
    std::mt19937 rng(20261022);
    size_t checked = 0, undescribed = 0, mism = 0;
    for (int n = 0; n < 20; ++n) {
        const md::fcs::Record& r = file.records[rng() % file.records.size()];
        const auto card = br.Card(r.string_id);
        auto cf = [&](const std::string& name) -> const CardField* { for (const CardField& c : card) if (c.name == name) return &c; return nullptr; };
        auto fl = [](float f) { char b[32]; std::snprintf(b, sizeof b, "%g", f); return std::string(b); };
        auto enum_ok = [&](const CardField& c, int v) {
            if (c.value == std::to_string(v)) return true;
            const size_t dot = c.value.find('.'); if (dot == std::string::npos) return false;
            const EnumDef* ed = schema.FindEnum(c.value.substr(0, dot)); if (!ed) return false;
            for (const EnumValue& x : ed->values) if (x.name == c.value.substr(dot + 1)) return x.value == v;
            return false;
        };
        for (const auto& kv : r.bools) { const CardField* c = cf(kv.first); if (!c) { ++undescribed; continue; } ++checked; if (c->value != (kv.second ? "true" : "false")) { ++mism; std::printf("  bool %s %s: card '%s' raw %d\n", r.string_id.c_str(), kv.first.c_str(), c->value.c_str(), kv.second); } }
        for (const auto& kv : r.floats) { const CardField* c = cf(kv.first); if (!c) { ++undescribed; continue; } ++checked; if (c->value != fl(kv.second)) { ++mism; std::printf("  float %s %s: card '%s' raw %g\n", r.string_id.c_str(), kv.first.c_str(), c->value.c_str(), kv.second); } }
        for (const auto& kv : r.ints) { const CardField* c = cf(kv.first); if (!c) { ++undescribed; continue; } ++checked; const bool ok = c->type == "enum" ? enum_ok(*c, kv.second) : c->value == std::to_string(kv.second); if (!ok) { ++mism; std::printf("  int %s %s: card '%s' raw %d\n", r.string_id.c_str(), kv.first.c_str(), c->value.c_str(), kv.second); } }
        for (const auto& kv : r.strings) { const CardField* c = cf(kv.first); if (!c) { ++undescribed; continue; } ++checked; if (c->value != kv.second) { ++mism; std::printf("  string %s %s\n", r.string_id.c_str(), kv.first.c_str()); } }
        for (const auto& kv : r.files) { const CardField* c = cf(kv.first); if (!c) { ++undescribed; continue; } ++checked; if (c->value != kv.second) { ++mism; std::printf("  file %s %s\n", r.string_id.c_str(), kv.first.c_str()); } }
        for (const auto& kv : r.references) { const CardField* c = cf(kv.first); if (!c) { ++undescribed; continue; } ++checked; if (c->type != "refs" || (kv.second.empty() != c->value.empty())) { ++mism; std::printf("  refs %s %s\n", r.string_id.c_str(), kv.first.c_str()); } }
        CHECK(br.Instances(r.string_id).size() <= r.instances.size());
    }
    std::printf("card vs gamedata.base, 20 random records: %zu values compared, %zu mismatches, %zu raw fields not described by the schema\n", checked, mism, undescribed);
    CHECK(mism == 0);
    CHECK(checked > 100);
}

static std::string cf2(const DataBrowser& br, const std::string& sid, const char* name) { for (const CardField& c : br.Card(sid)) if (c.name == name) return c.value; return "<none>"; }

static void BrowserTest() {
    W f; f.i32(16).i32(1).str("a").str("d").str("").str("").i32(10).i32(3);
    Rec(f, 1, "Zeta", "1-z", 1, 2.5f); Rec(f, 2, "Alpha", "2-a", 0, 1.0f); Rec(f, 3, "Mid", "3-m", 1, 4.0f);
    md::fcs::File file; md::fcs::Error e;
    CHECK(md::fcs::Parse(f.b.data(), f.b.size(), file, e));
    md::fcs::GameDataContainer data; data.ApplyFile("t.mod", file);

    Schema s; std::string err;
    CHECK(ParseText("enum Kind { A, B }\n[WEAPON]\nStats:\nweight: 9.0 \"mass\"\nkind: Kind.A \"which\"\nlabel: \"\" \"text\"\nparts: BUILDING_PART \"the parts\"\nunset: 7\n"
                    "[FCS_LAYOUT]\nAll: WEAPON\n\tOnlyB: WEAPON \"kind==B\"\n\tNotB: WEAPON \"kind!=B;weight=1\"\n", s, err));
    DataBrowser br(s, data);
    const auto all = br.Items(s.layout[0]);
    CHECK(all.size() == 3 && all[0].name == "Alpha" && all[1].name == "Mid" && all[2].name == "Zeta");
    const auto onlyb = br.Items(s.layout[0].children[0]);
    CHECK(onlyb.size() == 2 && onlyb[0].name == "Mid" && onlyb[1].name == "Zeta");
    const auto notb = br.Items(s.layout[0].children[1]);
    CHECK(notb.size() == 1 && notb[0].string_id == "2-a");
    const auto card = br.Card("1-z");
    CHECK(card.size() == 5);
    auto cf = [&](const char* n) -> const CardField* { for (const CardField& c : card) if (c.name == n) return &c; return nullptr; };
    CHECK(cf("weight") && cf("weight")->value == "2.5" && cf("weight")->def == "9.0" && cf("weight")->changed && cf("weight")->category == "Stats");
    CHECK(cf("kind") && cf("kind")->value == "Kind.B" && cf("kind")->type == "enum");
    CHECK(cf("label") && cf("label")->value == "Zeta!");
    CHECK(cf("parts") && cf("parts")->value == "p-1" && cf("parts")->type == "refs");
    CHECK(cf("unset") && cf("unset")->value == "7" && !cf("unset")->changed);
    CHECK(br.Card("nope").empty());

    // F3: the edit layer
    EditSession ed(s, data); br.SetEdits(&ed);
    CHECK(ed.StateOf("1-z", "weight") == FieldState::Original && !ed.CanUndo());
    CHECK(ed.Set("1-z", "weight", "7.25", err) && ed.StateOf("1-z", "weight") == FieldState::Modified);
    CHECK(cf2(br, "1-z", "weight") == "7.25");
    CHECK(ed.Set("1-z", "kind", "Kind.A", err) && cf2(br, "1-z", "kind") == "Kind.A");
    CHECK(ed.Set("1-z", "label", "New name", err) && cf2(br, "1-z", "label") == "New name");
    CHECK(ed.ModifiedCount() == 3);
    CHECK(!ed.Set("1-z", "weight", "abc", err) && !err.empty());           // bad text is refused, nothing changes
    CHECK(!ed.Set("1-z", "nosuch", "1", err));
    CHECK(!ed.Set("zzz", "weight", "1", err));
    CHECK(ed.ModifiedCount() == 3);
    CHECK(ed.Undo() && ed.StateOf("1-z", "label") == FieldState::Original && cf2(br, "1-z", "label") == "Zeta!");
    CHECK(ed.Redo() && cf2(br, "1-z", "label") == "New name");
    CHECK(ed.Set("1-z", "weight", "2.5", err));                            // the base value: back to ORIGINAL
    CHECK(ed.StateOf("1-z", "weight") == FieldState::Original && ed.ModifiedCount() == 2);
    CHECK(ed.Undo() && ed.StateOf("1-z", "weight") == FieldState::Modified && cf2(br, "1-z", "weight") == "7.25");
    CHECK(ed.Revert("1-z", "kind") && ed.StateOf("1-z", "kind") == FieldState::Original && !ed.Revert("1-z", "kind"));
    CHECK(ed.Undo() && ed.StateOf("1-z", "kind") == FieldState::Modified);   // undo of a revert
    while (ed.Undo()) {}
    CHECK(ed.ModifiedCount() == 0 && !ed.CanUndo() && ed.CanRedo());
    CHECK(ed.Set("1-z", "weight", "1", err) && !ed.CanRedo());              // a new edit clears redo
    // a bitset enum and a colour, from their own schema
    Schema s2; CHECK(ParseText("enum Bits { B0, B1, B2 }\n[WEAPON]\nmask: Bits.B0 bitset\ntint: #102030\nflag: false\nn: 3\n", s2, err));
    EditSession e2(s2, data);
    CHECK(e2.Set("1-z", "mask", "B1|B2", err) && std::get<int32_t>(*e2.Edited("1-z", "mask")) == ((1 << 1) | (1 << 2)));   // bit number = the enum value
    CHECK(e2.Set("1-z", "tint", "#ff8000", err) && std::get<int32_t>(*e2.Edited("1-z", "tint")) == 0xFF8000);
    CHECK(e2.Set("1-z", "flag", "True", err) && std::get<bool>(*e2.Edited("1-z", "flag")));
    CHECK(!e2.Set("1-z", "n", "1.5", err));
    CHECK(e2.Set("1-z", "n", "-12", err) && std::get<int32_t>(*e2.Edited("1-z", "n")) == -12);
    br.SetEdits(nullptr);
}

int main() {
    BrowserTest();
    Schema s; std::string err;
    CHECK(ParseText(kEnums, s, err));
    CHECK(ParseText(kDef, s, err));
    CHECK(s.enums.size() == 2);
    const EnumDef* c = s.FindEnum("Color");
    CHECK(c && c->values.size() == 3 && c->values[1].value == 5 && c->values[2].value == 6);
    const EnumDef* f = s.FindEnum("Fill");
    CHECK(f && f->shared && f->values.size() == 2 && f->values[0].description == "nothing inside");
    CHECK(s.sections.size() == 1 && s.sections[0].types.size() == 2);
    const auto w = s.FieldsOf("GADGET");
    CHECK(w.size() == 14);
    auto find = [&](const char* n) -> const Field* { for (const Field* x : w) if (x->name == n) return x; return nullptr; };
    CHECK(find("name2") && find("name2")->kind == FieldKind::String && find("name2")->description == "the name // not a comment" && find("name2")->category == "details");
    CHECK(find("count") && find("count")->kind == FieldKind::Int && find("count")->default_text == "3" && find("count")->description == "how many");
    CHECK(find("ratio") && find("ratio")->kind == FieldKind::Float);
    CHECK(find("on") && find("on")->kind == FieldKind::Bool);
    CHECK(find("tint") && find("tint")->kind == FieldKind::Color);
    CHECK(find("kind") && find("kind")->kind == FieldKind::Enum && find("kind")->enum_name == "Color" && find("kind")->bitset);
    CHECK(find("mesh") && find("mesh")->kind == FieldKind::File && find("mesh")->default_text == "Ogre mesh|*.mesh");
    CHECK(find("tex") && find("tex")->kind == FieldKind::File);
    CHECK(find("text") && find("text")->multiline && find("text")->description == "long\ntext");
    CHECK(find("parts0") && find("parts0")->looped);
    CHECK(find("link") && find("link")->kind == FieldKind::RefList && find("link")->ref_type == "WIDGET" && find("link")->limit == 1 && find("link")->required);
    CHECK(find("slots") && find("slots")->limit == 4 && find("slots")->default_text == "(0, 24,0)");
    CHECK(find("cond") && find("cond")->condition.find("is 3 or 4") != std::string::npos);
    CHECK(find("other") && find("other")->category == "Second");
    CHECK(s.sections[0].owned.size() == 2 && s.sections[0].translate == "all");
    CHECK(s.layout.size() == 2 && s.layout[0].name == "Top" && s.layout[0].type == "WIDGET" && s.layout[0].filter == "count!=0");
    CHECK(s.layout[0].children.size() == 1 && s.layout[0].children[0].children.size() == 1 && s.layout[0].children[0].children[0].name == "Deep");
    CHECK(s.settings["data folder"] == "data/");

    Schema bad; CHECK(!ParseText("[X]\nno colon here\n", bad, err));
    Schema dup; CHECK(!ParseText("enum A { X }\nenum A { Y }\n", dup, err));

    if (const char* kd = std::getenv("KENSHI_DIR")) {   // not in CI: the install's own files must at least parse
        Schema real;
        if (LoadDir(kd, real, err)) {
            size_t nf = 0, kinds[10] = {};
            for (const Section& sec : real.sections) { nf += sec.fields.size(); for (const Field& fl : sec.fields) ++kinds[(int)fl.kind]; }
            RealCardTest(kd, real);
            std::printf("real files: %zu sections, %zu fields (bool %zu float %zu int %zu string %zu colour %zu enum %zu null %zu file %zu reflist %zu refstring %zu), %zu enums, %zu top layout nodes\n",
                        real.sections.size(), nf, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], kinds[6], kinds[7], kinds[8], kinds[9], real.enums.size(), real.layout.size());
        }
        else { std::printf("FAIL real files: %s\n", err.c_str()); ++fails; }
    }
    std::printf(fails ? "FAILED\n" : "PASS\n");
    return fails ? 1 : 0;
}
