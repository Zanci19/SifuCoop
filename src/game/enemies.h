#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {

// Host-authoritative enemy synchronisation.
//
// One machine's game is the source of truth for every enemy: their AI decides
// what happens, and the other machine's copies have their brains stopped and
// are driven from what the host reports. There is no alternative: hitboxes,
// animation timings, parry windows and AI all live inside the Windows
// executable, so only a running Sifu can decide whether a hit landed. A
// separate server process could relay packets but could never adjudicate.
//
// Enemies are paired between machines by a hash of their object name. Sifu
// pre-spawns every enemy into a pool with deterministic names, so the same
// enemy carries the same name on both machines -- no spawn hooking, no
// agreement on ordering, no fragile index matching.

void InitEnemies(std::uintptr_t module_base);

// Called each frame from the tick hook.
void TickEnemies();

// Resolves an enemy by the id used on the wire. Null when this machine has no
// such enemy, which is normal for a moment after a level loads.
ue::UObject* FindEnemyByHash(std::uint32_t hash);

// The wire id of the enemy that owns `attack_component`, or 0 if it is not one
// of ours. The attack hook only ever sees the component, so this is how an
// enemy's swing is turned into something the peer can act on -- and it is a
// table lookup rather than a walk back up to the actor, because it runs on the
// hot path of every attack in the level.
std::uint32_t EnemyHashForAttackComponent(const void* attack_component);

// Rows for the overlay's live sync table.
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

// Writes the whole tracked set to the log, pooled entries included. The
// overlay's table deliberately hides those; this is what you read when an
// enemy is missing and you need to know whether it exists at all.
void DumpRoster();

}  // namespace sifucoop::game
