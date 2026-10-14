// Column-major 4x4 helpers for md::character (glTF-like layout). Header-only, no dependencies.
#pragma once
#include <array>
#include <cmath>

namespace md::character {

using M4 = std::array<float, 16>;

inline M4 Identity() { M4 m{}; m[0] = m[5] = m[10] = m[15] = 1.f; return m; }

inline M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c) for (int rr = 0; rr < 4; ++rr) { float s = 0; for (int k = 0; k < 4; ++k) s += a[k * 4 + rr] * b[c * 4 + k]; r[c * 4 + rr] = s; }
    return r;
}

// translation * rotation (quaternion x, y, z, w) * optional scale
inline M4 Trs(const float t[3], const float q[4], const float* s) {
    float x = q[0], y = q[1], z = q[2], w = q[3], n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 0) { x /= n; y /= n; z /= n; w /= n; }
    M4 m = Identity();
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w); m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w); m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w); m[9] = 2 * (y * z - x * w); m[10] = 1 - 2 * (x * x + y * y);
    if (s) for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) m[c * 4 + r] *= s[c];
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
    return m;
}

// inverse of a rigid-plus-scale matrix (rotation scaled per column): transpose-scale the 3x3, then -R^-1 t
inline M4 InvAffine(const M4& m) {
    M4 r = Identity();
    for (int c = 0; c < 3; ++c) {
        float l2 = m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2];
        float inv = l2 > 0 ? 1.f / l2 : 0.f;
        for (int k = 0; k < 3; ++k) r[k * 4 + c] = m[c * 4 + k] * inv;   // (M^-1)[c][k] = M[k][c] / |col c|^2
    }
    for (int k = 0; k < 3; ++k) r[12 + k] = -(r[k] * m[12] + r[4 + k] * m[13] + r[8 + k] * m[14]);
    return r;
}

} // namespace md::character
