// Checks before saving a mod and search over the loaded data (#196 F7). Problems are the ones the layer itself can show; each names the record and field.
#pragma once
#include "mod_edit.h"
#include <string>
#include <vector>

namespace md::fcsdef {

// FCS ErrorCode table (FCS_SCHEMA section 9): severity, code, and the resolution actions it offers (flags as in the FCS).
enum ErrorAction : unsigned { kDeleteItem = 1, kRevertItem = 2, kRemoveInvalidReferences = 4, kTakeOwnership = 8 };
struct Problem {
    enum Level { Info, Warning, Critical } level = Warning;
    std::string code;          // FCS ErrorCode name (UndefinedReference, ...); our own extras start with a lower-case word
    unsigned actions = 0;      // ErrorAction flags
    std::string record, field, what, target;
};
// Applies one resolution action to the problem's record: DeleteItem = Remove, RevertItem = drop every change of the record,
// RemoveInvalidReferences = remove each link of the record to a missing / removed / wrong-type target. TakeOwnership is not needed here (we keep no «missing base» records).
bool ApplyFix(EditSession& ed, const Schema& schema, const Problem& p, ErrorAction action);

// Touched records only. Codes: UndefinedReference / ReferenceToDeletedItem / ReferenceToIncorrectType (CRITICAL, RemoveInvalidReferences), TooManyReferences,
// MissingRequiredReference (WARNING), OrphanedDialogueObject (WARNING, DeleteItem), InvalidWhoEnum (CRITICAL), ItemAlreadyDefined (CRITICAL, RevertItem); extras: emptyName, enumOutOfRange, requiredNotSet.
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
