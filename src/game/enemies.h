#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {

void InitEnemies(std::uintptr_t module_base);

void TickEnemies();

void ForgetEnemyWorldObjects();
void NotifyPuppetWillBeDestroyed(ue::UObject* puppet);

ue::UObject* FindEnemyByHash(std::uint32_t hash);

std::uint32_t EnemyHashForActor(const void* actor);
std::uint32_t EnemyHashForAttackComponent(const void* attack_component);
std::uint32_t EnemyHashForHealthComponent(const void* health_component);

bool ApplyMirroredEnemyTargetForAttack(std::uint32_t hash);

void NoteEnemyDeathAnimation(std::uint32_t hash, ue::UObject* animation);

bool EnemyRunsLocalBrain(std::uint32_t hash);

bool EnemyActionsAreLocallyAuthoritative(std::uint32_t hash, bool include_dead = false);

void NoteEnemyReactionOrder(const void* actor, unsigned int order_type);

bool EnemyMustRemainDead(const void* health_component);

struct EnemyAttackContext {
    ue::UObject* actor = nullptr;
    ue::UObject* attack_component = nullptr;
    ue::UObject* ai_fighting = nullptr;
    ue::UObject* target = nullptr;
};

bool PrepareMirroredEnemyAttack(std::uint32_t hash, EnemyAttackContext* out);

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

}
