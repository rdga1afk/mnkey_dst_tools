#pragma once
// First-run window (#201 R5, owner 23.10): "where are the Kenshi game files?". Shown before anything reads the install when no folder is saved yet (or with
// --choose-kenshi-folder). The detected Steam folder is pre-filled; Enter / the button validates it by data/gamedata.base, saves it to ~/.config/monkeydust and
// sets it for this process. Drawn by ourselves (CPU bitmap + the splash overlay pipeline) with an embedded font: Kenshi's own skins are not available yet.
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

extern "C" const uint8_t md_firstrun_font_data[];
extern "C" const uint32_t md_firstrun_font_size_v;

namespace firstrun {
struct ListEntry {
    std::string label;     // what is drawn (UTF-8)
    std::string path;      // folder it stands for ("" for the header row)
    std::string tag;       // "Steam" for a detected install, "" otherwise
    bool kenshi = false;   // holds data/gamedata.base
};
struct State {
    std::string path;      // UTF-8, what the player edits
    std::string error;     // shown under the field
    // folder list under the field (the arrow button): follows the text of the field, the last component filters it
    bool list_open = false;
    int scroll = 0;        // first visible row
    std::vector<ListEntry> entries;
    // layout written by Render for the mouse (x, y, w, h in pixels)
    int arrow_rect[4] = {}, list_rect[4] = {};
    int row_h = 1, rows_visible = 10;
};
constexpr int kListRows = 10;   // at most; fewer when the window is small
// Rebuilds st.entries from st.path (detected installs, ~ and /, "..", the sub-folders of the typed folder that start with the typed last component).
void RefreshList(State& st);
// Tab: completes the last path component against the folder (unique match, else the common prefix).
void Complete(State& st);
// The whole window as RGBA8 (w x h); the confirm / quit rectangles are written as x, y, w, h (pixels) for the mouse; st gets the arrow / list layout.
void Render(int w, int h, State& st, bool caret, std::vector<uint8_t>& rgba, int confirm_rect[4], int quit_rect[4]);
// Validates `st.path` (trimmed, "~" expanded); on success saves it and calls md::SetKenshiDir. Fills st.error otherwise.
bool Confirm(State& st);
// Runs the window until a folder is confirmed (true) or the player quits (false). Needs the window and the GPU device.
bool Run(SDL_Window* window);
}
