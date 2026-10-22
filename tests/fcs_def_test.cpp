// Synthetic mini .def files (not Kenshi data) through the schema reader. With KENSHI_DIR set, also loads the real files and checks only that they parse.
#include "../editor/data/data_browser.h"
#include "../editor/data/dialog_model.h"
#include "../editor/data/mod_check.h"
#include "../editor/data/translation.h"
#include "../editor/data/mod_write.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <tuple>
#include <set>
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

// F5 on the real install: change a weapon, add and copy an item, write a mod of our own to a temp file, read it back, apply it on top of the base and compare with the layer
static void RealWriteTest(const std::string& kd, const Schema& schema) {
    md::fcs::File base; md::fcs::Error e;
    if (!md::fcs::ReadFile(kd + "/data/gamedata.base", base, e)) { std::printf("FAIL base: %s\n", e.Message().c_str()); ++fails; return; }
    md::fcs::GameDataContainer data; data.ApplyFile("gamedata.base", base);
    EditSession ed(schema, data, "md_test.mod");
    std::string sword_id, field; float old_v = 0;
    for (const md::fcs::GameData* w : data.OfType(2)) {   // the first weapon with a float field the schema describes
        for (const auto& kv : w->floats) { bool known = false; for (const Field* f : schema.FieldsOf("WEAPON")) if (f->name == kv.first) known = true; if (known) { sword_id = w->string_id; field = kv.first; old_v = kv.second; break; } }
        if (!sword_id.empty()) break;
    }
    CHECK(!sword_id.empty());
    std::string err;
    CHECK(ed.Set(sword_id, field, std::to_string(old_v + 1.5f), err));
    const std::string item = ed.NewRecord(4, "MD test item");
    CHECK(ed.Duplicate(sword_id, "MD sword copy") != "");
    CHECK(ed.SetName(sword_id, "MD renamed weapon"));
    const std::string path = (std::filesystem::temp_directory_path() / "md_f5_test.mod").string();
    ModInfo info; info.description = "F5 test";
    CHECK(WriteMod(path, ed, schema, info, err, kd + "/data"));
    md::fcs::File back;
    CHECK(md::fcs::ReadFile(path, back, e));
    CHECK(back.header.filetype == 17 && back.header.id_counter == ed.LastId() && back.header.dependencies == "gamedata.base");
    md::fcs::GameDataContainer after; after.ApplyFile("gamedata.base", base); after.ApplyFile("md_test.mod", back);
    const md::fcs::GameData* w = after.Get(sword_id);
    CHECK(w && w->name == "MD renamed weapon");
    CHECK(w && w->floats.count(field) && std::abs(w->floats.at(field) - (old_v + 1.5f)) < 1e-4f);
    const md::fcs::GameData* ni = after.Get(item);
    CHECK(ni && ni->name == "MD test item" && ni->kind == 4);
    CHECK(after.OfType(2).size() == data.OfType(2).size() + 1);   // the copy
    size_t unchanged_diff = 0;                                      // every other record is as in the base
    for (const std::string& id : data.order) { if (id == sword_id) continue; const md::fcs::GameData* a = data.Get(id); const md::fcs::GameData* b = after.Get(id); if (a && b && (a->floats != b->floats || a->ints != b->ints || a->name != b->name)) ++unchanged_diff; }
    CHECK(unchanged_diff == 0);
    // the file is small: only the changes
    std::printf("F5: mod of %zu records, %lld bytes, applied on gamedata.base: sword '%s' %s %g -> %g\n", back.records.size(), (long long)std::filesystem::file_size(path), w ? w->name.c_str() : "?", field.c_str(), old_v, w && w->floats.count(field) ? w->floats.at(field) : 0.f);
    std::string err2;
    CHECK(!WriteMod(kd + "/data/gamedata.base", ed, schema, info, err2, kd + "/data"));   // never over a game file
    std::filesystem::remove(path);
}

// F6 on the install: every package / dialogue of gamedata.base + Dialogue.mod builds a tree; an edited dialogue survives WriteMod
static void RealDialogTest(const std::string& kd, Schema& schema) {
    md::fcs::GameDataContainer data; md::fcs::Error e;
    for (const char* n : {"gamedata.base", "Dialogue.mod"}) {
        md::fcs::File f; if (!md::fcs::ReadFile(kd + "/data/" + n, f, e)) { std::printf("skip F6 real: %s\n", n); return; }
        data.ApplyFile(n, f);
    }
    {   // merge on the install: gamedata.base + (Dialogue.mod imported as edits, written, applied) == gamedata.base + Dialogue.mod
        md::fcs::File b0, dm0; md::fcs::GameDataContainer only_base;
        if (md::fcs::ReadFile(kd + "/data/gamedata.base", b0, e) && md::fcs::ReadFile(kd + "/data/Dialogue.mod", dm0, e)) {
            only_base.ApplyFile("gamedata.base", b0);
            EditSession m(schema, only_base, "md_test.mod"); size_t sk = 0; ModInfo mi;
            CHECK(m.ImportFile(dm0, &sk));
            md::fcs::File out = BuildMod(m, schema, mi);
            md::fcs::GameDataContainer viaedit; viaedit.ApplyFile("gamedata.base", b0); viaedit.ApplyFile("md_test.mod", out);
            auto sorted_cats = [](md::fcs::GameData g) { std::sort(g.references.begin(), g.references.end()); return g.references; };   // category order only is not kept (file order inside a category is)
            auto sorted_refs = [](md::fcs::GameData g) { for (auto& c : g.references) std::sort(c.second.begin(), c.second.end()); std::sort(g.references.begin(), g.references.end()); return g.references; };
            size_t order_only = 0; for (const std::string& id : data.order) { const auto* x = data.Get(id); const auto* y = viaedit.Get(id); if (x && y && sorted_cats(*x) != sorted_cats(*y) && sorted_refs(*x) == sorted_refs(*y)) ++order_only; }
            std::printf("  reference lists differing only in order: %zu\n", order_only);
            size_t diff = 0; for (const std::string& id : data.order) { const auto* x = data.Get(id); const auto* y = viaedit.Get(id); if (!x || !y || x->name != y->name || x->ints != y->ints || x->floats != y->floats || x->strings != y->strings || (sorted_cats(*x) != sorted_cats(*y))) { if (diff++ < 4 && x && y) { for (size_t ci = 0; ci < x->references.size() && ci < y->references.size(); ++ci) if (x->references[ci] != y->references[ci]) { std::printf("  cat %s/%s sizes %zu %zu first:", x->references[ci].first.c_str(), y->references[ci].first.c_str(), x->references[ci].second.size(), y->references[ci].second.size()); for (size_t k = 0; k < 3 && k < x->references[ci].second.size() && k < y->references[ci].second.size(); ++k) std::printf(" %s|%s", x->references[ci].second[k].first.c_str(), y->references[ci].second[k].first.c_str()); std::printf("\n"); } { std::printf("  differs %s: name %d ints %d floats %d strings %d refs %d\n", id.c_str(), x->name != y->name, x->ints != y->ints, x->floats != y->floats, x->strings != y->strings, x->references != y->references); for (auto& kv : x->strings) { auto f = y->strings.find(kv.first); if (f == y->strings.end() || f->second != kv.second) std::printf("    string %s: '%.40s' vs '%.40s'\n", kv.first.c_str(), kv.second.c_str(), f == y->strings.end() ? "<none>" : f->second.c_str()); } for (auto& kv : y->strings) if (!x->strings.count(kv.first)) std::printf("    extra %s\n", kv.first.c_str()); } } } }
            std::printf("F7 merge: Dialogue.mod -> %zu edited/new records, %zu vector fields skipped, %zu of %zu records differ from the plain load\n", out.records.size(), sk, diff, data.order.size());
            CHECK(diff == 0 && viaedit.order.size() == data.order.size());
        }
    }
    const int code_enums = AddCodeEnums(schema, "docs/research/kenshi_specs/fcs_code_enums.json");   // from the main repo's cwd; skipped elsewhere
    AddDialogActionFields(schema);
    EditSession ed(schema, data, "md_test.mod"); DialogModel dm(ed, schema, data);
    const auto dialogues = dm.AllDialogues();
    size_t nodes = 0, with_cond = 0, with_eff = 0, cycles = 0, texts = 0, deepest = 0;
    std::function<size_t(const DialogNode&)> depth = [&](const DialogNode& n) { size_t d = 0; for (auto& c : n.children) d = std::max(d, depth(c)); return d + 1; };
    std::function<void(const DialogNode&)> walk = [&](const DialogNode& n) {
        ++nodes; with_cond += !n.conditions.empty(); with_eff += !n.effects.empty(); cycles += n.cycle; texts += n.texts.size();
        for (auto& c : n.children) walk(c);
    };
    std::set<std::string> uniq; std::function<void(const std::string&)> reach = [&](const std::string& id) { if (!uniq.insert(id).second) return; for (const std::string& c : dm.ChildLines(id)) reach(c); };
    for (const std::string& d : dialogues) reach(d);
    for (const std::string& d : dialogues) { DialogNode t = dm.Tree(d, 12); walk(t); deepest = std::max(deepest, depth(t)); }
    CHECK(!dialogues.empty() && uniq.size() > dialogues.size() && !dm.Packages().empty());
    std::printf("F6 real: %zu packages, %zu dialogues, %zu distinct lines+dialogues; trees cut at depth 12: %zu nodes (%zu with conditions, %zu with effects, %zu cycles, %zu texts), deepest tree %zu, %d code enums\n",
                dm.Packages().size(), dialogues.size(), uniq.size(), nodes, with_cond, with_eff, cycles, texts, deepest, code_enums);
    if (code_enums == 0) return;
    // edit one real dialogue: a new reply, a condition, a new dialogue in a package
    std::string pick; for (const std::string& d : dialogues) if (dm.CountNodes(dm.Tree(d)) >= 2) { pick = d; break; }
    CHECK(!pick.empty());
    const size_t before = dm.CountNodes(dm.Tree(pick));
    const std::string line = dm.AddLine(pick, "MD test reply", "T_ME");
    CHECK(!line.empty() && !dm.AddCondition(line, "DC_RELATIONS", "T_TARGET", "CE_MORE_THAN", 10).empty());
    const std::string pkg = dm.Packages()[0];
    const size_t pkg_before = dm.DialoguesOf(pkg).size();
    const std::string nd = dm.NewDialogue("MD test dialogue", pkg, "EV_ANNOUNCEMENT");
    CHECK(!nd.empty() && !dm.AddLine(nd, "Hello", "T_ME").empty());
    std::string err; ModInfo info;
    const std::string path = (std::filesystem::temp_directory_path() / "md_f6_real.mod").string();
    CHECK(WriteMod(path, ed, schema, info, err, kd + "/data"));
    md::fcs::File back; CHECK(md::fcs::ReadFile(path, back, e));
    md::fcs::GameDataContainer after;
    for (const char* n : {"gamedata.base", "Dialogue.mod"}) { md::fcs::File f; md::fcs::ReadFile(kd + "/data/" + n, f, e); after.ApplyFile(n, f); }
    after.ApplyFile("md_test.mod", back);
    EditSession ed2(schema, after, "md_test.mod"); DialogModel dm2(ed2, schema, after);
    CHECK(dm2.CountNodes(dm2.Tree(pick)) == before + 1 && dm2.DialoguesOf(pkg).size() == pkg_before + 1 && dm2.AllDialogues().size() == dialogues.size() + 1);
    DialogNode t2 = dm2.Tree(pick); bool found = false;
    for (auto& c : t2.children) if (c.sid == line && c.texts.size() == 1 && c.texts[0] == "MD test reply" && c.conditions.size() == 1 && c.conditions[0].what == "DC_RELATIONS") found = true;
    CHECK(found);
    std::printf("F6 real: edited dialogue round-trips through the mod (%lld bytes, %zu records)\n", (long long)std::filesystem::file_size(path), back.records.size());
    std::filesystem::remove(path);
}

// MD_FCS_LIST_UNDESCRIBED=<file>: every (item type, field, value type) of gamedata.base that the schema does not describe, with a count and an example
static void ListUndescribed(const std::string& kd, const Schema& schema, const char* out_path) {
    md::fcs::File file; md::fcs::Error e;
    if (!md::fcs::ReadFile(kd + "/data/gamedata.base", file, e)) return;
    struct Row { size_t n = 0; std::string example; };
    std::map<std::tuple<std::string, std::string, std::string>, Row> rows;
    for (const md::fcs::Record& r : file.records) {
        const std::string type = md::fcs::ItemTypeName(r.kind);
        std::set<std::string> known; for (const Field* f : schema.FieldsOf(type)) known.insert(f->name);
        auto add = [&](const std::string& name, const char* vt, const std::string& ex) { if (known.count(name)) return; Row& row = rows[{type, name, vt}]; if (!row.n++) row.example = r.string_id + " = " + ex; };
        char b[48];
        for (const auto& kv : r.bools) add(kv.first, "bool", kv.second ? "true" : "false");
        for (const auto& kv : r.floats) { std::snprintf(b, sizeof b, "%g", kv.second); add(kv.first, "float", b); }
        for (const auto& kv : r.ints) add(kv.first, "int", std::to_string(kv.second));
        for (const auto& kv : r.vec3s) { std::snprintf(b, sizeof b, "%g %g %g", kv.second[0], kv.second[1], kv.second[2]); add(kv.first, "vec3", b); }
        for (const auto& kv : r.vec4s) { std::snprintf(b, sizeof b, "%g %g %g %g", kv.second[0], kv.second[1], kv.second[2], kv.second[3]); add(kv.first, "vec4", b); }
        for (const auto& kv : r.strings) add(kv.first, "string", kv.second.substr(0, 40));
        for (const auto& kv : r.files) add(kv.first, "file", kv.second.substr(0, 40));
        for (const auto& kv : r.references) add(kv.first, "refs", kv.second.empty() ? "" : kv.second[0].target);
    }
    FILE* o = std::fopen(out_path, "w"); if (!o) return;
    std::fprintf(o, "item type\tfield\tvalue type\trecords\texample\n");
    for (const auto& [k, v] : rows) std::fprintf(o, "%s\t%s\t%s\t%zu\t%s\n", std::get<0>(k).c_str(), std::get<1>(k).c_str(), std::get<2>(k).c_str(), v.n, v.example.c_str());
    std::fclose(o);
    std::printf("undescribed (type, field, value type) in gamedata.base: %zu -> %s\n", rows.size(), out_path);
}

// F7 on the install: what we extract from gamedata.base against the msgids of the shipped ru_RU gamedata.po
static void RealTranslationTest(const std::string& kd, const Schema& schema) {
    md::fcs::File b; md::fcs::Error e;
    if (!md::fcs::ReadFile(kd + "/data/gamedata.base", b, e)) return;
    std::map<std::string, std::string> po;
    if (!ReadPo(kd + "/locale/ru_RU/gamedata.po", po)) { std::printf("skip translation: no ru_RU/gamedata.po\n"); return; }
    md::fcs::GameDataContainer data; data.ApplyFile("gamedata.base", b);
    DataBrowser br(schema, data);
    const auto ex = ExtractStrings(schema, data, br);
    std::set<std::string> ids, covered; for (const TrEntry& t : ex) { ids.insert(t.msgid); if (po.count(t.msgid)) covered.insert(t.msgid); }
    size_t translated = 0; for (const auto& kv : po) if (!kv.second.empty()) ++translated;
    std::printf("F7 translation: %zu strings from gamedata.base (%zu distinct); ru_RU gamedata.po has %zu msgids (%zu translated); %zu of our distinct strings are in it (%.1f%%)\n",
                ex.size(), ids.size(), po.size(), translated, covered.size(), ids.empty() ? 0.0 : 100.0 * covered.size() / ids.size());
    { std::map<std::string, std::pair<size_t, size_t>> by; std::map<std::string, std::string> ex1; for (const TrEntry& t : ex) { auto& c = by[t.type + (t.field.empty() ? " name" : " " + t.field)]; ++c.first; if (!po.count(t.msgid)) { ++c.second; ex1[t.type + (t.field.empty() ? " name" : " " + t.field)] = t.msgid.substr(0, 40); } }
      for (auto& kv : by) if (kv.second.second) std::printf("   missing %-32s %zu of %zu  e.g. '%s'\n", kv.first.c_str(), kv.second.second, kv.second.first, ex1[kv.first].c_str()); }
    CHECK(!ids.empty() && covered.size() * 2 > ids.size());   // plausibility only, not parity: the shipped .po is older than the data and ALL also gives non-text fields (bone names, prefixes)
}

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

// F4: references, instances, new / copied / removed records, OWNED
static void RefTest() {
    W f; f.i32(16).i32(1).str("a").str("d").str("").str("").i32(10).i32(3);
    auto rec = [&](int kind, int id, const std::string& name, const std::string& sid, const std::string& ref_target, bool inst) {
        f.i32(0).i32(kind).i32(id).str(name).str(sid).i32(0).i32(0).i32(0).i32(0).i32(0).i32(0).i32(0).i32(0);   // no bools .. no files
        if (ref_target.empty()) f.i32(0); else f.i32(1).str("parts").i32(1).str(ref_target).i32(1).i32(2).i32(3);
        if (inst) f.i32(1).str("i1").str("3-t").f32(1).f32(2).f32(3).f32(1).f32(0).f32(0).f32(0).i32(0); else f.i32(0);
    };
    rec(2, 1, "Sword", "1-w", "2-p", true);    // WEAPON owns part 2-p through `parts`
    rec(4, 2, "Blade", "2-p", "", false);      // ITEM
    rec(4, 3, "Target", "3-t", "", false);
    md::fcs::File file; md::fcs::Error e;
    CHECK(md::fcs::Parse(f.b.data(), f.b.size(), file, e));
    md::fcs::GameDataContainer data; data.ApplyFile("t.mod", file); data.id_counter = 100;
    Schema s; std::string err;
    CHECK(ParseText("[WEAPON]\nOWNED: parts\nparts: ITEM\nweight: 1.0\n[ITEM]\nweight: 1.0\n", s, err));
    EditSession ed(s, data, "md_test.mod");
    CHECK(ed.OwnerOf("2-p") == "1-w" && ed.OwnedOf("1-w").size() == 1);
    // references: change values, add, remove (a base link is removed with v2 = INT32_MAX), undo
    CHECK(ed.Refs("1-w", "parts").size() == 1 && ed.Refs("1-w", "parts")[0].second == (EditSession::Triple{1, 2, 3}));
    CHECK(ed.SetRef("1-w", "parts", "2-p", {7, 8, 9}) && ed.Refs("1-w", "parts")[0].second == (EditSession::Triple{7, 8, 9}));
    CHECK(ed.SetRef("1-w", "parts", "3-t", {0, 0, 0}) && ed.Refs("1-w", "parts").size() == 2);
    CHECK(ed.RemoveRef("1-w", "parts", "3-t") && ed.Refs("1-w", "parts").size() == 1 && !ed.RefLayer().at("1-w").at("parts").count("3-t"));   // a link only the layer had just disappears
    CHECK(ed.RemoveRef("1-w", "parts", "2-p") && ed.Refs("1-w", "parts").empty() && ed.RefLayer().at("1-w").at("parts").at("2-p")[2] == INT32_MAX);
    CHECK(!ed.RemoveRef("1-w", "parts", "nope"));
    CHECK(ed.Undo() && ed.Refs("1-w", "parts").size() == 1);
    {   // F7: checks and search
        EditSession c(s, data, "md_test.mod");
        CHECK(CheckMod(c, s, data).empty());
        c.SetRef("1-w", "parts", "nope-id", {0, 0, 0});
        auto pr = CheckMod(c, s, data);
        CHECK(pr.size() == 1 && pr[0].level == Problem::Error && pr[0].field == "parts");
        c.Remove("2-p");   // 1-w still links to it
        pr = CheckMod(c, s, data);
        bool warn = false; for (auto& p : pr) if (p.record == "1-w" && p.target == "2-p" && p.level == Problem::Warning && p.fix == Problem::RemoveLink) warn = true;
        CHECK(warn);
        {   // fix action, changes list, referencing, find & replace, bulk set (own sessions)
            EditSession e(s, data, "e.mod"); e.SetRef("1-w", "parts", "nope-id", {0, 0, 0});
            auto pr2 = CheckMod(e, s, data);
            CHECK(pr2.size() == 1 && pr2[0].fix == Problem::RemoveLink && pr2[0].target == "nope-id" && ApplyFix(e, pr2[0]) && CheckMod(e, s, data).empty());
            CHECK(ChangeList(e).empty() || ChangeList(e)[0].state == "modified");
            e.SetName("1-w", "Renamed"); CHECK(ChangeList(e).size() == 1 && ChangeList(e)[0].what.find("renamed") != std::string::npos);
            auto rf = Referencing(e, data, "2-p"); CHECK(rf.size() == 1 && rf[0].string_id == "1-w" && rf[0].category == "parts");
            std::string e3; CHECK(SetMany(e, {"1-w", "2-p"}, "weight", "3", e3) == 2);
            auto h = FindReplace(e, data, "Renam", "Axe", "", false); CHECK(h.size() == 1 && h[0].before == "Renamed" && h[0].after == "Axeed" && e.NameOf("1-w") == "Renamed");
            h = FindReplace(e, data, "Renam", "Axe", "", true); CHECK(e.NameOf("1-w") == "Axeed");
        }
        CHECK(SearchAll(c, data, "swo").size() == 1 && SearchAll(c, data, "1-w").size() == 1 && SearchAll(c, data, "BLADE", "ITEM").empty() == true);   // 2-p removed
        // merge and cleanup: a mod written from session A imported into session B gives the same mod
        EditSession a(s, data, "a.mod"); std::string e2;
        const std::string n1 = a.NewRecord(2, "Merged weapon"); a.Set(n1, "weight", "9", e2); a.SetRef(n1, "parts", "3-t", {1, 2, 3}); a.SetName("1-w", "Merged name");
        ModInfo mi; md::fcs::File fa = BuildMod(a, s, mi);
        EditSession b(s, data, "b.mod");
        size_t sk = 99; CHECK(b.ImportFile(fa, &sk) && sk == 0);
        CHECK(b.NameOf("1-w") == "Merged name" && b.NameOf(n1) == "Merged weapon" && b.Refs(n1, "parts").size() == 1 && b.StateOfRecord(n1) == RecordState::New);
        md::fcs::File fb = BuildMod(b, s, mi);
        std::vector<uint8_t> ba, bb; md::fcs::Error se;
        CHECK(md::fcs::Serialize(fa, ba, se) && md::fcs::Serialize(fb, bb, se) && ba == bb);
        // cleanup: a new record that was removed again, a link to it
        const std::string tmp = b.NewRecord(2, "Temp"); b.SetRef("1-w", "parts", tmp, {0, 0, 0}); b.Remove(tmp);
        {   // a changed base link to a record that is gone becomes a removal marker
            EditSession z(s, data, "z.mod");
            z.SetRef("1-w", "parts", "2-p", {7, 8, 9}); z.Remove("2-p");
            CHECK(z.Cleanup() >= 1 && z.Refs("1-w", "parts").empty() && z.RefLayer().at("1-w").at("parts").at("2-p")[2] == INT32_MAX);
        }
        CHECK(b.Cleanup() >= 2 && b.StateOfRecord(tmp) != RecordState::New && b.Refs("1-w", "parts").size() == 1 && b.Cleanup() == 0);
        CHECK(b.Undo() && b.StateOfRecord(tmp) == RecordState::Removed);
        CHECK(SearchAll(c, data, "targ", "ITEM").size() == 1 && SearchAll(c, data, "targ", "WEAPON").empty());
    }
    // instances
    CHECK(ed.Instances("1-w").size() == 1);
    md::fcs::Instance ni; ni.id = "i2"; ni.target = "3-t"; ni.position[0] = 5;
    CHECK(ed.SetInstance("1-w", ni) && ed.Instances("1-w").size() == 2);
    CHECK(ed.RemoveInstance("1-w", "i1") && ed.Instances("1-w").size() == 1 && ed.InstanceLayer().at("1-w").at("i1").target.empty());
    CHECK(ed.RemoveInstance("1-w", "i2") && ed.Instances("1-w").empty());
    // new record: ids continue from the counter, the string id carries the mod name
    const std::string nr = ed.NewRecord(2, "Fresh");
    CHECK(nr == "101-md_test.mod" && ed.StateOfRecord(nr) == RecordState::New && ed.NameOf(nr) == "Fresh" && ed.Exists(nr));
    std::string err2;
    CHECK(ed.Set(nr, "weight", "4", err2) && ed.EditedText(nr, "weight", err2) && err2 == "4");
    CHECK(ed.LastId() == 101);
    // duplicate: the owned part is copied too and the copy points at the copy
    const std::string cp = ed.Duplicate("1-w", "Sword copy");
    CHECK(!cp.empty() && ed.NameOf(cp) == "Sword copy" && ed.StateOfRecord(cp) == RecordState::New);
    const auto cr = ed.Refs(cp, "parts");
    CHECK(cr.size() == 1 && cr[0].first != "2-p" && ed.NameOf(cr[0].first) == "Blade" && cr[0].second == (EditSession::Triple{7, 8, 9}));
    CHECK(ed.OwnerOf(cr[0].first) == cp);
    CHECK(ed.NewRecords().size() == 3);   // Fresh, Sword copy, Blade copy
    // remove: the owner and what it owns
    CHECK(ed.Remove("1-w") && !ed.Exists("1-w") && !ed.Exists("2-p") && ed.Exists("3-t") && ed.StateOfRecord("2-p") == RecordState::Removed);
    CHECK(ed.Restore("1-w") && ed.Exists("1-w") && !ed.Exists("2-p"));
    CHECK(ed.SetName("1-w", "Renamed") && ed.NameOf("1-w") == "Renamed" && ed.StateOfRecord("1-w") == RecordState::Modified);
    while (ed.Undo()) {}
    CHECK(ed.ModifiedCount() == 0 && ed.Exists("1-w") && ed.NameOf("1-w") == "Sword" && ed.TouchedRecords().empty() && ed.LastId() == 100);
}

// F6: dialogues on synthetic records (kinds: 73 package, 18 dialogue, 19 line, 31 action) with a synthetic code-enum file
static std::string DialogRec(W& f, int kind, int id, const std::string& name, const std::string& sid, std::vector<std::pair<std::string, int>> ints,
                             std::vector<std::pair<std::string, std::string>> strs, std::vector<std::pair<std::string, std::vector<std::pair<std::string, int>>>> refs) {
    f.i32(0).i32(kind).i32(id).str(name).str(sid).i32(0).i32(0).i32(0);
    f.i32((int)ints.size()); for (auto& i : ints) f.str(i.first).i32(i.second);
    f.i32(0).i32(0);
    f.i32((int)strs.size()); for (auto& x : strs) f.str(x.first).str(x.second);
    f.i32(0);
    f.i32((int)refs.size());
    for (auto& r : refs) { f.str(r.first).i32((int)r.second.size()); for (auto& t : r.second) f.str(t.first).i32(t.second).i32(0).i32(0); }
    f.i32(0);
    return sid;
}
static void DialogTest() {
    const std::string js = (std::filesystem::temp_directory_path() / "md_f6_enums.json").string();
    { FILE* o = std::fopen(js.c_str(), "w"); std::fputs(R"({"TalkerEnum":[["T_ME",0],["T_TARGET",1]],"ComparisonEnum":[["CE_EQUALS",0],["CE_LESS_THAN",1]],
        "DialogConditionEnum":[["DC_NONE",0],["DC_RELATIONS",1],["DC_PLAYERMONEY",2]],"DialogActionEnum":[["DA_NONE",0],["DA_TRADE",1]],
        "EventTriggerEnum":[["EV_NONE",0],["EV_PLAYER_TALK_TO_ME",1],["EV_ANNOUNCEMENT",2]]})", o); std::fclose(o); }
    W f; f.i32(16).i32(1).str("a").str("d").str("").str("").i32(10).i32(6);
    DialogRec(f, 73, 1, "Pack", "1-p", {}, {}, {{"dialogs", {{"2-d", 1}}}});
    DialogRec(f, 18, 2, "Hello", "2-d", {}, {}, {{"lines", {{"3-l", 100}}}, {"conditions", {{"5-c", 0}}}});
    DialogRec(f, 19, 3, "L1", "3-l", {{"speaker", 1}}, {{"text0", "Hi"}, {"text1", "Hey"}}, {{"lines", {{"4-l", 100}}}, {"effects", {{"6-e", 0}}}});
    DialogRec(f, 19, 4, "L2", "4-l", {{"speaker", 0}}, {{"text0", "Bye"}}, {});
    DialogRec(f, 31, 5, "c", "5-c", {{"condition name", 1}, {"who", 1}, {"compare by", 1}, {"tag", 20}}, {}, {});
    DialogRec(f, 31, 6, "e", "6-e", {{"action name", 1}, {"action value", 7}}, {}, {});
    md::fcs::File file; md::fcs::Error e;
    CHECK(md::fcs::Parse(f.b.data(), f.b.size(), file, e));
    md::fcs::GameDataContainer data; data.ApplyFile("t.mod", file); data.id_counter = 100;
    Schema s; std::string err;
    CHECK(ParseText("[DIALOGUE]\nOWNED: lines\n[DIALOGUE_LINE]\nOWNED: effects\ntext0: \"\" multiline looped\nspeaker: 0\n[DIALOGUE,DIALOGUE_LINE]\nOWNED: conditions\n", s, err));
    CHECK(AddCodeEnums(s, js) == 5 && s.FindEnum("TalkerEnum")->values.size() == 2);
    AddDialogActionFields(s); AddDialogActionFields(s);   // second call adds nothing
    CHECK(s.FieldsOf("DIALOG_ACTION").size() == 7 && s.FieldsOf("DIALOGUE_LINE").size() == 2 && s.FindField("DIALOGUE_LINE", "text7") && s.FindField("DIALOGUE_LINE", "text7")->name == "text0" && !s.FindField("DIALOGUE_LINE", "speaker7"));
    EditSession ed(s, data, "md_test.mod");
    DialogModel dm(ed, s, data);
    CHECK(dm.Packages().size() == 1 && dm.AllDialogues().size() == 1);
    const auto ds = dm.DialoguesOf("1-p");
    CHECK(ds.size() == 1 && ds[0].sid == "2-d" && ds[0].trigger_name == "EV_PLAYER_TALK_TO_ME");
    DialogNode t = dm.Tree("2-d");
    CHECK(dm.CountNodes(t) == 3 && t.conditions.size() == 1 && t.conditions[0].what == "DC_RELATIONS" && t.conditions[0].detail == "T_TARGET CE_LESS_THAN 20");
    CHECK(t.children.size() == 1 && t.children[0].speaker == "T_TARGET" && t.children[0].texts.size() == 2 && t.children[0].texts[1] == "Hey");
    CHECK(t.children[0].effects.size() == 1 && t.children[0].effects[0].what == "DA_TRADE" && t.children[0].effects[0].detail == "7");
    CHECK(t.children[0].children.size() == 1 && t.children[0].children[0].texts[0] == "Bye");
    // old files store `compare by` as text
    { EditSession e2(s, data, "x.mod"); DialogModel d2(e2, s, data); (void)d2; }
    // edits
    const std::string nl = dm.AddLine("4-l", "Wait!", "T_TARGET");
    CHECK(!nl.empty() && dm.AddLine("4-l", "x", "NOPE").empty());
    CHECK(dm.SetText("3-l", 0, "Hello there") && dm.SetText("3-l", 2, "Yo"));
    const std::string nc = dm.AddCondition("3-l", "DC_PLAYERMONEY", "T_ME", "CE_EQUALS", 500);
    const std::string ne = dm.AddEffect("4-l", "DA_TRADE", 3);
    CHECK(!nc.empty() && !ne.empty() && dm.AddCondition("3-l", "DC_NOPE", "T_ME", "CE_EQUALS", 1).empty());
    const std::string nd = dm.NewDialogue("Fresh", "1-p", "EV_ANNOUNCEMENT");
    CHECK(!nd.empty() && dm.DialoguesOf("1-p").size() == 2 && dm.AddLine(nd, "First", "T_ME") != "");
    CHECK(dm.AllDialogues().size() == 2);
    t = dm.Tree("2-d");
    CHECK(dm.CountNodes(t) == 4 && t.children[0].texts[0] == "Hello there" && t.children[0].texts.size() == 3 && t.children[0].conditions.size() == 1);
    CHECK(t.children[0].children[0].children.size() == 1 && t.children[0].children[0].children[0].texts[0] == "Wait!" && t.children[0].children[0].effects.size() == 1);
    // the mod
    const std::string path = (std::filesystem::temp_directory_path() / "md_f6_test.mod").string();
    ModInfo info; CHECK(WriteMod(path, ed, s, info, err));
    md::fcs::File back; CHECK(md::fcs::ReadFile(path, back, e));
    md::fcs::GameDataContainer after; after.ApplyFile("t.mod", file); after.ApplyFile("md_test.mod", back);
    EditSession ed2(s, after, "md_test.mod"); DialogModel dm2(ed2, s, after);
    DialogNode t2 = dm2.Tree("2-d");
    CHECK(dm2.CountNodes(t2) == 4 && t2.children[0].texts[2] == "Yo" && t2.children[0].children[0].children[0].texts[0] == "Wait!" && dm2.DialoguesOf("1-p").size() == 2);
    // remove a line: unlinked, and its effect owned by it goes with it
    CHECK(dm.RemoveLine("2-d", "3-l"));
    CHECK(dm.CountNodes(dm.Tree("2-d")) == 1 && !ed.Exists("3-l") && !ed.Exists("6-e"));
    CHECK(dm.RemoveAction("2-d", "conditions", "5-c") && dm.Tree("2-d").conditions.empty());
    while (ed.Undo()) {}
    CHECK(dm.CountNodes(dm.Tree("2-d")) == 3);
    std::filesystem::remove(path); std::filesystem::remove(js);
    CHECK(AddCodeEnums(s, "/nonexistent.json") == 0);
}

static void TranslationTest() {
    CHECK(PoUnescape(PoEscape("a\"b\\c\nd")) == "a\"b\\c\nd");
    const std::string po = (std::filesystem::temp_directory_path() / "md_tr.po").string();
    { FILE* o = std::fopen(po.c_str(), "w"); std::fputs("msgid \"\"\nmsgstr \"\"\n\"X: y\\n\"\n\n#. Type: ITEM\nmsgid \"Sword\"\nmsgstr \"Meh\"\n\nmsgid \"Long \"\n\"line\"\nmsgstr \"\"\n\"Dovge\"\n\" slovo\"\n", o); std::fclose(o); }
    std::map<std::string, std::string> m;
    CHECK(ReadPo(po, m) && m.size() == 2 && m["Sword"] == "Meh" && m["Long line"] == "Dovge slovo");
    std::filesystem::remove(po);
    W f; f.i32(16).i32(1).str("a").str("d").str("").str("").i32(10).i32(2);
    Rec(f, 1, "Zeta", "1-z", 1, 2.5f); Rec(f, 2, "Alpha", "2-a", 0, 1.0f);
    md::fcs::File file; md::fcs::Error e;
    CHECK(md::fcs::Parse(f.b.data(), f.b.size(), file, e));
    md::fcs::GameDataContainer data; data.ApplyFile("t.mod", file);
    Schema s; std::string err;
    CHECK(ParseText("[WEAPON]\nkind: 0\nlabel: \"\"\nTRANSLATE: ALL if kind is 1\n", s, err));
    DataBrowser br(s, data);
    auto ex = ExtractStrings(s, data, br);
    CHECK(ex.size() == 2 && ex[0].msgid == "Zeta" && ex[1].field == "label" && ex[1].msgid == "Zeta!");   // Alpha has kind 0: condition false
    const std::string pot = MakePot(ex);
    CHECK(pot.find("msgid \"Zeta!\"") != std::string::npos && pot.find("#. Type: WEAPON") != std::string::npos);
}

// F7 (FCS_SCHEMA section 8): looped variants on the card, fields outside the schema kept and read-only, Clean, `compare by` as text
static void OutsideSchemaTest() {
    W f; f.i32(16).i32(1).str("a").str("d").str("").str("").i32(10).i32(2);
    DialogRec(f, 19, 1, "L", "1-l", {{"speaker", 1}, {"old thing", 7}}, {{"text0", "A"}, {"text1", "B"}, {"text2", "C"}, {"obsolete", "x"}}, {});
    DialogRec(f, 31, 2, "c", "2-c", {{"condition name", 1}, {"who", 0}, {"tag", 5}}, {{"compare by", "<"}}, {});
    md::fcs::File file; md::fcs::Error e;
    CHECK(md::fcs::Parse(f.b.data(), f.b.size(), file, e));
    md::fcs::GameDataContainer data; data.ApplyFile("t.mod", file); data.id_counter = 10;
    Schema s; std::string err;
    CHECK(ParseText("[DIALOGUE_LINE]\ntext0: \"\" multiline looped\nspeaker: 0\n", s, err));
    AddDialogActionFields(s);
    EditSession ed(s, data, "md_test.mod"); DataBrowser br(s, data); br.SetEdits(&ed);
    CHECK(ed.Set("1-l", "text3", "D", err) && ed.Set("1-l", "text1", "B2", err));
    const auto card = br.Card("1-l");
    std::vector<std::string> names; for (const CardField& c : card) names.push_back(c.name);
    auto idx = [&](const char* n) { for (size_t i = 0; i < names.size(); ++i) if (names[i] == n) return (int)i; return -1; };
    CHECK(idx("text0") >= 0 && idx("text1") == idx("text0") + 1 && idx("text2") == idx("text0") + 2 && idx("text3") == idx("text0") + 3);   // variants follow their stem, in number order
    CHECK(card[idx("text1")].value == "B2" && card[idx("text1")].edited && card[idx("text2")].value == "C" && !card[idx("text2")].edited && card[idx("text3")].value == "D");
    CHECK(idx("obsolete") > idx("text3") && card[idx("obsolete")].readonly && card[idx("old thing")].readonly && card[idx("old thing")].value == "7" && card[idx("obsolete")].category.find("outside") != std::string::npos);
    // duplicate carries variants (base and edited)
    const std::string cp = ed.Duplicate("1-l", "copy");
    std::string t2; CHECK(ed.EditedText(cp, "text2", t2) && t2 == "C" && ed.EditedText(cp, "text3", t2) && t2 == "D" && ed.EditedText(cp, "text1", t2) && t2 == "B2");
    // Clean drops layer fields the schema does not describe (an imported one), keeps looped variants
    md::fcs::File imp; imp.header.filetype = 17; imp.header.version = 1; md::fcs::Record r; r.lead = 1; r.kind = 19; r.id = 50; r.string_id = "1-l"; r.name = "L"; r.datatype = 1;
    r.strings.emplace_back("junk", "z"); r.strings.emplace_back("text5", "E"); imp.records.push_back(r);
    CHECK(ed.ImportFile(imp));
    CHECK(ed.CleanOutsideSchema() == 1 && ed.EditedText("1-l", "text5", t2) && t2 == "E" && !ed.Edited("1-l", "junk"));
    DialogModel dm(ed, s, data);
    CHECK(dm.Tree("1-l").texts.size() == 4);   // text0..text3; text5 is behind a gap
    // `compare by` stored as text in an old file
    CHECK(ed.SetRef("1-l", "conditions", "2-c", {0, 0, 0}));
    const DialogNode n = dm.Shallow("1-l");
    CHECK(n.conditions.size() == 1 && n.conditions[0].detail == "0 1 5");
}

static void ConditionTest() {
    std::vector<CardField> card(3);
    card[0].name = "slot"; card[0].value = "AttachSlot.ATTACH_BODY";
    card[1].name = "single gender"; card[1].value = "false";
    card[2].name = "mode"; card[2].value = "DUAL_TEXTURE";
    CHECK(EvalCondition("\"slot\" is AttachSlot.ATTACH_BODY", card) && !EvalCondition("\"slot\" is AttachSlot.ATTACH_HEAD", card));
    CHECK(EvalCondition("\"single gender\" is False", card) && !EvalCondition("\"single gender\" is True", card));
    CHECK(EvalCondition("\"mode\" is MapFeatureMode.DUAL_TEXTURE or MapFeatureMode.DUAL_TRIPLANAR", card) && !EvalCondition("\"mode\" not MapFeatureMode.DUAL_TEXTURE or MapFeatureMode.X", card));
    card.push_back(CardField()); card.back().name = "ratio"; card.back().value = "1.5";
    card.push_back(CardField()); card.back().name = "file"; card.back().value = "foo.txt";
    CHECK(EvalCondition("\"ratio\" is 1.5", card) && !EvalCondition("\"ratio\" is 5", card));                       // floats are not cut at the dot
    CHECK(EvalCondition("\"slot\" is \"AttachSlot.ATTACH_BODY\"", card) && EvalCondition("\"file\" is foo.txt", card));   // quoted and unquoted forms agree
    CHECK(EvalCondition("\"mode\" not MapFeatureMode.TERRAIN", card) && EvalCondition("\"nope\" is X", card) && EvalCondition("garbage", card));
}

int main() {
    ConditionTest();
    OutsideSchemaTest();
    TranslationTest();
    RefTest();
    DialogTest();
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
            if (const char* lp = std::getenv("MD_FCS_LIST_UNDESCRIBED")) ListUndescribed(kd, real, lp);
            RealCardTest(kd, real); RealWriteTest(kd, real); RealDialogTest(kd, real);
            RealTranslationTest(kd, real);
            std::printf("real files: %zu sections, %zu fields (bool %zu float %zu int %zu string %zu colour %zu enum %zu null %zu file %zu reflist %zu refstring %zu), %zu enums, %zu top layout nodes\n",
                        real.sections.size(), nf, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], kinds[6], kinds[7], kinds[8], kinds[9], real.enums.size(), real.layout.size());
        }
        else { std::printf("FAIL real files: %s\n", err.c_str()); ++fails; }
    }
    std::printf(fails ? "FAILED\n" : "PASS\n");
    return fails ? 1 : 0;
}
