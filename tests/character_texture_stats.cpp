// Hair texture statistics (coordinator 6809885574): decodes a hair DDS on the CPU (read-only) and reports mean / min / max of R, G, B, A over the texels that pass the alpha test of the record,
// and what the hair shaders (char_hair.frag, char_body.frag overlay: albedo = tint x clamp(texel[diffuse channel]) x 0.7, cut where texel[alpha channel] < rejection/255) give with the record's
// channel mapping and with a naive "texel RGB x tint" mapping. usage: md_character_texture_stats <dds> <diffuse ch> <alpha ch> <rejection> <tint r g b 0..1>
#include <monkey_dust/io/dds_decode.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

// DXT3 (BC2) top mip, read-only: the engine's DecodeDdsRgba handles BC1 / BC3 / 32-bit only, and HairMod_*.dds are DXT3.
static bool DecodeDxt3(const std::string& path, int& w, int& h, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary); std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0 || memcmp(&d[84], "DXT3", 4) != 0) return false;
    memcpy(&h, &d[12], 4); memcpy(&w, &d[16], 4);
    out.assign(size_t(w) * h * 4, 0);
    const uint8_t* b = &d[128];
    auto c565 = [](uint16_t c, int* o) { o[0] = ((c >> 11) & 31) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; };
    for (int by = 0; by < h / 4; ++by) for (int bx = 0; bx < w / 4; ++bx, b += 16) {
        uint16_t c0, c1; memcpy(&c0, b + 8, 2); memcpy(&c1, b + 10, 2); uint32_t bits; memcpy(&bits, b + 12, 4);
        int pal[4][3]; c565(c0, pal[0]); c565(c1, pal[1]);
        for (int k = 0; k < 3; ++k) { pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3; pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3; }   // DXT3 colours are always 4-colour
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            const int i = y * 4 + x; uint8_t* o = &out[(size_t(by * 4 + y) * w + bx * 4 + x) * 4];
            const int sel = (bits >> (2 * i)) & 3; o[0] = uint8_t(pal[sel][0]); o[1] = uint8_t(pal[sel][1]); o[2] = uint8_t(pal[sel][2]);
            uint16_t a; memcpy(&a, b + (i / 4) * 2, 2); o[3] = uint8_t(((a >> (4 * (i % 4))) & 15) * 17);
        }
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 8) return 2;
    int w = 0, h = 0; std::vector<uint8_t> px;
    if (!DecodeDxt3(argv[1], w, h, px) && !md::DecodeDdsRgba(argv[1], w, h, px)) { std::printf("decode failed\n"); return 1; }
    const int dc = std::atoi(argv[2]), ac = std::atoi(argv[3]); const float rej = float(std::atof(argv[4])) / 255.f;
    const float tint[3] = {float(std::atof(argv[5])), float(std::atof(argv[6])), float(std::atof(argv[7]))};
    double sum[4] = {0, 0, 0, 0}, mn[4] = {255, 255, 255, 255}, mx[4] = {0, 0, 0, 0}, rec[3] = {0, 0, 0}, naive[3] = {0, 0, 0}; long pass = 0, all = long(w) * h;
    for (long i = 0; i < all; ++i) {
        const uint8_t* p = &px[size_t(i) * 4];
        if (p[ac] / 255.f < rej) continue;
        ++pass;
        for (int c = 0; c < 4; ++c) { sum[c] += p[c]; mn[c] = std::min<double>(mn[c], p[c]); mx[c] = std::max<double>(mx[c], p[c]); }
        const float v = std::min(1.f, p[dc] / 255.f);
        for (int c = 0; c < 3; ++c) { rec[c] += tint[c] * v * 0.7f; naive[c] += tint[c] * (p[c] / 255.f) * 0.7f; }
    }
    std::printf("%s %dx%d, alpha-passing texels %ld of %ld (%.1f%%), diffuse ch %d alpha ch %d rejection %.0f\n", argv[1], w, h, pass, all, 100.0 * pass / all, dc, ac, rej * 255.f);
    if (!pass) return 0;
    const char* nm = "RGBA";
    for (int c = 0; c < 4; ++c) std::printf("  %c mean %.1f min %.0f max %.0f\n", nm[c], sum[c] / pass, mn[c], mx[c]);
    std::printf("  shader, record mapping: mean albedo %.3f %.3f %.3f (tint %.2f %.2f %.2f)\n", rec[0] / pass, rec[1] / pass, rec[2] / pass, tint[0], tint[1], tint[2]);
    std::printf("  naive texel RGB x tint : mean albedo %.3f %.3f %.3f\n", naive[0] / pass, naive[1] / pass, naive[2] / pass);
    return 0;
}
