#pragma once
// #119: face culling of a Kenshi (Ogre) material. Scripts: <KENSHI_DIR>/data/**/*.material; `material NAME : PARENT { ... cull_hardware none ... }`,
// the value is inherited from the parent unless the material declares its own; no declaration in the chain = Ogre's default (culled).
#include <string>

// true: the faces are drawn from both sides (cull_hardware none); a material that is not in any script is reported as two-sided (no change of the picture).
bool KenshiMaterialTwoSided(const std::string& name);
