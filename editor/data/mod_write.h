// Builds a .mod (v17) from the edit layer (#196 F5): only what the layer changed. Written to a file of our own (never into the install's files, never over a game .mod).
#pragma once
#include "mod_edit.h"
#include <monkey_dust/io/fcs.h>
#include <string>

namespace md::fcsdef {

struct ModInfo {
    std::string author = "md", description, dependencies = "gamedata.base";   // dependencies: comma separated file names this mod builds on
    int32_t version = 1;
};

// The records of the layer as an FCS file: a record that exists in the loaded data is MODIFIES (bit 0, bit 1 when renamed) and holds only the changed fields,
// reference links (a removed link is v2 = INT32_MAX) and instances (a removed one has an empty target); a new record holds everything it has; a removed record
// holds the bool REMOVED = true. id_counter = the layer's last id. Records come out in string_id order.
md::fcs::File BuildMod(const EditSession& edits, const Schema& schema, const ModInfo& info);

// Build + serialise to `path`. Refuses a path inside `<install>/data` that names an existing file (never overwrite a game file) when `install_data_dir` is given.
bool WriteMod(const std::string& path, const EditSession& edits, const Schema& schema, const ModInfo& info, std::string& err, const std::string& install_data_dir = "");

}  // namespace md::fcsdef
