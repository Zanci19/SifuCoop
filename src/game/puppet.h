#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {

void InitPuppet(std::uintptr_t module_base);

// Spawns a second character in front of the local player. Returns false and
// logs the reason on failure. Safe to call when one already exists (no-op).
bool SpawnPuppet();

// Destroys the puppet if one is alive.
void DespawnPuppet();

// Called each frame; handles the debug hotkeys.
void TickPuppet();

// Steers an actor toward a target using the game's movement component, so it
// produces real locomotion animation. Shared with enemy replication -- an
// enemy puppet and a player puppet are driven identically.
void DriveActorTo(ue::UObject* actor, const ue::FVector& target, const ue::FRotator& rotation);

// The live puppet, or null. Never cache the result: the puppet is destroyed on
// level change and by F10.
ue::UObject* GetPuppet();

}  // namespace sifucoop::game

