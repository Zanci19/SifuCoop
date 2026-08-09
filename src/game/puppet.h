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
void DriveActorTo(ue::UObject* actor, const ue::FVector& target, const ue::FRotator& rotation,
                  const ue::FVector& reported_velocity);

// The live puppet, or null. Never cache the result: the puppet is destroyed on
// level change and by F10.
ue::UObject* GetPuppet();

// Spawns a bare clone of the local player's character class at a position, with
// nothing configured on it. Used by the second-player path, which needs a body
// of its own to possess after the game mode hands it player one's.
ue::UObject* SpawnPlayerClone(const ue::FVector& location, const ue::FRotator& rotation);

// True only once Sifu's own relationship system has been set to friendly
// BETWEEN the local player and the remote player's body AND that has been read
// back and confirmed. It is the gate on replaying the peer's attacks: without a
// confirmed exemption a replayed swing is a live hitbox that kills the person
// you are playing with, so "we asked for it" is not good enough.
bool FriendlyRelationshipVerified();

}  // namespace sifucoop::game

