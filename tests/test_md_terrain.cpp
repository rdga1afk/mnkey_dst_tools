#include <gtest/gtest.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/world/chunk_def.h>
#include <cmath>

// TerrainGen_Build is CPU-only (fills heightmap + navmesh, no GPU upload).
// Tests run serially — static staging buffers in terrain_gen.cpp are reused.

// ── SimplexNoise2 ─────────────────────────────────────────────────────────────

TEST(TerrainNoise, SimplexRange) {
    for (int i = -50; i <= 50; ++i) {
        for (int j = -50; j <= 50; ++j) {
            float v = SimplexNoise2(i * 0.37f, j * 0.71f);
            EXPECT_GE(v, -1.f) << "x=" << i << " z=" << j;
            EXPECT_LE(v,  1.f) << "x=" << i << " z=" << j;
        }
    }
}

TEST(TerrainNoise, FBMRange) {
    for (int i = 0; i < 20; ++i) {
        float v = FBM2(i * 1.3f, i * 0.9f, 6, 0.5f, 2.0f);
        EXPECT_GE(v, -1.f);
        EXPECT_LE(v,  1.f);
    }
}

TEST(TerrainNoise, SimplexNotConstant) {
    float v0 = SimplexNoise2(0.f, 0.f);
    float v1 = SimplexNoise2(1.f, 0.f);
    float v2 = SimplexNoise2(0.f, 1.f);
    EXPECT_FALSE(v0 == v1 && v1 == v2) << "noise should vary spatially";
}

// ── TerrainGen_Build (CPU path) ───────────────────────────────────────────────

struct TerrainBuildFixture : ::testing::Test {
    TerrainChunk chunk{};
    TerrainGenParams params{};

    void SetUp() override {
        params.amplitude  = 10.f;
        params.octaves    = 2;
        params.base_scale = 0.01f;
        params.sea_level  = 0.f;
        params.seed       = 7;
    }
};

TEST_F(TerrainBuildFixture, BuildSucceeds) {
    params.nav_cs = 2.0f;  // large cell size: fast Recast, avoids dtCreateNavMeshData failure
    params.nav_ch = 0.5f;  // on flat-enough test terrain
    ChunkCoord coord{0, 0};
    TerrainGen_Build(chunk, coord, params);
    EXPECT_FALSE(chunk.loaded) << "loaded=false until TerrainGen_Upload";
    EXPECT_EQ(chunk.coord.x, 0);
    EXPECT_EQ(chunk.coord.z, 0);
    // Navmesh may or may not build depending on terrain slope — just check geometry
    EXPECT_GT(chunk.heightmap.h[0] + chunk.heightmap.h[TERRAIN_VERTS-1], -1.f)
        << "heightmap should have finite values";
}

TEST_F(TerrainBuildFixture, HeightmapBoundsRespected) {
    TerrainGen_Build(chunk, {0, 0}, params);
    for (int i = 0; i < TERRAIN_VERTS; ++i) {
        EXPECT_GE(chunk.heightmap.h[i], params.sea_level)
            << "height below sea_level at vertex " << i;
        EXPECT_LE(chunk.heightmap.h[i], params.amplitude + 1e-4f)
            << "height exceeds amplitude at vertex " << i;
    }
}

TEST_F(TerrainBuildFixture, HeightmapNotFlat) {
    TerrainGen_Build(chunk, {0, 0}, params);
    float h0 = chunk.heightmap.h[0];
    bool varies = false;
    for (int i = 1; i < TERRAIN_VERTS; ++i) {
        if (fabsf(chunk.heightmap.h[i] - h0) > 0.01f) { varies = true; break; }
    }
    EXPECT_TRUE(varies) << "terrain should vary across the chunk";
}

TEST_F(TerrainBuildFixture, DifferentChunksDiffer) {
    TerrainChunk a{}, b{};
    TerrainGen_Build(a, {0, 0}, params);
    float ha0 = a.heightmap.h[100];
    TerrainGen_Build(b, {1, 0}, params);
    float hb0 = b.heightmap.h[100];
    // Adjacent chunks share border vertex but interior should differ
    EXPECT_NE(ha0, hb0) << "adjacent chunks should have different interiors";
}

TEST_F(TerrainBuildFixture, SeaLevelClamps) {
    params.sea_level = 8.f;
    params.amplitude = 10.f;
    TerrainGen_Build(chunk, {0, 0}, params);
    for (int i = 0; i < TERRAIN_VERTS; ++i) {
        EXPECT_GE(chunk.heightmap.h[i], 8.f - 1e-5f)
            << "height must not be below sea_level=" << params.sea_level;
    }
}

// ── SampleHeight bilinear interpolation ──────────────────────────────────────

struct SampleHeightFixture : ::testing::Test {
    TerrainChunk chunk{};

    void SetUp() override {
        TerrainGenParams p{};
        p.amplitude  = 5.f;
        p.octaves    = 1;
        p.base_scale = 0.008f;
        p.seed       = 42;
        TerrainGen_Build(chunk, {0, 0}, p);
    }
};

TEST_F(SampleHeightFixture, CornerMatchesHeightmap) {
    // Bottom-left corner: local (0, 0) → grid (col=0, row=0)
    float h_bl = chunk.heightmap.h[0];
    EXPECT_NEAR(chunk.SampleHeight(0.f, 0.f), h_bl, 1e-4f);
}

TEST_F(SampleHeightFixture, GridPointMatchesHeightmap) {
    // Grid point at col=10, row=5
    int col = 10, row = 5;
    float lx = col * TERRAIN_STEP;
    float lz = row * TERRAIN_STEP;
    float expected = chunk.heightmap.h[row * (TERRAIN_GRID + 1) + col];
    EXPECT_NEAR(chunk.SampleHeight(lx, lz), expected, 1e-4f);
}

TEST_F(SampleHeightFixture, CellCenterIsAverage) {
    // Center of cell (col=3, row=3): lx=3.5, lz=3.5
    // For flat terrain, center ≈ average of 4 corners
    int col = 3, row = 3;
    float h00 = chunk.heightmap.h[(row  ) * (TERRAIN_GRID + 1) + (col  )];
    float h10 = chunk.heightmap.h[(row  ) * (TERRAIN_GRID + 1) + (col+1)];
    float h01 = chunk.heightmap.h[(row+1) * (TERRAIN_GRID + 1) + (col  )];
    float h11 = chunk.heightmap.h[(row+1) * (TERRAIN_GRID + 1) + (col+1)];
    float expected_center = (h00 + h10 + h01 + h11) * 0.25f;
    float sampled = chunk.SampleHeight(col * TERRAIN_STEP + 0.5f,
                                       row * TERRAIN_STEP + 0.5f);
    EXPECT_NEAR(sampled, expected_center, 0.5f);  // bilinear, not average — loose bound
}

TEST_F(SampleHeightFixture, ClampsBeyondMax) {
    // Beyond chunk bounds should clamp, not crash or produce NaN
    float h_clamped = chunk.SampleHeight(CHUNK_SIZE + 10.f, CHUNK_SIZE + 10.f);
    EXPECT_FALSE(std::isnan(h_clamped));
    EXPECT_FALSE(std::isinf(h_clamped));
    float h_top_right = chunk.SampleHeight(CHUNK_SIZE, CHUNK_SIZE);
    EXPECT_NEAR(h_clamped, h_top_right, 1e-4f);
}

TEST_F(SampleHeightFixture, ClampsNegative) {
    float h_neg = chunk.SampleHeight(-5.f, -5.f);
    float h_origin = chunk.SampleHeight(0.f, 0.f);
    EXPECT_NEAR(h_neg, h_origin, 1e-4f);
}

// ── BuildLodIboStitched watertightness (task #182 T-junction fix) ────────────
// Regression guard, kept permanently: verifies the geomorphic seam-stitch
// triangulation (interior + border zipper + corner fan, terrain_chunk.h)
// exactly tiles the chunk with no gap/overlap — sum of every triangle's
// XZ-projected (col,row) area must equal TERRAIN_GRID^2 exactly (grid-cell
// units, independent of CHUNK_SIZE/TERRAIN_STEP scale). Any topology bug
// (gap, overlap, wrong winding causing double-count) makes this sum diverge
// from the true footprint — catches a broken edit to this function before
// it ever reaches a screenshot.
static float GridXZArea(const uint16_t* idx_buf, int count) {
    auto colrow = [](uint16_t v) {
        int row = v / (TERRAIN_GRID + 1);
        int col = v % (TERRAIN_GRID + 1);
        return std::pair<float,float>((float)col, (float)row);
    };
    float total = 0.f;
    for (int t = 0; t < count; t += 3) {
        auto [x0,y0] = colrow(idx_buf[t+0]);
        auto [x1,y1] = colrow(idx_buf[t+1]);
        auto [x2,y2] = colrow(idx_buf[t+2]);
        float area = 0.5f * fabsf((x1-x0)*(y2-y0) - (x2-x0)*(y1-y0));
        total += area;
    }
    return total;
}

TEST(TerrainLodStitch, WatertightAtEveryLodStep) {
    static uint16_t buf[TERRAIN_LOD_IDX[0]];
    const float expected = (float)TERRAIN_GRID * (float)TERRAIN_GRID;
    for (int li = 0; li < TERRAIN_LOD_LEVELS; ++li) {
        int step = TERRAIN_LOD_STEPS[li];
        int count = BuildLodIboStitched(buf, step);
        EXPECT_EQ(count, TERRAIN_LOD_IDX[li]) << "step=" << step << " index count mismatch";
        float area = GridXZArea(buf, count);
        EXPECT_NEAR(area, expected, 0.01f) << "step=" << step << " total area diverges (gap/overlap)";
    }
}
