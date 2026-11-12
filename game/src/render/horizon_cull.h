#pragma once
// CPU horizon map for the terrain G-buffer nodes (Hi-Z design note docs/research/HIZ_UPPER_BOUND_2026-11-12.md, coordinator 6818756433 / 6821801207).
// A table of the running maximum tangent of the terrain elevation angle seen from the camera (360 azimuth bins x 20 log distance steps) is built on a worker thread from TerrainAtlas_SampleWorld;
// Filter() drops the selected LOD nodes whose highest point lies below the horizon of every azimuth bin they span (+-3 bins, margin 0.02 in tangent) up to the distance before them.
// Conservative: anything unsure is kept; a node nearer than 8 m is always kept; with a table older than 20 m of camera drift nothing is dropped. Off by default (md.set_horizon_cull / MD_HORIZON_CULL=1).
#include <monkey_dust/world/terrain_lod_tree.h>
namespace md {
struct HorizonCullStats { int kept = 0, dropped = 0; double filter_ms = 0, build_ms = 0; long builds = 0; };
bool HorizonCullEnabled();
void HorizonCullSet(bool on);
// cam = absolute camera position (m). Returns the new node count (the nodes are compacted in place).
int HorizonCullFilter(const float cam[3], TerrainLodNode* nodes, int n);
HorizonCullStats HorizonCullGetStats();
}
