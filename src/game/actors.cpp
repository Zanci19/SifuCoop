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

bool SetPresentationVelocity(ue::UObject* actor, const ue::FVector& velocity) {
    if (!actor || !g_get_movement_component) return false;
    ue::UObject* movement = g_get_movement_component(actor);
    if (!movement) return false;
    std::memcpy(reinterpret_cast<std::uint8_t*>(movement) + kMovementVelocityOffset,
                &velocity, sizeof(velocity));

    // AActor::GetVelocity and Sifu's foot IK observe this root-scene value.
    // K2_GetRootComponent returns the puppet's capsule; never touch player 0.
    struct RootParams {
        ue::UObject* ReturnValue;
    } root = {};
    if (!ue::CallFunction(actor, L"K2_GetRootComponent", &root) || !root.ReturnValue) {
        return false;
    }
    std::memcpy(reinterpret_cast<std::uint8_t*>(root.ReturnValue) +
                    kSceneComponentVelocityOffset,
                &velocity, sizeof(velocity));
    return true;
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
    return params.ReturnValue;
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
