#include "splash_screen.h"
#include <monkey_dust/io/kenshi_dir.h>
#include <monkey_dust/io/kenshi_loading_screen.h>
#include <monkey_dust/render/gpu_device.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_gpu.h>
#include "stb_image_write.h"
#include "stb_image.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

SplashScreen& SplashScreen::Get() { static SplashScreen inst; return inst; }

namespace {
// Geometry of Kenshi_LoadingScreen.layout (fractions of the screen / of the panel) and the two fonts of kenshi_fonts.xml (TrueType size in pt at 50 dpi -> pixels = size * 50 / 72, for a 1080 high window).
constexpr float kPanelX = 0.34375f, kPanelY = 0.657407f, kPanelW = 0.3125f, kPanelH = 0.277778f;
constexpr float kTitleY = 0.0133333f, kTitleH = 0.3f, kTipX = 0.00833333f, kTipY = 0.316667f, kTipW = 0.983333f, kTipH = 0.666667f;
constexpr float kTitlePx = 110.f * 50.f / 72.f, kTipPx = 26.f * 50.f / 72.f;
// Kenshi_ProgressBarFill template (kenshi_templates.xml:43, root 150 x 20) created by the exe at (0.9, 0) of the screen, size (0.1, 0.02); sub-skin rectangles in Kenshi_UI.png from kenshi_skins.xml:647-684.
constexpr float kBarX = 0.9f, kBarY = 0.f, kBarW = 0.1f, kBarH = 0.02f;
struct SkinRect { int x, y, w, h; };
constexpr SkinRect kFrameSrc[9] = { {890,503,44,5}, {934,503,64,5}, {998,503,51,5}, {890,508,44,9}, {934,508,64,9}, {998,508,51,9}, {890,517,44,6}, {934,517,64,6}, {998,517,51,6} };
constexpr SkinRect kEmptySrc = { 900, 479, 140, 16 }, kFilledSrc = { 900, 458, 140, 16 };
constexpr uint8_t kTitleRgb[3] = { 222, 216, 207 };   // 0.871345 0.845139 0.810198

std::vector<uint8_t> ReadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

float MeasureLine(const stbtt_fontinfo& fi, float scale, const std::string& s) {
    float w = 0;
    for (char c : s) { int adv, lsb; stbtt_GetCodepointHMetrics(&fi, (unsigned char)c, &adv, &lsb); w += adv * scale; }
    return w;
}

// Rasterises `text` (word wrapped to max_w, each line centred when `center`, else left aligned) into an RGBA bitmap of width max_w; returns the height.
int RasterText(const std::vector<uint8_t>& ttf, float em_px, const std::string& text, const uint8_t rgb[3], int max_w, bool center, std::vector<uint8_t>& rgba, float* text_w) {
    stbtt_fontinfo fi;
    if (ttf.empty() || !stbtt_InitFont(&fi, ttf.data(), 0)) return 0;
    const float scale = stbtt_ScaleForMappingEmToPixels(&fi, em_px);
    int asc, desc, gap; stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
    const int line_h = (int)((asc - desc + gap) * scale + 0.5f);
    std::vector<std::string> lines;
    std::string cur, word;
    auto flush_word = [&]() { if (word.empty()) return; const std::string t = cur.empty() ? word : cur + " " + word; if (!cur.empty() && MeasureLine(fi, scale, t) > (float)max_w) { lines.push_back(cur); cur = word; } else cur = t; word.clear(); };
    for (char c : text) { if (c == '\n') { flush_word(); lines.push_back(cur); cur.clear(); } else if (c == ' ') flush_word(); else word += c; }
    flush_word(); if (!cur.empty() || lines.empty()) lines.push_back(cur);
    const int h = line_h * (int)lines.size() + 2;
    rgba.assign((size_t)max_w * h * 4, 0);
    float widest = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        const float lw = MeasureLine(fi, scale, lines[li]); if (lw > widest) widest = lw;
        float x = center ? ((float)max_w - lw) * 0.5f : 0.f;
        const int base = (int)(li * line_h + asc * scale + 0.5f);
        for (char c : lines[li]) {
            int adv, lsb, x0, y0, x1, y1;
            stbtt_GetCodepointHMetrics(&fi, (unsigned char)c, &adv, &lsb);
            stbtt_GetCodepointBitmapBox(&fi, (unsigned char)c, scale, scale, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                std::vector<uint8_t> g((size_t)gw * gh);
                stbtt_MakeCodepointBitmap(&fi, g.data(), gw, gh, gw, scale, scale, (unsigned char)c);
                for (int yy = 0; yy < gh; ++yy) for (int xx = 0; xx < gw; ++xx) {
                    const int px = (int)x + x0 + xx, py = base + y0 + yy;
                    if (px < 0 || px >= max_w || py < 0 || py >= h) continue;
                    uint8_t* d = &rgba[((size_t)py * max_w + px) * 4];
                    const uint8_t a = g[(size_t)yy * gw + xx];
                    if (a > d[3]) { d[0] = rgb[0]; d[1] = rgb[1]; d[2] = rgb[2]; d[3] = a; }
                }
            }
            x += adv * scale;
        }
    }
    if (text_w) *text_w = widest;
    return h;
}
}  // namespace

void SplashScreen::Pick(uint32_t seed) {
    md::kenshi::MsvcRand rng(seed);
    rng.Next();
    const int pic = md::kenshi::PickIndex(rng.Next(), (int)data_.group_names.size());
    const std::string& tex = data_.TextureFor(pic);
    bg_tex_.Shutdown();
    if (!tex.empty()) bg_tex_.InitFromFile((root_ + "/data/gui/gfx/" + tex).c_str());
    tip_.clear();
    if (!data_.tips.empty()) tip_ = data_.tips[(size_t)md::kenshi::PickIndex(rng.Next(), (int)data_.tips.size())];
    text_for_h_ = 0;   // the tip texture is rebuilt on the next frame
}

void SplashScreen::Init(md::GpuDeviceHandle dev) {
    if (!dev) return;

    GpuPipeline::Desc od;
    od.vert_path            = "shaders/overlay.vert";
    od.frag_path            = "shaders/overlay.frag";
    od.vert_uniform_bufs    = 1;
    od.frag_samplers        = 1;
    od.has_depth_target     = false;
    od.raster.blend_enable  = true;
    od.raster.depth_test    = false;
    od.raster.depth_write   = false;
    od.raster.cull_back     = false;
    od.layout.count         = 0;
    if (!pipeline_.Create(od)) return;

    const uint8_t panel_px[4] = { 20, 17, 14, 215 };
    panel_tex_.InitFromMemory(panel_px, 1, 1);

    // The picture, the tip and the fonts come from the player's install; without one the screen stays dark.
    root_ = md::KenshiDir();
    if (!root_.empty() && md::kenshi::LoadLoadingScreenData(root_, data_)) {
        Pick((uint32_t)SDL_GetTicks() ^ (uint32_t)SDL_GetPerformanceCounter());
        title_font_ = ReadFile(root_ + "/data/gui/fonts/MailartRubberstamp-Regular.ttf");
        tip_font_ = ReadFile(root_ + "/data/gui/fonts/Exo2-SemiBold.ttf");
        LoadBarSkins();
    }
    progress_ = 0.f;
    ready_    = true;
}

void SplashScreen::LoadBarSkins() {
    int iw = 0, ih = 0, ic = 0;
    uint8_t* img = stbi_load((root_ + "/data/gui/gfx/Kenshi_UI.png").c_str(), &iw, &ih, &ic, 4);
    if (!img) return;
    auto cut = [&](GpuTexture& t, const SkinRect& r) {
        if (r.x + r.w > iw || r.y + r.h > ih) return;
        std::vector<uint8_t> px((size_t)r.w * r.h * 4);
        for (int y = 0; y < r.h; ++y) memcpy(&px[(size_t)y * r.w * 4], img + ((size_t)(r.y + y) * iw + r.x) * 4, (size_t)r.w * 4);
        t.InitFromMemory(px.data(), r.w, r.h);
    };
    for (int i = 0; i < 9; ++i) cut(bar_frame_[i], kFrameSrc[i]);
    cut(bar_empty_, kEmptySrc);
    cut(bar_fill_, kFilledSrc);
    stbi_image_free(img);
}

void SplashScreen::SetProgress(float frac01) {
    progress_ = frac01 < 0.f ? 0.f : (frac01 > 1.f ? 1.f : frac01);   
}

void SplashScreen::Present() {
    if (!ready_) return;
    md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
    if (!cmd) return;
    uint32_t w = 0, h = 0;
    md::GpuTextureHandle swap = md::GpuDevice::Get().AcquireSwapchainTexture(cmd, &w, &h);
    if (!swap || w == 0 || h == 0) { md::GpuDevice::Get().Submit(cmd); return; }
    DrawInto(cmd, swap, w, h);
    md::GpuDevice::Get().Submit(cmd);
}

void SplashScreen::DrawInto(md::GpuCommandBufferHandle cmd, SDL_GPUTexture* swap, uint32_t w, uint32_t h) {
    // Text textures for this window size (the fonts are specified for 1080 lines; everything scales with the height).
    if (text_for_h_ != (int)h && !title_font_.empty()) {
        text_for_h_ = (int)h;
        const float s = (float)h / 1080.f;
        const int pw = (int)(kPanelW * (float)w), ph = (int)(kPanelH * (float)h);
        std::vector<uint8_t> px;
        float tw = 0;
        // the title is centred by the width of the text with two dots, so it does not move when the dots change
        RasterText(title_font_, kTitlePx * s, "Loading..", kTitleRgb, pw, false, px, &title_ref_w_px_);
        for (int d = 0; d < 4; ++d) {
            const int th = RasterText(title_font_, kTitlePx * s, std::string("Loading") + std::string((size_t)d, '.'), kTitleRgb, pw, false, px, &tw);
            title_tex_[d].Shutdown();
            if (th > 0) title_tex_[d].InitFromMemory(px.data(), pw, th);
            title_w_px_[d] = (float)pw; title_h_px_ = (float)th;
        }
        tip_tex_.Shutdown();
        if (!tip_.empty() && !tip_font_.empty()) {
            const int tipw = (int)(kTipW * (float)pw);
            const int th = RasterText(tip_font_, kTipPx * s, tip_, kTitleRgb, tipw, true, px, nullptr);
            if (th > 0) tip_tex_.InitFromMemory(px.data(), tipw, th);
            tip_w_px_ = (float)tipw; tip_h_px_ = (float)th;
        }
        (void)ph;
    }

    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd        = cmd;
    cpd.color_tex[0]  = swap;
    cpd.load_color = false;
    cb.BeginColorPass(cpd);
    cb.BindPipeline(&pipeline_);

    struct OverlayRect { float x, y, w, h; };
    auto draw = [&](GpuTexture& t, OverlayRect r) {
        if (!t.Valid()) return;
        SDL_GPUTextureSamplerBinding tsb{ t.SDLTexture(), t.SDLSampler() };
        cb.BindFragmentSamplers(0, &tsb, 1);
        cb.PushVertexUniforms(0, &r, sizeof(r));
        cb.Draw(6);
    };

    draw(bg_tex_, { 0.f, 0.f, 1.f, 1.f });
    if (bg_tex_.Valid() || !title_font_.empty()) {
        draw(panel_tex_, { kPanelX, kPanelY, kPanelW, kPanelH });
        const float fw = (float)w, fh = (float)h, pw = kPanelW * fw, ph = kPanelH * fh;
        const int dots = fixed_dots_ >= 0 ? fixed_dots_ : (int)((SDL_GetTicks() / 1000u) & 3u);
        if (title_tex_[dots].Valid()) {
            const float x = kPanelX * fw + (pw - title_ref_w_px_) * 0.5f;
            const float y = kPanelY * fh + (kTitleY + kTitleH * 0.5f) * ph - title_h_px_ * 0.5f;
            draw(title_tex_[dots], { x / fw, y / fh, title_w_px_[dots] / fw, title_h_px_ / fh });
        }
        if (tip_tex_.Valid()) {
            const float x = kPanelX * fw + kTipX * pw;
            const float y = kPanelY * fh + (kTipY + kTipH * 0.5f) * ph - tip_h_px_ * 0.5f;
            draw(tip_tex_, { x / fw, y / fh, tip_w_px_ / fw, tip_h_px_ / fh });
        }
    }

    if (bar_fill_.Valid()) {
        // root 150 x 20 stretched to the screen rectangle: empty skin (10,2,131,15), track place (11,3,129,13) filled to `progress_`, then the 159 x 20 frame on top (corners fixed, middle stretches)
        const float fw = (float)w, fh = (float)h;
        const float ox = kBarX * fw, oy = kBarY * fh, sx = kBarW * fw / 150.f, sy = kBarH * fh / 20.f;
        auto rect = [&](float x, float y, float rw, float rh) { return OverlayRect{ (ox + x * sx) / fw, (oy + y * sy) / fh, rw * sx / fw, rh * sy / fh }; };
        draw(bar_empty_, rect(10.f, 2.f, 131.f, 15.f));
        draw(bar_fill_, rect(11.f, 3.f, 129.f * progress_, 13.f));
        const float cx[4] = { 0.f, 44.f, 44.f + (150.f - 95.f), 150.f }, cy[4] = { 0.f, 5.f, 14.f, 20.f };
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                draw(bar_frame_[row * 3 + col], rect(cx[col], cy[row], cx[col + 1] - cx[col], cy[row + 1] - cy[row]));
    }

    cb.EndPass();
}

bool SplashScreen::SaveFrame(const char* png_path, int w, int h, uint32_t seed) {
    if (!ready_ || w <= 0 || h <= 0) return false;
    if (seed != 0 && !root_.empty()) Pick(seed);
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GetGPUSwapchainTextureFormat(dev, md::GpuDevice::Get().Window());
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(dev, &ti);
    if (!target) return false;
    text_for_h_ = 0;
    md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
    if (!cmd) { SDL_ReleaseGPUTexture(dev, target); return false; }
    fixed_dots_ = seed != 0 ? 1 : -1;   // a fixed picture is a fixed frame: the dots follow the clock otherwise (the gate view differed by the dot depending on the second of the start)
    DrawInto(cmd, target, (uint32_t)w, (uint32_t)h);
    fixed_dots_ = -1;
    md::GpuDevice::Get().Submit(cmd);
    text_for_h_ = 0;   // the next real frame rebuilds the text for the window
    // read back
    SDL_GPUTransferBufferCreateInfo tci{}; tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tci.size = (Uint32)(w * h * 4);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tci);
    SDL_GPUCommandBuffer* dc = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(dc);
    SDL_GPUTextureRegion reg{}; reg.texture = target; reg.w = (Uint32)w; reg.h = (Uint32)h; reg.d = 1;
    SDL_GPUTextureTransferInfo dst{}; dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(cp, &reg, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(dc);
    SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f);
    bool ok = false;
    if (const uint8_t* m = (const uint8_t*)SDL_MapGPUTransferBuffer(dev, tb, false)) {
        const bool bgra = ti.format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || ti.format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; ++i) { rgb[i * 3] = m[i * 4 + (bgra ? 2 : 0)]; rgb[i * 3 + 1] = m[i * 4 + 1]; rgb[i * 3 + 2] = m[i * 4 + (bgra ? 0 : 2)]; }
        SDL_UnmapGPUTransferBuffer(dev, tb);
        ok = stbi_write_png(png_path, w, h, 3, rgb.data(), w * 3) != 0;
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTexture(dev, target);
    return ok;
}

void SplashScreen::Shutdown() {
    if (!ready_) return;
    bg_tex_.Shutdown();
    panel_tex_.Shutdown();
    for (GpuTexture& t : title_tex_) t.Shutdown();
    tip_tex_.Shutdown();
    for (GpuTexture& t : bar_frame_) t.Shutdown();
    bar_empty_.Shutdown(); bar_fill_.Shutdown();
    // Deliberately NOT pipeline_.Destroy() here. GpuPipeline::Create()
    // dedupes SDL_GPUGraphicsPipeline objects by a (vert,frag,features,
    // raster) hash in a process-global cache with no refcounting
    // (gpu_hal_pipeline.cpp's s_pipe_cache) -- this pipeline_'s Desc is
    // byte-identical to SceneRender::overlay_pipeline's (same overlay.vert/
    // frag, same raster bits), so both wrappers share the exact same SDL
    // handle. Destroy() unconditionally calls SDL_ReleaseGPUGraphicsPipeline
    // on that shared handle without evicting it from the cache, which left
    // SceneRender::overlay_pipeline holding a dangling pointer -- confirmed
    // live via coredump: SDL_BindGPUGraphicsPipeline on it segfaulted deep
    // in libvulkan_intel.so the next time NpcRender::DrawHudOverlays drew
    // the camera-recording button. Leaving pipeline_ non-destroyed here
    // does not leak the real SDL resource: the shared handle stays owned by
    // SceneRender::overlay_pipeline for the rest of the process, and the
    // whole cache is freed by MdPipeCache_Shutdown() at exit regardless.
    ready_ = false;
}
