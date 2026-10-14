// IMPORT / EXPORT of the character editor: a `.bod2` (CHARACTER_APPEARANCE record, see bod2.h; field list from M:docs/characters.md "Body file"): `race` reference, `sex female`, strings
// `head` / `hair style` / `beard` / `idle stance`, vec3 `Skin Tone`, one float per slider (raw value), and the face pose weights keyed by the body mesh's pose names (FacePoseWeights).
#pragma once
#include "session.h"
#include <string>

namespace md::character {

bool ExportAppearance(const Session& s, const std::string& path, std::string& error);
bool ImportAppearance(Session& s, const std::string& path, std::string& error);

} // namespace md::character
