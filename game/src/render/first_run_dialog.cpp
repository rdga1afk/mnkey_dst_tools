#include "first_run_dialog.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <SDL3/SDL.h>
#include <stb_truetype.h>
#include "stb_image_write.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace firstrun {
namespace {
constexpr uint8_t kOk[3] = { 140, 205, 130 }, kRow[3] = { 46, 40, 33 }, kHead[3] = { 28, 24, 20 }, kBg[3] = { 18, 15, 12 }, kPanel[3] = { 36, 31, 26 }, kField[3] = { 12, 10, 8 }, kText[3] = { 222, 216, 207 }, kDim[3] = { 150, 142, 130 }, kErr[3] = { 230, 120, 100 }, kBtn[3] = { 78, 66, 52 };

void Fill(std::vector<uint8_t>& px, int w, int h, int x0, int y0, int rw, int rh, const uint8_t c[3]) {
    for (int y = std::max(0, y0); y < std::min(h, y0 + rh); ++y) for (int x = std::max(0, x0); x < std::min(w, x0 + rw); ++x) { uint8_t* d = &px[((size_t)y * w + x) * 4]; d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 255; }
}

size_t Utf8Next(const std::string& s, size_t i, int& cp) {
    const unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > s.size()) n = 1;
    cp = n == 1 ? c : (c & (0xFF >> (n + 1)));
    for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    return i + n;
}

stbtt_fontinfo& Font() { static stbtt_fontinfo fi; static bool ok = stbtt_InitFont(&fi, md_firstrun_font_data, 0); (void)ok; return fi; }

float TextWidth(const std::string& s, float scale) {
    stbtt_fontinfo& fi = Font(); float w = 0;
    for (size_t i = 0; i < s.size();) { int cp; i = Utf8Next(s, i, cp); int adv, lsb; stbtt_GetCodepointHMetrics(&fi, cp, &adv, &lsb); w += adv * scale; }
    return w;
}

// one line at (x, y_top); returns its width
float DrawText(std::vector<uint8_t>& px, int w, int h, float x, int y, float em_px, const std::string& s, const uint8_t col[3], float clip_x1) {
    stbtt_fontinfo& fi = Font();
    const float scale = stbtt_ScaleForMappingEmToPixels(&fi, em_px);
    int asc, desc, gap; stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
    const int base = y + (int)(asc * scale + 0.5f);
    const float x_start = x;
    for (size_t i = 0; i < s.size();) {
        int cp; i = Utf8Next(s, i, cp);
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&fi, cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&fi, cp, scale, scale, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<uint8_t> g((size_t)gw * gh);
            stbtt_MakeCodepointBitmap(&fi, g.data(), gw, gh, gw, scale, scale, cp);
            for (int yy = 0; yy < gh; ++yy) for (int xx = 0; xx < gw; ++xx) {
                const int X = (int)x + x0 + xx, Y = base + y0 + yy;
                if (X < 0 || X >= w || Y < 0 || Y >= h || X >= (int)clip_x1) continue;
                uint8_t* d = &px[((size_t)Y * w + X) * 4]; const int a = g[(size_t)yy * gw + xx];
                for (int k = 0; k < 3; ++k) d[k] = (uint8_t)((d[k] * (255 - a) + col[k] * a) / 255);
            }
        }
        x += adv * scale;
    }
    return x - x_start;
}
}  // namespace

namespace {
namespace fs = std::filesystem;
std::string Home() { const char* h = std::getenv("HOME"); return h ? h : ""; }
// "<folder>/<prefix>" -> folder (always ends with '/') and the typed last component
void SplitTyped(const std::string& typed, std::string& dir, std::string& prefix) {
    std::string t = typed;
    if (t.rfind("~/", 0) == 0 || t == "~") t = Home() + t.substr(1);
    const size_t k = t.rfind('/');
    if (k == std::string::npos) { dir = Home() + "/"; prefix = t; return; }
    dir = t.substr(0, k + 1); prefix = t.substr(k + 1);
}
bool StartsWithI(const std::string& s, const std::string& p) {
    if (p.size() > s.size()) return false;
    for (size_t i = 0; i < p.size(); ++i) if (tolower((unsigned char)s[i]) != tolower((unsigned char)p[i])) return false;
    return true;
}
}  // namespace

void RefreshList(State& st) {
    st.entries.clear();
    std::string dir, prefix; SplitTyped(st.path, dir, prefix);
    const std::string home = Home();
    if (prefix.empty() && !home.empty())
        for (const std::string& c : md::FindSteamKenshi(home)) st.entries.push_back({ c, c, "Steam", true });
    if (!home.empty()) st.entries.push_back({ "~  (home)", home + "/", "", md::IsKenshiFolder(home) });
    st.entries.push_back({ "/", "/", "", false });
    std::error_code ec;
    if (dir.size() > 1) {   // parent of the shown folder
        std::string up = dir; up.pop_back();
        const size_t k = up.rfind('/');
        st.entries.push_back({ "..", k == std::string::npos ? std::string("/") : up.substr(0, k + 1), "", false });
    }
    std::vector<ListEntry> subs;
    for (fs::directory_iterator it(dir.empty() ? "/" : dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && subs.size() < 3000; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        const std::string name = it->path().filename().string();
        if (!prefix.empty() && !StartsWithI(name, prefix)) continue;
        const std::string full = dir + name;
        subs.push_back({ name, full + "/", "", md::IsKenshiFolder(full) });
    }
    std::sort(subs.begin(), subs.end(), [](const ListEntry& a, const ListEntry& b) {   // normal folders A-Z, then hidden ones A-Z
        const bool ha = a.label[0] == '.', hb = b.label[0] == '.';
        return ha != hb ? hb : strcasecmp(a.label.c_str(), b.label.c_str()) < 0; });
    st.entries.insert(st.entries.end(), subs.begin(), subs.end());
    const int most = std::max(0, (int)st.entries.size() - st.rows_visible);
    st.scroll = std::min(std::max(st.scroll, 0), most);
}

void Complete(State& st) {
    std::string dir, prefix; SplitTyped(st.path, dir, prefix);
    std::vector<std::string> names; std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2; if (!it->is_directory(e2)) continue;
        const std::string n = it->path().filename().string();
        if (n.compare(0, prefix.size(), prefix) == 0) names.push_back(n);
    }
    if (names.empty()) return;
    std::string common = names[0];
    for (const std::string& n : names) { size_t k = 0; while (k < common.size() && k < n.size() && common[k] == n[k]) ++k; common.resize(k); }
    while (!common.empty() && ((unsigned char)common.back() & 0xC0) == 0x80 && false) common.pop_back();
    if (common.size() <= prefix.size() && names.size() > 1) return;
    st.path = dir + common + (names.size() == 1 ? "/" : "");
    st.error.clear();
}

void Render(int w, int h, State& st, bool caret, std::vector<uint8_t>& px, int confirm_rect[4], int quit_rect[4]) {
    px.assign((size_t)w * h * 4, 255);
    Fill(px, w, h, 0, 0, w, h, kBg);
    const float s = std::max(0.4f, std::min(768.f / 1080.f, std::min(((float)h - 40.f) / 430.f, ((float)w - 40.f) / 1000.f)));   // fixed pixel size (owner: the 1366x768 look = 1080p values x 768/1080); shrinks only when the window is smaller than the dialog
    const int pw = (int)std::min((float)w * 0.9f, 1000.f * s), ph0 = (int)(430 * s), rows = std::max(2, std::min(kListRows, (int)((h * 0.96f - ph0 - 12 * s) / (34 * s)))), list_h = st.list_open ? (int)(std::min(rows, (int)st.entries.size()) * 34 * s) + (int)(12 * s) : 0, ph = ph0 + list_h, px0 = (w - pw) / 2, py0 = std::max(0, (h - ph) / 2), pad = (int)(40 * s);
    Fill(px, w, h, px0, py0, pw, ph, kPanel);
    DrawText(px, w, h, (float)(px0 + pad), py0 + (int)(30 * s), 44 * s, "monkeydust", kText, 1e9f);
    DrawText(px, w, h, (float)(px0 + pad), py0 + (int)(100 * s), 26 * s, "Where are the Kenshi game files?", kText, 1e9f);
    DrawText(px, w, h, (float)(px0 + pad), py0 + (int)(140 * s), 20 * s, "The folder that contains data/gamedata.base. It is only read, never changed.", kDim, 1e9f);
    const int fx = px0 + pad, fy = py0 + (int)(190 * s), fw = pw - 2 * pad, fh = (int)(48 * s);
    const int abw = fh;   // the arrow button: a square at the right end of the field
    Fill(px, w, h, fx, fy, fw - abw - (int)(6 * s), fh, kField);
    st.arrow_rect[0] = fx + fw - abw; st.arrow_rect[1] = fy; st.arrow_rect[2] = abw; st.arrow_rect[3] = fh;
    Fill(px, w, h, st.arrow_rect[0], fy, abw, fh, kBtn);
    for (int r = 0; r < (int)(10 * s); ++r) {   // a triangle: down when closed, up when open
        const int hw = st.list_open ? r : (int)(10 * s) - r, yy = fy + fh / 2 - (int)(5 * s) + r;
        Fill(px, w, h, st.arrow_rect[0] + abw / 2 - hw, yy, 2 * hw + 1, 1, kText);
    }
    const float em = 24 * s, scale = stbtt_ScaleForMappingEmToPixels(&Font(), em);
    std::string shown = st.path; const float avail = (float)(fw - abw) - 36 * s;
    while (shown.size() > 1 && TextWidth(shown, scale) > avail) { int cp; const size_t n = Utf8Next(shown, 0, cp); shown.erase(0, n); }   // long paths show their tail
    const float tw = DrawText(px, w, h, (float)(fx + 12 * s), fy + (int)(9 * s), em, shown, kText, (float)(fx + fw - abw - 12));
    if (caret) Fill(px, w, h, (int)(fx + 12 * s + tw + 2), fy + (int)(8 * s), std::max(2, (int)(2 * s)), fh - (int)(16 * s), kText);
    if (!st.error.empty()) DrawText(px, w, h, (float)fx, fy + fh + (int)(14 * s), 20 * s, st.error, kErr, 1e9f);
    const int by = py0 + ph - pad - (int)(52 * s), bw = (int)(260 * s), bh = (int)(52 * s);
    confirm_rect[0] = px0 + pw - pad - bw; confirm_rect[1] = by; confirm_rect[2] = bw; confirm_rect[3] = bh;
    quit_rect[0] = confirm_rect[0] - (int)(20 * s) - bw; quit_rect[1] = by; quit_rect[2] = bw; quit_rect[3] = bh;
    for (const int* r : { confirm_rect, quit_rect }) Fill(px, w, h, r[0], r[1], r[2], r[3], kBtn);
    auto centred = [&](const int* r, const char* t) { const float e = 22 * s; const float tw2 = TextWidth(t, stbtt_ScaleForMappingEmToPixels(&Font(), e)); DrawText(px, w, h, r[0] + (r[2] - tw2) * 0.5f, r[1] + (int)(12 * s), e, t, kText, 1e9f); };
    centred(confirm_rect, "Continue (Enter)");
    centred(quit_rect, "Quit (Esc)");
    if (st.list_open) {
        st.row_h = (int)(34 * s); st.rows_visible = rows;
        const int lx = fx, ly = fy + fh + (int)(4 * s), lw = fw - 0, shown_rows = std::min(rows, (int)st.entries.size());
        st.list_rect[0] = lx; st.list_rect[1] = ly; st.list_rect[2] = lw; st.list_rect[3] = st.row_h * std::max(1, shown_rows);
        Fill(px, w, h, lx - 2, ly - 2, lw + 4, st.list_rect[3] + 4, kBtn);
        const float e = 21 * s;
        for (int i = 0; i < shown_rows; ++i) {
            const int idx = st.scroll + i; if (idx >= (int)st.entries.size()) break;
            const ListEntry& en = st.entries[idx];
            const int ry = ly + i * st.row_h;
            Fill(px, w, h, lx, ry, lw, st.row_h - 1, (i & 1) ? kRow : kHead);
            float tx = (float)(lx + 12 * s);
            tx += DrawText(px, w, h, tx, ry + (int)(5 * s), e, en.label, en.kenshi ? kOk : (en.label[0] == '.' && en.label != ".." ? kDim : kText), (float)(lx + lw - 150 * s)) + 12 * s;
            if (!en.tag.empty()) DrawText(px, w, h, tx, ry + (int)(5 * s), e * 0.85f, "[" + en.tag + "]", kDim, (float)(lx + lw - 150 * s));
            if (en.kenshi) { const std::string t = "Kenshi: data/gamedata.base"; const float tw3 = TextWidth(t, stbtt_ScaleForMappingEmToPixels(&Font(), e * 0.75f)); DrawText(px, w, h, (float)(lx + lw) - tw3 - 10 * s, ry + (int)(7 * s), e * 0.75f, t, kOk, 1e9f); }
        }
        if ((int)st.entries.size() > rows) {   // scroll bar
            const int bh2 = std::max((int)(18 * s), st.list_rect[3] * rows / (int)st.entries.size());
            const int by2 = ly + (st.list_rect[3] - bh2) * st.scroll / std::max(1, (int)st.entries.size() - rows);
            Fill(px, w, h, lx + lw - (int)(6 * s), by2, (int)(5 * s), bh2, kDim);
        }
    }
}

bool Confirm(State& st) {
    std::string p = st.path;
    while (!p.empty() && (p.back() == ' ' || p.back() == '\n' || p.back() == '\r' || p.back() == '/')) p.pop_back();
    size_t a = 0; while (a < p.size() && p[a] == ' ') ++a; p.erase(0, a);
    const char* home = std::getenv("HOME");
    if (p.rfind("~/", 0) == 0 && home) p = std::string(home) + p.substr(1);
    if (p.empty() || !md::IsKenshiFolder(p)) { st.error = "No data/gamedata.base in that folder - pick the folder of the Kenshi game."; fprintf(stderr, "[FirstRun] rejected: %s\n", p.c_str()); return false; }
    if (home) md::SaveKenshiDir(p, home);
    md::SetKenshiDir(p);
    st.path = p; st.error.clear();
    fprintf(stderr, "[FirstRun] saved: %s\n", p.c_str());
    return true;
}

bool Run(SDL_Window* window) {
    GpuPipeline pipe;
    GpuPipeline::Desc od;
    od.vert_path = "shaders/overlay.vert"; od.frag_path = "shaders/overlay.frag";
    od.vert_uniform_bufs = 1; od.frag_samplers = 1; od.has_depth_target = false;
    od.raster.blend_enable = true; od.raster.depth_test = false; od.raster.depth_write = false; od.raster.cull_back = false; od.layout.count = 0;
    if (!pipe.Create(od)) return false;
    State st; st.path = md::DetectedKenshiDir();
    int cr[4] = {}, qr[4] = {};
    GpuTexture tex; int tw = 0, th = 0; bool caret = true, dirty = true; Uint64 caret_t = SDL_GetTicks();
    SDL_StartTextInput(window);
    if (const char* z = getenv("MD_FIRSTRUN_SIZE")) { int zw = 0, zh = 0; if (sscanf(z, "%dx%d", &zw, &zh) == 2) { SDL_SetWindowFullscreen(window, false); SDL_RestoreWindow(window); SDL_SetWindowSize(window, zw, zh); SDL_SyncWindow(window); } }   // test: render at a given window size
    // Test hook (scripts/test_first_run.sh): MD_FIRSTRUN_KEYS = steps separated by '|': `text:<utf8>` types it, `clear` empties the field, `enter` presses Enter, `click` clicks Continue, `quit` presses Esc.
    // One step per frame; the outcome of each Confirm is logged. MD_FIRSTRUN_ONLY=1 leaves the process after the window closes.
    std::vector<std::string> steps;
    if (const char* k = getenv("MD_FIRSTRUN_KEYS")) { std::string a = k; size_t i = 0; while (i <= a.size()) { size_t j = a.find('|', i); if (j == std::string::npos) j = a.size(); steps.push_back(a.substr(i, j - i)); i = j + 1; } }
    size_t step = 0; int settle = 0;
    std::string last_entered;   // the folder the previous click went into: a double click selects it (the list has changed under the cursor by then)
    bool result = false, done = false;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { done = true; }
            else if (e.type == SDL_EVENT_TEXT_INPUT) { st.path += e.text.text; st.error.clear(); st.scroll = 0; RefreshList(st); dirty = true; }
            else if (e.type == SDL_EVENT_MOUSE_WHEEL && st.list_open) { st.scroll -= (int)(e.wheel.y * 3); RefreshList(st); dirty = true; }
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Scancode sc = e.key.scancode; const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
                if (sc == SDL_SCANCODE_ESCAPE) { if (st.list_open) { st.list_open = false; dirty = true; } else done = true; }
                else if (sc == SDL_SCANCODE_TAB) { Complete(st); st.scroll = 0; RefreshList(st); dirty = true; }
                else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) { if (Confirm(st)) { result = true; done = true; } dirty = true; }
                else if (sc == SDL_SCANCODE_BACKSPACE) { while (!st.path.empty() && ((unsigned char)st.path.back() & 0xC0) == 0x80) st.path.pop_back(); if (!st.path.empty()) st.path.pop_back(); st.error.clear(); st.scroll = 0; RefreshList(st); dirty = true; }
                else if (ctrl && sc == SDL_SCANCODE_V) { if (char* c = SDL_GetClipboardText()) { st.path += c; SDL_free(c); st.error.clear(); st.scroll = 0; RefreshList(st); dirty = true; } }
                else if (ctrl && sc == SDL_SCANCODE_U) { st.path.clear(); st.scroll = 0; RefreshList(st); dirty = true; }
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                int ww = 0, wh = 0, pw_ = 0, ph_ = 0; SDL_GetWindowSize(window, &ww, &wh); SDL_GetWindowSizeInPixels(window, &pw_, &ph_);
                const float mx = e.button.x * (ww ? (float)pw_ / ww : 1.f), my = e.button.y * (wh ? (float)ph_ / wh : 1.f);
                auto in = [&](const int* r) { return mx >= r[0] && mx < r[0] + r[2] && my >= r[1] && my < r[1] + r[3]; };
                if (in(st.arrow_rect)) { st.list_open = !st.list_open; st.scroll = 0; RefreshList(st); dirty = true; }
                else if (st.list_open && in(st.list_rect)) {
                    const int row = st.scroll + (int)((my - st.list_rect[1]) / st.row_h);
                    if (e.button.clicks >= 2 && !last_entered.empty()) {   // double click: take the folder the first click went into
                        st.path = last_entered; while (st.path.size() > 1 && st.path.back() == '/') st.path.pop_back();
                        st.list_open = false; st.error.clear(); dirty = true;
                    } else if (row >= 0 && row < (int)st.entries.size()) {
                        st.path = last_entered = st.entries[row].path; st.error.clear(); st.scroll = 0; RefreshList(st); dirty = true;
                    }
                }
                else if (in(cr)) { if (Confirm(st)) { result = true; done = true; } dirty = true; }
                else if (in(qr)) done = true;
            }
        }
        if (step < steps.size() && tw > 0 && !dirty) {   // after the first rendered frame (the button rectangles exist)
            const std::string& a = steps[step++];
            SDL_Event ev{};
            if (a.rfind("text:", 0) == 0) { ev.type = SDL_EVENT_TEXT_INPUT; const std::string t = a.substr(5); static std::string keep; keep = t; ev.text.text = keep.c_str(); SDL_PushEvent(&ev); }
            else if (a == "clear") { ev.type = SDL_EVENT_KEY_DOWN; ev.key.scancode = SDL_SCANCODE_U; ev.key.mod = SDL_KMOD_LCTRL; SDL_PushEvent(&ev); }
            else if (a == "enter") { ev.type = SDL_EVENT_KEY_DOWN; ev.key.scancode = SDL_SCANCODE_RETURN; SDL_PushEvent(&ev); }
            else if (a == "quit") { ev.type = SDL_EVENT_KEY_DOWN; ev.key.scancode = SDL_SCANCODE_ESCAPE; SDL_PushEvent(&ev); }
            else if (a == "arrow") {
                int ww = 0, wh = 0, pw_ = 0, ph_ = 0; SDL_GetWindowSize(window, &ww, &wh); SDL_GetWindowSizeInPixels(window, &pw_, &ph_);
                ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN; ev.button.button = SDL_BUTTON_LEFT; ev.button.clicks = 1;
                ev.button.x = (st.arrow_rect[0] + st.arrow_rect[2] / 2) * (pw_ ? (float)ww / pw_ : 1.f); ev.button.y = (st.arrow_rect[1] + st.arrow_rect[3] / 2) * (ph_ ? (float)wh / ph_ : 1.f); SDL_PushEvent(&ev);
            }
            else if (a.rfind("pick:", 0) == 0 || a.rfind("dbl:", 0) == 0) {   // click (or double click) the list row with this label
                const bool dbl = a[0] == 'd'; const std::string name = a.substr(a.find(':') + 1);
                int idx = -1; for (int i = 0; i < (int)st.entries.size(); ++i) if (st.entries[i].label == name) { idx = i; break; }
                if (idx < 0 || !st.list_open || st.list_rect[3] <= 0) { fprintf(stderr, "[FirstRun] test: no row \"%s\"\n", name.c_str()); --step; }   // retry next frame (the list is drawn one frame after it opens)
                else {
                    if (idx < st.scroll || idx >= st.scroll + st.rows_visible) { st.scroll = idx; RefreshList(st); }
                    int ww = 0, wh = 0, pw_ = 0, ph_ = 0; SDL_GetWindowSize(window, &ww, &wh); SDL_GetWindowSizeInPixels(window, &pw_, &ph_);
                    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN; ev.button.button = SDL_BUTTON_LEFT; ev.button.clicks = 1;
                    ev.button.x = (st.list_rect[0] + st.list_rect[2] / 2) * (pw_ ? (float)ww / pw_ : 1.f);
                    ev.button.y = (st.list_rect[1] + (idx - st.scroll) * st.row_h + st.row_h / 2) * (ph_ ? (float)wh / ph_ : 1.f);
                    SDL_PushEvent(&ev);
                    if (dbl) { ev.button.clicks = 2; SDL_PushEvent(&ev); }
                }
            }
            else if (a == "click") {
                int ww = 0, wh = 0, pw_ = 0, ph_ = 0; SDL_GetWindowSize(window, &ww, &wh); SDL_GetWindowSizeInPixels(window, &pw_, &ph_);
                ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN; ev.button.button = SDL_BUTTON_LEFT;
                ev.button.x = (cr[0] + cr[2] / 2) * (pw_ ? (float)ww / pw_ : 1.f); ev.button.y = (cr[1] + cr[3] / 2) * (ph_ ? (float)wh / ph_ : 1.f); SDL_PushEvent(&ev);
            }
        }
        if (SDL_GetTicks() - caret_t > 500) { caret = !caret; caret_t = SDL_GetTicks(); dirty = true; }
        md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
        if (!cmd) { SDL_Delay(16); continue; }
        uint32_t w = 0, h = 0;
        md::GpuTextureHandle swap = md::GpuDevice::Get().AcquireSwapchainTexture(cmd, &w, &h);
        if (!swap || !w || !h) { md::GpuDevice::Get().Submit(cmd); SDL_Delay(16); continue; }
        if (dirty || (int)w != tw || (int)h != th) {
            std::vector<uint8_t> px; Render((int)w, (int)h, st, caret, px, cr, qr);
            if (const char* shot = getenv("MD_FIRSTRUN_SHOT")) if (step >= steps.size() && ++settle > 4) { stbi_write_png(shot, (int)w, (int)h, 4, px.data(), (int)w * 4); done = true; }   // gate / debugging: write the frame after the scripted steps and leave
            tex.Shutdown(); tex.InitFromMemory(px.data(), (int)w, (int)h); tw = (int)w; th = (int)h; dirty = false;
        }
        GpuCommandBuffer cb; GpuCommandBuffer::ColorPassDesc cpd; cpd.cmd = cmd; cpd.color_tex[0] = swap; cpd.load_color = false;
        cb.BeginColorPass(cpd); cb.BindPipeline(&pipe);
        struct R { float x, y, w, h; } r{ 0.f, 0.f, 1.f, 1.f };
        SDL_GPUTextureSamplerBinding tsb{ tex.SDLTexture(), tex.SDLSampler() };
        cb.BindFragmentSamplers(0, &tsb, 1); cb.PushVertexUniforms(0, &r, sizeof(r)); cb.Draw(6);
        cb.EndPass();
        md::GpuDevice::Get().Submit(cmd);
        SDL_Delay(16);
    }
    SDL_StopTextInput(window);
    tex.Shutdown();
    if (getenv("MD_FIRSTRUN_ONLY")) { fprintf(stderr, "[FirstRun] %s\n", result ? "confirmed" : "closed"); std::exit(result ? 0 : 3); }
    return result;
}
}  // namespace firstrun
