#pragma once
// WorldMapUI — fullscreen world map overlay.
// Press M → open/close. Click zone pin → fast travel.
// Renders: Kenshi areasmap (256×256) scaled to screen + zone pins.

#include "world_registry.h"
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <cstring>
#include <cmath>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

// Zone pin data for the world map overlay.
// Built dynamically from WorldRegistry (which builds the zones and their grid cells from the install).
struct ZonePin {
    const char* slug;   // points into WorldRegistry ZoneRecord::name (stable)
    const char* label;  // points into WorldRegistry ZoneRecord::display_name
    int         zx, zz; // Kenshi zone grid coords (0..63)
};

class WorldMapUI {
public:
    static WorldMapUI& Get() { static WorldMapUI s; return s; }

    bool     IsOpen()   const { return open_; }
    int      PinCount() const { return pin_count_; }
    ZonePin  GetPin(int i) const { return built_pins_[i]; }

    // Map zone coords → screen pixel center (for ImGui label overlay)
    void GetPinScreenPos(int i, int sw, int sh, int& px, int& py) const {
        int pw, ph;
        PinScreenRect(built_pins_[i].zx, built_pins_[i].zz, sw, sh, px, py, pw, ph);
        px += pw / 2;
        py += ph / 2;
    }

    // Visible map viewport rect (for clipping label overlays to the actual
    // map square, not the full screen — the map is a 90%-height square
    // centred on screen, not fullscreen, so screen-bounds clipping lets
    // off-window pin labels render past the texture edges when zoomed in).
    void GetMapRect(int sw, int sh, float& ml, float& mt, float& mw, float& mh) const {
        MapRect(sw, sh, ml, mt, mw, mh);
    }

    // Populate built_pins_ from WorldRegistry; call after WorldRegistry is loaded.
    void BuildPins() {
        int count = 0;
        ZoneRecord* zones = WorldRegistry::Get().GetAll(count);
        pin_count_ = 0;
        for (int i = 0; i < count && pin_count_ < WR_MAX_ZONES; ++i) {
            if (zones[i].grid_x == 0 && zones[i].grid_z == 0) continue;
            if (zones[i].name[0] == '\0') continue;
            built_pins_[pin_count_].slug  = zones[i].name;
            built_pins_[pin_count_].label = zones[i].display_name[0]
                                            ? zones[i].display_name
                                            : zones[i].name;
            built_pins_[pin_count_].zx   = zones[i].grid_x;
            built_pins_[pin_count_].zz   = zones[i].grid_z;
            pin_count_++;
        }
    }

    void Toggle() {
        open_ = !open_;
        selected_zone_[0] = '\0';
        if (!open_) SDL_SetWindowTitle(SDL_GetWindowFromID(1), "monkeydust");
        else { zoom_ = 1.0f; pan_u_ = 0.5f; pan_v_ = 0.5f; }
    }
    void SetPlayerAngle(float rot_y) { player_rot_y_ = rot_y; }
    // u, v in [0,1] — full 64×64 Kenshi zone grid mapped to [0,1]
    void SetPlayerMapUV(float u, float v) { player_map_u_ = u; player_map_v_ = v; }

    // Scroll = zoom around mouse cursor.
    void Zoom(float delta, float mx, float my, int sw, int sh) {
        if (!open_) return;
        float ml, mt, mw, mh;
        MapRect(sw, sh, ml, mt, mw, mh);
        float vx = (mw > 0.f) ? (mx - ml) / mw : 0.5f;
        float vy = (mh > 0.f) ? (my - mt) / mh : 0.5f;
        float u_mouse = pan_u_ + (vx - 0.5f) / zoom_;
        float v_mouse = pan_v_ + (vy - 0.5f) / zoom_;
        zoom_ = (delta > 0.f) ? zoom_ * 1.08f : zoom_ * (1.f/1.08f);
        if (zoom_ < 1.0f) zoom_ = 1.0f;
        if (zoom_ > 16.0f) zoom_ = 16.0f;
        if (zoom_ < 1.01f) { pan_u_ = 0.5f; pan_v_ = 0.5f; return; }
        pan_u_ = u_mouse - (vx - 0.5f) / zoom_;
        pan_v_ = v_mouse - (vy - 0.5f) / zoom_;
        ClampPan_();
    }

    // WASD pan: du/dv are viewport-fractions (caller passes PAN_SPD * dt).
    void Pan(float du, float dv) {
        if (!open_ || zoom_ < 1.01f) return;
        pan_u_ += du / zoom_;
        pan_v_ += dv / zoom_;
        ClampPan_();
    }

    // Direct zoom/pan set — for scripted/--exec verification (no real
    // mouse-scroll input needed to reach a specific zoomed/panned view).
    void SetZoomPan(float zoom, float pan_u, float pan_v) {
        zoom_ = zoom < 1.f ? 1.f : (zoom > 16.f ? 16.f : zoom);
        pan_u_ = pan_u; pan_v_ = pan_v;
        ClampPan_();
    }

    void Close() {
        open_ = false;
        zoom_ = 1.0f; pan_u_ = 0.5f; pan_v_ = 0.5f;
        SDL_SetWindowTitle(SDL_GetWindowFromID(1), "monkeydust");
    }

    // Returns slug of zone to travel to, or nullptr if none chosen
    const char* PendingTravel() const {
        return pending_travel_[0] ? pending_travel_ : nullptr;
    }
    void ClearPendingTravel() { pending_travel_[0] = '\0'; }

    void Init(md::GpuDeviceHandle dev);
    void Shutdown(md::GpuDeviceHandle dev);

    // Call every frame when open; handles mouse hover/click
    // Returns true if a zone was selected for travel.
    bool Update(float mx, float my, int sw, int sh, bool lmb_click);

    // Draw overlay into existing render pass
    void Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
              int sw, int sh, float mx, float my);

    md::GpuTextureHandle MapTexture() const { return map_tex_.SDLTexture(); }
    SDL_GPUSampler* Sampler()    const { return map_tex_.SDLSampler(); }
    GpuPipeline&    Pipeline()         { return pipeline_; }
    GpuPipeline&    PinPipeline()      { return pin_pipeline_; }

private:
    GpuTexture      map_tex_;
    GpuPipeline     pipeline_;      // textured quad for areasmap
    GpuPipeline     pin_pipeline_;  // coloured quads for zone pins

    float    player_rot_y_ = 0.f;
    float    player_map_u_ = 0.5f;  // precise world position on the map [0,1]
    float    player_map_v_ = 0.5f;
    float    zoom_   = 1.0f;        // 1=full map visible, up to 8=zoomed in
    float    pan_u_  = 0.5f;        // UV at screen centre when zoomed
    float    pan_v_  = 0.5f;
    bool     open_      = false;
    int      pin_count_ = 0;
    ZonePin  built_pins_[WR_MAX_ZONES];
    char     selected_zone_[WR_NAME_LEN] = {};
    char     pending_travel_[WR_NAME_LEN] = {};

    // Fixed viewport square (90% of screen height, centred). Zoom is applied via UV rect.
    void MapRect(int sw, int sh, float& ml, float& mt, float& mw, float& mh) const {
        mh = 0.90f * sh;
        mw = mh;
        ml = (sw - mw) * 0.5f;
        mt = (sh - mh) * 0.5f;
    }

    // Map zone grid coords → screen rect (accounts for zoom/pan).
    void PinScreenRect(int zx, int zz, int sw, int sh,
                       int& px, int& py, int& pw, int& ph) const {
        float ml, mt, mw, mh;
        MapRect(sw, sh, ml, mt, mw, mh);
        float fu = (float)zx / 63.f;
        float fv = (float)zz / 63.f;
        // Apply zoom/pan: convert UV → viewport position
        float sx = ml + ((fu - pan_u_) * zoom_ + 0.5f) * mw;
        float sy = mt + ((fv - pan_v_) * zoom_ + 0.5f) * mh;
        pw = (int)(mw / 63.f * 0.6f * zoom_);
        if (pw < 4) pw = 4;
        ph = pw;
        px = (int)sx - pw / 2;
        py = (int)sy - ph / 2;
    }

    void ClampPan_() {
        float half = 0.5f / zoom_;
        if (pan_u_ < half) pan_u_ = half;
        if (pan_u_ > 1.f - half) pan_u_ = 1.f - half;
        if (pan_v_ < half) pan_v_ = half;
        if (pan_v_ > 1.f - half) pan_v_ = 1.f - half;
    }
};
