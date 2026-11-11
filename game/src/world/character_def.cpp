#include "character_def.h"
#include <monkey_dust/render/ssbo.h>
#include <monkey_dust/platform/md_log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <SDL3/SDL_gpu.h>

// ── GPU buffer ────────────────────────────────────────────────────────────────
static SSBO              s_buf;
static GpuCharAppearance s_slots[500] = {};
static int               s_max        = 0;
static bool              s_dirty      = false;

static void fill_default(GpuCharAppearance& a) {
    a.body_scale[0] = 1.f; a.body_scale[1] = 1.f;
    a.body_scale[2] = 1.f; a.body_scale[3] = 1.f;
    a.skin_color[0] = 0.82f; a.skin_color[1] = 0.65f;
    a.skin_color[2] = 0.52f; a.skin_color[3] = 0.f;  // strength=0 → no tint by default
}

void CharDef_InitGPU(int max_slots) {
    s_max = (max_slots > 500) ? 500 : max_slots;
    for (int i = 0; i < s_max; ++i) fill_default(s_slots[i]);
    s_buf.Init(s_max * (int)sizeof(GpuCharAppearance),
               SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
    // Upload defaults synchronously so GPU buffer is never zero-initialized.
    // Zero body_scale would collapse all NPC vertices to origin on the first frame.
    s_buf.Upload(s_slots, s_max * (int)sizeof(GpuCharAppearance));
    s_dirty = false;
}

void CharDef_SetSlot(int slot, const CharacterDef& def) {
    if (slot < 0 || slot >= s_max) return;
    auto& a = s_slots[slot];
    a.body_scale[0] = def.height;
    a.body_scale[1] = def.bulk;
    a.body_scale[2] = def.skintone_sat;
    a.body_scale[3] = def.skintone_bri;
    a.skin_color[0] = def.skin_r;
    a.skin_color[1] = def.skin_g;
    a.skin_color[2] = def.skin_b;
    a.skin_color[3] = def.color_strength;
    s_dirty = true;
}

void CharDef_FlushGPU(void* sdl_cmd) {
    if (!s_dirty || s_max <= 0) return;
    s_buf.UploadInCmd(static_cast<md::GpuCommandBufferHandle>(sdl_cmd),
                      s_slots, s_max * (int)sizeof(GpuCharAppearance), 0);
    s_dirty = false;
}

void* CharDef_GetSDLBuffer() {
    return s_buf.SDLBuffer();
}

// ── JSON save ─────────────────────────────────────────────────────────────────
bool CharDef_SaveJSON(const CharacterDef& def, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { MD_LOG(MD_LOG_WARNING, "[CharDef] save failed: %s", path); return false; }
    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"%s\",\n", def.name);
    fprintf(f, "  \"sex\": %d,\n",  def.sex);
    fprintf(f, "  \"race\": %d,\n", def.race);
    // Save body[] and face[] arrays
    fprintf(f, "  \"body\": [");
    for (int i = 0; i < CHARDEF_BODY_N; ++i)
        fprintf(f, "%s%.4f", i ? "," : "", def.body[i]);
    fprintf(f, "],\n");
    fprintf(f, "  \"face\": [");
    for (int i = 0; i < CHARDEF_FACE_N; ++i)
        fprintf(f, "%s%.4f", i ? "," : "", def.face[i]);
    fprintf(f, "],\n");
    fprintf(f, "  \"height\": %.4f,\n", def.height);
    fprintf(f, "  \"bulk\": %.4f,\n",   def.bulk);
    fprintf(f, "  \"skin_r\": %.4f,\n", def.skin_r);
    fprintf(f, "  \"skin_g\": %.4f,\n", def.skin_g);
    fprintf(f, "  \"skin_b\": %.4f,\n", def.skin_b);
    fprintf(f, "  \"hair_r\": %.4f,\n", def.hair_r);
    fprintf(f, "  \"hair_g\": %.4f,\n", def.hair_g);
    fprintf(f, "  \"hair_b\": %.4f,\n", def.hair_b);
    fprintf(f, "  \"color_strength\": %.4f,\n", def.color_strength);
    fprintf(f, "  \"skintone_sat\": %.4f,\n", def.skintone_sat);
    fprintf(f, "  \"skintone_bri\": %.4f,\n", def.skintone_bri);
    fprintf(f, "  \"morph_count\": %d", def.morph_count);
    if (def.morph_count > 0) {
        fprintf(f, ",\n  \"morphs\": [");
        for (int i = 0; i < def.morph_count; ++i)
            fprintf(f, "%s%.4f", i ? "," : "", def.morph_weights[i]);
        fprintf(f, "]");
    }
    fprintf(f, "\n}\n");
    fclose(f);
    return true;
}

// ── JSON load (strstr-based, no external parser) ──────────────────────────────
static float parse_float_after(const char* buf, const char* key) {
    const char* p = strstr(buf, key);
    if (!p) return 0.f;
    p = strchr(p, ':');
    if (!p) return 0.f;
    return (float)atof(p + 1);
}
static int parse_int_after(const char* buf, const char* key) {
    const char* p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    return atoi(p + 1);
}
static void parse_str_after(const char* buf, const char* key, char* out, int out_sz) {
    const char* p = strstr(buf, key);
    if (!p) { out[0] = '\0'; return; }
    p = strchr(p, ':');
    if (!p) { out[0] = '\0'; return; }
    while (*p && (*p == ':' || *p == ' ' || *p == '"')) ++p;
    int n = 0;
    while (*p && *p != '"' && n < out_sz - 1) out[n++] = *p++;
    out[n] = '\0';
}

bool CharDef_LoadJSON(CharacterDef& def, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { MD_LOG(MD_LOG_WARNING, "[CharDef] load failed: %s", path); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); return false; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);

    parse_str_after(buf, "\"name\"",          def.name, 32);
    def.sex            = (uint8_t)parse_int_after(buf, "\"sex\"");
    def.race           = (uint8_t)parse_int_after(buf, "\"race\"");

    // Parse body[] array (Kenshi-style body sliders)
    {
        const char* bp = strstr(buf, "\"body\"");
        if (bp) {
            bp = strchr(bp, '[');
            if (bp) {
                ++bp;
                for (int i = 0; i < CHARDEF_BODY_N; ++i) {
                    while (*bp && (*bp == ' ' || *bp == ',')) ++bp;
                    if (!*bp || *bp == ']') break;
                    def.body[i] = (float)atof(bp);
                    while (*bp && *bp != ',' && *bp != ']') ++bp;
                }
            }
        }
    }

    // Parse face[] array (Kenshi-style face sliders)
    {
        const char* fp = strstr(buf, "\"face\"");
        if (fp) {
            fp = strchr(fp, '[');
            if (fp) {
                ++fp;
                for (int i = 0; i < CHARDEF_FACE_N; ++i) {
                    while (*fp && (*fp == ' ' || *fp == ',')) ++fp;
                    if (!*fp || *fp == ']') break;
                    def.face[i] = (float)atof(fp);
                    while (*fp && *fp != ',' && *fp != ']') ++fp;
                }
            }
        }
    }

    // Compute height and bulk from body[] (same formulas as character_editor.h).
    // eff_height = body[2]/100 + (body[7]/100-1)*0.07 + (body[4]/35-1)*0.03
    // eff_frame  = body[3]/100 + shoulder/arm/chest/hip offsets, clamped [0.55,1.45]
    {
        auto cl_f = [](float x, float lo, float hi) -> float {
            return x < lo ? lo : (x > hi ? hi : x);
        };
        float H   = def.body[2] / 100.f;
        float LL  = def.body[7] / 100.f;
        float Po  = def.body[4] / 35.f;
        float eff_h = H + (LL - 1.f) * 0.07f + (Po - 1.f) * 0.03f;
        def.height = cl_f(eff_h, 0.55f, 1.45f);

        float Fr  = def.body[3]  / 100.f;
        float Sh  = def.body[8]  / 100.f;
        float Ab  = def.body[9]  / 100.f;
        float Ch  = def.body[12] / 100.f;
        float Hip = def.body[15] / 100.f;
        float eff_fr = Fr + (Sh - 1.f) * 0.05f + (Ab - 1.f) * 0.04f
                          + (Ch - 1.f) * 0.06f  + (Hip - 1.f) * 0.04f;
        def.bulk = cl_f(eff_fr, 0.55f, 1.45f);
    }

    // Legacy fallback: if body[] was absent (old chardef), use explicit height/bulk keys.
    if (def.height < 0.5f || def.height > 2.f) {
        float h = parse_float_after(buf, "\"height\"");
        def.height = (h >= 0.5f && h <= 2.f) ? h : 1.f;
    }
    if (def.bulk < 0.3f || def.bulk > 2.f) {
        float b = parse_float_after(buf, "\"bulk\"");
        def.bulk = (b >= 0.3f && b <= 2.f) ? b : 1.f;
    }

    def.skin_r         = parse_float_after(buf, "\"skin_r\"");
    def.skin_g         = parse_float_after(buf, "\"skin_g\"");
    def.skin_b         = parse_float_after(buf, "\"skin_b\"");
    def.hair_r         = parse_float_after(buf, "\"hair_r\"");
    def.hair_g         = parse_float_after(buf, "\"hair_g\"");
    def.hair_b         = parse_float_after(buf, "\"hair_b\"");
    def.color_strength = parse_float_after(buf, "\"color_strength\"");
    {
        float s = parse_float_after(buf, "\"skintone_sat\"");
        def.skintone_sat = (s > 0.01f) ? s : 1.0f;  // default 1.0 if missing
        def.skintone_bri = parse_float_after(buf, "\"skintone_bri\"");
    }
    def.morph_count    = (uint8_t)parse_int_after(buf, "\"morph_count\"");

    // Parse morphs array
    if (def.morph_count > 0) {
        const char* mp = strstr(buf, "\"morphs\"");
        if (mp) {
            mp = strchr(mp, '[');
            if (mp) {
                ++mp;
                int n = def.morph_count < CHARDEF_MAX_MORPHS ? def.morph_count : CHARDEF_MAX_MORPHS;
                for (int i = 0; i < n; ++i) {
                    while (*mp && (*mp == ' ' || *mp == ',')) ++mp;
                    if (!*mp || *mp == ']') break;
                    def.morph_weights[i] = (float)atof(mp);
                    while (*mp && *mp != ',' && *mp != ']') ++mp;
                }
            }
        }
    }

    free(buf);
    return true;
}
