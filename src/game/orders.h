#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {

bool InstallOrderHook(std::uintptr_t module_base, ue::UObject* any_character);

bool IsOrderHookInstalled();

void ArmReplicatedKillInstigator(void* health_component, ue::UObject* instigator);
void ClearReplicatedKillInstigator();

bool InstallPlayOrderHook(std::uintptr_t module_base);

bool HaveAttackTemplate();

bool ApplyAttackToActor(ue::UObject* actor);

void ReplayAttackOnPuppet();

void ReplayAttackOnNearestEnemy();

void ApplyRemoteOrder(std::uint32_t order_type, std::int32_t attack_index,
                      std::int32_t attack_depth);

void PumpRemoteOrders();

void InvalidateAttackTemplate();

}
