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

// Sends a desired world-space velocity through the character movement component.
// This is native engine steering (not a transform write), so movement, collision
// and locomotion animation stay on the normal Sifu path. A true result means the
// request was dispatched; callers that need recovery still watch displacement.
// Publishes presentation velocity to UE's UMovementComponent. Used with the
// network puppet's transform drive so Sifu's AnimBP sees locomotion even though
// no physical AddMovementInput chase is running.
bool SetPresentationVelocity(ue::UObject* actor, const ue::FVector& velocity);

// The two components SetPresentationVelocity writes through, resolved once.
//
// Resolving them costs a reflection call (K2_GetRootComponent), and there is
// one place that must publish velocity and cannot afford reflection: inside
// UPlayerAnim's native update, where a nested ProcessEvent crashed the game.
// Resolve on the game thread, write from wherever.
struct PresentationTargets {
    ue::UObject* movement = nullptr;
    ue::UObject* root = nullptr;

    bool valid() const { return movement != nullptr && root != nullptr; }
};

PresentationTargets ResolvePresentationTargets(ue::UObject* actor);

// Pure memcpy into the two fields. No reflection, no allocation, safe to call
// from an engine callback.
void WritePresentationVelocity(const PresentationTargets& targets,
                               const ue::FVector& velocity);

// The locomotion band (ESpeedState: 0=V0 idle, 1=V1 walk, 2=V2 run, 3=V3 sprint).
//
// This is the SOURCE of the value. UPlayerAnim::m_SpeedState is only a copy that
// NativeUpdateAnimation refreshes from the movement component every frame, so
// writing the anim instance -- which the mod did for weeks -- changes nothing.
// A replicated body has no input, so the movement component computes V0 at every
// speed unless it is told otherwise. Native call, safe from an engine callback.
bool SetMovementSpeedState(ue::UObject* movement_component, int state);

// Same, resolving the movement component first (one native call). For driven
// enemies, which never pass through the player animation hook.
bool SetActorSpeedState(ue::UObject* actor, int state);
bool RequestDirectMove(ue::UObject* actor, const ue::FVector& desired_velocity,
                       bool force_max_speed = false);

// Hard reposition, for corrections too large to walk off.
//
// Tries K2_TeleportTo first, which refuses when the destination would not fit,
// and falls back to a sweep-free move when it does. A replicated body is a
// picture of where someone else already is, so "it would not fit" is not a
// reason to leave it behind. The result is verified by reading the actor's
// position back, so a true return means it really is there.
bool TeleportActor(ue::UObject* actor, const ue::FVector& location,
                   const ue::FRotator& rotation);

// Running totals: how often the fallback was needed, and how often even that
// failed to move the body. Both are reported in the heartbeat.
void GetTeleportFallbackCounts(std::uint32_t* fallbacks, std::uint32_t* hard_failures);

// --- Relationships ----------------------------------------------------------
//
// Sifu keeps a per-actor relationship map, USocialComponent::m_Relationships,
// a TMap<AActor*, ERelationshipTypes>. This is how the game knows one grunt
// must not punch another, and it is the only per-instigator switch the mod has
// found for who may fight whom.
//
// The enum was recovered from the generated enumerator-name table in the exe,
// in declaration order. Note that there are two distinct hostile values --
// `Enemy` is a standing disposition, `Fight` is being in a fight right now --
// and that Sifu ships a `Coop` type, which is exactly what this mod is doing.
namespace relationship {
constexpr int kUnknown = -1;
constexpr int kEnemy = 0;
constexpr int kFight = 1;
constexpr int kObject = 2;
constexpr int kNeutral = 3;
constexpr int kCoop = 4;
constexpr int kAlly = 5;
constexpr int kCount = 6;
const char* Name(int value);
}  // namespace relationship

ue::UObject* GetSocialComponent(ue::UObject* character);

// ABaseCharacter::BPF_GetRelationship. kUnknown when it cannot be asked;
// kNeutral is a real answer and is also what the game returns for a pair it has
// never heard of, so a readback of Neutral after writing something else means
// the write did not land.
int ReadRelationship(ue::UObject* from_actor, ue::UObject* to_actor);

// USocialComponent::BPF_ServerChangeRelationship. Returns whether the call was
// dispatched -- NOT whether it took effect. Always read it back.
bool WriteRelationship(ue::UObject* social, ue::UObject* toward, int value);

// Number of element slots in the relationship map. Used only as evidence:
// if a write neither changes the readback nor grows the map, the setter is a
// no-op and no amount of retrying will help.
int RelationshipMapSize(ue::UObject* social);

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

// Strips a trailing UE4 runtime instance number (`Base_2147475019`) off a leaf
// name in place, returning true and the number when one was present. That
// number is allocated by a per-process counter, so it is different on every
// machine -- hashing it made runtime-spawned enemies unpairable across a real
// two-machine session. See the comment in actors.cpp.
bool SplitRuntimeSuffix(char* name, std::uint32_t* number_out);

// Hash for a name that carried a runtime suffix: the stripped name plus the
// enemy's spawn ordinal within its group, which is stable on both machines.
std::uint32_t HashNameWithOrdinal(const char* text, int ordinal);
std::uint32_t ActorHash(ue::UObject* actor);

}  // namespace sifucoop::game
