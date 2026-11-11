#include "char_def.h"
#include <monkey_dust/render/char_customization.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── Kenshi neutral defaults (authoritative: tools/editor/character_editor.h) ──

static const float kBodyDef[CHARDEF_BODY_N] = {
     50.f,  50.f, 100.f, 100.f,  35.f,  68.f,  53.f, 100.f,
    100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f, 100.f
};
static const float kBodyLo[CHARDEF_BODY_N] = {
      0.f,  0.f, 80.f, 80.f,  0.f, 45.f, 25.f, 85.f,
     90.f, 70.f, 65.f, 85.f, 80.f, 70.f, 70.f, 75.f, 70.f, 80.f
};
static const float kBodyHi[CHARDEF_BODY_N] = {
    100.f, 100.f, 120.f, 120.f,  99.f,  90.f,  80.f, 115.f,
    110.f, 145.f, 130.f, 115.f, 140.f, 190.f, 130.f, 145.f, 130.f, 120.f
};

static const float kFaceDef[CHARDEF_FACE_N] = {
    100.f, 100.f, 108.f, 110.f, 100.f, 100.f, 100.f, 100.f,
    100.f, 100.f, 100.f, 103.f, 105.f, 100.f,  90.f, 125.f,
    100.f, 100.f,  90.f, 100.f, 100.f,  90.f,   0.f,   0.f
};
static const float kFaceLo[CHARDEF_FACE_N] = {
     90.f,  90.f,  65.f,  70.f,  90.f,  50.f,  80.f,  85.f,
     95.f,  50.f,  70.f,  90.f,  60.f,  90.f,  50.f,  50.f,
     85.f,  85.f,  40.f, -50.f,  20.f,  50.f, -100.f, -165.f
};
static const float kFaceHi[CHARDEF_FACE_N] = {
    110.f, 110.f, 150.f, 150.f, 110.f, 150.f, 120.f, 115.f,
    105.f, 150.f, 130.f, 115.f, 150.f, 110.f, 130.f, 200.f,
    115.f, 115.f, 140.f, 250.f, 180.f, 130.f, 100.f, 165.f
};

// ── CharDef ───────────────────────────────────────────────────────────────────

CharDef::CharDef() {
    memset(name, 0, sizeof(name));
    strncpy(name, "NPC", 4);
    sex            = 0;
    race_row       = 0;
    color_strength = 0.55f;
    skin_rgb[0] = 0.82f; skin_rgb[1] = 0.65f; skin_rgb[2] = 0.52f;
    hair_rgb[0] = 0.18f; hair_rgb[1] = 0.12f; hair_rgb[2] = 0.08f;
    hair_style = 0;
    for (int i = 0; i < CHARDEF_BODY_N; ++i) body[i]   = kBodyDef[i];
    for (int i = 0; i < CHARDEF_FACE_N; ++i) face[i]   = kFaceDef[i];
    for (int i = 0; i < CHARDEF_HAIR_N; ++i) hair_f[i] = 40.f;
}

// ── JSON parser (strstr-based, no external lib) ───────────────────────────────

static float pf(const char* b, const char* k) {
    const char* p = strstr(b, k); if (!p) return 0.f;
    p = strchr(p, ':'); return p ? (float)atof(p + 1) : 0.f;
}
static int pi(const char* b, const char* k) {
    const char* p = strstr(b, k); if (!p) return 0;
    p = strchr(p, ':'); return p ? atoi(p + 1) : 0;
}
static void ps(const char* b, const char* k, char* out, int sz) {
    const char* p = strstr(b, k); if (!p) { out[0] = '\0'; return; }
    p = strchr(p, ':'); if (!p) { out[0] = '\0'; return; }
    while (*p && (*p == ':' || *p == ' ' || *p == '"')) ++p;
    int n = 0; while (*p && *p != '"' && n < sz - 1) out[n++] = *p++;
    out[n] = '\0';
}
static void pa(const char* b, const char* k, float* arr, int n, const float* defs) {
    const char* p = strstr(b, k);
    if (p) {
        p = strchr(p, '['); if (p) { ++p;
            for (int i = 0; i < n; ++i) {
                while (*p && (*p == ' ' || *p == ',')) ++p;
                if (!*p || *p == ']') break;
                arr[i] = (float)atof(p);
                while (*p && *p != ',' && *p != ']') ++p;
            }
            return;
        }
    }
    for (int i = 0; i < n; ++i) arr[i] = defs[i];
}

bool CharDef::LoadFromFile(const char* path, CharDef& out) {
    FILE* f = fopen(path, "r"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16384) { fclose(f); return false; }
    char* buf = (char*)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    CharDef tmp;  // start from defaults
    ps(buf, "\"name\"", tmp.name, 32);
    tmp.sex      = (uint8_t)pi(buf, "\"sex\"");
    tmp.race_row = (uint8_t)pi(buf, "\"race_row\"");
    tmp.skin_rgb[0]    = pf(buf, "\"skin_r\"");
    tmp.skin_rgb[1]    = pf(buf, "\"skin_g\"");
    tmp.skin_rgb[2]    = pf(buf, "\"skin_b\"");
    tmp.hair_rgb[0]    = pf(buf, "\"hair_r\"");
    tmp.hair_rgb[1]    = pf(buf, "\"hair_g\"");
    tmp.hair_rgb[2]    = pf(buf, "\"hair_b\"");
    tmp.color_strength = pf(buf, "\"color_str\"");
    if (tmp.color_strength < 0.01f) tmp.color_strength = 0.55f;
    tmp.hair_style = pi(buf, "\"hair_style\"");
    if (tmp.hair_style < 0 || tmp.hair_style >= CHARDEF_HAIR_STYLE_N) tmp.hair_style = 0;

    pa(buf, "\"body\"",   tmp.body,   CHARDEF_BODY_N, kBodyDef);
    pa(buf, "\"face\"",   tmp.face,   CHARDEF_FACE_N, kFaceDef);
    pa(buf, "\"hair_f\"", tmp.hair_f, CHARDEF_HAIR_N, tmp.hair_f);

    // Clamp to valid Kenshi ranges
    for (int i = 0; i < CHARDEF_BODY_N; ++i)
        if (tmp.body[i] < kBodyLo[i] || tmp.body[i] > kBodyHi[i])
            tmp.body[i] = kBodyDef[i];
    for (int i = 0; i < CHARDEF_FACE_N; ++i)
        if (tmp.face[i] < kFaceLo[i] || tmp.face[i] > kFaceHi[i])
            tmp.face[i] = kFaceDef[i];

    free(buf);
    out = tmp;
    return true;
}

// ── CharBodyState_InitFromDef ─────────────────────────────────────────────────
// Delegates bone-scale math to CharCustomization_ComputeScales() (engine/,
// shared with the editor's SetBoneScalesFromDef()) instead of a hand-duplicated
// copy. This file used to carry its own re-derivation of the same formula,
// which had drifted from several bug fixes made only on the editor/engine side
// (hand width was Hn² instead of linear Hn, feet scaled with Height causing
// bloat, clavicle scaled with arm bulk causing a shoulder "sphere", no
// minimum-bulk clamp on arms causing T-pose collapse at low Arm Bulk, no
// minimum clamp on calf bulk causing near-zero LBS collapse) — see
// char_customization.cpp's comments for each fix. Bone indices are for
// md_human.glb (30-bone Kenshi skeleton); see CLAUDE.md.
//
// scales.rot[]/qrot_delta[] (editor's procedural posture S-curve) are
// intentionally NOT copied here — the game applies posture via a separate,
// animation-clip-driven mechanism (OzzAnimator::ApplyPostureDelta, threaded
// through CharBodyState::posture below), matching how the editor itself
// overrides/zeroes scales.rot[] for posture-affected bones once its own
// apply_slider() animation-delta step runs (SetBoneScalesFromDef in
// editor_char_preview_sdlgpu.cpp).
void CharBodyState_InitFromDef(CharBodyState& out, const CharDef& def) {
    out.Reset();  // start from identity; fills pos_scales too

    out.posture = def.body[4];  // raw slider value, NOT a scale — see CharBodyState::posture's doc comment

    CharScales scales;
    CharCustomization_ComputeScales(def.body, def.face, /*bone_count=*/30, scales);
    for (int b = 0; b < BODY_MAX_BONES; ++b) {
        out.bone_scales[b][0] = scales.bone[b][0];
        out.bone_scales[b][1] = scales.bone[b][1];
        out.bone_scales[b][2] = scales.bone[b][2];
        out.pos_scales[b][0]  = scales.pos[b][0];
        out.pos_scales[b][1]  = scales.pos[b][1];
        out.pos_scales[b][2]  = scales.pos[b][2];
    }
}
