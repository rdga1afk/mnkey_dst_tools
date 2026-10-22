// Checks before saving a mod and search over the loaded data (#196 F7). Problems are the ones the layer itself can show; each names the record and field.
#pragma once
#include "mod_edit.h"
#include <string>
#include <vector>

namespace md::fcsdef {

struct Problem { enum Level { Error, Warning } level = Error; std::string record, field, what; };

// Touched records only (new / modified / removed): a link to a record that does not exist or was removed (Error), more links than the field's SINGLE / N limit (Error),
// a REQUIRED field with no value on a new record (Warning), an empty name (Warning), an enum value the enum does not list (Warning), a removed record that
// other loaded records still link to (Warning).
std::vector<Problem> CheckMod(const EditSession& ed, const Schema& schema, const md::fcs::GameDataContainer& base);

struct Hit { std::string string_id, name, where; };   // where: "name", "id" or the string field that matched
// Case-insensitive substring over names, string_ids and string fields of every record (limit hits); optional item type name filter ("" = all).
std::vector<Hit> SearchAll(const EditSession& ed, const md::fcs::GameDataContainer& base, const std::string& text, const std::string& item_type = "", size_t limit = 500);

}  // namespace md::fcsdef
