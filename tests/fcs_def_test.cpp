// Synthetic mini .def files (not Kenshi data) through the schema reader. With KENSHI_DIR set, also loads the real files and checks only that they parse.
#include "../editor/data/fcs_def.h"
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

int main() {
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
            std::printf("real files: %zu sections, %zu fields (bool %zu float %zu int %zu string %zu colour %zu enum %zu null %zu file %zu reflist %zu refstring %zu), %zu enums, %zu top layout nodes\n",
                        real.sections.size(), nf, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], kinds[6], kinds[7], kinds[8], kinds[9], real.enums.size(), real.layout.size());
        }
        else { std::printf("FAIL real files: %s\n", err.c_str()); ++fails; }
    }
    std::printf(fails ? "FAILED\n" : "PASS\n");
    return fails ? 1 : 0;
}
