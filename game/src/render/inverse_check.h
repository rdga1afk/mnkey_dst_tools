#pragma once
// Debug invariant (DE-273 item 4): an inverse matrix handed to a light / fog ray pass must be camera-relative (the inverse of projection x rotation, camera at the origin).
// Test: the clip point (0, 0, 0.5, 1) must unproject to within `limit_m` of the origin; an inverse of an absolute view-projection puts it at the camera position (about 1.4e4 m in the Kenshi world).
// Column-major, depth [0, 1].
#include <cmath>

namespace md {
inline double UnprojectedDistance(const float inv[16]) {
    double r[4];
    const double v[4] = { 0.0, 0.0, 0.5, 1.0 };
    for (int i = 0; i < 4; ++i) r[i] = (double)inv[i] * v[0] + (double)inv[4 + i] * v[1] + (double)inv[8 + i] * v[2] + (double)inv[12 + i] * v[3];
    if (std::fabs(r[3]) < 1e-30) return 1e30;
    const double x = r[0] / r[3], y = r[1] / r[3], z = r[2] / r[3];
    return std::sqrt(x * x + y * y + z * z);
}
inline bool InverseIsCameraRelative(const float inv[16], double limit_m = 1000.0) { return UnprojectedDistance(inv) < limit_m; }
} // namespace md
