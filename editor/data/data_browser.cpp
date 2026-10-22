#include "data_browser.h"
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
    for (const Field* f : s.FieldsOf(type_name)) if (f->name == name) return f;
    return nullptr;
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
    switch (f->kind) {
        case FieldKind::Bool: { auto it = rec.bools.find(field); if (it != rec.bools.end()) out = it->second ? "true" : "false"; break; }
        case FieldKind::Float: { auto it = rec.floats.find(field); if (it != rec.floats.end()) out = Num(it->second); break; }
        case FieldKind::Int: case FieldKind::Color: { auto it = rec.ints.find(field); if (it != rec.ints.end()) out = std::to_string(it->second); break; }
        case FieldKind::Enum: { auto it = rec.ints.find(field); if (it != rec.ints.end()) out = EnumNameOf(schema_, f->enum_name, it->second); break; }
        case FieldKind::String: { auto it = rec.strings.find(field); if (it != rec.strings.end()) out = it->second; break; }
        case FieldKind::File: { auto it = rec.files.find(field); if (it != rec.files.end()) out = it->second; break; }
        case FieldKind::RefList: case FieldKind::RefString: {
            out.clear();
            if (const auto* refs = rec.Refs(field))
                for (const auto& r : *refs) { if (!out.empty()) out += ", "; out += r.first; }
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
        if (node.filter.empty() || Passes(*r, node.filter)) rows.push_back({ r->string_id, r->name });
    std::sort(rows.begin(), rows.end(), [](const ItemRow& a, const ItemRow& b) { return a.name != b.name ? a.name < b.name : a.string_id < b.string_id; });
    return rows;
}

std::vector<CardField> DataBrowser::Card(const std::string& string_id) const {
    std::vector<CardField> card;
    const md::fcs::GameData* rec = data_.Get(string_id);
    if (!rec) return card;
    for (const Field* f : schema_.FieldsOf(md::fcs::ItemTypeName(rec->kind))) {
        CardField c;
        c.category = f->category; c.name = f->name; c.type = TypeName(f->kind); c.def = f->default_text; c.description = f->description;
        FieldText(*rec, f->name, c.value);
        switch (f->kind) {
            case FieldKind::Bool: c.changed = rec->bools.count(f->name) != 0; break;
            case FieldKind::Float: c.changed = rec->floats.count(f->name) != 0; break;
            case FieldKind::Int: case FieldKind::Color: case FieldKind::Enum: c.changed = rec->ints.count(f->name) != 0; break;
            case FieldKind::String: c.changed = rec->strings.count(f->name) != 0; break;
            case FieldKind::File: c.changed = rec->files.count(f->name) != 0; break;
            default: c.changed = rec->Refs(f->name) != nullptr; break;
        }
        card.push_back(std::move(c));
    }
    return card;
}

}  // namespace md::fcsdef
