#pragma once
#include <cstdint>
#include <cstring>

// ── CharacterDef ──────────────────────────────────────────────────────────────
// Persistent character appearance. JSON: CharDef_SaveJSON / CharDef_LoadJSON.
// GPU:   CharDef_SetSlot → CharDef_FlushGPU → AppearanceBuf (set=0, binding=4
//        in animated.vert: height/bulk scale + skin colour).

static constexpr int CHARDEF_MAX_MORPHS = 48;
static constexpr int CHARDEF_BODY_N     = 18;
static constexpr int CHARDEF_FACE_N     = 24;

struct CharacterDef {
    char  name[32]   = {};
    uint8_t sex      = 0;     // 0=male  1=female
    uint8_t race     = 0;     // 0=human 1=shek  2=hive ...
    uint8_t _pad[2]  = {};

    // Шар 0 — Kenshi-style body/face sliders (bone-scale customisation).
    // Defaults: slider=100 → no deformation.
    float body[CHARDEF_BODY_N] = {50,50,100,100,0,68,53,100,100,100,100,100,100,100,100,100,100,100};
    float face[CHARDEF_FACE_N] = {100,100,108,110,100,100,100,100,100,100,100,103,105,100,90,125,100,100,90,100,100,90,0,0};

    // Шар 1 — body shape  (computed from body[] on load; kept for GPU compat)
    float height     = 1.0f;  // [0.80, 1.20]  global Y scale in mesh space
    float bulk       = 1.0f;  // [0.70, 1.40]  global XZ scale

    // Шар 2 — colour
    float skin_r = 0.82f, skin_g = 0.65f, skin_b = 0.52f;  // base skin tint (linear RGB)
    float hair_r = 0.18f, hair_g = 0.12f, hair_b = 0.08f;  // hair colour (linear RGB)
    float color_strength = 0.55f;  // [0,1] tint intensity
    // Kenshi-style skintone adjust (applied in shader via colorize; RE: character.hlsl)
    float skintone_sat = 1.0f;  // [0.0, 2.0]  saturation multiplier  (1=neutral)
    float skintone_bri = 0.0f;  // [-0.3, 0.3] brightness additive    (0=neutral)

    // Шар 3 — morph weights (indexed by OGRE pose / proc-morph order)
    float   morph_weights[CHARDEF_MAX_MORPHS] = {};
    uint8_t morph_count = 0;
};

// GPU layout — std430, 32 bytes per NPC slot in AppearanceBuf.
struct GpuCharAppearance {
    float body_scale[4];   // x=height  y=bulk  z=skintone_sat  w=skintone_bri
    float skin_color[4];   // rgb=skin tint  a=strength [0,1]
};
static_assert(sizeof(GpuCharAppearance) == 32, "GpuCharAppearance size mismatch");

// ── JSON persistence (strstr-based, no external libs) ─────────────────────────
bool CharDef_SaveJSON(const CharacterDef& def, const char* path);
bool CharDef_LoadJSON(CharacterDef& def, const char* path);

// ── GPU buffer management ─────────────────────────────────────────────────────
void  CharDef_InitGPU(int max_slots);       // call once after GPU device ready
void  CharDef_SetSlot(int slot, const CharacterDef& def);
void  CharDef_FlushGPU(void* sdl_cmd);     // uploads dirty data inside cmd
void* CharDef_GetSDLBuffer();              // returns SDL_GPUBuffer*
