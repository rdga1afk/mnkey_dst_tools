// Assets of a race read from the Kenshi install: the body mesh of a gender (it carries the face poses), its skeleton link, and which slider targets the mesh has.
// Files are found through md::KenshiResources (case-insensitive, by name). No file names are listed in code: they come from the RACE record (`male mesh`, `female mesh`).
#pragma once
#include "editor_limits.h"
#include "races.h"
#include <monkey_dust/io/kenshi_resources.h>
#include <monkey_dust/io/ogre_mesh.h>
#include <string>
#include <vector>

namespace md::character {

// Body mesh of `gender` for a race (a single-gender race, or a race with no female mesh, uses the male mesh). False + `error` when the file is not found or unreadable.
bool LoadBodyMesh(const md::KenshiResources& res, const RaceInfo& race, Gender gender, md::ogre::Mesh& out, std::string& rel_path, std::string& error);

// One line per (race, gender, slider with a target): does the body mesh have a pose with that name?
//   race_id|gender|limits_file|category|slider|target=<name>:<0|1>|opposite=<name>:<0|1>        (an absent target prints `-`, counted present)
//   mesh_missing|race_id|gender|<mesh file name>                                                  (mesh not found / unreadable)
std::string DumpPoseCoverage(const RaceData& races, const std::vector<EditorLimits>& limits, const md::KenshiResources& res);

} // namespace md::character
