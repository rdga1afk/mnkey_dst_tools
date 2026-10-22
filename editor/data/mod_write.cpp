#include "mod_write.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace md::fcsdef {
namespace {

const Field* FieldOf(const Schema& s, int32_t kind, const std::string& name) {
    for (const Field* f : s.FieldsOf(md::fcs::ItemTypeName(kind))) if (f->name == name) return f;
    return nullptr;
}

}  // namespace

md::fcs::File BuildMod(const EditSession& ed, const Schema& schema, const ModInfo& info) {
    md::fcs::File f;
    md::fcs::Header& h = f.header;
    h.filetype = md::fcs::FILETYPE_V17; h.version = info.version; h.author = info.author; h.description = info.description; h.dependencies = info.dependencies;
    h.tail = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };   // SaveCounter 1, last merge 0, no merge info (as Dialogue.mod)
    h.id_counter = ed.LastId();
    for (const std::string& sid : ed.TouchedRecords()) {
        md::fcs::Record r;
        r.lead = 1;   // computed by Serialize
        r.kind = ed.KindOf(sid); r.id = std::atoi(sid.c_str()); r.name = ed.NameOf(sid); r.string_id = sid;
        const std::vector<std::string> created = ed.NewRecords();
        const bool is_new = std::find(created.begin(), created.end(), sid) != created.end();
        r.datatype = is_new ? 0u : 1u;
        for (const auto& kv : ed.FieldLayer(sid)) {
            const Field* fl = FieldOf(schema, r.kind, kv.first);
            if (!fl) continue;
            if (const bool* b = std::get_if<bool>(&kv.second)) r.bools.emplace_back(kv.first, *b);
            else if (const float* x = std::get_if<float>(&kv.second)) r.floats.emplace_back(kv.first, *x);
            else if (const int32_t* i = std::get_if<int32_t>(&kv.second)) r.ints.emplace_back(kv.first, *i);
            else if (fl->kind == FieldKind::File) r.files.emplace_back(kv.first, std::get<std::string>(kv.second));
            else r.strings.emplace_back(kv.first, std::get<std::string>(kv.second));
        }
        if (!is_new && ed.IsRenamed(sid)) r.datatype |= 2u;
        if (ed.StateOfRecord(sid) == RecordState::Removed) r.bools.emplace_back("REMOVED", true);
        auto rl = ed.RefLayer().find(sid);
        if (rl != ed.RefLayer().end())
            for (const auto& cat : rl->second) {
                std::vector<md::fcs::Reference> refs;
                for (const auto& t : cat.second) { md::fcs::Reference ref; ref.target = t.first; for (int k = 0; k < 3; ++k) ref.values[k] = t.second[k]; refs.push_back(ref); }
                r.references.emplace_back(cat.first, std::move(refs));
            }
        auto il = ed.InstanceLayer().find(sid);
        if (il != ed.InstanceLayer().end()) for (const auto& kv : il->second) r.instances.push_back(kv.second);
        f.records.push_back(std::move(r));
    }
    return f;
}

bool WriteMod(const std::string& path, const EditSession& ed, const Schema& schema, const ModInfo& info, std::string& err, const std::string& install_data_dir) {
    namespace fs = std::filesystem;
    if (!install_data_dir.empty()) {
        std::error_code ec;
        const fs::path p = fs::weakly_canonical(path, ec), d = fs::weakly_canonical(install_data_dir, ec);
        if (p.parent_path() == d && fs::exists(p)) { err = "refusing to overwrite " + p.filename().string() + " of the install"; return false; }
    }
    md::fcs::Error e;
    if (!md::fcs::WriteFile(path, BuildMod(ed, schema, info), e)) { err = e.Message(); return false; }
    return true;
}

}  // namespace md::fcsdef
