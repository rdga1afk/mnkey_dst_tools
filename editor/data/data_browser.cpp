#include "data_browser.h"
#include <map>
#include <type_traits>
#include <algorithm>
#include <cstdio>
#include <sstream>

namespace md::fcsdef {
namespace {

const char* TypeName(FieldKind k) {
    switch (k) {
        case FieldKind::Bool: return "bool"; case FieldKind::Float: return "float"; case FieldKind::Int: return "int"; case FieldKind::String: return "string";
        case FieldKind::Color: return "colour"; case FieldKind::Enum: return "enum"; case FieldKind::Null: return "null"; case FieldKind::File: return "file";
        case FieldKind::RefList: return "refs"; case FieldKind::RefString: return "refstring";
    }
    return "?";
}

std::string Num(float f) { char b[32]; std::snprintf(b, sizeof b, "%g", f); return b; }

const Field* FindField(const Schema& s, const std::string& type_name, const std::string& name) {
    return s.FindField(type_name, name);
}

// the integer an enum value name stands for; false when the enum or the name is unknown
bool EnumValueOf(const Schema& s, const std::string& enum_name, const std::string& value, int& out) {
    const EnumDef* e = s.FindEnum(enum_name);
    if (!e) return false;
    for (const EnumValue& v : e->values) if (v.name == value) { out = v.value; return true; }
    return false;
}

std::string EnumNameOf(const Schema& s, const std::string& enum_name, int value) {
    if (const EnumDef* e = s.FindEnum(enum_name))
        for (const EnumValue& v : e->values) if (v.value == value) return enum_name + "." + v.name;
    return std::to_string(value);
}

std::string Lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

}  // namespace

bool DataBrowser::FieldText(const md::fcs::GameData& rec, const std::string& field, std::string& out) const {
    const Field* f = FindField(schema_, md::fcs::ItemTypeName(rec.kind), field);
    if (!f) return false;
    out = f->default_text;
    if (edits_ && edits_->EditedText(rec.string_id, field, out)) return true;
    switch (f->kind) {
        case FieldKind::Bool: { auto it = rec.bools.find(field); if (it != rec.bools.end()) out = it->second ? "true" : "false"; break; }
        case FieldKind::Float: { auto it = rec.floats.find(field); if (it != rec.floats.end()) out = Num(it->second); break; }
        case FieldKind::Int: case FieldKind::Color: { auto it = rec.ints.find(field); if (it != rec.ints.end()) out = std::to_string(it->second); break; }
        case FieldKind::Enum: {
            auto it = rec.ints.find(field); if (it == rec.ints.end()) break;
            const EnumDef* e = f->bitset ? schema_.FindEnum(f->enum_name) : nullptr;
            if (f->bitset) {   // enum value N is bit 1 << N (FCS_SCHEMA section 9): names of the set bits joined with |, raw hex without the enum
                if (!e || e->values.empty()) { char b[16]; std::snprintf(b, sizeof b, "0x%X", (unsigned)it->second); out = b; break; }
                out.clear(); for (const EnumValue& ev : e->values) if (ev.value >= 0 && ev.value < 32 && (((unsigned)it->second >> ev.value) & 1u)) { if (!out.empty()) out += "|"; out += ev.name; }
                if (out.empty()) out = "0";
                break;
            }
            out = EnumNameOf(schema_, f->enum_name, it->second); break;
        }
        case FieldKind::String: { auto it = rec.strings.find(field); if (it != rec.strings.end()) out = it->second; break; }
        case FieldKind::File: { auto it = rec.files.find(field); if (it != rec.files.end()) out = it->second; break; }
        case FieldKind::RefList: case FieldKind::RefString: {
            out.clear();
            std::vector<std::string> ids;
            if (edits_) for (const auto& r : edits_->Refs(rec.string_id, field)) ids.push_back(r.first);
            else if (const auto* refs = rec.Refs(field)) for (const auto& r : *refs) ids.push_back(r.first);
            for (const std::string& id : ids) { if (!out.empty()) out += ", "; std::string n = edits_ ? edits_->NameOf(id) : (data_.Get(id) ? data_.Get(id)->name : std::string()); out += n.empty() ? id : n; }   // the target's name, its id when it is not loaded
            break;
        }
        default: break;
    }
    return true;
}

bool DataBrowser::Passes(const md::fcs::GameData& rec, const std::string& filter) const {
    std::istringstream in(filter);
    std::string term;
    while (std::getline(in, term, ';')) {
        if (term.empty()) continue;
        std::string name, value; bool neg = false;
        size_t p = term.find("!=");
        if (p != std::string::npos) { neg = true; name = term.substr(0, p); value = term.substr(p + 2); }
        else if ((p = term.find("==")) != std::string::npos) { name = term.substr(0, p); value = term.substr(p + 2); }
        else if ((p = term.find('=')) != std::string::npos) { name = term.substr(0, p); value = term.substr(p + 1); }
        else continue;
        auto trim = [](std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); size_t a = 0; while (a < s.size() && s[a] == ' ') ++a; return s.substr(a); };
        name = trim(name); value = trim(value);
        std::string have;
        bool eq;
        if (!FieldText(rec, name, have)) eq = false;
        else {
            const Field* f = FindField(schema_, md::fcs::ItemTypeName(rec.kind), name);
            if (f->kind == FieldKind::Enum) {   // `building category==DOORS` : compare by the value of the enum name
                int want = 0, got = 0;
                const bool known = EnumValueOf(schema_, f->enum_name, value, want);
                const size_t dot = have.find('.');
                const bool have_known = dot != std::string::npos && EnumValueOf(schema_, f->enum_name, have.substr(dot + 1), got);
                eq = known && have_known ? want == got : Lower(have.substr(dot == std::string::npos ? 0 : dot + 1)) == Lower(value);
            } else eq = Lower(have) == Lower(value);
        }
        if (neg ? eq : !eq) return false;
    }
    return true;
}

std::vector<ItemRow> DataBrowser::Items(const LayoutNode& node) const {
    std::vector<ItemRow> rows;
    if (node.type.empty()) return rows;
    int kind = -1;
    for (int k = 0; k < md::fcs::ITEM_TYPE_COUNT; ++k) if (node.type == md::fcs::ItemTypeName(k)) { kind = k; break; }
    if (kind < 0) return rows;
    for (const md::fcs::GameData* r : data_.OfType(kind))
        if (node.filter.empty() || Passes(*r, node.filter)) {
            if (edits_) {
                const RecordState st = edits_->StateOfRecord(r->string_id);
                rows.push_back({ r->string_id, edits_->NameOf(r->string_id) + (st == RecordState::Removed ? " [removed]" : st == RecordState::Modified ? " *" : "") });
            } else rows.push_back({ r->string_id, r->name });
        }
    if (edits_ && node.filter.empty()) for (const std::string& n : edits_->NewRecords()) if (edits_->KindOf(n) == kind) rows.push_back({ n, edits_->NameOf(n) + " [new]" });
    std::sort(rows.begin(), rows.end(), [](const ItemRow& a, const ItemRow& b) { return a.name != b.name ? a.name < b.name : a.string_id < b.string_id; });
    return rows;
}

bool EvalCondition(const std::string& cond, const std::vector<CardField>& card) {
    if (cond.size() < 3 || cond[0] != '"') return true;
    const size_t q = cond.find('"', 1); if (q == std::string::npos) return true;
    const std::string field = cond.substr(1, q - 1);
    std::string rest = cond.substr(q + 1);
    auto lower = [](std::string s) { for (char& ch : s) ch = (char)tolower((unsigned char)ch); return s; };
    auto trim = [](std::string s) { size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t"); return a == std::string::npos ? std::string() : s.substr(a, b - a + 1); };
    rest = trim(rest);
    bool negate;
    if (rest.compare(0, 3, "is ") == 0) negate = false; else if (rest.compare(0, 4, "not ") == 0) negate = true; else return true;
    rest = trim(rest.substr(negate ? 4 : 3));
    const CardField* other = nullptr; for (const CardField& c : card) if (c.name == field) { other = &c; break; }
    if (!other) return true;
    auto bare = [&](std::string v) {   // unquote, lower-case, drop an `Enum.` prefix (identifier + dot + one more word) the same way on both sides
        v = trim(v); if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        v = lower(v); const size_t d = v.find('.');
        if (d == std::string::npos || d == 0 || v.find('.', d + 1) != std::string::npos || !isalpha((unsigned char)v[0]) || v.find(' ') != std::string::npos) return v;
        for (size_t i = 0; i < d; ++i) if (!isalnum((unsigned char)v[i]) && v[i] != '_') return v;
        const std::string tail = v.substr(d + 1); size_t k = 0; while (k < tail.size() && isdigit((unsigned char)tail[k])) ++k;
        return k == tail.size() ? v : tail;   // 1.5 and "foo.5" stay whole; Enum.NAME -> name
    };
    const std::string have = bare(other->value);
    bool any = false;
    for (size_t pos = 0; pos <= rest.size();) {
        size_t e = rest.find(" or ", pos); if (e == std::string::npos) e = rest.size();
        if (bare(rest.substr(pos, e - pos)) == have) any = true;
        pos = e + 4;
    }
    return negate ? !any : any;
}

std::vector<CardField> DataBrowser::Card(const std::string& string_id) const {
    std::vector<CardField> card;
    const md::fcs::GameData* rec = data_.Get(string_id);
    md::fcs::GameData tmp;   // a record only the edit layer has
    if (!rec && edits_ && edits_->KindOf(string_id) >= 0) { tmp.kind = edits_->KindOf(string_id); tmp.string_id = string_id; tmp.name = edits_->NameOf(string_id); rec = &tmp; }
    if (!rec) return card;
    for (const Field* f : schema_.FieldsOf(md::fcs::ItemTypeName(rec->kind))) {
        CardField c;
        c.category = f->category; c.name = f->name; c.type = TypeName(f->kind); c.def = f->default_text; c.description = f->description; c.bitset = f->bitset; c.enum_name = f->enum_name;
        FieldText(*rec, f->name, c.value);
        c.edited = edits_ && edits_->StateOf(string_id, f->name) == FieldState::Modified;
        switch (f->kind) {
            case FieldKind::Bool: c.changed = rec->bools.count(f->name) != 0; break;
            case FieldKind::Float: c.changed = rec->floats.count(f->name) != 0; break;
            case FieldKind::Int: case FieldKind::Color: case FieldKind::Enum: c.changed = rec->ints.count(f->name) != 0; break;
            case FieldKind::String: c.changed = rec->strings.count(f->name) != 0; break;
            case FieldKind::File: c.changed = rec->files.count(f->name) != 0; break;
            default: c.changed = edits_ ? !edits_->Refs(string_id, f->name).empty() : rec->Refs(f->name) != nullptr; break;
        }
        card.push_back(std::move(c));
    }
    size_t i = 0;
    for (const Field* f : schema_.FieldsOf(md::fcs::ItemTypeName(rec->kind))) { if (!f->condition.empty()) card[i].visible = EvalCondition(f->condition, card); ++i; }
    // looped variants (text1, text2 ...: FCS_SCHEMA section 8) right after their stem, and what the schema does not describe at the end, read-only
    const char* type = md::fcs::ItemTypeName(rec->kind);
    std::map<std::string, std::vector<std::string>> variants;   // stem0 -> variant names
    std::vector<CardField> outside;
    auto scan = [&](const auto& m, const char* vt) {
        for (const auto& kv : m) {
            const Field* f = schema_.FindField(type, kv.first);
            if (f && f->name != kv.first) { variants[f->name].push_back(kv.first); continue; }
            if (f) continue;
            CardField c; c.category = "Other fields (outside schema)"; c.name = kv.first; c.type = vt; c.readonly = true; c.changed = true;
            if constexpr (std::is_same_v<std::decay_t<decltype(kv.second)>, bool>) c.value = kv.second ? "true" : "false";
            else if constexpr (std::is_same_v<std::decay_t<decltype(kv.second)>, float>) c.value = Num(kv.second);
            else if constexpr (std::is_same_v<std::decay_t<decltype(kv.second)>, int32_t>) c.value = std::to_string(kv.second);
            else if constexpr (std::is_same_v<std::decay_t<decltype(kv.second)>, std::string>) c.value = kv.second;
            else { char b[96]; std::snprintf(b, sizeof b, "%g %g %g", (double)kv.second[0], (double)kv.second[1], (double)kv.second[2]); c.value = b; }
            outside.push_back(std::move(c));
        }
    };
    scan(rec->bools, "bool"); scan(rec->floats, "float"); scan(rec->ints, "int"); scan(rec->strings, "string"); scan(rec->files, "file"); scan(rec->vec3s, "vec3"); scan(rec->vec4s, "vec4");
    if (edits_) for (const auto& kv : edits_->FieldLayer(string_id)) { const Field* f = schema_.FindField(type, kv.first); if (f && f->name != kv.first && !rec->strings.count(kv.first) && !rec->ints.count(kv.first) && !rec->floats.count(kv.first) && !rec->bools.count(kv.first) && !rec->files.count(kv.first)) variants[f->name].push_back(kv.first); }
    for (const auto& r : rec->references) if (!schema_.FindField(type, r.first)) { CardField c; c.category = "Other fields (outside schema)"; c.name = r.first; c.type = "refs"; c.readonly = true; c.changed = true; c.value = std::to_string(r.second.size()) + " links"; outside.push_back(std::move(c)); }
    if (!variants.empty()) {
        std::vector<CardField> out;
        for (CardField& c : card) {
            out.push_back(c);
            auto v = variants.find(c.name); if (v == variants.end()) continue;
            std::sort(v->second.begin(), v->second.end(), [](const std::string& a, const std::string& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; });
            for (const std::string& name : v->second) {
                CardField x = c; x.name = name; x.value.clear(); FieldText(*rec, name, x.value);
                x.edited = edits_ && edits_->StateOf(string_id, name) == FieldState::Modified; x.changed = true; x.def.clear();
                out.push_back(std::move(x));
            }
        }
        card = std::move(out);
    }
    for (CardField& c : outside) card.push_back(std::move(c));
    return card;
}

std::vector<CardInstance> DataBrowser::Instances(const std::string& string_id) const {
    std::vector<CardInstance> out;
    const md::fcs::GameData* rec = data_.Get(string_id);
    std::vector<md::fcs::Instance> eff;
    if (edits_) eff = edits_->Instances(string_id); else if (rec) eff = rec->instances;
    if (!rec && eff.empty()) return out;
    for (const md::fcs::Instance& in : eff) {
        if (in.target.empty()) continue;
        CardInstance c; c.id = in.id; c.target_id = in.target; c.states = in.states;
        for (int k = 0; k < 3; ++k) c.pos[k] = in.position[k];
        const std::string tn = edits_ ? edits_->NameOf(in.target) : (data_.Get(in.target) ? data_.Get(in.target)->name : std::string());
        c.target_name = tn.empty() ? in.target : tn;
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace md::fcsdef
