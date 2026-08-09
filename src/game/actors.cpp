#include "actors.h"

#include <windows.h>

#include <cstring>

#include "../core/log.h"
#include "../core/offsets.g.h"

namespace sifucoop::game {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;

using GetHealthComponentFn = ue::UObject*(__fastcall*)(ue::UObject* actor);
using GetAttackComponentFn = ue::UObject*(__fastcall*)(ue::UObject* actor);
using GetDefenseComponentFn = ue::UObject*(__fastcall*)(ue::UObject* actor);
using ApplyDamageFn = void(__fastcall*)(ue::UObject* health, float amount);
using IsDownFn = bool(__fastcall*)(const ue::UObject* health);
using IsDeadFn = bool(__fastcall*)(const ue::UObject* health);
using SetIsDownFn = void(__fastcall*)(ue::UObject* health, bool down);
using SetDownStateFn = void(__fastcall*)(ue::UObject* health, int state, bool force,
                                         bool* out);
using GetFactionFn = int(__fastcall*)(const ue::UObject* actor);
using SetFactionFn = void(__fastcall*)(ue::UObject* actor, int faction);
using SetInvincibilityFn = void(__fastcall*)(ue::UObject* actor, bool invincible);
using BrainClassFn = void*(__fastcall*)();
using GetMovementComponentFn = ue::UObject*(__fastcall*)(const ue::UObject* actor);
using RequestDirectMoveFn = void(__fastcall*)(ue::UObject* component, const ue::FVector& velocity, bool force_max_speed);

GetHealthComponentFn g_get_health = nullptr;
GetAttackComponentFn g_get_attack = nullptr;
GetDefenseComponentFn g_get_defense = nullptr;
ApplyDamageFn g_apply_damage = nullptr;
IsDownFn g_is_down = nullptr;
IsDeadFn g_is_dead = nullptr;
SetIsDownFn g_set_is_down = nullptr;
SetDownStateFn g_set_down_state = nullptr;
GetFactionFn g_get_faction = nullptr;
SetFactionFn g_set_faction = nullptr;
SetInvincibilityFn g_set_invincibility = nullptr;
BrainClassFn g_brain_class = nullptr;
GetMovementComponentFn g_get_movement_component = nullptr;
RequestDirectMoveFn g_request_direct_move = nullptr;
// UFightingMovementComponent::SetSpeedState(ESpeedState). See the note on
// SetMovementSpeedState below for why this, and not the anim instance, is the
// thing that has to be written.
using SetSpeedStateFn = void(__fastcall*)(ue::UObject*, std::uint8_t);
SetSpeedStateFn g_set_movement_speed_state = nullptr;

bool g_ready = false;

// EDownState, in declaration order.
constexpr int kDownStateDown = 0;
constexpr int kDownStateNone = 9;

// Reading a member of a component we resolved ourselves is safe, but the offset
// arrives from a generated table -- so a zero (meaning "this build did not
// provide it") must never be treated as "offset 0", which would read the vtable
// pointer as a float.
float* FloatAt(ue::UObject* object, std::uint32_t offset) {
    if (!object || offset == 0) return nullptr;
    return reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(object) + offset);
}

ue::UObject** ObjectAt(ue::UObject* object, std::uint32_t offset) {
    if (!object || offset == 0) return nullptr;
    return reinterpret_cast<ue::UObject**>(reinterpret_cast<std::uint8_t*>(object) + offset);
}

}  // namespace

void InitActors(std::uintptr_t base) {
    g_get_health =
        reinterpret_cast<GetHealthComponentFn>(base + offsets::UCharacterHealthComponent_Get);
    g_get_attack = reinterpret_cast<GetAttackComponentFn>(base + offsets::UAttackComponent_Get);
    g_get_defense =
        reinterpret_cast<GetDefenseComponentFn>(base + offsets::UDefenseComponent_Get);
    g_apply_damage =
        reinterpret_cast<ApplyDamageFn>(base + offsets::UHealthComponent_BPF_ApplyDamage);
    g_is_down = reinterpret_cast<IsDownFn>(base + offsets::UCharacterHealthComponent_IsDown);
    g_is_dead = reinterpret_cast<IsDeadFn>(base + offsets::UHealthComponent_IsDead);
    g_set_is_down =
        reinterpret_cast<SetIsDownFn>(base + offsets::UCharacterHealthComponent_SetIsDown);
    g_set_down_state = reinterpret_cast<SetDownStateFn>(
        base + offsets::UCharacterHealthComponent_InternalSetDownState);
    g_get_faction =
        reinterpret_cast<GetFactionFn>(base + offsets::AFightingCharacter_GetFaction);
    g_set_faction =
        reinterpret_cast<SetFactionFn>(base + offsets::AFightingCharacter_BPF_SetFaction);
    g_set_invincibility = reinterpret_cast<SetInvincibilityFn>(
        base + offsets::AFightingCharacter_BPF_SetInvincibility);
    g_brain_class = reinterpret_cast<BrainClassFn>(base + offsets::UBrainComponent_StaticClass);
    g_get_movement_component = offsets::ACharacter_GetMovementComponent
                                   ? reinterpret_cast<GetMovementComponentFn>(base + offsets::ACharacter_GetMovementComponent)
                                   : nullptr;
    g_request_direct_move = offsets::UCharacterMovementComponent_RequestDirectMove
                                 ? reinterpret_cast<RequestDirectMoveFn>(base + offsets::UCharacterMovementComponent_RequestDirectMove) : nullptr;
    g_set_movement_speed_state =
        offsets::UFightingMovementComponent_SetSpeedState
            ? reinterpret_cast<SetSpeedStateFn>(
                  base + offsets::UFightingMovementComponent_SetSpeedState)
            : nullptr;

    // Only the offsets that would silently corrupt a read are treated as
    // mandatory; the rest degrade to "that feature is off".
    g_ready = offsets::UCharacterHealthComponent_Get != 0 &&
              offsets::M_UHealthComponent_fHealth != 0 &&
              offsets::M_UHealthComponent_fMaxHealth != 0;

    SC_LOG("actors: %s (health=+0x%X maxhealth=+0x%X guard=+0x%X combo=+0x%X)",
           g_ready ? "ready" : "DEGRADED -- vitals unavailable on this build",
           offsets::M_UHealthComponent_fHealth, offsets::M_UHealthComponent_fMaxHealth,
           offsets::M_UDefenseComponent_fCurrentGuard, offsets::M_UAttackComponent_DefaultCombo);
}

bool ActorsReady() { return g_ready; }

Fighter ResolveFighter(ue::UObject* actor) {
    Fighter fighter;
    if (!actor) return fighter;
    fighter.actor = actor;
    if (g_get_health) fighter.health = g_get_health(actor);
    if (g_get_attack) fighter.attack = g_get_attack(actor);
    if (g_get_defense) fighter.defense = g_get_defense(actor);
    return fighter;
}

float GetHealth(const Fighter& fighter) {
    const float* value = FloatAt(fighter.health, offsets::M_UHealthComponent_fHealth);
    return value ? *value : 0.f;
}

float GetMaxHealth(const Fighter& fighter) {
    const float* value = FloatAt(fighter.health, offsets::M_UHealthComponent_fMaxHealth);
    return value ? *value : 0.f;
}

void SetHealth(const Fighter& fighter, float health) {
    float* value = FloatAt(fighter.health, offsets::M_UHealthComponent_fHealth);
    if (!value) return;
    if (health < 0.f) health = 0.f;
    *value = health;
}

float GetGuard(const Fighter& fighter) {
    const float* value = FloatAt(fighter.defense, offsets::M_UDefenseComponent_fCurrentGuard);
    return value ? *value : 0.f;
}

void SetGuard(const Fighter& fighter, float guard) {
    float* value = FloatAt(fighter.defense, offsets::M_UDefenseComponent_fCurrentGuard);
    if (!value) return;
    if (guard < 0.f) guard = 0.f;
    *value = guard;
}

void ApplyDamage(const Fighter& fighter, float amount) {
    if (!fighter.health || !g_apply_damage || amount <= 0.f) return;
    g_apply_damage(fighter.health, amount);
}

bool IsDown(const Fighter& fighter) {
    if (!fighter.health || !g_is_down) return false;
    return g_is_down(fighter.health);
}

bool IsDead(const Fighter& fighter) {
    if (!fighter.health || !g_is_dead) return false;
    return g_is_dead(fighter.health);
}

void SetDown(const Fighter& fighter, bool down) {
    if (!fighter.health) return;
    // The flag alone only records the fact; InternalSetDownState is what makes
    // the character actually fall over. Learned when the puppet refused to die.
    if (g_set_is_down) g_set_is_down(fighter.health, down);
    if (g_set_down_state) {
        bool scratch = false;
        g_set_down_state(fighter.health, down ? kDownStateDown : kDownStateNone, true, &scratch);
    }
}

int GetFaction(ue::UObject* actor) {
    if (!actor || !g_get_faction) return -1;
    return g_get_faction(actor) & 0xFF;
}

void SetFaction(ue::UObject* actor, int faction) {
    if (!actor || !g_set_faction) return;
    g_set_faction(actor, faction);
}

void SetInvincible(ue::UObject* actor, bool invincible) {
    if (!actor || !g_set_invincibility) return;
    g_set_invincibility(actor, invincible);
}

ue::UObject* GetDefaultCombo(const Fighter& fighter) {
    ue::UObject** slot = ObjectAt(fighter.attack, offsets::M_UAttackComponent_DefaultCombo);
    return slot ? *slot : nullptr;
}

bool StopBrain(ue::UObject* actor) {
    if (!actor || !g_brain_class) return false;

    struct ControllerParams {
        ue::UObject* ReturnValue;
    } controller = {};
    if (!ue::CallFunction(actor, L"GetController", &controller) || !controller.ReturnValue) {
        return false;
    }

    struct ComponentParams {
        void* ComponentClass;
        ue::UObject* ReturnValue;
    } component = {};
    component.ComponentClass = g_brain_class();
    if (!ue::CallFunction(controller.ReturnValue, L"GetComponentByClass", &component) ||
        !component.ReturnValue) {
        return false;
    }

    // FString Reason, by value. Zeroed is a valid empty string and its
    // destructor on a null pointer is a no-op.
    struct FStringParam {
        void* data;
        std::int32_t num;
        std::int32_t max;
    } reason = {};
    return ue::CallFunction(component.ReturnValue, L"StopLogic", &reason);
}

void SetActorPresent(ue::UObject* actor, bool present) {
    if (!actor) return;

    struct HiddenParams {
        bool bNewHidden;
    } hidden = {};
    hidden.bNewHidden = !present;
    ue::CallFunction(actor, L"SetActorHiddenInGame", &hidden);

    struct CollisionParams {
        bool bNewActorEnableCollision;
    } collision = {};
    collision.bNewActorEnableCollision = present;
    ue::CallFunction(actor, L"SetActorEnableCollision", &collision);
}


// UE 4.26 fields recovered from the shipped PDB. Sifu's locomotion/foot-IK
// graph reads the root component's ComponentVelocity (not merely the movement
// component's Velocity), which is why the first direct-drive presentation fix
// still left legs idle.
constexpr std::uintptr_t kMovementVelocityOffset = 0xD4;
constexpr std::uintptr_t kSceneComponentVelocityOffset = 0x150;

// The character's real velocity, straight off its movement component.
//
// This replaces differencing the actor's position between frames, which is how
// the mod used to produce the velocity it puts on the wire. That looked
// equivalent and is not: the transform only changes on frames where movement
// actually integrated, and at 165 fps against a 60 Hz movement update most
// frames repeat the previous position. The derived velocity was therefore the
// true speed on some frames and exactly zero on the rest.
//
// Downstream that strobe is fatal rather than merely noisy. BaseMovementDB
// gives the V0->V1 blend 0.3 s and V0->V3 a full second, so a speed band that
// flips several times a second restarts a blend that never completes, and the
// character stays in the pose it started from. That is "the remote player lifts
// a leg and stops".
bool GetActorVelocity(ue::UObject* actor, ue::FVector* out) {
    if (!actor || !out || !g_get_movement_component) return false;
    ue::UObject* movement = g_get_movement_component(actor);
    if (!movement) return false;
    std::memcpy(out, reinterpret_cast<const std::uint8_t*>(movement) + kMovementVelocityOffset,
                sizeof(*out));
    return true;
}

PresentationTargets ResolvePresentationTargets(ue::UObject* actor) {
    PresentationTargets targets;
    if (!actor || !g_get_movement_component) return targets;
    targets.movement = g_get_movement_component(actor);

    // AActor::GetVelocity and Sifu's foot IK observe this root-scene value.
    // K2_GetRootComponent returns the puppet's capsule; never touch player 0.
    struct RootParams {
        ue::UObject* ReturnValue;
    } root = {};
    if (ue::CallFunction(actor, L"K2_GetRootComponent", &root)) targets.root = root.ReturnValue;
    return targets;
}

void WritePresentationVelocity(const PresentationTargets& targets,
                               const ue::FVector& velocity) {
    if (targets.movement) {
        std::memcpy(reinterpret_cast<std::uint8_t*>(targets.movement) + kMovementVelocityOffset,
                    &velocity, sizeof(velocity));
    }
    if (targets.root) {
        std::memcpy(reinterpret_cast<std::uint8_t*>(targets.root) + kSceneComponentVelocityOffset,
                    &velocity, sizeof(velocity));
    }
}

// Where the locomotion band actually lives.
//
// UPlayerAnim::m_SpeedState is a COPY. The value belongs to the movement
// component, which computes it during its own tick from input a replicated body
// does not have -- so for the puppet and for every driven enemy it read V0 at
// every speed, and the graph faithfully played the idle state at 850 units/s.
// Writing the anim instance's copy, which is what the mod did for weeks, is
// writing a mirror: NativeUpdateAnimation refreshes it from here on the next
// frame regardless.
//
// Order matters as much as the value. Written from inside the animation update,
// after the movement component has ticked and before the graph samples it --
// the same placement that made the velocity injection work.
bool SetMovementSpeedState(ue::UObject* movement_component, int state) {
    if (!movement_component || !g_set_movement_speed_state) return false;
    if (state < 0 || state > 3) return false;  // ESpeedState V0..V3
    g_set_movement_speed_state(movement_component, static_cast<std::uint8_t>(state));
    return true;
}

bool SetActorSpeedState(ue::UObject* actor, int state) {
    if (!actor || !g_get_movement_component) return false;
    return SetMovementSpeedState(g_get_movement_component(actor), state);
}

bool SetPresentationVelocity(ue::UObject* actor, const ue::FVector& velocity) {
    const PresentationTargets targets = ResolvePresentationTargets(actor);
    WritePresentationVelocity(targets, velocity);
    return targets.valid();
}
bool RequestDirectMove(ue::UObject* actor, const ue::FVector& desired_velocity,
                       bool force_max_speed) {
    if (!actor || !g_get_movement_component || !g_request_direct_move) return false;

    // Do not cache this raw pointer. Pawns and their movement components are
    // rebuilt on respawn, travel and pooling; resolving this lightweight native
    // getter each tick is safer than ever calling a stale component.
    ue::UObject* component = g_get_movement_component(actor);
    if (!component) return false;
    g_request_direct_move(component, desired_velocity, force_max_speed);
    return true;  // dispatched; a caller may still watch actual displacement
}

// How often the sweep-free fallback below was needed, and how often even that
// left the actor somewhere other than where it was told to go.
std::uint32_t g_teleport_fallbacks = 0;
std::uint32_t g_teleport_hard_failures = 0;

// Move without the encroachment test.
//
// K2_SetActorLocation's parameter block contains an FHitResult whose exact
// layout is not worth recovering, so the buffer is generously oversized and
// zeroed: ProcessEvent copies each parameter at the offset the UFunction says,
// and everything we do not fill stays zero. Only the first two parameters
// matter and both sit at offsets that cannot move -- NewLocation at 0x00,
// bSweep at 0x0C. bTeleport is left false, which is correct for a body that is
// not simulating physics.
bool SetLocationNoSweep(ue::UObject* actor, const ue::FVector& location) {
    std::uint8_t params[512] = {};
    std::memcpy(params + 0x00, &location, sizeof(location));
    params[0x0C] = 0;  // bSweep
    return ue::CallFunction(actor, L"K2_SetActorLocation", params);
}

bool SetRotationDirect(ue::UObject* actor, const ue::FRotator& rotation) {
    struct Params {
        ue::FRotator NewRotation;
        bool bTeleportPhysics;
        bool ReturnValue;
    } params = {};
    params.NewRotation = rotation;
    params.bTeleportPhysics = true;
    return ue::CallFunction(actor, L"K2_SetActorRotation", &params);
}

bool TeleportActor(ue::UObject* actor, const ue::FVector& location,
                   const ue::FRotator& rotation) {
    // K2_TeleportTo's parameter block is just (FVector, FRotator, bool) -- no
    // FHitResult whose layout would have to be guessed.
    struct Params {
        ue::FVector DestLocation;
        ue::FRotator DestRotation;
        bool ReturnValue;
    } params = {};
    params.DestLocation = location;
    params.DestRotation = rotation;
    // Two different questions, and returning the wrong one hid a failure: the
    // call succeeding means the UFunction was found, while ReturnValue is
    // whether the actor actually moved. K2_TeleportTo refuses when the
    // destination would not fit, so a caller that only checked the former
    // believed it had repositioned something that had not budged.
    if (!ue::CallFunction(actor, L"K2_TeleportTo", &params)) return false;
    if (params.ReturnValue) return true;

    // Refused. Until now that was the end of it: the puppet simply was not
    // moved, and the live log showed 798 of 823 consecutive frames refused
    // while it sat 70 units behind the peer. It was not lagging, it was being
    // left where it was -- and driven enemies went through the same path with
    // the result discarded entirely, so they stood still on the joining side.
    //
    // A replicated body is not a physical object arriving somewhere; it is a
    // picture of where someone else already is. If the capsule does not fit,
    // the answer is to put it there anyway, not to stay behind.
    ++g_teleport_fallbacks;
    SetLocationNoSweep(actor, location);
    SetRotationDirect(actor, rotation);

    // Trust nothing: confirm it actually landed. Anything that still cannot be
    // moved is a genuinely stuck body and worth knowing about.
    //
    // Sampled rather than checked every time. A refusal can affect the puppet
    // and every driven enemy on the same frame, and the confirmation is another
    // ProcessEvent each -- which is exactly the per-enemy-per-frame reflection
    // cost this file exists to avoid. Four times a second is plenty to notice a
    // body that is truly stuck.
    static DWORD last_verify_ms = 0;
    const DWORD verify_now = GetTickCount();
    if (verify_now - last_verify_ms < 250) return true;
    last_verify_ms = verify_now;

    ue::FVector now = {};
    if (!ue::GetActorLocation(actor, &now)) return false;
    const float dx = now.X - location.X;
    const float dy = now.Y - location.Y;
    const float dz = now.Z - location.Z;
    const bool landed = (dx * dx + dy * dy + dz * dz) < (4.f * 4.f);
    if (!landed) ++g_teleport_hard_failures;
    return landed;
}

void GetTeleportFallbackCounts(std::uint32_t* fallbacks, std::uint32_t* hard_failures) {
    if (fallbacks) *fallbacks = g_teleport_fallbacks;
    if (hard_failures) *hard_failures = g_teleport_hard_failures;
}

namespace relationship {
const char* Name(int value) {
    switch (value) {
        case kEnemy: return "Enemy";
        case kFight: return "Fight";
        case kObject: return "Object";
        case kNeutral: return "Neutral";
        case kCoop: return "Coop";
        case kAlly: return "Ally";
        default: return "?";
    }
}
}  // namespace relationship

ue::UObject* GetSocialComponent(ue::UObject* character) {
    if (!character) return nullptr;
    struct Params {
        ue::UObject* ReturnValue;
    } params = {};
    if (!ue::CallFunction(character, L"BPF_GetSocialComponent", &params)) return nullptr;
    return params.ReturnValue;
}

int ReadRelationship(ue::UObject* from_actor, ue::UObject* to_actor) {
    if (!from_actor || !to_actor) return relationship::kUnknown;
    struct Params {
        ue::UObject* Actor;
        std::uint8_t ReturnValue;
    } params = {};
    params.Actor = to_actor;
    if (!ue::CallFunction(from_actor, L"BPF_GetRelationship", &params)) {
        return relationship::kUnknown;
    }
    return params.ReturnValue;
}

bool WriteRelationship(ue::UObject* social, ue::UObject* toward, int value) {
    if (!social || !toward || value < 0) return false;
    // BPF_ServerChangeRelationship forwards to MulticastChangeRelationship, and
    // that implementation walks the actor it is handed. Restarting a level
    // crashed here -- reading a live-looking heap address inside
    // SocialComponent.cpp:420 -- because the enemy or puppet being pointed at
    // had already been torn down with the old world while this side was still
    // asserting relationships against it. Both ends are checked, because either
    // one can be the dead one.
    if (!ue::IsValidObject(social) || !ue::IsValidObject(toward)) return false;
    struct Params {
        ue::UObject* Actor;
        std::uint8_t eRelation;
    } params = {};
    params.Actor = toward;
    params.eRelation = static_cast<std::uint8_t>(value);
    return ue::CallFunction(social, L"BPF_ServerChangeRelationship", &params);
}

// USocialComponent::m_Relationships, offset from Unreal's property table for
// USocialComponent. A TMap is a TSet of pairs, whose first member is the
// sparse array's TArray {void* Data; int32 Num; int32 Max} -- so the element
// count sits 8 bytes in. Read-only, and only ever used as evidence about
// whether a write landed.
constexpr std::uintptr_t kSocialRelationshipsMap = 0x0318;

int RelationshipMapSize(ue::UObject* social) {
    if (!social) return -1;
    std::int32_t num = 0;
    std::memcpy(&num,
                reinterpret_cast<const std::uint8_t*>(social) + kSocialRelationshipsMap + 8,
                sizeof(num));
    if (num < 0 || num > 4096) return -1;  // implausible: do not report a guess
    return num;
}

bool IsPooled(const ue::FVector& location) { return location.Z < kPooledZ; }

std::uint32_t HashName(const char* text) {
    std::uint32_t hash = 2166136261u;
    for (const char* p = text; p && *p; ++p) {
        hash ^= static_cast<std::uint8_t>(*p);
        hash *= 16777619u;
    }
    return hash;
}

bool LeafName(ue::UObject* object, char* out, int out_size) {
    if (!out || out_size <= 0) return false;
    out[0] = '\0';
    char path[256] = {};
    if (!ue::GetObjectPathName(object, path, sizeof(path))) return false;
    const char* leaf = strrchr(path, '.');
    leaf = leaf ? leaf + 1 : path;
    lstrcpynA(out, leaf, out_size);
    return out[0] != '\0';
}

std::uint32_t ActorHash(ue::UObject* actor) {
    char name[128] = {};
    if (!LeafName(actor, name, sizeof(name))) return 0;
    return HashName(name);
}

// UE4 names an actor spawned at runtime `Base_<number>`, where the number comes
// from a per-process counter that walks *down* from MAX_int32. It therefore
// differs on every machine and every launch: the same grunt in the same fight
// was `000_AISpawner_Group015_Grunt_Character_2147475019` on one machine and
// `..._2147473188` on the other.
//
// This is what broke enemy pairing between two real machines. Enemies placed in
// the level's pool are named `..._C_0` and match perfectly, which is why a
// pooled roster of 62 enemies tested clean across launches -- but the enemies
// that actually fight a room are spawned by an AISpawner at runtime, and every
// one of those hashed differently on each side. The receiving machine found no
// enemy for any id the host sent (driven=0, unmatched=every active enemy).
//
// Anything at or above this floor is a runtime counter value rather than a real
// instance index; `..._C_0` and `..._Group015` stay untouched.
constexpr std::uint32_t kRuntimeNameFloor = 2000000000u;

bool SplitRuntimeSuffix(char* name, std::uint32_t* number_out) {
    if (!name || !number_out) return false;
    char* underscore = strrchr(name, '_');
    if (!underscore || !underscore[1]) return false;

    unsigned long long value = 0;
    for (const char* p = underscore + 1; *p; ++p) {
        if (*p < '0' || *p > '9') return false;   // not a pure number: leave it alone
        value = value * 10 + static_cast<unsigned long long>(*p - '0');
        if (value > 0xFFFFFFFFull) return false;  // absurd; treat as part of the name
    }
    if (value < kRuntimeNameFloor) return false;

    *number_out = static_cast<std::uint32_t>(value);
    *underscore = '\0';
    return true;
}

std::uint32_t HashNameWithOrdinal(const char* text, int ordinal) {
    // Same FNV-1a step as HashName, continued over the ordinal, so two enemies
    // spawned by one group stay distinct. Only ever applied to names that had a
    // runtime suffix, so plain pooled names keep the exact hash they always had.
    std::uint32_t hash = HashName(text);
    hash ^= static_cast<std::uint32_t>(ordinal) & 0xFFu;
    hash *= 16777619u;
    return hash;
}

}  // namespace sifucoop::game
