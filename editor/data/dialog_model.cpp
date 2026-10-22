#include "dialog_model.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace md::fcsdef {

namespace {

constexpr int kMaxTextVariants = 64;   // text0..text33 exist in the shipped data

Section* FindSectionFor(Schema& s, const char* type) {
    for (Section& sec : s.sections) for (const std::string& t : sec.types) if (t == type) return &sec;
    return nullptr;
}
bool HasField(const Schema& s, const char* type, const std::string& name) {
    for (const Field* f : s.FieldsOf(type)) if (f->name == name) return true;
    return false;
}
Field IntField(const char* name, const char* cat) { Field f; f.name = name; f.category = cat; f.kind = FieldKind::Int; return f; }

// {"Enum": [["NAME", v], ...], ...}: only this shape, nothing else is parsed.
bool ParseCodeEnums(const std::string& t, std::vector<EnumDef>& out) {
    size_t i = 0;
    auto ws = [&] { while (i < t.size() && isspace((unsigned char)t[i])) ++i; };
    auto str = [&](std::string& s) {
        ws(); if (i >= t.size() || t[i] != '"') return false;
        size_t e = ++i; while (e < t.size() && t[e] != '"') { if (t[e] == '\\') ++e; ++e; }
        if (e >= t.size()) return false; s = t.substr(i, e - i); i = e + 1; return true;
    };
    auto ch = [&](char c) { ws(); if (i < t.size() && t[i] == c) { ++i; return true; } return false; };
    if (!ch('{')) return false;
    while (!ch('}')) {
        EnumDef e; if (!str(e.name) || !ch(':') || !ch('[')) return false;
        while (!ch(']')) {
            std::string n; ws();
            if (!ch('[') || !str(n) || !ch(',')) return false;
            ws(); char* end = nullptr; long v = strtol(t.c_str() + i, &end, 10);
            if (end == t.c_str() + i) return false;
            i = end - t.c_str();
            if (!ch(']')) return false;
            e.values.push_back({n, (int)v, ""});
            ch(',');
        }
        out.push_back(std::move(e));
        ch(',');
    }
    return true;
}

}  // namespace

void AddDialogActionFields(Schema& s) {
    if (!FindSectionFor(s, "DIALOG_ACTION")) { Section sec; sec.types = {"DIALOG_ACTION"}; s.sections.push_back(sec); }
    Section* a = FindSectionFor(s, "DIALOG_ACTION");
    for (const char* n : {"condition name", "action name", "compare by", "who", "tag", "action value"})
        if (!HasField(s, "DIALOG_ACTION", n)) a->fields.push_back(IntField(n, "Base"));
    if (!HasField(s, "DIALOG_ACTION", "stringvar")) { Field f; f.name = "stringvar"; f.category = "Base"; f.kind = FieldKind::String; a->fields.push_back(f); }
}

int AddCodeEnums(Schema& s, const std::string& path_in) {
    std::string path = path_in;
    std::ifstream in;
    if (!path.empty()) in.open(path);
    else if (const char* e = getenv("MD_FCS_ENUMS")) in.open(e);
    else for (const char* d : {"../docs/research/kenshi_specs/fcs_code_enums.json", "docs/research/kenshi_specs/fcs_code_enums.json"}) { in.open(d); if (in) break; in.clear(); }   // next to the private main repo, when tools is built inside it
    if (!in) return 0;
    std::stringstream ss; ss << in.rdbuf();
    std::vector<EnumDef> defs; if (!ParseCodeEnums(ss.str(), defs)) return 0;
    int n = 0;
    for (EnumDef& d : defs) {
        EnumDef* cur = nullptr;
        for (EnumDef& e : s.enums) if (e.name == d.name) { cur = &e; break; }
        if (!cur) { s.enums.push_back(d); ++n; continue; }
        if (!cur->values.empty()) continue;   // the .def lists it itself: that wins
        cur->values = d.values; ++n;
    }
    return n;
}

void DialogModel::Discard(const std::string& sid) { ed_.Remove(sid); ed_.Cleanup(); }   // a record whose set-up failed must not stay in the mod

std::string DialogModel::EnumName(const char* en, int value) const {
    if (const EnumDef* e = schema_.FindEnum(en)) for (const EnumValue& v : e->values) if (v.value == value) return v.name;
    return std::to_string(value);
}
bool DialogModel::EnumNumber(const char* en, const std::string& name, int& out) const {
    if (const EnumDef* e = schema_.FindEnum(en)) for (const auto& v : e->values) if (v.name == name) { out = v.value; return true; }
    char* end = nullptr; long v = strtol(name.c_str(), &end, 10);
    if (end && *end == 0 && !name.empty()) { out = (int)v; return true; }
    return false;
}

int32_t DialogModel::IntOf(const std::string& sid, const char* field, int32_t def) const {
    if (const EditValue* v = ed_.Edited(sid, field)) if (const int32_t* i = std::get_if<int32_t>(v)) return *i;
    if (const auto* r = base_.Get(sid)) { auto it = r->ints.find(field); if (it != r->ints.end()) return it->second; }
    return def;
}
bool DialogModel::StrOf(const std::string& sid, const std::string& field, std::string& out) const {
    if (const EditValue* v = ed_.Edited(sid, field)) if (const std::string* s = std::get_if<std::string>(v)) { out = *s; return true; }
    if (const auto* r = base_.Get(sid)) { auto it = r->strings.find(field); if (it != r->strings.end()) { out = it->second; return true; } }
    return false;
}

// `compare by`: a number, or in old files the text `==` / `<` / `>` (FCS_SCHEMA section 8)
int DialogModel::CompareOf(const std::string& sid) const {
    std::string s;
    if (!ed_.Edited(sid, "compare by") && StrOf(sid, "compare by", s)) return s == "<" ? 1 : s == ">" ? 2 : 0;
    return IntOf(sid, "compare by");
}

DialogAction DialogModel::Describe(const std::string& sid) const {
    DialogAction a; a.sid = sid;
    const int cond = IntOf(sid, "condition name"), act = IntOf(sid, "action name");
    const int cmp = CompareOf(sid);
    if (cond) {
        a.what = EnumName("DialogConditionEnum", cond);
        std::string sv; StrOf(sid, "stringvar", sv);
        a.detail = EnumName("TalkerEnum", IntOf(sid, "who")) + " " + EnumName("ComparisonEnum", cmp) + " " + std::to_string(IntOf(sid, "tag"));
        if (!sv.empty()) a.detail += " \"" + sv + "\"";
    } else {
        a.what = EnumName("DialogActionEnum", act);
        a.detail = std::to_string(IntOf(sid, "action value"));
    }
    return a;
}

namespace {
std::vector<std::string> ByName(const EditSession& ed, std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end(), [&](const std::string& a, const std::string& b) { return ed.NameOf(a) < ed.NameOf(b); });
    return ids;
}
}  // namespace

std::vector<std::string> DialogModel::Packages() const {
    std::vector<std::string> ids;
    for (const auto* r : base_.OfType(kItemDialoguePackage)) if (ed_.Exists(r->string_id)) ids.push_back(r->string_id);
    for (const std::string& n : ed_.NewRecords()) if (ed_.KindOf(n) == kItemDialoguePackage && ed_.Exists(n)) ids.push_back(n);
    return ByName(ed_, ids);
}
std::vector<std::string> DialogModel::AllDialogues() const {
    std::vector<std::string> ids;
    for (const auto* r : base_.OfType(kItemDialogue)) if (ed_.Exists(r->string_id)) ids.push_back(r->string_id);
    for (const std::string& n : ed_.NewRecords()) if (ed_.KindOf(n) == kItemDialogue && ed_.Exists(n)) ids.push_back(n);
    return ByName(ed_, ids);
}
std::vector<DialogRef> DialogModel::DialoguesOf(const std::string& package) const {
    std::vector<DialogRef> out;
    for (const auto& [t, v] : ed_.Refs(package, "dialogs")) {
        if (!ed_.Exists(t)) continue;
        out.push_back({t, ed_.NameOf(t), v[0], EnumName("EventTriggerEnum", v[0])});
    }
    return out;
}

DialogNode DialogModel::Tree(const std::string& root, int max_depth) const {
    std::vector<std::string> path;
    std::set<std::string> on_path;
    struct Rec {
        const DialogModel& m; std::vector<std::string>& path; std::set<std::string>& on;
        DialogNode Build(const std::string& sid, int depth) {
            DialogNode n; n.sid = sid;
            if (on.count(sid)) { n.cycle = true; return n; }
            on.insert(sid);
            if (m.ed_.KindOf(sid) == kItemDialogueLine) {
                n.speaker = m.EnumName("TalkerEnum", m.IntOf(sid, "speaker"));
                for (int i = 0; i < kMaxTextVariants; ++i) {
                    std::string t; if (!m.StrOf(sid, "text" + std::to_string(i), t)) { if (i == 0) continue; break; }
                    n.texts.push_back(t);
                }
                for (const auto& [e, v] : m.ed_.Refs(sid, "effects")) if (m.ed_.Exists(e)) n.effects.push_back(m.Describe(e));
            }
            for (const auto& [c, v] : m.ed_.Refs(sid, "conditions")) if (m.ed_.Exists(c)) n.conditions.push_back(m.Describe(c));
            if (depth < kMaxDepth) for (const auto& [l, v] : m.ed_.Refs(sid, "lines")) if (m.ed_.Exists(l)) n.children.push_back(Build(l, depth + 1));
            on.erase(sid);
            return n;
        }
        int kMaxDepth;
    } r{*this, path, on_path, max_depth};
    return r.Build(root, 0);
}

size_t DialogModel::CountNodes(const DialogNode& n) const {
    size_t c = 1; for (const auto& ch : n.children) c += CountNodes(ch); return c;
}

std::vector<std::string> DialogModel::ChildLines(const std::string& sid) const {
    std::vector<std::string> out;
    for (const auto& [l, v] : ed_.Refs(sid, "lines")) if (ed_.Exists(l)) out.push_back(l);
    return out;
}

std::string DialogModel::AddLine(const std::string& parent, const std::string& text, const std::string& speaker) {
    const int32_t pk = ed_.KindOf(parent);
    if (pk != kItemDialogue && pk != kItemDialogueLine) return "";
    int sp = 0; if (!EnumNumber("TalkerEnum", speaker, sp)) return "";
    const std::string sid = ed_.NewRecord(kItemDialogueLine, text.substr(0, 40));
    std::string err;
    if (!ed_.Set(sid, "text0", text, err) || !ed_.Set(sid, "speaker", std::to_string(sp), err)) { Discard(sid); return ""; }
    ed_.SetRef(parent, "lines", sid, {100, 0, 0});
    return sid;
}

bool DialogModel::RemoveLine(const std::string& parent, const std::string& line) {
    if (!ed_.Exists(line)) return false;
    if (!ed_.RemoveRef(parent, "lines", line)) return false;
    ed_.Remove(line);
    return true;
}

bool DialogModel::SetText(const std::string& line, int variant, const std::string& text) {
    if (ed_.KindOf(line) != kItemDialogueLine || variant < 0 || variant >= kMaxTextVariants) return false;
    std::string err; return ed_.Set(line, "text" + std::to_string(variant), text, err);
}

std::string DialogModel::AddCondition(const std::string& owner, const std::string& cond, const std::string& who, const std::string& cmp, int value) {
    const int32_t ok = ed_.KindOf(owner);
    if (ok != kItemDialogue && ok != kItemDialogueLine) return "";
    int c, w, m;
    if (!EnumNumber("DialogConditionEnum", cond, c) || !EnumNumber("TalkerEnum", who, w) || !EnumNumber("ComparisonEnum", cmp, m)) return "";
    const std::string sid = ed_.NewRecord(kItemDialogAction, cond);
    std::string err;
    if (!ed_.Set(sid, "condition name", std::to_string(c), err) || !ed_.Set(sid, "who", std::to_string(w), err) ||
        !ed_.Set(sid, "compare by", std::to_string(m), err) || !ed_.Set(sid, "tag", std::to_string(value), err)) { Discard(sid); return ""; }
    ed_.SetRef(owner, "conditions", sid, {0, 0, 0});
    return sid;
}

std::string DialogModel::AddEffect(const std::string& line, const std::string& action, int value) {
    if (ed_.KindOf(line) != kItemDialogueLine) return "";
    int a; if (!EnumNumber("DialogActionEnum", action, a)) return "";
    const std::string sid = ed_.NewRecord(kItemDialogAction, action);
    std::string err;
    if (!ed_.Set(sid, "action name", std::to_string(a), err) || !ed_.Set(sid, "action value", std::to_string(value), err)) { Discard(sid); return ""; }
    ed_.SetRef(line, "effects", sid, {0, 0, 0});
    return sid;
}

bool DialogModel::RemoveAction(const std::string& owner, const std::string& category, const std::string& action) {
    if (category != "conditions" && category != "effects") return false;
    if (!ed_.RemoveRef(owner, category, action)) return false;
    ed_.Remove(action);
    return true;
}

bool DialogModel::SetTrigger(const std::string& package, const std::string& dialogue, const std::string& trigger) {
    int t; if (!EnumNumber("EventTriggerEnum", trigger, t) || ed_.KindOf(package) != kItemDialoguePackage) return false;
    return ed_.SetRef(package, "dialogs", dialogue, {t, 0, 0});
}

std::string DialogModel::NewDialogue(const std::string& name, const std::string& package, const std::string& trigger) {
    int t; if (!EnumNumber("EventTriggerEnum", trigger, t) || ed_.KindOf(package) != kItemDialoguePackage) return "";
    const std::string sid = ed_.NewRecord(kItemDialogue, name);
    ed_.SetRef(package, "dialogs", sid, {t, 0, 0});
    return sid;
}

}  // namespace md::fcsdef
