#include "mod_edit.h"
#include <algorithm>
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
                bool found = false;   // bitset: enum value N is bit 1 << N (FCS_SCHEMA section 9)
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
        if (f.kind == FieldKind::Enum && f.bitset) {   // names of the set bits joined with |; raw hex when the enum is unknown
            const EnumDef* e = schema_.FindEnum(f.enum_name);
            if (!e || e->values.empty()) { char b[16]; std::snprintf(b, sizeof b, "0x%X", (unsigned)*i); return b; }
            std::string s; for (const EnumValue& ev : e->values) if (ev.value >= 0 && ev.value < 32 && (((unsigned)*i >> ev.value) & 1u)) { if (!s.empty()) s += "|"; s += ev.name; }
            return s.empty() ? "0" : s;
        }
        if (f.kind == FieldKind::Enum) if (const EnumDef* e = schema_.FindEnum(f.enum_name)) for (const EnumValue& ev : e->values) if (ev.value == *i) return f.enum_name + "." + ev.name;
        return std::to_string(*i);
    }
    return std::get<std::string>(v);
}

void EditSession::Push() { undo_.push_back(layer_); redo_.clear(); }

int32_t EditSession::KindOf(const std::string& sid) const {
    auto c = layer_.created.find(sid); if (c != layer_.created.end()) return c->second.kind;
    const md::fcs::GameData* r = base_.Get(sid); return r ? r->kind : -1;
}

std::string EditSession::NameOf(const std::string& sid) const {
    auto n = layer_.names.find(sid); if (n != layer_.names.end()) return n->second;
    auto c = layer_.created.find(sid); if (c != layer_.created.end()) return c->second.name;
    const md::fcs::GameData* r = base_.Get(sid); return r ? r->name : std::string();
}

bool EditSession::Exists(const std::string& sid) const { return KindOf(sid) >= 0 && !layer_.removed.count(sid); }

bool EditSession::Set(const std::string& sid, const std::string& field, const std::string& text, std::string& err) {
    const int32_t kind = KindOf(sid);
    if (kind < 0) { err = "no record " + sid; return false; }
    const Field* f = nullptr;
    f = schema_.FindField(md::fcs::ItemTypeName(kind), field);
    if (!f) { err = std::string("the schema has no field '") + field + "' for " + md::fcs::ItemTypeName(kind); return false; }
    EditValue nv;
    if (!Parse(*f, text, nv, err)) return false;
    const EditValue* cur = Edited(sid, field);
    if (cur && EqualValue(*cur, nv)) return true;
    EditValue bv;
    const md::fcs::GameData* rec = base_.Get(sid);
    Field vf = *f; vf.name = field;   // a looped variant (text3) reads its own value
    const bool same_as_base = rec && BaseValue(vf, *rec, bv) && EqualValue(bv, nv);
    if (!cur && same_as_base) return true;
    Push();
    if (same_as_base) { auto it = layer_.fields.find(sid); it->second.erase(field); if (it->second.empty()) layer_.fields.erase(it); }
    else layer_.fields[sid][field] = nv;
    return true;
}

bool EditSession::Revert(const std::string& sid, const std::string& field) {
    if (!Edited(sid, field)) return false;
    Push();
    auto it = layer_.fields.find(sid); it->second.erase(field); if (it->second.empty()) layer_.fields.erase(it);
    return true;
}

bool EditSession::Undo() { if (undo_.empty()) return false; redo_.push_back(layer_); layer_ = undo_.back(); undo_.pop_back(); return true; }
bool EditSession::Redo() { if (redo_.empty()) return false; undo_.push_back(layer_); layer_ = redo_.back(); redo_.pop_back(); return true; }

const EditValue* EditSession::Edited(const std::string& sid, const std::string& field) const {
    auto it = layer_.fields.find(sid); if (it == layer_.fields.end()) return nullptr;
    auto f = it->second.find(field); return f == it->second.end() ? nullptr : &f->second;
}

FieldState EditSession::StateOf(const std::string& sid, const std::string& field) const { return Edited(sid, field) ? FieldState::Modified : FieldState::Original; }

bool EditSession::EditedText(const std::string& sid, const std::string& field, std::string& out) const {
    const EditValue* v = Edited(sid, field);
    if (!v) return false;
    const int32_t kind = KindOf(sid);
    if (const Field* f = schema_.FindField(md::fcs::ItemTypeName(kind), field)) { out = Format(*f, *v); return true; }
    return false;
}

size_t EditSession::ModifiedCount() const {
    size_t n = layer_.created.size() + layer_.removed.size() + layer_.names.size();
    for (const auto& r : layer_.fields) n += r.second.size();
    for (const auto& r : layer_.refs) for (const auto& c : r.second) n += c.second.size();
    for (const auto& r : layer_.insts) n += r.second.size();
    return n;
}

std::vector<std::pair<std::string, std::string>> EditSession::ModifiedFields() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& r : layer_.fields) for (const auto& f : r.second) out.emplace_back(r.first, f.first);
    return out;
}

// ---------------- F4 ----------------

std::string EditSession::NewRecord(int32_t kind, const std::string& name) {
    Push();
    NewRec n; n.kind = kind; n.name = name; n.id = ++layer_.last_id;
    const std::string sid = std::to_string(n.id) + "-" + mod_name_;
    layer_.created[sid] = n;
    return sid;
}

const std::vector<std::string>* EditSession::OwnedCategories(int32_t kind) const {
    const char* t = md::fcs::ItemTypeName(kind);
    for (const Section& s : schema_.sections) for (const std::string& x : s.types) if (x == t && !s.owned.empty()) return &s.owned;
    return nullptr;
}

std::vector<std::string> EditSession::OwnedOf(const std::string& sid) const {
    std::vector<std::string> out;
    const std::vector<std::string>* cats = OwnedCategories(KindOf(sid));
    if (!cats) return out;
    for (const std::string& c : *cats)
        for (const auto& r : Refs(sid, c)) if (Exists(r.first) && std::find(out.begin(), out.end(), r.first) == out.end()) out.push_back(r.first);
    return out;
}

std::string EditSession::OwnerOf(const std::string& sid) const {
    for (const auto& kv : layer_.created) for (const std::string& o : OwnedOf(kv.first)) if (o == sid) return kv.first;
    for (const std::string& id : base_.order) { if (!base_.Get(id)) continue; for (const std::string& o : OwnedOf(id)) if (o == sid) return id; }
    return "";
}

bool EditSession::Remove(const std::string& sid) {
    if (!Exists(sid)) return false;
    Push();
    std::vector<std::string> todo{sid};
    while (!todo.empty()) {
        const std::string cur = todo.back(); todo.pop_back();
        if (layer_.removed.count(cur)) continue;
        std::vector<std::string> own = OwnedOf(cur);
        layer_.removed.insert(cur);
        for (const std::string& o : own) todo.push_back(o);
    }
    return true;
}

bool EditSession::Restore(const std::string& sid) {
    if (!layer_.removed.count(sid)) return false;
    Push(); layer_.removed.erase(sid); return true;
}

RecordState EditSession::StateOfRecord(const std::string& sid) const {
    if (layer_.removed.count(sid)) return RecordState::Removed;
    if (layer_.created.count(sid)) return RecordState::New;
    if (layer_.fields.count(sid) || layer_.refs.count(sid) || layer_.insts.count(sid) || layer_.names.count(sid)) return RecordState::Modified;
    return RecordState::Original;
}

bool EditSession::SetName(const std::string& sid, const std::string& name) {
    if (!Exists(sid) || NameOf(sid) == name) return Exists(sid);
    Push();
    auto c = layer_.created.find(sid);
    if (c != layer_.created.end()) c->second.name = name; else layer_.names[sid] = name;
    return true;
}

bool EditSession::SetRef(const std::string& sid, const std::string& cat, const std::string& target, Triple v) {
    if (!Exists(sid)) return false;
    if (v[2] == kRemoved) return false;   // use RemoveRef
    for (const auto& r : Refs(sid, cat)) if (r.first == target && r.second == v) return true;
    Push(); PutRef(sid, cat, target, v); return true;
}

bool EditSession::RemoveRef(const std::string& sid, const std::string& cat, const std::string& target) {
    if (!Exists(sid)) return false;
    bool present = false; for (const auto& r : Refs(sid, cat)) if (r.first == target) present = true;
    if (!present) return false;
    Push();
    const md::fcs::GameData* rec = base_.Get(sid);
    const auto* b = rec ? rec->Refs(cat) : nullptr;
    bool in_base = false; if (b) for (const auto& r : *b) if (r.first == target) in_base = true;
    if (in_base) PutRef(sid, cat, target, Triple{kRemoved, kRemoved, kRemoved});
    else { auto& m = layer_.refs[sid][cat]; m.erase(target); if (m.empty()) layer_.refs[sid].erase(cat); if (layer_.refs[sid].empty()) layer_.refs.erase(sid); }
    return true;
}

void EditSession::PutRef(const std::string& sid, const std::string& cat, const std::string& target, Triple v) {
    auto& m = layer_.refs[sid][cat];
    if (!m.count(target)) layer_.ref_seq[sid][cat][target] = ++layer_.seq;
    m[target] = v;
}

std::vector<std::string> EditSession::RefOrder(const std::string& sid, const std::string& cat) const {
    std::vector<std::pair<uint32_t, std::string>> o;
    auto it = layer_.refs.find(sid); if (it == layer_.refs.end()) return {};
    auto c = it->second.find(cat); if (c == it->second.end()) return {};
    auto s = layer_.ref_seq.find(sid);
    for (const auto& kv : c->second) { uint32_t q = 0; if (s != layer_.ref_seq.end()) { auto sc = s->second.find(cat); if (sc != s->second.end()) { auto st = sc->second.find(kv.first); if (st != sc->second.end()) q = st->second; } } o.push_back({q, kv.first}); }
    std::sort(o.begin(), o.end());
    std::vector<std::string> r; for (auto& x : o) r.push_back(x.second);
    return r;
}

std::vector<std::pair<std::string, EditSession::Triple>> EditSession::Refs(const std::string& sid, const std::string& cat) const {
    std::vector<std::pair<std::string, Triple>> out;
    const md::fcs::GameData* rec = base_.Get(sid);
    if (rec) if (const auto* b = rec->Refs(cat)) for (const auto& r : *b) out.push_back({r.first, r.second});
    auto it = layer_.refs.find(sid);
    if (it != layer_.refs.end()) {
        auto c = it->second.find(cat);
        if (c != it->second.end())
            for (const std::string& tg : RefOrder(sid, cat)) {
                const auto kv = std::make_pair(tg, c->second.at(tg));
                auto pos = std::find_if(out.begin(), out.end(), [&](const auto& x) { return x.first == kv.first; });
                if (kv.second[2] == kRemoved) { if (pos != out.end()) out.erase(pos); }
                else if (pos != out.end()) pos->second = kv.second; else out.push_back({kv.first, kv.second});
            }
    }
    return out;
}

bool EditSession::SetInstance(const std::string& sid, const md::fcs::Instance& inst) {
    if (!Exists(sid) || inst.id.empty() || inst.target.empty()) return false;
    Push(); layer_.insts[sid][inst.id] = inst; return true;
}

bool EditSession::RemoveInstance(const std::string& sid, const std::string& inst_id) {
    if (!Exists(sid)) return false;
    bool present = false; for (const auto& i : Instances(sid)) if (i.id == inst_id) present = true;
    if (!present) return false;
    Push();
    const md::fcs::GameData* rec = base_.Get(sid);
    bool in_base = false; if (rec) for (const auto& i : rec->instances) if (i.id == inst_id) in_base = true;
    if (in_base) { md::fcs::Instance gone; gone.id = inst_id; layer_.insts[sid][inst_id] = gone; }   // empty target = removed
    else { layer_.insts[sid].erase(inst_id); if (layer_.insts[sid].empty()) layer_.insts.erase(sid); }
    return true;
}

std::vector<md::fcs::Instance> EditSession::Instances(const std::string& sid) const {
    std::vector<md::fcs::Instance> out;
    const md::fcs::GameData* rec = base_.Get(sid);
    if (rec) for (const auto& i : rec->instances) out.push_back(i);
    auto it = layer_.insts.find(sid);
    if (it != layer_.insts.end())
        for (const auto& kv : it->second) {
            auto pos = std::find_if(out.begin(), out.end(), [&](const auto& x) { return x.id == kv.first; });
            if (kv.second.target.empty()) { if (pos != out.end()) out.erase(pos); }
            else if (pos != out.end()) *pos = kv.second; else out.push_back(kv.second);
        }
    return out;
}

std::string EditSession::DuplicateInner(const std::string& sid, const std::string& name, std::map<std::string, std::string>& copied) {
    auto done = copied.find(sid); if (done != copied.end()) return done->second;
    NewRec n; n.kind = KindOf(sid); n.name = name; n.id = ++layer_.last_id;
    const std::string nsid = std::to_string(n.id) + "-" + mod_name_;
    layer_.created[nsid] = n; copied[sid] = nsid;
    const md::fcs::GameData* rec = base_.Get(sid);
    for (const Field* f : schema_.FieldsOf(md::fcs::ItemTypeName(n.kind))) {   // the effective value of every field the source has
        EditValue v;
        if (const EditValue* e = Edited(sid, f->name)) layer_.fields[nsid][f->name] = *e;
        else if (rec && BaseValue(*f, *rec, v)) layer_.fields[nsid][f->name] = v;
    }
    if (rec) {   // looped variants (text1, text2 ...) the source has
        const char* type = md::fcs::ItemTypeName(n.kind);
        auto variants = [&](const auto& m) { for (const auto& kv : m) if (!Edited(sid, kv.first)) { const Field* f = schema_.FindField(type, kv.first); if (f && f->name != kv.first) { Field vf = *f; vf.name = kv.first; EditValue v; if (BaseValue(vf, *rec, v)) layer_.fields[nsid][kv.first] = v; } } };
        variants(rec->strings); variants(rec->ints); variants(rec->floats); variants(rec->bools); variants(rec->files);
    }
    for (const auto& kv : FieldLayer(sid)) if (!layer_.fields[nsid].count(kv.first)) layer_.fields[nsid][kv.first] = kv.second;   // edited variants
    const std::vector<std::string>* own = OwnedCategories(n.kind);
    std::set<std::string> cats;
    if (rec) for (const auto& c : rec->references) cats.insert(c.first);
    auto ri = layer_.refs.find(sid); if (ri != layer_.refs.end()) for (const auto& c : ri->second) cats.insert(c.first);
    for (const std::string& cat : cats) {
        const bool owned_cat = own && std::find(own->begin(), own->end(), cat) != own->end();
        for (const auto& r : Refs(sid, cat)) {
            std::string target = r.first;
            if (owned_cat && Exists(target)) target = DuplicateInner(target, NameOf(target), copied);   // owned records are copied with their owner
            PutRef(nsid, cat, target, r.second);
        }
    }
    for (const md::fcs::Instance& i : Instances(sid)) { md::fcs::Instance c = i; layer_.insts[nsid][c.id] = c; }
    return nsid;
}

std::string EditSession::Duplicate(const std::string& sid, const std::string& name) {
    if (!Exists(sid)) return "";
    Push();
    std::map<std::string, std::string> copied;
    return DuplicateInner(sid, name, copied);
}

std::vector<std::string> EditSession::NewRecords() const { std::vector<std::string> o; for (const auto& kv : layer_.created) o.push_back(kv.first); return o; }
std::vector<std::string> EditSession::RemovedRecords() const { return std::vector<std::string>(layer_.removed.begin(), layer_.removed.end()); }

std::vector<std::string> EditSession::TouchedRecords() const {
    std::set<std::string> s;
    for (const auto& kv : layer_.fields) s.insert(kv.first);
    for (const auto& kv : layer_.refs) s.insert(kv.first);
    for (const auto& kv : layer_.insts) s.insert(kv.first);
    for (const auto& kv : layer_.created) s.insert(kv.first);
    for (const auto& kv : layer_.names) s.insert(kv.first);
    for (const std::string& r : layer_.removed) s.insert(r);
    return std::vector<std::string>(s.begin(), s.end());
}

bool EditSession::ImportFile(const md::fcs::File& file, size_t* skipped) {
    Push();
    size_t skip = 0;
    for (const md::fcs::Record& r : file.records) {
        const std::string& sid = r.string_id;
        const bool in_base = base_.Get(sid) != nullptr;
        if (!in_base) {
            NewRec n; n.kind = r.kind; n.id = r.id; n.name = r.name;
            layer_.created[sid] = n;
            layer_.last_id = std::max(layer_.last_id, std::max(r.id, file.header.id_counter));
        } else if (r.name != base_.Get(sid)->name && (r.datatype & md::fcs::DATATYPE_SETS_NAME)) layer_.names[sid] = r.name;
        for (const auto& kv : r.bools) { if (kv.first == "REMOVED") { if (kv.second) layer_.removed.insert(sid); else layer_.removed.erase(sid); } else layer_.fields[sid][kv.first] = kv.second; }
        for (const auto& kv : r.floats) layer_.fields[sid][kv.first] = kv.second;
        for (const auto& kv : r.ints) layer_.fields[sid][kv.first] = kv.second;
        for (const auto& kv : r.strings) layer_.fields[sid][kv.first] = kv.second;
        for (const auto& kv : r.files) { layer_.fields[sid][kv.first] = kv.second; layer_.file_fields.insert({sid, kv.first}); }
        skip += r.vec3s.size() + r.vec4s.size();
        for (const auto& cat : r.references) for (const md::fcs::Reference& ref : cat.second) PutRef(sid, cat.first, ref.target, Triple{ref.values[0], ref.values[1], ref.values[2]});
        for (const md::fcs::Instance& in : r.instances) layer_.insts[sid][in.id] = in;
    }
    layer_.last_id = std::max(layer_.last_id, file.header.id_counter);
    if (skipped) *skipped = skip;
    return true;
}

bool EditSession::RevertRecord(const std::string& sid) {
    if (!layer_.fields.count(sid) && !layer_.refs.count(sid) && !layer_.insts.count(sid) && !layer_.names.count(sid) && !layer_.removed.count(sid) && !layer_.created.count(sid)) return false;
    Push();
    layer_.fields.erase(sid); layer_.refs.erase(sid); layer_.ref_seq.erase(sid); layer_.insts.erase(sid); layer_.names.erase(sid); layer_.removed.erase(sid); layer_.created.erase(sid);
    for (auto q = layer_.file_fields.begin(); q != layer_.file_fields.end();) q = q->first == sid ? layer_.file_fields.erase(q) : std::next(q);
    return true;
}

size_t EditSession::CleanOutsideSchema() {
    Push();
    size_t n = 0;
    for (auto it = layer_.fields.begin(); it != layer_.fields.end();) {
        const char* type = md::fcs::ItemTypeName(KindOf(it->first));
        for (auto f = it->second.begin(); f != it->second.end();) {
            if (!schema_.FindField(type, f->first)) { f = it->second.erase(f); ++n; } else ++f;
        }
        for (auto q = layer_.file_fields.begin(); q != layer_.file_fields.end();) q = (q->first == it->first && !it->second.count(q->second)) ? layer_.file_fields.erase(q) : std::next(q);
        it = it->second.empty() ? layer_.fields.erase(it) : std::next(it);
    }
    for (auto& rec : layer_.refs) {
        const char* type = md::fcs::ItemTypeName(KindOf(rec.first));
        for (auto c = rec.second.begin(); c != rec.second.end();) {
            const Field* f = schema_.FindField(type, c->first);
            if (!f || (f->kind != FieldKind::RefList && f->kind != FieldKind::RefString)) { n += c->second.size(); c = rec.second.erase(c); } else ++c;
        }
    }
    for (auto it = layer_.refs.begin(); it != layer_.refs.end();) it = it->second.empty() ? layer_.refs.erase(it) : std::next(it);
    if (n == 0) undo_.pop_back();
    return n;
}

size_t EditSession::Cleanup() {
    Push();
    size_t n = 0;
    for (auto it = layer_.created.begin(); it != layer_.created.end();) {
        if (layer_.removed.count(it->first)) {
            layer_.fields.erase(it->first); layer_.refs.erase(it->first); layer_.insts.erase(it->first); layer_.removed.erase(it->first);
            it = layer_.created.erase(it); ++n;
        } else ++it;
    }
    for (auto& rec : layer_.refs)
        for (auto& cat : rec.second)
            for (auto t = cat.second.begin(); t != cat.second.end();) {
                const bool marker = t->second[2] == kRemoved;
                if (!marker && !Exists(t->first)) {
                    const md::fcs::GameData* br = base_.Get(rec.first); const auto* bl = br ? br->Refs(cat.first) : nullptr;
                    bool in_base = false; if (bl) for (const auto& x : *bl) if (x.first == t->first) in_base = true;
                    if (in_base) { t->second = Triple{kRemoved, kRemoved, kRemoved}; ++t; }   // a base link to a gone record is removed with a marker, not just forgotten
                    else t = cat.second.erase(t);
                    ++n;
                } else ++t;
            }
    for (auto it = layer_.refs.begin(); it != layer_.refs.end();) {
        for (auto c = it->second.begin(); c != it->second.end();) c = c->second.empty() ? it->second.erase(c) : std::next(c);
        it = it->second.empty() ? layer_.refs.erase(it) : std::next(it);
    }
    for (auto it = layer_.fields.begin(); it != layer_.fields.end();) it = it->second.empty() ? layer_.fields.erase(it) : std::next(it);
    for (auto it = layer_.insts.begin(); it != layer_.insts.end();) it = it->second.empty() ? layer_.insts.erase(it) : std::next(it);
    if (n == 0) { undo_.pop_back(); }
    return n;
}

}  // namespace md::fcsdef
