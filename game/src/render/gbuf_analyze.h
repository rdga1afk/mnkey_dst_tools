#pragma once
// The pure CPU part of the A2 G-buffer check (no GPU headers): see gbuf_check.h.
#include <cstddef>
#include <cstdint>
namespace gbuf_check {
// A = decoded albedo, B = aux target (world normal as stored rgb 0..1, distance in alpha, 0 = not written), both RGBA16F bit patterns, n pixels. Returns true for PASS; text gets the verdict line.
bool Analyze(const uint16_t* A, const uint16_t* B, size_t n, char* text, size_t text_size);
// The raw R32F distance target, n pixels: FAIL on any NaN / Inf / negative value; the text gets the counts.
bool AnalyzeDistance(const float* d, size_t n, char* text, size_t text_size);
// The grass overlay's own decoded targets (A, B as above, D = its raw R32F distance), w x h: counts the written pixels (aux alpha or distance > 0) that are NaN / Inf, negative in albedo, or with |normal| outside 0.5..2;
// (negative albedo means below -0.1: 8-bit YCoCg quantisation leaves -0.03 on dark orange); the text lists the first 6 such pixels with their values. Returns the number of flagged pixels (the A2 grass limit: > 20 = FAIL, coordinator 6942103153).
size_t AnalyzeGrass(const uint16_t* A, const uint16_t* B, const float* D, int w, int h, char* text, size_t text_size);
}
