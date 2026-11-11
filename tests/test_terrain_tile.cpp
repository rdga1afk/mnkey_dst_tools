#include <gtest/gtest.h>
#include <monkey_dust/world/terrain_tile.h>
#include <monkey_dust/world/chunk_def.h>

// TerrainTile (TERRAIN_CA_REBUILD_PROMPT.md Phase 1, "зона-як-тайл" data
// model) — pure logic-level verification, same reasoning as
// test_terrain_patch_grid.cpp: isolate the coordinate/clamping math from
// any real Kenshi data or GPU involvement before anything builds on it.

TEST(TerrainTileGrid, ForZone_ComputesAbsoluteOrigin) {
    TerrainTile t = TerrainTileGrid::ForZone(5, 9);
    EXPECT_EQ(t.gx, 5);
    EXPECT_EQ(t.gz, 9);
    EXPECT_FLOAT_EQ(t.origin_x, 5.f * CHUNK_SIZE);
    EXPECT_FLOAT_EQ(t.origin_z, 9.f * CHUNK_SIZE);
    EXPECT_FLOAT_EQ(t.extent, CHUNK_SIZE);
    EXPECT_NE(t.layers, nullptr);
}

TEST(TerrainTileGrid, ForZone_ClampsOutOfRangeIndices) {
    TerrainTile lo = TerrainTileGrid::ForZone(-5, -100);
    EXPECT_EQ(lo.gx, 0);
    EXPECT_EQ(lo.gz, 0);

    TerrainTile hi = TerrainTileGrid::ForZone(999, 63);
    EXPECT_EQ(hi.gx, TerrainTileGrid::kGridN - 1);
    EXPECT_EQ(hi.gz, 63);
}

TEST(TerrainTileGrid, AdjacentTiles_ShareExactEdge_NoGapNoOverlap) {
    // Same invariant kenshi2mde.py --validate checks on real production
    // data (8064 zone-to-zone edges, max mismatch 0.0m) — here at the
    // pure-math level: tile (gx+1) starts exactly where tile (gx) ends.
    TerrainTile a = TerrainTileGrid::ForZone(10, 20);
    TerrainTile b = TerrainTileGrid::ForZone(11, 20);
    EXPECT_FLOAT_EQ(a.origin_x + a.extent, b.origin_x);
    EXPECT_FLOAT_EQ(a.origin_z, b.origin_z);
}

TEST(TerrainTile, Contains_TrueInsideFalseOutside) {
    TerrainTile t = TerrainTileGrid::ForZone(3, 4);
    EXPECT_TRUE(t.Contains(t.origin_x, t.origin_z));
    EXPECT_TRUE(t.Contains(t.origin_x + t.extent * 0.5f, t.origin_z + t.extent * 0.5f));
    EXPECT_FALSE(t.Contains(t.origin_x - 1.f, t.origin_z));
    EXPECT_FALSE(t.Contains(t.origin_x + t.extent, t.origin_z));  // half-open range
    EXPECT_FALSE(t.Contains(t.origin_x, t.origin_z + t.extent));
}

TEST(TerrainTileGrid, ForWorldPos_RoundTripsWithForZone) {
    TerrainTile by_zone = TerrainTileGrid::ForZone(7, 12);
    // A world position well inside that zone's rect must resolve back to
    // the same tile.
    float wx = by_zone.origin_x + by_zone.extent * 0.25f;
    float wz = by_zone.origin_z + by_zone.extent * 0.75f;
    TerrainTile by_pos = TerrainTileGrid::ForWorldPos(wx, wz);
    EXPECT_EQ(by_pos.gx, by_zone.gx);
    EXPECT_EQ(by_pos.gz, by_zone.gz);
}

TEST(TerrainTileGrid, ForWorldPos_ClampsOutsideWorldBounds) {
    TerrainTile neg = TerrainTileGrid::ForWorldPos(-1000.f, -1000.f);
    EXPECT_EQ(neg.gx, 0);
    EXPECT_EQ(neg.gz, 0);

    float world_size = (float)TerrainTileGrid::kGridN * CHUNK_SIZE;
    TerrainTile far = TerrainTileGrid::ForWorldPos(world_size + 5000.f, world_size + 5000.f);
    EXPECT_EQ(far.gx, TerrainTileGrid::kGridN - 1);
    EXPECT_EQ(far.gz, TerrainTileGrid::kGridN - 1);
}
