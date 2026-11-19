// Grass pages (engine/include/monkey_dust/world/grass_pages.h): the page a blade belongs to and the conservative box-in-frustum test the grass pass uses to skip invisible pages.
#include <gtest/gtest.h>
#include <monkey_dust/world/grass_pages.h>
#include <cmath>
#include <cstring>

using namespace md::world;

namespace {
// column-major view-projection of a camera at `eye` looking along +z, vertical fov 60 deg, aspect 16:9, depth 0..1 (the Vulkan convention), near 0.5, far 400 (metres)
void Vp(const float eye[3], float yaw, float out[16]) {
    const float fy = 1.f / std::tan(0.5f * 1.0471976f), fx = fy / (16.f / 9.f), n = 0.5f, f = 400.f;
    const float c = std::cos(yaw), s = std::sin(yaw);
    // view: rotate about y by yaw, translate by -eye
    float v[16] = { c, 0, -s, 0,  0, 1, 0, 0,  s, 0, c, 0,  0, 0, 0, 1 };
    v[12] = -(c * eye[0] + s * eye[2]); v[13] = -eye[1]; v[14] = -(-s * eye[0] + c * eye[2]);
    float p[16] = {}; p[0] = fx; p[5] = fy; p[10] = f / (f - n); p[11] = 1.f; p[14] = -f * n / (f - n);
    for (int cc = 0; cc < 4; ++cc) for (int r = 0; r < 4; ++r) { float a = 0; for (int k = 0; k < 4; ++k) a += p[k * 4 + r] * v[cc * 4 + k]; out[cc * 4 + r] = a; }
}
PageBox Box(float cx, float cz, float half = 115.2f) { return { { cx - half, 0.f, cz - half }, { cx + half, 3.f, cz + half } }; }   // a 230.4 m page
}   // namespace

TEST(GrassPages, PageKeyFollowsTheZoneGridInHalfZones) {
    EXPECT_EQ(GrassPageKey(-147456.0, -147456.0), 0);                          // the grid origin
    EXPECT_EQ(GrassPageKey(-147456.0 + 2303.9, -147456.0), 0);
    EXPECT_EQ(GrassPageKey(-147456.0 + 2304.0, -147456.0), 4096);             // next page along x
    EXPECT_EQ(GrassPageKey(-147456.0, -147456.0 + 2304.0), 1);                // next page along z
    EXPECT_EQ(GrassPageKey(-147456.0 - 0.1, -147456.0), -4096);               // negative side floors
}

TEST(GrassPages, VisiblePageSetForFixedCameras) {
    const float eye[3] = { 0.f, 1.7f, 0.f };
    float vp[16]; Vp(eye, 0.f, vp);   // looking along +z
    EXPECT_TRUE(PageVisible(vp, Box(0.f, 150.f)));         // straight ahead
    EXPECT_TRUE(PageVisible(vp, Box(0.f, 0.f)));           // the page the camera stands in
    EXPECT_FALSE(PageVisible(vp, Box(0.f, -400.f)));       // behind
    EXPECT_FALSE(PageVisible(vp, Box(900.f, 100.f)));      // far to the right
    EXPECT_FALSE(PageVisible(vp, Box(-900.f, 100.f)));     // far to the left
    EXPECT_FALSE(PageVisible(vp, Box(0.f, 900.f)));        // beyond the far plane
    // a 9 x 9 page grid around the camera, 230.4 m each: the visible count for the two views is fixed (regression guard)
    auto count = [&](float yaw) { float m[16]; Vp(eye, yaw, m); int n = 0; for (int i = -4; i <= 4; ++i) for (int j = -4; j <= 4; ++j) n += PageVisible(m, Box(i * 230.4f, j * 230.4f)) ? 1 : 0; return n; };
    const int ahead = count(0.f), turned = count(1.5707963f);
    EXPECT_GT(ahead, 3); EXPECT_LT(ahead, 30);               // about a quarter of the grid at fov 60 and far 400
    EXPECT_LT(ahead, 81); EXPECT_GT(turned, 3); EXPECT_LT(turned, 30);
    // pages of a grid are never culled when their centre is inside the frustum
    float m[16]; Vp(eye, 0.f, m);
    for (int i = -4; i <= 4; ++i) for (int j = -4; j <= 4; ++j) {
        const float x = i * 230.4f, z = j * 230.4f, cx = m[0] * x + m[4] * 1.f + m[8] * z + m[12], w = m[3] * x + m[7] * 1.f + m[11] * z + m[15];
        const float cy = m[1] * x + m[5] * 1.f + m[9] * z + m[13], cz = m[2] * x + m[6] * 1.f + m[10] * z + m[14];
        if (w > 0.f && std::fabs(cx) < w && std::fabs(cy) < w && cz > 0.f && cz < w) EXPECT_TRUE(PageVisible(m, Box(x, z))) << i << " " << j;
    }
}

TEST(GrassPages, CellKeyIsPageThenEightByEightCell) {
    const double o = -147456.0;
    EXPECT_EQ(GrassCellKey(o, o), 0);
    EXPECT_EQ(GrassCellKey(o + 287.9, o), 0);
    EXPECT_EQ(GrassCellKey(o + 288.0, o), 8);                    // next cell along x inside the page
    EXPECT_EQ(GrassCellKey(o, o + 288.0), 1);                    // next cell along z
    EXPECT_EQ(GrassCellKey(o + 2303.9, o + 2303.9), 63);         // last cell of page 0
    EXPECT_EQ(GrassCellKey(o + 2304.0, o), 4096 * 64);           // first cell of the next page along x
    EXPECT_EQ(GrassCellKey(o + 3000.0, o + 100.0) / 64, GrassPageKey(o + 3000.0, o + 100.0));
}

TEST(GrassPages, VisibleCellSetForFixedCameras) {
    const float eye[3] = { 0.f, 1.7f, 0.f };
    // a 28.8 m cell grid 20 x 20 around the camera (576 m square): the visible set for two views is fixed (regression guard) and smaller than the page set it replaces
    auto count = [&](float yaw, float side, int n) { float m[16]; Vp(eye, yaw, m); int v = 0; for (int i = -n; i < n; ++i) for (int j = -n; j < n; ++j) v += PageVisible(m, Box((i + 0.5f) * side, (j + 0.5f) * side, side * 0.5f)) ? 1 : 0; return v; };
    const int cells = count(0.f, 28.8f, 10), turned = count(1.5707963f, 28.8f, 10);
    EXPECT_EQ(cells, 128); EXPECT_EQ(turned, 128);   // of 400 cells
    EXPECT_LT(cells, 400);
    float m[16]; Vp(eye, 0.f, m);
    EXPECT_FALSE(PageVisible(m, Box(0.f, -100.f, 14.4f)));       // a cell behind the camera
    EXPECT_TRUE(PageVisible(m, Box(0.f, 100.f, 14.4f)));
}
