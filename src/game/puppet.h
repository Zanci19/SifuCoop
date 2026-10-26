#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {

void InitPuppet(std::uintptr_t module_base);

bool SpawnPuppet();

void DespawnPuppet();

void PreparePuppetLifecycle();

void TickPuppet();

bool CoopGameplayActive();

void NotifyLocalAttackForCosmetic();
void NotifyLocalSequenceSent();

void DriveActorTo(ue::UObject* actor, const ue::FVector& target, const ue::FRotator& rotation,
                  const ue::FVector& reported_velocity);

ue::UObject* GetPuppet();

// Reads the action animation an enemy's anim instance is currently playing
// (UPlayerAnim::m_LastActionAnim and its cursor). False when the instance is
// not UPlayerAnim-sized, so nothing is read past a smaller object.
bool ReadEnemyActionAnim(ue::UObject* anim_instance, ue::UObject** asset, float* cursor);

ue::UObject* SpawnPlayerClone(const ue::FVector& location, const ue::FRotator& rotation);

bool FriendlyRelationshipVerified();

}
