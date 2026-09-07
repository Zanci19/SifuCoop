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

ue::UObject* SpawnPlayerClone(const ue::FVector& location, const ue::FRotator& rotation);

bool FriendlyRelationshipVerified();

}
