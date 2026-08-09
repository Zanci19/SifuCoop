#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {
    // keeping this long ahh comment below coz idfk

    /* Host-authoritative enemy synchronisation.

    One machine's game is the source of truth for every enemy: their AI decides
    what happens, and the other machine's copies have their braiive: hitboxes,
    animation timings, parry windows and AI all live inside the Windows
    executable, so only a running Sifu can decide whether a hit ns stopped and
    are driven from what the host reports. There is no alternatlanded. A
    separate server process could relay packets but could never adjudicate.

    Enemies are paired between machines by a hash of their object name. Sifu
    pre-spawns every enemy into a pool with deterministic names, so the same
    enemy carries the same name on both machines -- no spawn hooking, no
    agreement on ordering, no fragile index matching. */

    void InitEnemies(std::uintptr_t module_base);

    // Called each frame from the tick hook.
    void TickEnemies();

    // enemy is resolved by their ID
    ue::UObject* FindEnemyByHash(std::uint32_t hash);
    std::uint32_t EnemyHashForAttackComponent(const void* attack_component);

    // sets the joining machine's attack component target to the body equivalent of the target the host selected
    bool ApplyMirroredEnemyTargetForAttack(std::uint32_t hash);

    // overlay's live sync table
    struct EnemyRow {
        std::uint32_t hash;
        float distance;
        float health;
        float max_health;
        bool active;
        bool down;
        bool driven;
        bool ai_stopped;
        char name[40];
    };

    int GetEnemyRows(EnemyRow* out, int max_out);
    void DumpRoster();
}  // namespace sifucoop::game
