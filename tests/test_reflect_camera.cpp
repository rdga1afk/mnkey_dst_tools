#include <gtest/gtest.h>
#include <monkey_dust/render/reflect_camera.h>
#include <cmath>

namespace {
// our perspective: column-major, depth [0, 1], camera looks down -z (view space), clip.w = -z
void Persp(double fovy, double aspect, double zn, double zf, double m[16]) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0;
    const double f = 1.0 / std::tan(fovy * 0.5);
    m[0] = f / aspect; m[5] = f; m[10] = zf / (zn - zf); m[11] = -1.0; m[14] = zn * zf / (zn - zf);
}
void Mul4(const double* m, const double v[4], double out[4]) { for (int r = 0; r < 4; ++r) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3]; }
}

TEST(ReflectCamera, ReflectionMatrixMirrorsAboutThePlane) {
    const double n[3] = {0, 1, 0}; const double h = 12.5, d = -h;   // OGRE Plane(UNIT_Y, 12.5): n . p + d = 0 is y = 12.5
    double R[16]; md::reflect::ReflectionMatrix(n, d, R);
    double p[4] = {3, 20, -7, 1}, q[4]; Mul4(R, p, q);
    EXPECT_NEAR(q[0], 3, 1e-12); EXPECT_NEAR(q[1], 5, 1e-12); EXPECT_NEAR(q[2], -7, 1e-12);      // 12.5 - 7.5
    double on[4] = {1, h, 2, 1}, r2[4]; Mul4(R, on, r2);
    EXPECT_NEAR(r2[1], h, 1e-12);                                                              // a point on the plane stays
    double back[4]; Mul4(R, q, back); EXPECT_NEAR(back[1], 20, 1e-12);                          // twice = identity
    const double t[3] = {0.6, 0.8, 0.0}; double R2[16]; md::reflect::ReflectionMatrix(t, 0.0, R2);   // a tilted plane through the origin: the OGRE formula entries
    EXPECT_NEAR(R2[0], 1 - 2 * 0.36, 1e-12); EXPECT_NEAR(R2[4], -2 * 0.6 * 0.8, 1e-12); EXPECT_NEAR(R2[5], 1 - 2 * 0.64, 1e-12); EXPECT_NEAR(R2[10], 1.0, 1e-12);
}

TEST(ReflectCamera, ObliqueNearClipPutsTheNearPlaneOnThePlane) {
    double P[16], O[16]; Persp(60.0 * 3.14159265358979 / 180.0, 16.0 / 9.0, 0.5, 3000.0, P);
    // a water-like plane in view space, 10 m below the camera and slightly tilted; the camera and the far top corner are on its positive side
    double nrm[3] = {0.1, 0.99, 0.05}; const double len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    const double plane[4] = {nrm[0] / len, nrm[1] / len, nrm[2] / len, 10.0 / len};
    md::reflect::ObliqueNearClip(P, plane, O);
    // points on the plane: clip z' = 0 (the near plane); points in front of it (positive side): z' > 0; behind: z' < 0
    for (double x : {-6.0, 0.0, 9.0}) for (double z : {-20.0, -60.0, -200.0}) {
        const double y = -(plane[0] * x + plane[2] * z + plane[3]) / plane[1];   // plane height under (x, z)
        double pv[4] = {x, y, z, 1}, c[4]; Mul4(O, pv, c);
        EXPECT_NEAR(c[2], 0.0, 1e-9) << x << " " << z;
        double pos[4] = {x, y + 1.0, z, 1}, cp[4]; Mul4(O, pos, cp); EXPECT_GT(cp[2], 0.0);
        double neg[4] = {x, y - 1.0, z, 1}, cn[4]; Mul4(O, neg, cn); EXPECT_LT(cn[2], 0.0);
        EXPECT_NEAR(c[3], -z, 1e-9);                                                           // w untouched
    }
    for (int i : {0, 1, 3, 4, 5, 7, 8, 9, 11, 12, 13, 15}) EXPECT_DOUBLE_EQ(O[i], P[i]);       // only row 2 changes
}
