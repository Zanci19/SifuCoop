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
using SetRelationshipFn = void(__fastcall*)(ue::UObject* social, ue::UObject* toward,
                                             std::uint8_t relationship);

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
SetRelationshipFn g_set_relationship = nullptr;

using HealthKillFn = void(__fastcall*)(ue::UObject*, std::int32_t, ue::UObject*, ue::UObject*,
                                       bool, bool);
HealthKillFn g_health_kill = nullptr;

using HealthNotifyFn = void(__fastcall*)(ue::UObject*);
HealthNotifyFn g_on_rep_set_is_down = nullptr;
HealthNotifyFn g_on_character_stands_up = nullptr;

using SetSpeedStateFn = void(__fastcall*)(ue::UObject*, std::uint8_t);
SetSpeedStateFn g_set_movement_speed_state = nullptr;
using SetCurrentPoseAssetFn = void(__fastcall*)(ue::UObject*, ue::UObject*);
SetCurrentPoseAssetFn g_set_current_pose_asset = nullptr;

bool g_ready = false;

constexpr int kDownStateDown = 0;

constexpr int kDownStateDeath = 7;
constexpr int kDownStateNone = 9;

float* FloatAt(ue::UObject* object, std::uint32_t offset) {
    if (!object || offset == 0) return nullptr;
    return reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(object) + offset);
}

ue::UObject** ObjectAt(ue::UObject* object, std::uint32_t offset) {
    if (!object || offset == 0) return nullptr;
    return reinterpret_cast<ue::UObject**>(reinterpret_cast<std::uint8_t*>(object) + offset);
}

}

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
    g_set_current_pose_asset =
        offsets::USCAnimInstance_SetCurrentPoseAsset
            ? reinterpret_cast<SetCurrentPoseAssetFn>(
                  base + offsets::USCAnimInstance_SetCurrentPoseAsset)
            : nullptr;
    g_set_relationship = offsets::USocialComponent_SetRelationship
        ? reinterpret_cast<SetRelationshipFn>(
              base + offsets::USocialComponent_SetRelationship)
        : nullptr;
    g_health_kill = offsets::UHealthComponent_Kill
                        ? reinterpret_cast<HealthKillFn>(base + offsets::UHealthComponent_Kill)
                        : nullptr;
    g_on_rep_set_is_down =
        offsets::UCharacterHealthComponent_OnRepSetIsDown
            ? reinterpret_cast<HealthNotifyFn>(
                  base + offsets::UCharacterHealthComponent_OnRepSetIsDown)
            : nullptr;
    g_on_character_stands_up =
        offsets::UCharacterHealthComponent_OnCharacterStandsUp
            ? reinterpret_cast<HealthNotifyFn>(
                  base + offsets::UCharacterHealthComponent_OnCharacterStandsUp)
            : nullptr;

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

bool KillWithAnimation(const Fighter& fighter, ue::UObject* instigator,
                       ue::UObject* death_animation) {

    if (!fighter.health || !g_health_kill) return false;

    if (!instigator) return false;
    g_health_kill(fighter.health, 0, instigator, death_animation, false, false);
    return true;
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

void NotifyDownStateChanged(const Fighter& fighter, bool down) {
    if (!fighter.health) return;
    if (down) {
        if (g_on_rep_set_is_down) g_on_rep_set_is_down(fighter.health);
    } else if (g_on_character_stands_up) {
        g_on_character_stands_up(fighter.health);
    }
}

void SetDown(const Fighter& fighter, bool down) {
    if (!fighter.health) return;

    if (g_set_is_down) g_set_is_down(fighter.health, down);
    if (g_set_down_state) {
        bool scratch = false;
        g_set_down_state(fighter.health, down ? kDownStateDown : kDownStateNone, true, &scratch);
    }
}
void SetDeathState(const Fighter& fighter) {
    if (!fighter.health) return;
    if (g_set_is_down) g_set_is_down(fighter.health, true);
    if (g_set_down_state) {
        bool scratch = false;
        g_set_down_state(fighter.health, kDownStateDeath, true, &scratch);
    }
}

bool SetCurrentPoseAsset(ue::UObject* actor, ue::UObject* pose_asset) {
    if (!actor || !pose_asset || !g_set_current_pose_asset) return false;
    ue::UObject* anim_instance = ue::GetAnimInstance(actor);
    if (!anim_instance) return false;
    g_set_current_pose_asset(anim_instance, pose_asset);
    return true;
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

    struct FStringParam {
        void* data;
        std::int32_t num;
        std::int32_t max;
    } reason = {};
    return ue::CallFunction(component.ReturnValue, L"StopLogic", &reason);
}

bool StartBrain(ue::UObject* actor) {
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

    struct Empty {
    } none = {};
    return ue::CallFunction(component.ReturnValue, L"RestartLogic", &none);
}

void SetActorCollisionEnabled(ue::UObject* actor, bool enabled) {
    if (!actor) return;
    struct CollisionParams {
        bool bNewActorEnableCollision;
    } collision = {};
    collision.bNewActorEnableCollision = enabled;
    ue::CallFunction(actor, L"SetActorEnableCollision", &collision);
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

constexpr std::uintptr_t kMovementVelocityOffset = 0xD4;
constexpr std::uintptr_t kSceneComponentVelocityOffset = 0x150;

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

bool SetMovementSpeedState(ue::UObject* movement_component, int state) {
    if (!movement_component || !g_set_movement_speed_state) return false;
    if (state < 0 || state > 3) return false;
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

    ue::UObject* component = g_get_movement_component(actor);
    if (!component) return false;
    g_request_direct_move(component, desired_velocity, force_max_speed);
    return true;
}

std::uint32_t g_teleport_fallbacks = 0;
std::uint32_t g_teleport_hard_failures = 0;

bool SetLocationNoSweep(ue::UObject* actor, const ue::FVector& location) {
    std::uint8_t params[512] = {};
    std::memcpy(params + 0x00, &location, sizeof(location));
    params[0x0C] = 0;
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

    struct Params {
        ue::FVector DestLocation;
        ue::FRotator DestRotation;
        bool ReturnValue;
    } params = {};
    params.DestLocation = location;
    params.DestRotation = rotation;

    if (!ue::CallFunction(actor, L"K2_TeleportTo", &params)) return false;
    if (params.ReturnValue) return true;

    ++g_teleport_fallbacks;
    SetLocationNoSweep(actor, location);
    SetRotationDirect(actor, rotation);

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
}

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

int ReadRelationshipViaComponent(ue::UObject* from_actor, ue::UObject* to_actor) {
    if (!from_actor || !to_actor) return relationship::kUnknown;
    ue::UObject* social = GetSocialComponent(from_actor);
    if (!social) return relationship::kUnknown;
    struct Params {
        ue::UObject* Actor;
        std::uint8_t ReturnValue;
    } params = {};
    params.Actor = to_actor;
    if (!ue::CallFunction(social, L"BPF_GetRelationship", &params)) {
        return relationship::kUnknown;
    }
    return params.ReturnValue;
}

bool WriteRelationship(ue::UObject* social, ue::UObject* toward, int value) {
    if (!social || !toward || value < 0) return false;

    if (!ue::IsValidObject(social) || !ue::IsValidObject(toward)) return false;

    static ue::UObject* seen_world = nullptr;
    static DWORD world_settled_at = 0;
    ue::UObject* world = ue::GetWorld();
    if (!world) return false;
    const DWORD now = GetTickCount();
    if (world != seen_world) {
        seen_world = world;
        world_settled_at = now;
        return false;
    }
    constexpr DWORD kWorldSettleMs = 5000;
    if (now - world_settled_at < kWorldSettleMs) return false;

    if (g_set_relationship) {
        g_set_relationship(social, toward, static_cast<std::uint8_t>(value));
        return true;
    }

    struct Params {
        ue::UObject* Actor;
        std::uint8_t eRelation;
    } params = {};
    params.Actor = toward;
    params.eRelation = static_cast<std::uint8_t>(value);
    return ue::CallFunction(social, L"BPF_ServerChangeRelationship", &params);
}

constexpr std::uintptr_t kSocialRelationshipsMap = 0x0318;

int g_map_probe_first = -1;
bool g_map_probe_moved = false;

bool RelationshipMapProbeTrusted() { return g_map_probe_moved; }

int RelationshipMapSize(ue::UObject* social) {
    if (!social) return -1;
    std::int32_t num = 0;
    std::memcpy(&num,
                reinterpret_cast<const std::uint8_t*>(social) + kSocialRelationshipsMap + 8,
                sizeof(num));
    if (num < 0 || num > 4096) return -1;
    if (g_map_probe_first < 0) {
        g_map_probe_first = num;
    } else if (!g_map_probe_moved && num != g_map_probe_first) {
        g_map_probe_moved = true;
        SC_LOG("relationship: map probe MOVED %d -> %d -- offset 0x%03X is real and its "
               "count may be quoted as evidence",
               g_map_probe_first, num, static_cast<unsigned>(kSocialRelationshipsMap));
    }
    return num;
}

float GetActorTimeDilation(ue::UObject* actor) {
    if (!actor || offsets::M_AActor_CustomTimeDilation == 0) return 1.f;
    float value = 1.f;
    std::memcpy(&value,
                reinterpret_cast<const std::uint8_t*>(actor) +
                    offsets::M_AActor_CustomTimeDilation,
                sizeof(value));

    if (!(value > 0.01f) || value > 4.f) return 1.f;
    return value;
}

bool SetActorTimeDilation(ue::UObject* actor, float dilation) {
    if (!actor || offsets::M_AActor_CustomTimeDilation == 0) return false;
    if (!(dilation > 0.01f) || dilation > 4.f) dilation = 1.f;
    std::memcpy(reinterpret_cast<std::uint8_t*>(actor) +
                    offsets::M_AActor_CustomTimeDilation,
                &dilation, sizeof(dilation));
    return true;
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

constexpr std::uint32_t kRuntimeNameFloor = 2000000000u;

bool SplitRuntimeSuffix(char* name, std::uint32_t* number_out) {
    if (!name || !number_out) return false;
    char* underscore = strrchr(name, '_');
    if (!underscore || !underscore[1]) return false;

    unsigned long long value = 0;
    for (const char* p = underscore + 1; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10 + static_cast<unsigned long long>(*p - '0');
        if (value > 0xFFFFFFFFull) return false;
    }
    if (value < kRuntimeNameFloor) return false;

    *number_out = static_cast<std::uint32_t>(value);
    *underscore = '\0';
    return true;
}

std::uint32_t HashNameWithOrdinal(const char* text, int ordinal) {

    std::uint32_t hash = HashName(text);
    hash ^= static_cast<std::uint32_t>(ordinal) & 0xFFu;
    hash *= 16777619u;
    return hash;
}

}
