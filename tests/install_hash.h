// Hash references for the tests that build something from the player's game folder (the #201 rule keeps Kenshi-derived files out of the repository; a hash is not Kenshi data).
// MD_EXPECT_INSTALL_HASH(key, bytes, size): SHA-256 of the output against the row (key, game build) of tests/data/install_hashes.tsv.
//   - no row for this game build  -> the test FAILS with "unknown game build <label>" (reported, never a silent pass);
//   - MD_INSTALL_HASH_PRINT=1     -> prints the rows "INSTALL_HASH<TAB>key<TAB>build<TAB>sha256" instead of comparing (how the file is made; its notes name the commit).
// The hashes are the output of the code at the commit named in the file: a regression guard, not a proof of correctness.
#pragma once
#include <gtest/gtest.h>
#include <monkey_dust/io/kenshi_dir.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace md_test {

inline std::string Sha256(const void* data, size_t n) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int k) { return (x >> k) | (x << (32 - k)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) { const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i], t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) block(p + i);
    uint8_t tail[128] = {}; const size_t rem = n - i;
    if (rem) std::memcpy(tail, p + i, rem);
    tail[rem] = 0x80;
    const size_t tl = rem + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = uint64_t(n) * 8;
    for (int k = 0; k < 8; ++k) tail[tl - 1 - k] = uint8_t(bits >> (8 * k));
    block(tail); if (tl == 128) block(tail + 64);
    char out[65];
    for (int k = 0; k < 8; ++k) std::snprintf(out + 8 * k, 9, "%08x", h[k]);
    return out;
}

// the game build of the game folder ("1.0.68 Steam" or "unknown build")
inline std::string GameBuild() {
    std::string label; md::KenshiBuildWarning(md::KenshiDir(), &label); return label;
}

// 1 = equal, 0 = different, 2 = printed (MD_INSTALL_HASH_PRINT), -1 = no row for this build (the caller skips)
inline int CheckHash(const std::string& key, const std::string& sha, std::string* expected, std::string* build) {
    *build = GameBuild();
    if (const char* pr = std::getenv("MD_INSTALL_HASH_PRINT"); pr && *pr == '1') { std::printf("INSTALL_HASH\t%s\t%s\t%s\n", key.c_str(), build->c_str(), sha.c_str()); return 2; }
    std::ifstream in("tests/data/install_hashes.tsv");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line); std::string k, b, h;
        std::getline(ss, k, '\t'); std::getline(ss, b, '\t'); std::getline(ss, h, '\t');
        if (k == key && b == *build) { *expected = h; return h == sha ? 1 : 0; }
    }
    return -1;
}

} // namespace md_test

#define MD_EXPECT_INSTALL_HASH(KEY, DATA, SIZE)                                                                                               \
    do {                                                                                                                                       \
        std::string md_exp_, md_build_;                                                                                                        \
        const std::string md_sha_ = md_test::Sha256((DATA), (SIZE));                                                                           \
        const int md_r_ = md_test::CheckHash((KEY), md_sha_, &md_exp_, &md_build_);                                                            \
        if (md_r_ < 0) FAIL() << "unknown game build \"" << md_build_ << "\" for " << (KEY) << " (no row in tests/data/install_hashes.tsv)"; \
        if (md_r_ != 2) EXPECT_EQ(md_sha_, md_exp_) << (KEY) << " on build " << md_build_;                                                    \
    } while (0)
