#include "world_map_ui.h"
#include <monkey_dust/platform/input.h>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/data_files.h>

#include <SDL3/SDL_gpu.h>
#include <monkey_dust/render/gpu_pipeline.h>
#include <monkey_dust/render/gpu_sampler_texture.h>
#include <monkey_dust/render/gpu_pass_view.h>
#include <monkey_dust/render/gpu_hal_free_functions.h>
#include <monkey_dust/render/gpu_device.h>

void WorldMapUI::Init(md::GpuDeviceHandle dev) {
    (void)dev;
    // Load Kenshi areasmap
    GpuSamplerDesc map_sdesc;
    map_sdesc.min_filter = GpuSamplerDesc::Filter::LINEAR;
    map_sdesc.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    map_sdesc.wrap_s     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    map_sdesc.wrap_t     = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    map_sdesc.gen_mipmap = false;
    // world_map.png is tools/md_worldmap_gen.py's GUI_Map.dds + zone labels; without the committed copy (the release) the install's GUI_Map.dds is the map
    std::string map_path = md::DevFile("game/data/textures/world_map.png");
    if (map_path.empty() && !md::KenshiDir().empty()) map_path = md::KenshiDir() + "/data/gui/gfx/GUI_Map.dds";
    if (!map_tex_.InitFromFile(map_path.c_str(), map_sdesc))
        fprintf(stderr,"[WorldMap] failed to load %s\n", map_path.c_str());

    // Reuse existing overlay pipeline (textured fullscreen quad)
    GpuPipeline::Desc od;
    od.vert_path       = "shaders/overlay.vert";
    od.frag_path       = "shaders/overlay.frag";
    od.vert_uniform_bufs = 1;
    od.frag_samplers   = 1;
    od.has_depth_target = false;
    od.raster.blend_enable = true;
    od.raster.depth_test   = false;
    od.raster.depth_write  = false;
    od.raster.cull_back    = false;
    od.layout.count        = 0;
    pipeline_.Create(od);

    // Zone pin pipeline
    GpuPipeline::Desc pd;
    pd.vert_path         = "shaders/world_pin.vert";
    pd.frag_path         = "shaders/world_pin.frag";
    pd.vert_uniform_bufs = 1;
    pd.has_depth_target  = false;
    pd.raster.blend_enable = true;
    pd.raster.depth_test   = false;
    pd.raster.depth_write  = false;
    pd.raster.cull_back    = false;
    pd.layout.count        = 0;
    pin_pipeline_.Create(pd);
    BuildPins();
    fprintf(stdout, "[WorldMap] init: map=%s pins=%d\n", map_tex_.SDLTexture() ? "ok" : "FAIL", pin_count_);
}

void WorldMapUI::Shutdown(md::GpuDeviceHandle dev) {
    (void)dev;
    map_tex_.Shutdown();
}

bool WorldMapUI::Update(float mx, float my, int sw, int sh, bool lmb_click) {
    if (!open_) return false;
    const char* hovered_slug  = nullptr;
    const char* hovered_label = nullptr;
    for (int i = 0; i < pin_count_; ++i) {
        int px, py, pw, ph;
        PinScreenRect(built_pins_[i].zx, built_pins_[i].zz, sw, sh, px, py, pw, ph);
        int hx = px - pw, hy = py - ph, hw = pw*3, hh = ph*3;
        if ((int)mx >= hx && (int)mx < hx+hw && (int)my >= hy && (int)my < hy+hh) {
            hovered_slug  = built_pins_[i].slug;
            hovered_label = built_pins_[i].label;
            if (lmb_click) {
                strncpy(pending_travel_, built_pins_[i].slug, WR_NAME_LEN-1);
                selected_zone_[0] = '\0';
                open_ = false;
                fprintf(stdout, "[WorldMap] travel to: %s\n", built_pins_[i].slug);
                return true;
            }
        }
    }
    // Show hovered zone name in window title bar (no font renderer needed)
    if (hovered_label) {
        char title[128];
        snprintf(title, sizeof(title), "monkeydust  |  [M] Map  —  %s  (click to travel)",
                 hovered_label);
        SDL_SetWindowTitle(SDL_GetWindowFromID(1), title);
    } else {
        SDL_SetWindowTitle(SDL_GetWindowFromID(1),
                           "monkeydust  |  [M] World Map  —  hover a zone, click to travel");
    }
    if (lmb_click && !hovered_slug) selected_zone_[0] = '\0';
    return false;
}

void WorldMapUI::Draw(SDL_GPURenderPass* rp, md::GpuCommandBufferHandle cmd,
                      int sw, int sh, float mx, float my) {
    if (!open_ || !pipeline_.SDLPipeline()) return;

    // ── 1. Draw map texture — scissor clips viewport, zoomed quad does zoom ──
    if (map_tex_.SDLTexture() && map_tex_.SDLSampler()) {
        float ml, mt, mw, mh;
        MapRect(sw, sh, ml, mt, mw, mh);

        // Scissor = fixed viewport rectangle (prevents drawing outside map area)
        SDL_Rect scissor{ (int)ml, (int)mt, (int)mw, (int)mh };
        GpuSetScissor(rp, scissor);

        // Zoomed quad: UV 0→1 maps to full quad; scissor clips to viewport.
        // At zoom>1 the quad extends beyond the scissor, revealing only the
        // central (1/zoom) fraction of the texture — correct zoom behaviour.
        float cx = ml + mw * 0.5f;
        float cy = mt + mh * 0.5f;
        float qw = mw * zoom_;
        float qh = mh * zoom_;
        float qx = cx - pan_u_ * qw;   // shift so pan_u_ is at viewport centre
        float qy = cy - pan_v_ * qh;

        struct { float x, y, w, h; } rect = {
            qx / sw, qy / sh, qw / sw, qh / sh
        };
        GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
        pv.BindPipeline(&pipeline_);
        SDL_GPUTextureSamplerBinding tsb{ map_tex_.SDLTexture(), map_tex_.SDLSampler() };
        pv.BindFragmentSamplers(0, &tsb, 1);
        pv.PushVertexUniforms(0, &rect, sizeof(rect));
        pv.Draw(6, 1, 0, 0);
        // Scissor stays set to the map rect -- zone pins below must be
        // clipped to it too (real bug: this used to get reset to full-
        // screen HERE, before the pin draw call, so at zoom_>1 any pin
        // whose screen position fell outside the zoomed/panned map
        // viewport rendered directly onto the surrounding UI/background
        // instead of being clipped away, exactly like PinScreenRect's own
        // ImGui-label counterpart already guards against via its bounds
        // check in main.cpp -- the GPU pin quads had no equivalent).
    }

    // ── 2. Draw zone pins ─────────────────────────────────────────────────────
    if (!pin_pipeline_.SDLPipeline()) {
        SDL_Rect full{ 0, 0, sw, sh };
        GpuSetScissor(rp, full);
        return;
    }
    {
        float ml, mt, mw, mh;
        MapRect(sw, sh, ml, mt, mw, mh);
        SDL_Rect scissor{ (int)ml, (int)mt, (int)mw, (int)mh };
        GpuSetScissor(rp, scissor);
    }

    struct PinUBO {
        float pins[40][4];
        float sw_f, sh_f, player_angle, _p1;
    } pu{};
    pu.sw_f = (float)sw; pu.sh_f = (float)sh;
    pu.player_angle = player_rot_y_;

    auto& wr = WorldRegistry::Get();
    const char* cur = wr.CurrentZone();

    for (int i = 0; i < pin_count_ && i < 40; ++i) {
        int px, py, pw, ph;
        PinScreenRect(built_pins_[i].zx, built_pins_[i].zz, sw, sh, px, py, pw, ph);
        float state = 0.f;
        ZoneRecord* zr = wr.Find(built_pins_[i].slug);
        if (zr && zr->visited) state = 1.f;
        if (!strcmp(built_pins_[i].slug, cur)) state = 2.f;
        int hx = px-pw, hy = py-ph, hw = pw*3, hh = ph*3;
        if ((int)mx>=hx && (int)mx<hx+hw && (int)my>=hy && (int)my<hy+hh) state = 3.f;
        if (!strcmp(built_pins_[i].slug, selected_zone_)) state = 4.f;

        // Player pin (state==2): exact world-position UV, 2x size for
        // legibility. MUST go through the same zoom_/pan_ transform
        // PinScreenRect applies to every other pin (real bug fixed here:
        // this used to project player_map_u_/v_ straight onto MapRect
        // with no zoom/pan at all -- correct only by coincidence at the
        // default zoom_=1/pan=0.5,0.5, but as soon as the map is zoomed
        // or panned -- exactly what was being tested -- the marker stayed
        // at its unzoomed screen position while every zone pin correctly
        // moved, drifting far off from its real position and, combined
        // with the scissor bug above, off the visible map entirely).
        int draw_pw = (state == 2.f) ? pw * 2 : pw;
        int draw_px, draw_py;
        if (state == 2.f) {
            float ml2, mt2, mw2, mh2;
            MapRect(sw, sh, ml2, mt2, mw2, mh2);
            float sx2 = ml2 + ((player_map_u_ - pan_u_) * zoom_ + 0.5f) * mw2;
            float sy2 = mt2 + ((player_map_v_ - pan_v_) * zoom_ + 0.5f) * mh2;
            draw_px = (int)sx2 - draw_pw / 2;
            draw_py = (int)sy2 - draw_pw / 2;
        } else {
            draw_px = px - (draw_pw - pw) / 2;
            draw_py = py - (draw_pw - pw) / 2;
        }
        pu.pins[i][0] = (float)draw_px / sw;
        pu.pins[i][1] = (float)draw_py / sh;
        pu.pins[i][2] = (float)draw_pw / sw;
        pu.pins[i][3] = state;
    }

    {
        GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
        pv.BindPipeline(&pin_pipeline_);
        pv.PushVertexUniforms(0, &pu, sizeof(pu));
        pv.Draw(6, (uint32_t)pin_count_, 0, 0);
    }

    // Reset scissor to full screen for whatever draws next in this pass.
    SDL_Rect full{ 0, 0, sw, sh };
    GpuSetScissor(rp, full);
}
