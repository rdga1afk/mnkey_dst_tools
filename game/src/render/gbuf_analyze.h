#pragma once
// The pure CPU part of the A2 G-buffer check (no GPU headers): see gbuf_check.h.
#include <cstddef>
#include <cstdint>
namespace gbuf_check {
// A = decoded albedo, B = aux target (world normal as stored rgb 0..1, distance in alpha, 0 = not written), both RGBA16F bit patterns, n pixels. Returns true for PASS; text gets the verdict line.
bool Analyze(const uint16_t* A, const uint16_t* B, size_t n, char* text, size_t text_size);
// The raw R32F distance target, n pixels: FAIL on any NaN / Inf / negative value; the text gets the counts.
bool AnalyzeDistance(const float* d, size_t n, char* text, size_t text_size);
}
