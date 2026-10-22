#include "mod_check.h"
#include <algorithm>
#include <cctype>

namespace md::fcsdef {

namespace {
std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
}

std::vector<Problem> CheckMod(const EditSession& ed, const Schema& schema, const md::fcs::GameDataContainer& base) {
    std::vector<Problem> out;
    for (const std::string& sid : ed.TouchedRecords()) {
        const RecordState st = ed.StateOfRecord(sid);
        if (st == RecordState::Removed) {   // loaded records that still point at it
            for (const std::string& id : base.order) {
                const md::fcs::GameData* r = base.Get(id);
                if (!r || id == sid || !ed.Exists(id)) continue;
                for (const auto& cat : r->references) for (const auto& t : cat.second)
                    if (t.first == sid && ed.Exists(sid) == false) {
                        bool still = false; for (const auto& x : ed.Refs(id, cat.first)) if (x.first == sid) still = true;
                        if (still) out.push_back({Problem::Warning, sid, cat.first, "removed, but " + ed.NameOf(id) + " still links to it"});
                    }
            }
            continue;
        }
        if (ed.NameOf(sid).empty()) out.push_back({Problem::Warning, sid, "name", "empty name"});
        const int32_t kind = ed.KindOf(sid);
        for (const Field* f : schema.FieldsOf(md::fcs::ItemTypeName(kind))) {
            if (f->kind == FieldKind::RefList || f->kind == FieldKind::RefString) {
                const auto refs = ed.Refs(sid, f->name);
                for (const auto& r : refs) if (!ed.Exists(r.first)) out.push_back({Problem::Error, sid, f->name, "link to a missing or removed record " + r.first});
                if (f->limit > 0 && (int)refs.size() > f->limit) out.push_back({Problem::Error, sid, f->name, std::to_string(refs.size()) + " links, at most " + std::to_string(f->limit)});
                if (f->required && refs.empty() && st == RecordState::New) out.push_back({Problem::Warning, sid, f->name, "required, empty"});
            } else if (f->required && st == RecordState::New && !ed.Edited(sid, f->name)) out.push_back({Problem::Warning, sid, f->name, "required, not set"});
            if (f->kind == FieldKind::Enum && !f->bitset) {
                const EnumDef* e = schema.FindEnum(f->enum_name);
                const EditValue* v = ed.Edited(sid, f->name);
                if (e && !e->values.empty() && v) if (const int32_t* i = std::get_if<int32_t>(v)) {
                    bool ok = false; for (const EnumValue& x : e->values) if (x.value == *i) ok = true;
                    if (!ok) out.push_back({Problem::Warning, sid, f->name, "value " + std::to_string(*i) + " is not in " + f->enum_name});
                }
            }
        }
    }
    return out;
}

std::vector<Hit> SearchAll(const EditSession& ed, const md::fcs::GameDataContainer& base, const std::string& text, const std::string& item_type, size_t limit) {
    std::vector<Hit> out;
    const std::string q = Lower(text);
    if (q.empty()) return out;
    auto consider = [&](const std::string& sid, const md::fcs::GameData* r) {
        if (!ed.Exists(sid)) return;
        if (!item_type.empty() && item_type != md::fcs::ItemTypeName(ed.KindOf(sid))) return;
        const std::string name = ed.NameOf(sid);
        const char* where = nullptr; std::string field;
        if (Lower(name).find(q) != std::string::npos) where = "name";
        else if (Lower(sid).find(q) != std::string::npos) where = "id";
        else {
            for (const auto& kv : ed.FieldLayer(sid)) if (const std::string* s = std::get_if<std::string>(&kv.second)) if (Lower(*s).find(q) != std::string::npos) { field = kv.first; break; }
            if (field.empty() && r) for (const auto& kv : r->strings) if (!ed.Edited(sid, kv.first) && Lower(kv.second).find(q) != std::string::npos) { field = kv.first; break; }
        }
        if (where) out.push_back({sid, name, where}); else if (!field.empty()) out.push_back({sid, name, field});
    };
    for (const std::string& id : base.order) { if (out.size() >= limit) break; consider(id, base.Get(id)); }
    for (const std::string& id : ed.NewRecords()) { if (out.size() >= limit) break; consider(id, nullptr); }
    return out;
}

}  // namespace md::fcsdef
