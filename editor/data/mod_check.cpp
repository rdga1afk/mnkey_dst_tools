#include "mod_check.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <variant>

namespace md::fcsdef {

namespace {
std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
}

bool ApplyFix(EditSession& ed, const Problem& p) {
    if (p.fix == Problem::RemoveLink) return ed.RemoveRef(p.record, p.field, p.target);
    if (p.fix == Problem::SetName) return ed.SetName(p.record, "Unnamed");
    return false;
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
                        if (still) { Problem p{Problem::Warning, Problem::RemoveLink, id, cat.first, "removed record " + ed.NameOf(sid) + " is still linked from " + ed.NameOf(id), sid}; out.push_back(p); }
                    }
            }
            continue;
        }
        if (ed.NameOf(sid).empty()) out.push_back({Problem::Warning, Problem::SetName, sid, "name", "empty name", ""});
        const int32_t kind = ed.KindOf(sid);
        for (const Field* f : schema.FieldsOf(md::fcs::ItemTypeName(kind))) {
            if (f->kind == FieldKind::RefList || f->kind == FieldKind::RefString) {
                const auto refs = ed.Refs(sid, f->name);
                for (const auto& r : refs) if (!ed.Exists(r.first)) { Problem p{Problem::Error, Problem::RemoveLink, sid, f->name, "link to a missing or removed record " + r.first, r.first}; out.push_back(p); }
                if (f->limit > 0 && (int)refs.size() > f->limit) out.push_back({Problem::Error, Problem::None, sid, f->name, std::to_string(refs.size()) + " links, at most " + std::to_string(f->limit), ""});
                if (f->required && refs.empty() && st == RecordState::New) out.push_back({Problem::Warning, Problem::None, sid, f->name, "required, empty", ""});
            } else if (f->required && st == RecordState::New && !ed.Edited(sid, f->name)) out.push_back({Problem::Warning, Problem::None, sid, f->name, "required, not set", ""});
            if (f->kind == FieldKind::Enum && !f->bitset) {
                const EnumDef* e = schema.FindEnum(f->enum_name);
                const EditValue* v = ed.Edited(sid, f->name);
                if (e && !e->values.empty() && v) if (const int32_t* i = std::get_if<int32_t>(v)) {
                    bool ok = false; for (const EnumValue& x : e->values) if (x.value == *i) ok = true;
                    if (!ok) out.push_back({Problem::Warning, Problem::None, sid, f->name, "value " + std::to_string(*i) + " is not in " + f->enum_name, ""});
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

std::vector<ChangeRow> ChangeList(const EditSession& ed) {
    std::vector<ChangeRow> out;
    for (const std::string& sid : ed.TouchedRecords()) {
        ChangeRow r; r.string_id = sid; r.name = ed.NameOf(sid);
        const RecordState st = ed.StateOfRecord(sid);
        r.state = st == RecordState::New ? "new" : st == RecordState::Removed ? "removed" : "modified";
        std::string fields; for (const auto& kv : ed.FieldLayer(sid)) { if (!fields.empty()) fields += ", "; fields += kv.first; }
        std::string links; auto rl = ed.RefLayer().find(sid); if (rl != ed.RefLayer().end()) for (const auto& c : rl->second) { if (!links.empty()) links += ", "; links += c.first + "(" + std::to_string(c.second.size()) + ")"; }
        auto il = ed.InstanceLayer().find(sid);
        if (!fields.empty()) r.what += "fields: " + fields;
        if (!links.empty()) r.what += std::string(r.what.empty() ? "" : "; ") + "links: " + links;
        if (il != ed.InstanceLayer().end()) r.what += std::string(r.what.empty() ? "" : "; ") + "instances: " + std::to_string(il->second.size());
        if (ed.IsRenamed(sid)) r.what += std::string(r.what.empty() ? "" : "; ") + "renamed";
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<RefHit> Referencing(const EditSession& ed, const md::fcs::GameDataContainer& base, const std::string& target) {
    std::vector<RefHit> out;
    auto scan = [&](const std::string& id, const md::fcs::GameData* r) {
        if (id == target || !ed.Exists(id)) return;
        std::set<std::string> cats;
        if (r) for (const auto& c : r->references) cats.insert(c.first);
        auto rl = ed.RefLayer().find(id); if (rl != ed.RefLayer().end()) for (const auto& c : rl->second) cats.insert(c.first);
        for (const std::string& cat : cats) for (const auto& x : ed.Refs(id, cat)) if (x.first == target) out.push_back({id, ed.NameOf(id), cat});
    };
    for (const std::string& id : base.order) scan(id, base.Get(id));
    for (const std::string& id : ed.NewRecords()) scan(id, nullptr);
    return out;
}

size_t SetMany(EditSession& ed, const std::vector<std::string>& ids, const std::string& field, const std::string& text, std::string& err) {
    size_t n = 0;
    for (const std::string& id : ids) { std::string e; if (ed.Set(id, field, text, e)) ++n; else err = e; }
    return n;
}

std::vector<ReplaceHit> FindReplace(EditSession& ed, const md::fcs::GameDataContainer& base, const std::string& find, const std::string& with, const std::string& item_type, bool apply, size_t limit) {
    std::vector<ReplaceHit> out;
    if (find.empty()) return out;
    auto sub = [&](const std::string& s, std::string& res) { size_t p = s.find(find); if (p == std::string::npos) return false; res.clear(); size_t from = 0; while (p != std::string::npos) { res += s.substr(from, p - from) + with; from = p + find.size(); p = s.find(find, from); } res += s.substr(from); return true; };
    std::vector<std::string> ids = base.order; for (const std::string& n : ed.NewRecords()) ids.push_back(n);
    for (const std::string& id : ids) {
        if (out.size() >= limit) break;
        if (!ed.Exists(id)) continue;
        if (!item_type.empty() && item_type != md::fcs::ItemTypeName(ed.KindOf(id))) continue;
        std::string res;
        if (sub(ed.NameOf(id), res)) { out.push_back({id, "", ed.NameOf(id), res}); if (apply) ed.SetName(id, res); }
        const md::fcs::GameData* r = base.Get(id);
        std::set<std::string> fields; if (r) for (const auto& kv : r->strings) fields.insert(kv.first);
        for (const auto& kv : ed.FieldLayer(id)) if (std::holds_alternative<std::string>(kv.second)) fields.insert(kv.first);
        for (const std::string& f : fields) {
            std::string cur; const EditValue* e = ed.Edited(id, f);
            if (e) { if (const std::string* s = std::get_if<std::string>(e)) cur = *s; else continue; }
            else if (r) { auto it = r->strings.find(f); if (it == r->strings.end()) continue; cur = it->second; }
            if (sub(cur, res)) { out.push_back({id, f, cur, res}); if (apply) { std::string err; ed.Set(id, f, res, err); } }
        }
    }
    return out;
}

}  // namespace md::fcsdef
