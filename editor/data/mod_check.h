// Checks before saving a mod and search over the loaded data (#196 F7). Problems are the ones the layer itself can show; each names the record and field.
#pragma once
#include "mod_edit.h"
#include <string>
#include <vector>

namespace md::fcsdef {

struct Problem {
    enum Level { Error, Warning } level = Error;
    enum Fix { None, RemoveLink, SetName } fix = None;   // resolution the error window offers (FCS ErrorResolutionActions)
    std::string record, field, what, target;
};
// Applies the problem's resolution: RemoveLink drops the link `field` -> `target` of `record`; SetName gives the record the name "Unnamed". False when there is none.
bool ApplyFix(EditSession& ed, const Problem& p);

// Touched records only (new / modified / removed): a link to a record that does not exist or was removed (Error), more links than the field's SINGLE / N limit (Error),
// a REQUIRED field with no value on a new record (Warning), an empty name (Warning), an enum value the enum does not list (Warning), a removed record that
// other loaded records still link to (Warning).
std::vector<Problem> CheckMod(const EditSession& ed, const Schema& schema, const md::fcs::GameDataContainer& base);

struct Hit { std::string string_id, name, where; };   // where: "name", "id" or the string field that matched
// Case-insensitive substring over names, string_ids and string fields of every record (limit hits); optional item type name filter ("" = all).
std::vector<Hit> SearchAll(const EditSession& ed, const md::fcs::GameDataContainer& base, const std::string& text, const std::string& item_type = "", size_t limit = 500);

// ---- changes list, references to a record, bulk edits (F7) ----
struct ChangeRow { std::string string_id, name, state, what; };   // state: new / modified / removed; what: «fields: a, b; links: c; instances: 2; renamed»
std::vector<ChangeRow> ChangeList(const EditSession& ed);
struct RefHit { std::string string_id, name, category; };
// Who links to `target` (loaded data with the layer applied, and new records), by name.
std::vector<RefHit> Referencing(const EditSession& ed, const md::fcs::GameDataContainer& base, const std::string& target);
// `field` := text on every listed record; returns how many accepted it (one undo step per record; the rest left as they were).
size_t SetMany(EditSession& ed, const std::vector<std::string>& ids, const std::string& field, const std::string& text, std::string& err);
// Find and replace in the name and the string fields of records of `item_type` ("" = all): returns the hits (preview) and, when `apply`, changes them.
struct ReplaceHit { std::string string_id, field, before, after; };
std::vector<ReplaceHit> FindReplace(EditSession& ed, const md::fcs::GameDataContainer& base, const std::string& find, const std::string& with, const std::string& item_type, bool apply, size_t limit = 2000);

// Files the records point to (string `file` fields, loaded + edited) and whether a file of that name exists anywhere under `root` (the install). Missing first.
struct AssetRow { std::string file, kind_name; size_t records = 0; bool found = false; std::string example_record; };
std::vector<AssetRow> AssetList(const EditSession& ed, const md::fcs::GameDataContainer& base, const std::string& root, size_t limit = 5000);

}  // namespace md::fcsdef
