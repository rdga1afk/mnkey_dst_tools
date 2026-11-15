#include <gtest/gtest.h>
#include "../game/src/render/inverse_check.h"
#include <cmath>

namespace {
// inverse of a perspective (fovy 60, 16:9, near 0.5, far 3000, depth [0, 1], looks down -z), column-major, then optionally moved by the camera position
void InvPersp(const double cam[3], float out[16]) {
    const double zn = 0.5, zf = 3000.0, f = 1.0 / std::tan(30.0 * 3.14159265358979 / 180.0), a = 16.0 / 9.0;
    // P: m00 = f/a, m11 = f, m22 = zf/(zn-zf), m23 = zn*zf/(zn-zf) (row 2, col 3 in row notation), m32 = -1.   inverse by hand:
    const double p22 = zf / (zn - zf), p23 = zn * zf / (zn - zf);
    double m[16] = {};   // column-major inverse
    m[0] = a / f; m[5] = 1.0 / f; m[11] = 1.0 / p23; m[14] = -1.0; m[15] = p22 / p23;
    // translation by the camera after the unprojection: world = T * (inv * clip) in homogeneous form
    for (int c = 0; c < 4; ++c) for (int i = 0; i < 3; ++i) m[c * 4 + i] += cam[i] * m[c * 4 + 3];
    for (int i = 0; i < 16; ++i) out[i] = (float)m[i];
}
}

TEST(InverseCheck, CameraRelativeInversePasses) {
    const double cam[3] = {0, 0, 0}; float inv[16]; InvPersp(cam, inv);
    EXPECT_LT(md::UnprojectedDistance(inv), 5.0);
    EXPECT_TRUE(md::InverseIsCameraRelative(inv));
}

TEST(InverseCheck, AbsoluteCameraInverseFails) {
    const double cam[3] = {9647.7, 30.0, 15038.8}; float inv[16]; InvPersp(cam, inv);
    EXPECT_GT(md::UnprojectedDistance(inv), 1.0e4);
    EXPECT_FALSE(md::InverseIsCameraRelative(inv));
}
