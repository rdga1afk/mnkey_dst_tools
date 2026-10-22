#include "mod_edit.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace md::fcsdef {
namespace {

bool EqualValue(const EditValue& a, const EditValue& b) { return a == b; }

std::string Num(float f) { char b[32]; std::snprintf(b, sizeof b, "%g", f); return b; }

bool ParseInt(const std::string& t, int64_t& v) {
    char* e = nullptr; v = std::strtoll(t.c_str(), &e, 10);
    return e != t.c_str() && !*e;
}

}  // namespace

const Field* EditSession::FindField(const std::string& sid, const std::string& field) const {
    const md::fcs::GameData* rec = base_.Get(sid);
    if (!rec) return nullptr;
    for (const Field* f : schema_.FieldsOf(md::fcs::ItemTypeName(rec->kind))) if (f->name == field) return f;
    return nullptr;
}

bool EditSession::BaseValue(const Field& f, const md::fcs::GameData& rec, EditValue& out) const {
    switch (f.kind) {
        case FieldKind::Bool: { auto it = rec.bools.find(f.name); if (it == rec.bools.end()) return false; out = it->second; return true; }
        case FieldKind::Float: { auto it = rec.floats.find(f.name); if (it == rec.floats.end()) return false; out = it->second; return true; }
        case FieldKind::Int: case FieldKind::Color: case FieldKind::Enum: { auto it = rec.ints.find(f.name); if (it == rec.ints.end()) return false; out = it->second; return true; }
        case FieldKind::String: { auto it = rec.strings.find(f.name); if (it == rec.strings.end()) return false; out = it->second; return true; }
        case FieldKind::File: { auto it = rec.files.find(f.name); if (it == rec.files.end()) return false; out = it->second; return true; }
        default: return false;
    }
}

bool EditSession::Parse(const Field& f, const std::string& text, EditValue& out, std::string& err) const {
    int64_t iv = 0;
    switch (f.kind) {
        case FieldKind::Bool:
            if (text == "true" || text == "True" || text == "1") { out = true; return true; }
            if (text == "false" || text == "False" || text == "0") { out = false; return true; }
            err = "expected true / false"; return false;
        case FieldKind::Float: {
            char* e = nullptr; const float v = std::strtof(text.c_str(), &e);
            if (e == text.c_str() || *e || !std::isfinite(v)) { err = "expected a number"; return false; }
            out = v; return true;
        }
        case FieldKind::Int:
            if (!ParseInt(text, iv) || iv < INT32_MIN || iv > INT32_MAX) { err = "expected an integer"; return false; }
            out = (int32_t)iv; return true;
        case FieldKind::Color: {
            if (text.size() == 7 && text[0] == '#') { char* e = nullptr; const unsigned long v = std::strtoul(text.c_str() + 1, &e, 16); if (!*e) { out = (int32_t)(v & 0xFFFFFF); return true; } }
            if (ParseInt(text, iv)) { out = (int32_t)(iv & 0xFFFFFF); return true; }
            err = "expected #RRGGBB"; return false;
        }
        case FieldKind::Enum: {
            const EnumDef* e = schema_.FindEnum(f.enum_name);
            if (ParseInt(text, iv)) { out = (int32_t)iv; return true; }
            if (!e) { err = "enum " + f.enum_name + " is not described (use the number)"; return false; }
            // `Enum.NAME`, `NAME`, and for a bitset `A|B|C`
            int32_t acc = 0; size_t pos = 0; bool any = false;
            while (pos <= text.size()) {
                size_t bar = f.bitset ? text.find('|', pos) : std::string::npos; if (bar == std::string::npos) bar = text.size();
                std::string part = text.substr(pos, bar - pos);
                while (!part.empty() && part.front() == ' ') part.erase(part.begin());
                while (!part.empty() && part.back() == ' ') part.pop_back();
                const size_t dot = part.find('.'); if (dot != std::string::npos) part = part.substr(dot + 1);
                bool found = false;   // bitset: bit number = the enum value (UNVERIFIED against the exe: shipped data only holds 0)
                for (const EnumValue& v : e->values) if (v.name == part) { if (f.bitset) acc |= (int32_t)(1u << (unsigned)v.value); else acc = v.value; found = true; break; }
                if (!found) { err = "unknown value '" + part + "' of " + f.enum_name; return false; }
                any = true; pos = bar + 1;
                if (!f.bitset) break;
            }
            if (!any) { err = "empty"; return false; }
            out = acc; return true;
        }
        case FieldKind::String: case FieldKind::File: out = text; return true;
        default: err = "this field kind (references) is edited as a list, not as text"; return false;
    }
}

std::string EditSession::Format(const Field& f, const EditValue& v) const {
    if (const bool* b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    if (const float* x = std::get_if<float>(&v)) return Num(*x);
    if (const int32_t* i = std::get_if<int32_t>(&v)) {
        if (f.kind == FieldKind::Enum) if (const EnumDef* e = schema_.FindEnum(f.enum_name)) for (const EnumValue& ev : e->values) if (ev.value == *i) return f.enum_name + "." + ev.name;
        return std::to_string(*i);
    }
    return std::get<std::string>(v);
}

void EditSession::Apply(const std::string& sid, const std::string& field, bool has, const EditValue& v) {
    if (has) edits_[sid][field] = v;
    else { auto it = edits_.find(sid); if (it != edits_.end()) { it->second.erase(field); if (it->second.empty()) edits_.erase(it); } }
}

bool EditSession::Set(const std::string& sid, const std::string& field, const std::string& text, std::string& err) {
    const md::fcs::GameData* rec = base_.Get(sid);
    if (!rec) { err = "no record " + sid; return false; }
    const Field* f = FindField(sid, field);
    if (!f) { err = "the schema has no field '" + field + "' for " + md::fcs::ItemTypeName(rec->kind); return false; }
    EditValue nv;
    if (!Parse(*f, text, nv, err)) return false;
    Change c; c.sid = sid; c.field = field;
    if (const EditValue* cur = Edited(sid, field)) { c.had_old = true; c.old_v = *cur; }
    EditValue bv;
    const bool same_as_base = BaseValue(*f, *rec, bv) && EqualValue(bv, nv);
    if (c.had_old && EqualValue(c.old_v, nv)) return true;   // no change
    if (!c.had_old && same_as_base) return true;
    c.has_new = !same_as_base; c.new_v = nv;
    Apply(sid, field, c.has_new, nv);
    undo_.push_back(std::move(c)); redo_.clear();
    return true;
}

bool EditSession::Revert(const std::string& sid, const std::string& field) {
    const EditValue* cur = Edited(sid, field);
    if (!cur) return false;
    Change c; c.sid = sid; c.field = field; c.had_old = true; c.old_v = *cur; c.has_new = false;
    Apply(sid, field, false, *cur);
    undo_.push_back(std::move(c)); redo_.clear();
    return true;
}

bool EditSession::Undo() {
    if (undo_.empty()) return false;
    Change c = undo_.back(); undo_.pop_back();
    Apply(c.sid, c.field, c.had_old, c.old_v);
    redo_.push_back(std::move(c));
    return true;
}

bool EditSession::Redo() {
    if (redo_.empty()) return false;
    Change c = redo_.back(); redo_.pop_back();
    Apply(c.sid, c.field, c.has_new, c.new_v);
    undo_.push_back(std::move(c));
    return true;
}

const EditValue* EditSession::Edited(const std::string& sid, const std::string& field) const {
    auto it = edits_.find(sid); if (it == edits_.end()) return nullptr;
    auto f = it->second.find(field); return f == it->second.end() ? nullptr : &f->second;
}

FieldState EditSession::StateOf(const std::string& sid, const std::string& field) const { return Edited(sid, field) ? FieldState::Modified : FieldState::Original; }

bool EditSession::EditedText(const std::string& sid, const std::string& field, std::string& out) const {
    const EditValue* v = Edited(sid, field);
    if (!v) return false;
    const Field* f = FindField(sid, field);
    if (!f) return false;
    out = Format(*f, *v);
    return true;
}

size_t EditSession::ModifiedCount() const { size_t n = 0; for (const auto& r : edits_) n += r.second.size(); return n; }

std::vector<std::pair<std::string, std::string>> EditSession::ModifiedFields() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& r : edits_) for (const auto& f : r.second) out.emplace_back(r.first, f.first);
    return out;
}

}  // namespace md::fcsdef
