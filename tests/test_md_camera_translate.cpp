// MdCameraTranslated: the view direction of a camera moved to absolute coordinates must not depend on how far it is moved (window origins of the terrain).
#include <gtest/gtest.h>
#include <monkey_dust/render/md_camera.h>
#include <cmath>
#include <cstring>

static MdCamera LocalCam() {   // a fly-mode camera: the target is ~1 m from the position (the case that showed ~1 px)
    MdCamera c{};
    c.pos = { -2491.078f, 144.324f, 1463.952f };
    const float yaw = 0.7f, pitch = 0.43f;
    c.target = { c.pos.x + sinf(yaw) * cosf(pitch), c.pos.y - sinf(pitch), c.pos.z + cosf(yaw) * cosf(pitch) };
    c.up = { 0.f, 1.f, 0.f }; c.fovy = 60.f;
    return c;
}

TEST(MdCameraTranslated, ViewRotationIdenticalAtEveryWindowOrigin) {
    const MdCamera local = LocalCam();
    const float CS = 460.8f, tn = -2000.f;
    const int origins[3][2] = { { 26, 24 }, { 17, 29 }, { 0, 0 } };
    float ref[9]; bool have = false;
    for (const auto& o : origins) {
        const float dx = (local.pos.x - tn + o[0] * CS) - local.pos.x, dz = (local.pos.z - tn + o[1] * CS) - local.pos.z;
        const MdCamera a = MdCameraTranslated(local, dx, dz);
        const Mat4 v = a.ViewMatrix(); const float* m = mat4_ptr(v);
        float r[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };   // the 3x3 rotation of the column-major view matrix
        if (!have) { memcpy(ref, r, sizeof(r)); have = true; continue; }
        for (int i = 0; i < 9; ++i) EXPECT_LE(fabsf(r[i] - ref[i]), 1e-7f) << "origin (" << o[0] << "," << o[1] << ") element " << i;
    }
}

// Negative control: the same origins built the old way (the moved target minus the moved position, in float) must NOT give a window-independent rotation, else the test above could not catch the bug.
static void OldWayRotation(const MdCamera& local, int ox, int oz, float out[9]) {
    const float CS = 460.8f, tn = -2000.f;
    const float dx = (local.pos.x - tn + ox * CS) - local.pos.x, dz = (local.pos.z - tn + oz * CS) - local.pos.z;
    MdCamera a = local;
    a.pos.x += dx; a.target.x += dx; a.pos.z += dz; a.target.z += dz;   // the removed GraniteAbsCam: view = lookat(pos, target) of two ~1e4 m floats
    const Mat4 v = a.ViewMatrix(); const float* m = mat4_ptr(v);
    const float r[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] }; memcpy(out, r, sizeof(r));
}

TEST(MdCameraTranslated, NegativeControlOldWayDependsOnTheWindowOrigin) {
    const MdCamera local = LocalCam();
    const int origins[3][2] = { { 26, 24 }, { 17, 29 }, { 0, 0 } };
    float r[3][9]; for (int i = 0; i < 3; ++i) OldWayRotation(local, origins[i][0], origins[i][1], r[i]);
    float worst = 0.f;
    for (int i = 1; i < 3; ++i) for (int k = 0; k < 9; ++k) worst = fmaxf(worst, fabsf(r[i][k] - r[0][k]));
    EXPECT_GT(worst, 1e-7f) << "the old way gave the same rotation at all three origins: the control cannot show the bug";
}

TEST(MdCameraTranslated, PositionIsMovedAndPlainCameraUnchanged) {
    const MdCamera local = LocalCam();
    const MdCamera a = MdCameraTranslated(local, 100.f, -50.f);
    EXPECT_FLOAT_EQ(a.pos.x, local.pos.x + 100.f); EXPECT_FLOAT_EQ(a.pos.z, local.pos.z - 50.f); EXPECT_FLOAT_EQ(a.pos.y, local.pos.y);
    EXPECT_FALSE(local.has_dir);
    const Mat4 v0 = local.ViewMatrix(), v1 = MdCameraTranslated(local, 0.f, 0.f).ViewMatrix();   // a zero move gives the same view as the plain camera
    for (int i = 0; i < 16; ++i) EXPECT_NEAR(mat4_ptr(v0)[i], mat4_ptr(v1)[i], 2e-4f);
}
