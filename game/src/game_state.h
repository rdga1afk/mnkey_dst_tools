#pragma once
#include <monkey_dust/ecs/engine_context.h>
#include <cstdint>

// GameState — POD стан гри, наслідує md::EngineContext.
// delta_time / fps / now_s / logic_tick / frame_index — у базовому класі.
// Оновлюється раз на тік (10 TPS), читається кожен кадр (60 FPS).
struct GameState : md::EngineContext {
    float    player_x = 0.0f, player_y = 0.0f, player_z = 0.0f;
    float    player_hp = 100.0f, player_max_hp = 100.0f;
    uint32_t player_faction_id = 1;
    float    game_time_hours = 8.0f;   // 0.0 - 24.0
    bool     in_combat = false;
    int      nearest_enemy_id = -1;
    uint32_t active_quest_id    = 0;
    uint8_t  quest_flags[8]     = {0};
    bool     quest_completed    = false;
    uint32_t completed_quests[4] = {0,0,0,0};
    char     active_mod_id[64]  = {};   // v6: which Flare mod is active
    // Interior portal transition (set by InteriorSystem::Tick, consumed in main.cpp)
    uint16_t pending_zone_id  = 0;   // 0 = no pending transition
    uint8_t  _gs_pad[2]       = {};
    float    pending_spawn_x  = 0.f;
    float    pending_spawn_z  = 0.f;
    // W-2: zone name banner (Skyrim-style) — set on zone enter, fades over 4s.
    char     zone_banner_name[48] = {};  // zone display name to show
    float    zone_banner_timer    = 0.f; // countdown in seconds; 0 = hidden
};
