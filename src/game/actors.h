#pragma once

#include <cstdint>

#include "../ue/reflection.h"

// Typed access to the parts of a Sifu character the co-op layer has to read and
// write every frame: health, guard, faction, death, and the three combat
// components.
//
// Everything here goes through either a native call resolved from the PDB or a
// member offset recovered from Unreal's generated reflection tables -- never
// through ProcessEvent. That distinction matters: the previous code reached the
// health component with GetComponentByClass, which is a full ProcessEvent, and
// running one per enemy per frame for a roomful of enemies is not affordable.

namespace sifucoop::game {

void InitActors(std::uintptr_t module_base);

// True once the offsets this file needs were present in the build table. When
// false every accessor below degrades to a no-op rather than reading garbage.
bool ActorsReady();

// A character with its components resolved once. Cheap to build (three native
// calls) and safe to hold for the duration of a frame, never across one: the
// actor can be destroyed by a level change or returned to the pool.
struct Fighter {
    ue::UObject* actor = nullptr;
    ue::UObject* health = nullptr;
    ue::UObject* attack = nullptr;
    ue::UObject* defense = nullptr;

    bool valid() const { return actor != nullptr; }
};

Fighter ResolveFighter(ue::UObject* actor);

// --- Vitals ----------------------------------------------------------------
//
// Sifu exposes BPF_GetMaxHealth but nothing for the current value, which is why
// earlier work could only ever ask "is this character down". m_fHealth is a
// reflected property, so its offset comes out of the exe exactly.

float GetHealth(const Fighter& fighter);
float GetMaxHealth(const Fighter& fighter);

// Writes the value straight into the component. Correct for a puppet (whose
// outcome is decided on the machine that owns it) and for a client's enemies
// (whose outcome is decided by the host); wrong for anything that is supposed
// to resolve its own damage -- use ApplyDamage for that.
void SetHealth(const Fighter& fighter, float health);

// The guard/structure gauge. Cosmetic on a puppet, but its absence is very
// visible: a remote player would otherwise always look untouched.
float GetGuard(const Fighter& fighter);
void SetGuard(const Fighter& fighter, float guard);

// Runs the game's own damage path, so death, hit reaction and score all happen
// as they normally would. This is how the host applies the damage its peer
// dealt: FDamageInfos does not reflect its damage value, so it cannot be
// rebuilt, but this overload takes a plain float.
void ApplyDamage(const Fighter& fighter, float amount);

bool IsDown(const Fighter& fighter);
bool IsDead(const Fighter& fighter);

// Drives the visible down-state machine, not just the flag. SetIsDown alone
// leaves the character standing.
void SetDown(const Fighter& fighter, bool down);

// --- Identity and behaviour ------------------------------------------------

int GetFaction(ue::UObject* actor);
void SetFaction(ue::UObject* actor, int faction);
void SetInvincible(ue::UObject* actor, bool invincible);

// The character's own combo asset. Replaying an attack on a client-side enemy
// needs the tree that enemy actually uses; reading it locally means the wire
// never has to carry an asset path.
ue::UObject* GetDefaultCombo(const Fighter& fighter);

// Switches a character's AI off. Reflection rather than a direct call:
// UBrainComponent::StopLogic is an empty base that ICF folded onto unrelated
// stubs, but its exec thunk dispatches virtually and so reaches the behaviour
// tree's real implementation.
bool StopBrain(ue::UObject* actor);

// --- Presence --------------------------------------------------------------

// Hidden and collision together, because for our purposes they are one idea:
// "is this character part of the fight". Used to lift an enemy out of the pool
// on the client when the host has activated it, and to put back one the host
// has not -- a teleport alone leaves a pooled enemy invisible and intangible.
void SetActorPresent(ue::UObject* actor, bool present);

// Hard reposition, for corrections too large to walk off.
bool TeleportActor(ue::UObject* actor, const ue::FVector& location,
                   const ue::FRotator& rotation);

// --- Pool ------------------------------------------------------------------
//
// Sifu pre-spawns every enemy far below the level and lifts them in as needed,
// so "is this enemy in the fight" is a question about its Z coordinate.
constexpr float kPooledZ = -900000.f;
bool IsPooled(const ue::FVector& location);

// FNV-1a over a character's leaf object name. The pool makes those names
// identical on both machines, so this is how an enemy is addressed on the wire.
std::uint32_t HashName(const char* text);
bool LeafName(ue::UObject* object, char* out, int out_size);
std::uint32_t ActorHash(ue::UObject* actor);

}  // namespace sifucoop::game
