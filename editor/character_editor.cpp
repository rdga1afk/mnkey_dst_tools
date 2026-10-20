#include "character/appearance.h"
#include "character/appearance_io.h"
#include "character/race_stats.h"
#include "character/session.h"
#include "character_editor.h"
#include "editor_char_preview.h"
#include "imgui.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace CharacterEditor {
bool g_detached = false;
ImVec2 g_win_pos = {160.f, 60.f};
ImVec2 g_win_size = {900.f, 640.f};

using namespace md::character;
namespace {
Session g_session;
bool g_clothes = true;
char g_io_path[512] = "";
std::string g_confirm_msg;        // result of the last CONFIRM, shown in the RACE STATS panel header

// CONFIRM: the editor has no game character to give the appearance to; it writes the .bod2 to the last EXPORT / IMPORT path (character.bod2 before the first one).
bool Confirm() {
    std::string err;
    const std::string path = g_io_path[0] ? g_io_path : "character.bod2";
    const bool ok = md::character::ExportAppearance(g_session, path, err);
    g_confirm_msg = ok ? "Saved " + path : "Not saved: " + err;
    return ok;
}

}

// Loads the install (KENSHI_DIR) on the first call. False when it is missing: the tab then says so.
std::string g_load_error = "Kenshi not found: set KENSHI_DIR";
bool EnsureLoaded() {
    if (g_session.Loaded()) return true;
    static bool tried = false;
    if (tried) return false;
    tried = true;
    std::string dir = md::KenshiDir(), err;
    if (dir.empty()) { g_load_error = "Kenshi not found: set KENSHI_DIR"; fprintf(stderr, "[CharacterEditor] %s\n", g_load_error.c_str()); return false; }
    if (!g_session.Load(dir, err)) { g_load_error = "Kenshi data in " + dir + " not usable: " + err; fprintf(stderr, "[CharacterEditor] %s\n", g_load_error.c_str()); return false; }
    CharPreviewSDLGPU::Bind(&g_session);
    return true;
}

bool ConfirmForScenario() { return Confirm(); }

md::character::Session& GetSession() { return g_session; }

bool ApplyIo(int mode, const char* path, std::string& err) {
    return mode == 1 ? md::character::ImportAppearance(g_session, path, err) : md::character::ExportAppearance(g_session, path, err);
}

bool& Clothes() { return g_clothes; }
std::string& ConfirmMessage() { return g_confirm_msg; }

} // namespace CharacterEditor
