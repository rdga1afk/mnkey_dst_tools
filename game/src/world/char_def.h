#pragma once
#include <cstdint>
#include <monkey_dust/components/char_body_state.h>

static constexpr int CHARDEF_BODY_N = 18;
static constexpr int CHARDEF_FACE_N = 24;
static constexpr int CHARDEF_HAIR_N =  5;

// Hair mesh style names — game/data/hair/<name>.glb. Mirrors
// tools/editor/editor_char_preview_assets.cpp::s_hair_styles (same order/names,
// duplicated rather than shared across the editor/game boundary).
static constexpr int CHARDEF_HAIR_STYLE_N = 30;
static const char* const kHairStyles[CHARDEF_HAIR_STYLE_N] = {
    "hair01","hair02","hair03","hair04","hair05","hair06","hair07","hair08",
    "haircut_male01","haircut_male02","haircut_male03","haircut_male04","haircut_male05",
    "haircut_male06","haircut_male07","haircut_male08","haircut_male09","haircut_male10",
    "haircut_female01","haircut_female02","haircut_female03","haircut_female04",
    "haircut_female05","haircut_female06","haircut_female07","haircut_female08",
    "hairlongbald","frizzhair","nutfro","afrohair",
};

// Per-entity character definition loaded from .chardef JSON files.
// Mirrors tools/editor/character_editor.h::Def — same JSON format.
struct CharDef {
    char    name[32];
    uint8_t sex;         // 0=Male, 1=Female
    uint8_t race_row;
    float   body[CHARDEF_BODY_N];   // body sliders (neutral=100)
    float   face[CHARDEF_FACE_N];   // face sliders (neutral=100)
    float   hair_f[CHARDEF_HAIR_N];
    float   skin_rgb[3];
    float   hair_rgb[3];
    float   color_strength;
    int     hair_style;   // index into kHairStyles (char_def.h) — 0..CHARDEF_HAIR_STYLE_N-1

    CharDef();  // initializes to Kenshi neutral defaults

    // Load .chardef JSON into out. Returns false on I/O error (out unchanged).
    static bool LoadFromFile(const char* path, CharDef& out);
};

// Initialize CharBodyState::bone_scales from chardef body/face sliders.
// Replicates SetBoneScalesFromDef() from editor_char_preview_sdlgpu.h — identical formula.
// pos_scales are stored but not yet used in the game render path (stub: set from sliders).
void CharBodyState_InitFromDef(CharBodyState& out, const CharDef& def);
