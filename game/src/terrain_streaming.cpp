#include "game_common.h"

// render-audit-2026 §11.5 (DEBT-STREAM): TerrainStreamQueue::enqueue()
// failing (CAPACITY=14 full) used to fall back to a synchronous
// TerrainGen_Build+Upload+AddTerrainMesh+ClutterGen+Upload right here on
// the calling thread -- measured up to 214ms in one HandleTerrainStreaming()
// call, reproduced both after a single teleport and during simulated
// sustained movement (docs/RENDER_AUDIT_2026.md §11.4/§11.5), so this is
// not a rare edge case. Replaced with a bounded pending ring: a request
// that doesn't fit in the async queue THIS frame is queued here instead of
// built synchronously, and drained a few at a time on subsequent frames.
// This does not change which chunks are requested or in what order --
// only how many frames the request may wait before reaching the async
// queue. MAX_PENDING_STREAM covers the worst case of one
// HandleTerrainStreaming() call (2 axes * TNKN shift-steps * TNKN chunks
// per step = 2*9*9=162 at TNKN=9) with headroom.
struct PendingStreamRequest {
    TerrainChunk*    chunk;
    ChunkCoord       coord;
    TerrainGenParams params;
    JPH::BodyID*     jolt_id;
    int              atlas_ex;
    int              atlas_ez;
};
static constexpr int MAX_PENDING_STREAM = 256;
static PendingStreamRequest s_pending_stream[MAX_PENDING_STREAM];
static int s_pending_head  = 0;  // ring index of the next request to drain
static int s_pending_count = 0;  // number currently queued

static void PushPendingStream(TerrainChunk* chunk, ChunkCoord coord,
                               const TerrainGenParams& params,
                               JPH::BodyID* jolt_id, int ax, int az) {
    if (s_pending_count >= MAX_PENDING_STREAM) {
        fprintf(stderr, "[STREAM] pending ring full (MAX=%d) -- dropping "
                        "chunk request (%d,%d)\n", MAX_PENDING_STREAM, coord.x, coord.z);
        return;
    }
    int tail = (s_pending_head + s_pending_count) % MAX_PENDING_STREAM;
    s_pending_stream[tail] = PendingStreamRequest{chunk, coord, params, jolt_id, ax, az};
    ++s_pending_count;
}

// Budget: at most STREAM_BUDGET_CHUNKS successful enqueues OR
// STREAM_BUDGET_MS elapsed wall-clock, whichever comes first, per call --
// spreading a large backlog across many frames instead of one big
// synchronous stall. Stops (does not skip ahead) on the first enqueue()
// failure: the async queue is still full, and skipping to a later request
// would reorder chunk arrival without actually helping (the queue being
// full is the bottleneck, not which specific request is tried).
static constexpr int   STREAM_BUDGET_CHUNKS = 4;
static constexpr float STREAM_BUDGET_MS     = 3.0f;

static void DrainPendingStreamRequests() {
    if (s_pending_count == 0) return;
    double t0 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int done = 0;
    while (s_pending_count > 0 && done < STREAM_BUDGET_CHUNKS) {
        PendingStreamRequest& req = s_pending_stream[s_pending_head];
        if (!TerrainStreamQueue::Get().enqueue(req.chunk, req.coord, req.params,
                                                req.jolt_id, req.atlas_ex, req.atlas_ez))
            break;  // still full -- retry next frame, preserve request order
        s_pending_head = (s_pending_head + 1) % MAX_PENDING_STREAM;
        --s_pending_count;
        ++done;
        double ms = (std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count() - t0) * 1000.0;
        if (ms > STREAM_BUDGET_MS) break;
    }
}

// ─── HandleTerrainStreaming ───────────────────────────────────────────────────
// Shifts the 9×9 circular chunk window when the player drifts > 0.5 chunks
// from the centre of the currently loaded terrain grid.
// External linkage (not static): also called from RunScenarioMode's --exec
// tick loop (game/src/autonomy/scenario_driver.cpp) — that loop never ran
// the interactive per-frame code below, so the terrain window never
// followed cam_x/cam_z there, and could sit many km from the actual camera
// (task #158f/g).
void HandleTerrainStreaming(float cam_x, float cam_z, GameState& gs)
{
// Drains the pending-request ring every call (not just when a new shift
// triggers below) so a backlog keeps making progress even across frames
// with no further camera movement.
DrainPendingStreamRequests();
const float half   = (SceneRender::TNKN / 2) * CHUNK_SIZE;  // 1500m
const float trig   = CHUNK_SIZE * 0.5f;         // trigger at 250m — prefetch before zone boundary
float cx_w = SceneRender::Get().tnoff_x + half;  // world X of terrain centre
float cz_w = SceneRender::Get().tnoff_z + half;
float dx   = cam_x - cx_w;
float dz   = cam_z - cz_w;
int shift_x = 0, shift_z = 0;
if (dx >  trig) shift_x =  (int)(dx / CHUNK_SIZE);
if (dx < -trig) shift_x = -(int)(-dx / CHUNK_SIZE);
if (dz >  trig) shift_z =  (int)(dz / CHUNK_SIZE);
if (dz < -trig) shift_z = -(int)(-dz / CHUNK_SIZE);
if (shift_x || shift_z) {
    TerrainGenParams stgp = TerrainGenParams::ProcWorld();
    stgp.nav_cs = 50.f; stgp.nav_ch = 1.f;
    // X axis: advance circular base one step at a time, rebuild only new edge column.
    int absx = shift_x > 0 ? shift_x : -shift_x;
    int dirx = shift_x > 0 ? 1 : -1;
    for (int sx = 0; sx < absx && sx < SceneRender::TNKN; ++sx) {
        int nox = SceneRender::Get().zone_ox + dirx;
        int nox_clamped = nox < 0 ? 0 : (nox > 63-SceneRender::TNKN ? 63-SceneRender::TNKN : nox);
        // World-edge guard (task #173, zone-coord-desync): zone_ox is clamped
        // to [0,63-TNKN], but tnoff_x (the floating local origin ALL local-
        // space math, incl. TerrainQuery::GetHeight, is keyed off) previously
        // kept advancing every iteration regardless of the clamp -- so once
        // the camera flew past the world edge, tnoff_x drifted arbitrarily
        // far from the (frozen) loaded chunk data, and every subsequent
        // GetHeight/render query used stale local coords -> blue-sky void.
        // If the zone didn't actually move, stop: there is no more real data
        // in this direction, so tnoff_x must stay in lockstep with zone_ox.
        if (nox_clamped == SceneRender::Get().zone_ox) break;
        SceneRender::Get().tnoff_x    += dirx * CHUNK_SIZE;
        SceneRender::Get().zone_ox     = nox_clamped;
        SceneRender::Get().base_cx     = (SceneRender::Get().base_cx + dirx + SceneRender::TNKN) % SceneRender::TNKN;
        // ZoneState: mark new centre zone visited; purge NPC slots from the zone that left.
        // WorldRegistry::current_zone_ used to update ONLY on explicit
        // fast-travel/portal transitions (TransitionToZone) -- meaning the
        // world map's exact player marker (WorldMapUI's state==2 branch)
        // never tracked ordinary walking at all, staying stuck on whatever
        // zone was last transitioned into (or "border_zone" default on a
        // fresh game). This is the same real zone-boundary-crossing check
        // the 9x9 streaming window already performs every tick, so it's
        // free to piggyback on.
        if (ZoneRecord* cz = WorldRegistry::Get().FindByGrid(
                SceneRender::Get().zone_ox + SceneRender::TNKN/2,
                SceneRender::Get().zone_oz + SceneRender::TNKN/2))
            WorldRegistry::Get().SetCurrentZone(cz->name);
        stgp.world_offset_x = SceneRender::Get().tnoff_x;
        stgp.world_offset_z = SceneRender::Get().tnoff_z;
        stgp.force_noise    = false;
        stgp.zone_origin_x  = SceneRender::Get().zone_ox;
        stgp.zone_origin_z  = SceneRender::Get().zone_oz;
        int elx  = dirx > 0 ? SceneRender::TNKN-1 : 0;
        int phsx = (elx + SceneRender::Get().base_cx) % SceneRender::TNKN;
        // Item 2: stitch seam on incoming side before new chunks are built
        {
            int szx = stgp.zone_origin_x + (dirx > 0 ? elx - 1 : elx);
            for (int lz = 0; lz < SceneRender::TNKN; ++lz)
                TerrainAtlas_StitchEdge(szx, stgp.zone_origin_z + lz, 0);
        }
        for (int lz = 0; lz < SceneRender::TNKN; ++lz) {
            int phsz = (lz + SceneRender::Get().base_cz) % SceneRender::TNKN;
            auto& sc = SceneRender::Get().terrain_chunks[phsz][phsx];
            auto& jid = SceneRender::Get().terrain_jolt_ids[phsz][phsx];
            int ax = stgp.zone_origin_x + elx, az = stgp.zone_origin_z + lz;
            // Item 5 / render-audit-2026 §11.5: async enqueue; on overflow,
            // queue for later instead of building synchronously right here.
            if (!TerrainStreamQueue::Get().enqueue(&sc, {elx, lz}, stgp, &jid, ax, az))
                PushPendingStream(&sc, {elx, lz}, stgp, &jid, ax, az);
        }
    }
    // Z axis: advance circular base one step at a time, rebuild only new edge row.
    int absz = shift_z > 0 ? shift_z : -shift_z;
    int dirz = shift_z > 0 ? 1 : -1;
    for (int sz = 0; sz < absz && sz < SceneRender::TNKN; ++sz) {
        int noz = SceneRender::Get().zone_oz + dirz;
        int noz_clamped = noz < 0 ? 0 : (noz > 63-SceneRender::TNKN ? 63-SceneRender::TNKN : noz);
        if (noz_clamped == SceneRender::Get().zone_oz) break;  // world-edge guard (task #173, see X-loop above)
        SceneRender::Get().tnoff_z    += dirz * CHUNK_SIZE;
        SceneRender::Get().zone_oz     = noz_clamped;
        SceneRender::Get().base_cz     = (SceneRender::Get().base_cz + dirz + SceneRender::TNKN) % SceneRender::TNKN;
        if (ZoneRecord* cz = WorldRegistry::Get().FindByGrid(
                SceneRender::Get().zone_ox + SceneRender::TNKN/2,
                SceneRender::Get().zone_oz + SceneRender::TNKN/2))
            WorldRegistry::Get().SetCurrentZone(cz->name);
        stgp.world_offset_x = SceneRender::Get().tnoff_x;
        stgp.world_offset_z = SceneRender::Get().tnoff_z;
        stgp.force_noise    = false;
        stgp.zone_origin_x  = SceneRender::Get().zone_ox;
        stgp.zone_origin_z  = SceneRender::Get().zone_oz;
        int elz  = dirz > 0 ? SceneRender::TNKN-1 : 0;
        int phsz = (elz + SceneRender::Get().base_cz) % SceneRender::TNKN;
        // Item 2: stitch Z seam on incoming side before new chunks are built
        {
            int szy = stgp.zone_origin_z + (dirz > 0 ? elz - 1 : elz);
            for (int lx = 0; lx < SceneRender::TNKN; ++lx)
                TerrainAtlas_StitchEdge(stgp.zone_origin_x + lx, szy, 1);
        }
        for (int lx = 0; lx < SceneRender::TNKN; ++lx) {
            int phsx = (lx + SceneRender::Get().base_cx) % SceneRender::TNKN;
            auto& sc = SceneRender::Get().terrain_chunks[phsz][phsx];
            auto& jid = SceneRender::Get().terrain_jolt_ids[phsz][phsx];
            int ax = stgp.zone_origin_x + lx, az = stgp.zone_origin_z + elz;
            if (!TerrainStreamQueue::Get().enqueue(&sc, {lx, elz}, stgp, &jid, ax, az))
                PushPendingStream(&sc, {lx, elz}, stgp, &jid, ax, az);
        }
    }
    TerrainQuery::Get().Init(&SceneRender::Get().terrain_chunks[0][0], SceneRender::TNKN,
                            CHUNK_SIZE, SceneRender::Get().tnoff_x, SceneRender::Get().tnoff_z,
                            SceneRender::Get().base_cx, SceneRender::Get().base_cz,
                            SceneRender::Get().zone_ox, SceneRender::Get().zone_oz);
    PhysicsTerrainRegion::Get().SetOrigin(
        SceneRender::Get().tnoff_x, SceneRender::Get().tnoff_z,
        SceneRender::Get().zone_ox * CHUNK_SIZE, SceneRender::Get().zone_oz * CHUNK_SIZE);
}
}  // HandleTerrainStreaming

#ifdef MONKEY_DUST_EDITOR
// ─── HandleFlythroughStreaming ─────────────────────────────────────────────────
// Shifts terrain when the F3 flythrough camera drifts in editor mode.
// Must run every frame regardless of skip_world (editor has skip_world=true).
// External linkage: also called from RunScenarioMode's --exec tick loop
// (game/src/autonomy/scenario_driver.cpp) so a scripted md.set_camera_pose
// actually pulls the terrain window along with it over subsequent ticks
// (task #158f/g) — previously only the interactive loop below ever called
// this.
void HandleFlythroughStreaming()
{
// Flythrough terrain streaming — must be OUTSIDE if(!skip_world) because skip_world=true in editor.
if (EditorCore::Get().editor_open && !EditorCore::Get().cam_game_mode) {
    const float half = (SceneRender::TNKN / 2) * CHUNK_SIZE;
    const float trig = CHUNK_SIZE * 0.5f;
    float fx   = EditorCore::Get().cam_target.x;
    float fz   = EditorCore::Get().cam_target.z;
    float cx_w = SceneRender::Get().tnoff_x + half;
    float cz_w = SceneRender::Get().tnoff_z + half;
    float fdx  = fx - cx_w, fdz = fz - cz_w;
    int shift_x = 0, shift_z = 0;
    if (fdx >  trig) shift_x =  (int)(fdx / CHUNK_SIZE);
    if (fdx < -trig) shift_x = -(int)(-fdx / CHUNK_SIZE);
    if (fdz >  trig) shift_z =  (int)(fdz / CHUNK_SIZE);
    if (fdz < -trig) shift_z = -(int)(-fdz / CHUNK_SIZE);
    if (shift_x || shift_z) {
        TerrainGenParams stgp = TerrainGenParams::ProcWorld();
        stgp.nav_cs = 50.f; stgp.nav_ch = 1.f;
        int absx = shift_x > 0 ? shift_x : -shift_x, dirx = shift_x > 0 ? 1 : -1;
        for (int sx = 0; sx < absx && sx < SceneRender::TNKN; ++sx) {
            int nox = SceneRender::Get().zone_ox + dirx;
            int nox_clamped = nox < 0 ? 0 : (nox > 63-SceneRender::TNKN ? 63-SceneRender::TNKN : nox);
            if (nox_clamped == SceneRender::Get().zone_ox) break;  // world-edge guard (task #173)
            SceneRender::Get().tnoff_x += dirx * CHUNK_SIZE;
            SceneRender::Get().zone_ox  = nox_clamped;
            SceneRender::Get().base_cx = (SceneRender::Get().base_cx + dirx + SceneRender::TNKN) % SceneRender::TNKN;
            stgp.world_offset_x = SceneRender::Get().tnoff_x; stgp.world_offset_z = SceneRender::Get().tnoff_z;
            stgp.force_noise = false;
            stgp.zone_origin_x = SceneRender::Get().zone_ox; stgp.zone_origin_z = SceneRender::Get().zone_oz;
            int elx  = dirx > 0 ? SceneRender::TNKN-1 : 0;
            int phsx = (elx + SceneRender::Get().base_cx) % SceneRender::TNKN;
            { int szx = stgp.zone_origin_x + (dirx > 0 ? elx-1 : elx);
              for (int lz = 0; lz < SceneRender::TNKN; ++lz)
                  TerrainAtlas_StitchEdge(szx, stgp.zone_origin_z + lz, 0); }
            for (int lz = 0; lz < SceneRender::TNKN; ++lz) {
                int phsz = (lz + SceneRender::Get().base_cz) % SceneRender::TNKN;
                auto& sc  = SceneRender::Get().terrain_chunks[phsz][phsx];
                auto& jid = SceneRender::Get().terrain_jolt_ids[phsz][phsx];
                int ax = stgp.zone_origin_x + elx, az = stgp.zone_origin_z + lz;
                if (!TerrainStreamQueue::Get().enqueue(&sc, {elx, lz}, stgp, &jid, ax, az)) {
                    { std::lock_guard<std::mutex> tg_lock(TerrainGen_StagingMutex());
                      TerrainGen_Build(sc, {elx, lz}, stgp); TerrainGen_Upload(sc); }
                    if (sc.loaded) {
                        JoltWorld::Get().RemoveBody(jid);
                        jid = JoltWorld::Get().AddTerrainMesh(sc.heightmap.h, TERRAIN_GRID, TERRAIN_STEP,
                            sc.center_x - CHUNK_SIZE*0.5f, sc.center_z - CHUNK_SIZE*0.5f);
                    }
                }
            }
        }
        int absz = shift_z > 0 ? shift_z : -shift_z, dirz = shift_z > 0 ? 1 : -1;
        for (int sz = 0; sz < absz && sz < SceneRender::TNKN; ++sz) {
            int noz = SceneRender::Get().zone_oz + dirz;
            int noz_clamped = noz < 0 ? 0 : (noz > 63-SceneRender::TNKN ? 63-SceneRender::TNKN : noz);
            if (noz_clamped == SceneRender::Get().zone_oz) break;  // world-edge guard (task #173)
            SceneRender::Get().tnoff_z += dirz * CHUNK_SIZE;
            SceneRender::Get().zone_oz  = noz_clamped;
            SceneRender::Get().base_cz = (SceneRender::Get().base_cz + dirz + SceneRender::TNKN) % SceneRender::TNKN;
            stgp.world_offset_x = SceneRender::Get().tnoff_x; stgp.world_offset_z = SceneRender::Get().tnoff_z;
            stgp.force_noise = false;
            stgp.zone_origin_x = SceneRender::Get().zone_ox; stgp.zone_origin_z = SceneRender::Get().zone_oz;
            int elz  = dirz > 0 ? SceneRender::TNKN-1 : 0;
            int phsz = (elz + SceneRender::Get().base_cz) % SceneRender::TNKN;
            { int szy = stgp.zone_origin_z + (dirz > 0 ? elz-1 : elz);
              for (int lx = 0; lx < SceneRender::TNKN; ++lx)
                  TerrainAtlas_StitchEdge(stgp.zone_origin_x + lx, szy, 1); }
            for (int lx = 0; lx < SceneRender::TNKN; ++lx) {
                int phsx = (lx + SceneRender::Get().base_cx) % SceneRender::TNKN;
                auto& sc  = SceneRender::Get().terrain_chunks[phsz][phsx];
                auto& jid = SceneRender::Get().terrain_jolt_ids[phsz][phsx];
                int ax = stgp.zone_origin_x + lx, az = stgp.zone_origin_z + elz;
                if (!TerrainStreamQueue::Get().enqueue(&sc, {lx, elz}, stgp, &jid, ax, az)) {
                    { std::lock_guard<std::mutex> tg_lock(TerrainGen_StagingMutex());
                      TerrainGen_Build(sc, {lx, elz}, stgp); TerrainGen_Upload(sc); }
                    if (sc.loaded) {
                        JoltWorld::Get().RemoveBody(jid);
                        jid = JoltWorld::Get().AddTerrainMesh(sc.heightmap.h, TERRAIN_GRID, TERRAIN_STEP,
                            sc.center_x - CHUNK_SIZE*0.5f, sc.center_z - CHUNK_SIZE*0.5f);
                    }
                }
            }
        }
        TerrainQuery::Get().Init(&SceneRender::Get().terrain_chunks[0][0], SceneRender::TNKN,
                                CHUNK_SIZE, SceneRender::Get().tnoff_x, SceneRender::Get().tnoff_z,
                                SceneRender::Get().base_cx, SceneRender::Get().base_cz,
                                SceneRender::Get().zone_ox, SceneRender::Get().zone_oz);
        PhysicsTerrainRegion::Get().SetOrigin(
            SceneRender::Get().tnoff_x, SceneRender::Get().tnoff_z,
            SceneRender::Get().zone_ox * CHUNK_SIZE, SceneRender::Get().zone_oz * CHUNK_SIZE);
    }
}
}  // HandleFlythroughStreaming
#endif  // MONKEY_DUST_EDITOR
