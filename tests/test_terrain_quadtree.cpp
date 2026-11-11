#include <gtest/gtest.h>
#include <monkey_dust/world/terrain_quadtree.h>
#include <monkey_dust/world/terrain_quadtree_mesh.h>
#include <cmath>

// Updated 2026-09-06 for the flat-LOD replacement (docs/TERRAIN_FLAT_LOD_PLAN.md,
// 2026-09-05): every visible zone is drawn at the SAME fixed depth, so skirts/
// stitching/geomorph (the old adaptive-LOD system's way of hiding density
// mismatches between differently-sized neighbors) no longer exist -- there is
// no mismatch to hide. VisibleNode has no morph field; TerrainQuadtreeMesh has
// no skirt_indices; BuildTerrainQuadtreeStitchedIndices is gone entirely. CPU-
// only invariant tests for the shared index mesh and the per-frame quadtree
// traversal -- what's guaranteed now: bounded/deterministic SelectVisible
// output, index values within their documented ranges, and every returned
// node sharing the SAME depth (the flat-LOD invariant itself).

namespace {
constexpr float kChunkSize = 460.8f;
constexpr int   kZonesPerSide = 4;
constexpr float kWorldExtent = kChunkSize * kZonesPerSide;

float FlatHeightSampler(float world_x, float world_z) {
    (void)world_x; (void)world_z;
    return 0.f;
}

// Permissive frustum: all 4 planes point "everything is in front" -- same
// pattern test_terrain_patch_grid.cpp uses when culling isn't the point.
void PermissiveFrustum(float out[16]) {
    float planes[16] = {
        1, 0, 0, 1e9f,  -1, 0, 0, 1e9f,  0, 0, 1, 1e9f,  0, 0, -1, 1e9f,
    };
    for (int i = 0; i < 16; ++i) out[i] = planes[i];
}
} // namespace

TEST(TerrainQuadtreeMesh, FilledGridHasExpectedIndexCountAndOnlyRegularIndices) {
    md::TerrainQuadtreeMesh mesh = md::BuildTerrainQuadtreeMesh();
    constexpr size_t kExpectedFilled = (size_t)16 * 16 * 6;
    EXPECT_EQ(mesh.filled_indices.size(), kExpectedFilled);
    for (uint32_t idx : mesh.filled_indices) {
        EXPECT_LT(idx, (uint32_t)md::TerrainQuadtreeMesh::kRegularVertexCount);
    }
}

TEST(TerrainQuadtree, DepthThreeLeafTexelSizeMatchesTerrainStep) {
    // The fixed flat-LOD depth's own footprint = chunk_size / 2^3, texelSize
    // = that / 16 quads -- must equal TERRAIN_STEP (3.6m, terrain_chunk.h)
    // exactly, the whole reason depth=3 was chosen (terrain_quadtree.cpp's
    // kFlatLodDepth doc comment: matches the old adaptive system's own
    // native leaf resolution, applied uniformly instead of only near-camera).
    float leaf_size = kChunkSize / 8.0f;
    float texel = leaf_size / 16.0f;
    EXPECT_NEAR(texel, 3.6f, 0.001f);
}

TEST(TerrainQuadtree, SelectVisibleIsBoundedAndDeterministic) {
    TerrainQuadtree qt;
    qt.Init(0.f, 0.f, kWorldExtent, kChunkSize, FlatHeightSampler);

    float cam[3] = {kWorldExtent * 0.5f, 200.f, kWorldExtent * 0.5f};
    float fp[16];
    PermissiveFrustum(fp);

    constexpr int kMaxOut = 4096;
    TerrainQuadtree::VisibleNode out_a[kMaxOut];
    TerrainQuadtree::VisibleNode out_b[kMaxOut];
    int count_a = qt.SelectVisible(cam, fp, out_a, kMaxOut, TerrainQuadtree::kGameplayMaxRenderDistance);
    int count_b = qt.SelectVisible(cam, fp, out_b, kMaxOut, TerrainQuadtree::kGameplayMaxRenderDistance);

    EXPECT_GT(count_a, 0);
    EXPECT_LE(count_a, kMaxOut);
    ASSERT_EQ(count_a, count_b) << "SelectVisible must be a pure function of (cam, frustum, max_render_distance)";
    for (int i = 0; i < count_a; ++i) {
        EXPECT_FLOAT_EQ(out_a[i].origin_x, out_b[i].origin_x);
        EXPECT_FLOAT_EQ(out_a[i].origin_z, out_b[i].origin_z);
        EXPECT_FLOAT_EQ(out_a[i].size, out_b[i].size);
        EXPECT_EQ(out_a[i].depth, out_b[i].depth);
    }
}

TEST(TerrainQuadtree, EveryVisibleNodeHasSameDepthAndSaneSize) {
    // Flat-LOD's whole point: every visible zone is drawn at the SAME depth
    // (terrain_quadtree.h's own doc comment), so neighbors always share
    // identical vertex density at their border -- this is the invariant that
    // replaced the old adaptive system's "finer near camera, coarser far
    // away" behavior (see CameraProximityBiasesTowardFinerDepth in this
    // file's git history for what that used to check, pre-2026-09-05).
    TerrainQuadtree qt;
    qt.Init(0.f, 0.f, kWorldExtent, kChunkSize, FlatHeightSampler);

    float cam[3] = {kChunkSize * 0.5f, 50.f, kChunkSize * 0.5f}; // near one corner
    float fp[16];
    PermissiveFrustum(fp);

    constexpr int kMaxOut = 4096;
    TerrainQuadtree::VisibleNode out[kMaxOut];
    int count = qt.SelectVisible(cam, fp, out, kMaxOut, TerrainQuadtree::kGameplayMaxRenderDistance);
    ASSERT_GT(count, 0);

    int first_depth = out[0].depth;
    for (int i = 0; i < count; ++i) {
        const auto& n = out[i];
        EXPECT_EQ(n.depth, first_depth) << "flat-LOD requires every visible node to share the same depth";
        EXPECT_GT(n.size, 0.f);
        EXPECT_GE(n.origin_x, 0.f);
        EXPECT_LE(n.origin_x + n.size, kWorldExtent + 0.01f);
        EXPECT_GE(n.origin_z, 0.f);
        EXPECT_LE(n.origin_z + n.size, kWorldExtent + 0.01f);
    }
}

TEST(TerrainQuadtree, MaxRenderDistanceCullsFarZones) {
    // #182 / БОРГ-VISUAL-3's own reason max_render_distance is a parameter,
    // not a baked-in constant: an aerial/editor camera needs a much larger
    // cutoff than gameplay's kGameplayMaxRenderDistance (3000m), and a small
    // cutoff from a corner camera must NOT see the opposite corner of a
    // world several times wider than the cutoff.
    TerrainQuadtree qt;
    qt.Init(0.f, 0.f, kWorldExtent, kChunkSize, FlatHeightSampler);

    float cam[3] = {0.f, 50.f, 0.f}; // world corner
    float fp[16];
    PermissiveFrustum(fp);

    constexpr int kMaxOut = 4096;
    TerrainQuadtree::VisibleNode out[kMaxOut];
    // kWorldExtent is 4 zones/side (1843.2m) -- a render distance smaller
    // than one zone's diagonal must cull everything but the near corner.
    float small_distance = kChunkSize * 0.5f;
    int count = qt.SelectVisible(cam, fp, out, kMaxOut, small_distance);
    ASSERT_GT(count, 0) << "camera's own zone must still be visible";

    bool saw_far_corner = false;
    for (int i = 0; i < count; ++i) {
        float cx = out[i].origin_x + out[i].size * 0.5f;
        float cz = out[i].origin_z + out[i].size * 0.5f;
        float dx = cam[0] - cx, dz = cam[2] - cz;
        float dist = std::sqrt(dx * dx + dz * dz);
        EXPECT_LE(dist, small_distance + kChunkSize)
            << "node at distance " << dist << " should have been culled by max_render_distance=" << small_distance;
        // Opposite world corner is ~kWorldExtent*sqrt(2) away -- far beyond
        // small_distance -- must never appear in the culled output.
        if (out[i].origin_x + out[i].size >= kWorldExtent - 0.01f &&
            out[i].origin_z + out[i].size >= kWorldExtent - 0.01f) {
            saw_far_corner = true;
        }
    }
    EXPECT_FALSE(saw_far_corner) << "opposite world corner should have been culled by a small max_render_distance";
    // With depth fixed at 2^depth tiles/side per zone, a single visible zone
    // caps count well below the full 4x4-zone world's node total.
    EXPECT_LT(count, kMaxOut / 4) << "small max_render_distance should return far fewer nodes than the full world would";
}
