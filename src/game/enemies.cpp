#include "enemies.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../net/protocol.h"
#include "../net/session.h"
#include "../ue/reflection.h"
#include "actors.h"
#include "coop.h"
#include "puppet.h"
#include "orders.h"

namespace sifucoop::game {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;
namespace net = sifucoop::net;
namespace coop = sifucoop::coop;

struct TArrayRaw {
    ue::UObject** data = nullptr;
    std::int32_t num = 0;
    std::int32_t max = 0;
};

using GetAllActorsFn = void(__fastcall*)(const ue::UObject* world_context, void* actor_class,
                                         TArrayRaw* out_actors);
using StaticClassFn = void*(__fastcall*)();

using SetAttackTargetFn = void(__fastcall*)(ue::UObject*, ue::UObject*);
using GetTargetableActorComponentFn = ue::UObject*(__fastcall*)(const ue::UObject*);
using RegisterTargetableActorFn = void(__fastcall*)(ue::UObject*);
GetAllActorsFn g_get_all_actors = nullptr;
StaticClassFn g_fighting_character_class = nullptr;

SetAttackTargetFn g_set_attack_target = nullptr;
GetTargetableActorComponentFn g_get_targetable_actor_component = nullptr;
RegisterTargetableActorFn g_register_targetable_actor = nullptr;
TArrayRaw g_actors;
TArrayRaw g_spawners;
void* g_ai_spawner_class = nullptr;
bool g_ai_spawner_class_attempted = false;

bool KeyPressed(int vkey, bool* was_down) {
    const bool down = (GetAsyncKeyState(vkey) & 0x8000) != 0;
    const bool pressed = down && !*was_down;
    *was_down = down;
    return pressed;
}

struct Tracked {
    std::uint32_t hash = 0;

    std::uint32_t wire_hash = 0;
    std::uint32_t source_hash = 0;
    ue::UObject* actor = nullptr;
    ue::UObject* attack_component = nullptr;

    ue::UObject* ai_fighting = nullptr;

    char name[96] = {};

    bool runtime_named = false;
    std::uint32_t runtime_number = 0;
    int ordinal = 0;

    bool ai_stopped = false;
    DWORD last_ai_stop_ms = 0;

    bool was_down = false;
    bool parked = false;

    bool present = true;
    bool seen_from_host = false;
    bool ever_seen_from_host = false;

    DWORD missing_since = 0;
    float last_local_health = -1.f;
    float last_local_guard = -1.f;
    float reported_total = 0.f;
    float reported_guard_total = 0.f;
    float host_applied = 0.f;
    float host_guard_applied = 0.f;

    bool revive_refused = false;

    bool died_locally = false;

    bool death_anim_presented = false;

    std::uint8_t death_repair_attempts = 0;
    DWORD next_death_repair = 0;

    DWORD next_presence_refresh = 0;

    DWORD death_announce_until = 0;

    float distance = 0.f;
    float health = 0.f;
    float max_health = 0.f;
    bool active = false;
    bool driven = false;

    ue::FVector last_host_location = {};
    DWORD last_host_motion_ms = 0;
    bool have_host_motion = false;

    std::uint8_t host_target_flags = 0;
    ue::UObject* mirrored_target = nullptr;
    DWORD last_target_sample_ms = 0;
    DWORD peer_aggro_until = 0;
    DWORD last_peer_target_ms = 0;

    DWORD next_hostility_ms = 0;
    bool hostile_confirmed = false;

    bool local_brain = false;

    DWORD not_ours_since = 0;

    DWORD ours_wanted_since = 0;
    DWORD owned_since = 0;

    bool down_owner_retargeted = false;

    DWORD last_esync_ms = 0;

    DWORD last_replica_reaction_ms = 0;

    ue::UObject* pending_death_anim = nullptr;
    DWORD pending_death_anim_ms = 0;

    DWORD local_reaction_motion_until = 0;
};

Tracked g_tracked[net::kMaxTrackedEnemies];
int g_tracked_count = 0;
bool g_announce_empty_ownership = false;

ue::UObject* g_tracked_world = nullptr;
bool g_had_host_sweep = false;
bool g_refresh_requested = false;

bool TrackingIsCurrent() {
    return g_tracked_count > 0 && g_tracked_world != nullptr &&
           g_tracked_world == ue::GetWorld();
}

constexpr std::uintptr_t kAttackComponentTarget = 0x06B4;

constexpr std::uintptr_t kInternalIndexOffset = 0x0C;

std::int32_t InternalIndexOf(const ue::UObject* object) {
    if (!object) return -1;
    std::int32_t index = -1;
    std::memcpy(&index, reinterpret_cast<const std::uint8_t*>(object) + kInternalIndexOffset,
                sizeof(index));
    return index;
}

std::int32_t TargetIndexOf(const ue::UObject* attack_component) {
    if (!attack_component) return -1;
    std::int32_t index = -1;
    std::int32_t serial = 0;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(attack_component);
    std::memcpy(&index, bytes + kAttackComponentTarget, sizeof(index));
    std::memcpy(&serial, bytes + kAttackComponentTarget + 4, sizeof(serial));
    if (index < 0 || serial == 0) return -1;
    return index;
}

namespace combat_role {
constexpr int kNone = 0;
constexpr int kDirectOpponent = 1;
constexpr int kIndirectOpponent = 2;
constexpr int kNonOpponent = 3;
constexpr int kCount = 4;
const char* Name(int value) {
    switch (value) {
        case kDirectOpponent: return "DirectOpponent";
        case kIndirectOpponent: return "IndirectOpponent";
        case kNonOpponent: return "NonOpponent";
        default: return "None";
    }
}
}

constexpr std::uint8_t kBehaviorAlerted = 3;

StaticClassFn g_ai_fighting_class = nullptr;

ue::UObject* GetAIFightingComponent(ue::UObject* actor) {
    if (!actor || !g_ai_fighting_class) return nullptr;
    struct Params {
        void* ComponentClass;
        ue::UObject* ReturnValue;
    } params = {};
    params.ComponentClass = g_ai_fighting_class();
    if (!ue::CallFunction(actor, L"GetComponentByClass", &params)) return nullptr;
    return params.ReturnValue;
}

int ReadCombatRole(ue::UObject* ai_fighting) {
    if (!ai_fighting) return -1;
    struct Params {
        std::uint8_t ReturnValue;
    } params = {};
    if (!ue::CallFunction(ai_fighting, L"BPF_GetCurrentCombatRole", &params)) return -1;
    return params.ReturnValue;
}

ue::UObject* ReadAIEnemy(ue::UObject* ai_fighting) {
    if (!ai_fighting) return nullptr;
    struct Params {
        ue::UObject* ReturnValue;
    } params = {};
    if (!ue::CallFunction(ai_fighting, L"BPF_GetEnemy", &params)) return nullptr;
    return params.ReturnValue;
}

void WakeEnemyPerception(ue::UObject* ai_fighting, bool force_behaviour = false) {
    if (!ai_fighting) return;
    struct Empty {
    } none = {};
    ue::CallFunction(ai_fighting, L"ActivateEnemyDetectionTimer", &none);
    if (!force_behaviour) return;
    struct Params {
        std::uint8_t eBehavior;
    } params = {};
    params.eBehavior = kBehaviorAlerted;
    ue::CallFunction(ai_fighting, L"BPF_ForceEnemyReactionBehavior", &params);
}

bool ForceEnemy(ue::UObject* ai_fighting, ue::UObject* target, std::uint8_t behavior) {
    if (!ai_fighting || !target) return false;
    struct Params {
        ue::UObject* Actor;
        std::uint8_t eBehavior;
    } params = {};
    params.Actor = target;
    params.eBehavior = behavior;
    return ue::CallFunction(ai_fighting, L"BPF_ForceEnemy", &params);
}

int g_hostile_value = relationship::kUnknown;
int g_hostility_attempts = 0;
bool g_hostility_hopeless = false;

bool AssertHostileToward(Tracked& entry, ue::UObject* peer) {
    ue::UObject* social = GetSocialComponent(entry.actor);
    if (!social) return false;

    if (g_hostile_value != relationship::kUnknown) {
        WriteRelationship(social, peer, g_hostile_value);
        return ReadRelationship(entry.actor, peer) == g_hostile_value;
    }

    const int candidates[] = {relationship::kFight, relationship::kEnemy};
    for (const int value : candidates) {

        const int was_actor = ReadRelationship(entry.actor, peer);
        const int was_comp = ReadRelationshipViaComponent(entry.actor, peer);
        WriteRelationship(social, peer, value);
        const int now_actor = ReadRelationship(entry.actor, peer);
        const int now_comp = ReadRelationshipViaComponent(entry.actor, peer);

        static int last_shape = -1;
        const int shape = (was_actor + 1) * 1000 + (was_comp + 1) * 100 +
                          (now_actor + 1) * 10 + (now_comp + 1);
        if (shape != last_shape) {
            last_shape = shape;
            SC_LOG("relationship: enemy->puppet asked %d %s | actor %d -> %d | "
                   "component %d -> %d | %s",
                   value, relationship::Name(value), was_actor, now_actor, was_comp, now_comp,
                   was_actor == value ? "ALREADY that value -- this proves nothing"
                                      : "the value changed, so the write landed");
        }

        if (now_actor != value && now_comp != value) continue;
        g_hostile_value = value;
        SC_LOG("targets: enemies will hold '%s' toward your partner", relationship::Name(value));
        return true;
    }
    return false;
}

void MaintainPeerHostility(ue::UObject* peer) {
    if (!peer || g_hostility_hopeless) return;
    if (coop::Get().mode != coop::Mode::Coop) return;

    if (!TrackingIsCurrent()) return;

    const DWORD now = GetTickCount();
    int budget = 4;
    for (int i = 0; i < g_tracked_count && budget > 0; ++i) {
        Tracked& entry = g_tracked[i];
        if (!entry.active || !entry.actor) continue;
        if (entry.next_hostility_ms != 0 && static_cast<LONG>(entry.next_hostility_ms - now) > 0) {
            continue;
        }
        --budget;
        const bool held = AssertHostileToward(entry, peer);

        if (held) WakeEnemyPerception(entry.ai_fighting);
        entry.next_hostility_ms = now + (held ? 5000 : 1000);
        if (held == entry.hostile_confirmed) continue;
        entry.hostile_confirmed = held;
    }

    if (g_hostile_value == relationship::kUnknown && ++g_hostility_attempts > 200) {
        g_hostility_hopeless = true;
        SC_LOG("targets: no hostile relationship value would stick on any enemy -- "
               "BPF_ServerChangeRelationship does not appear to work on this build, so "
               "enemies will keep ignoring your partner until they are hit");
        coop::ReportProblem("enemies cannot be told your partner is an enemy");
    }

    static DWORD last_summary = 0;
    if (g_hostile_value != relationship::kUnknown && now - last_summary >= 10000) {
        last_summary = now;
        int confirmed = 0;
        int active = 0;
        for (int i = 0; i < g_tracked_count; ++i) {
            if (!g_tracked[i].active) continue;
            ++active;
            if (g_tracked[i].hostile_confirmed) ++confirmed;
        }
        if (active > 0) {
            SC_LOG("targets: %d of %d active enemies hold '%s' toward your partner", confirmed,
                   active, relationship::Name(g_hostile_value));
        }
    }
}

constexpr std::uint8_t kEnemyTargetMask =
    net::kEnemyTargetsHost | net::kEnemyTargetsPeer;

std::uint8_t HostTargetFlags(ue::UObject* attack_component, ue::UObject* host_player,
                             ue::UObject* peer_player) {

    const std::int32_t target = TargetIndexOf(attack_component);
    if (target < 0) return 0;
    if (target == InternalIndexOf(host_player)) return net::kEnemyTargetsHost;
    if (peer_player && target == InternalIndexOf(peer_player)) return net::kEnemyTargetsPeer;
    return 0;
}

void KeepClientBrainStopped(Tracked& entry, DWORD now) {

    constexpr DWORD kRetryMs = 750;
    if (entry.last_ai_stop_ms != 0 && now - entry.last_ai_stop_ms < kRetryMs) return;
    entry.last_ai_stop_ms = now;
    if (StopBrain(entry.actor)) entry.ai_stopped = true;
}

void FreezeDeadEnemy(Tracked& entry, DWORD now) {
    entry.peer_aggro_until = 0;
    entry.last_peer_target_ms = 0;
    entry.host_target_flags = 0;
    entry.mirrored_target = nullptr;
    KeepClientBrainStopped(entry, now);
}

void RegisterEnemyTargetable(ue::UObject* actor) {
    if (!actor || !g_get_targetable_actor_component || !g_register_targetable_actor) return;
    ue::UObject* targetable = g_get_targetable_actor_component(actor);
    if (targetable) g_register_targetable_actor(targetable);
}

bool ApplyMirroredTarget(Tracked& entry, std::uint8_t flags, bool force = false) {
    flags &= kEnemyTargetMask;
    entry.host_target_flags = flags;
    if (flags == 0 || (flags == kEnemyTargetMask) || !entry.attack_component) return false;

    ue::UObject* world = ue::GetWorld();
    ue::UObject* local = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    ue::UObject* remote = GetPuppet();
    const bool on_host = net::GetRole() == net::Role::Host;
    ue::UObject* desired = nullptr;
    if (flags == net::kEnemyTargetsHost) {
        desired = on_host ? local : remote;
    } else {
        desired = on_host ? remote : local;
    }
    if (!desired) return false;

    if (!force && entry.mirrored_target == desired &&
        TargetIndexOf(entry.attack_component) == InternalIndexOf(desired))
        return true;

    bool applied = false;
    if (g_set_attack_target) {
        g_set_attack_target(entry.attack_component, desired);
        applied = true;
    }

    struct Params {
        ue::UObject* current_attacked;
    } params = {};
    params.current_attacked = desired;
    if (ue::CallFunction(entry.attack_component, L"BPF_UpdateLockMoveTarget", &params)) {
        applied = true;
    }
    if (!applied) {
        SC_LOG("targets: %s could not apply mirrored target", entry.name);
        return false;
    }

    entry.mirrored_target = desired;
    SC_LOG("targets: %s mirrored to %s", entry.name,
           desired == local ? "local player" : "remote puppet");
    return true;
}

bool ForceAttackTarget(Tracked& entry, ue::UObject* desired) {
    if (!entry.attack_component || !desired || !g_set_attack_target) return false;

    if (coop::Get().force_enemy_engage && entry.ai_fighting &&
        ReadAIEnemy(entry.ai_fighting) != desired) {
        ForceEnemy(entry.ai_fighting, desired, kBehaviorAlerted);
    }

    WakeEnemyPerception(entry.ai_fighting);

    g_set_attack_target(entry.attack_component, desired);
    struct Params {
        ue::UObject* current_attacked;
    } params = {};
    params.current_attacked = desired;
    ue::CallFunction(entry.attack_component, L"BPF_UpdateLockMoveTarget", &params);

    return TargetIndexOf(entry.attack_component) == InternalIndexOf(desired);
}

using DirectorRegisterFn = void(__fastcall*)(ue::UObject* director, ue::UObject* target,
                                             std::uint8_t behavior,
                                             const ue::UObject* instigator);
using DirectorRemoveForTargetFn = void(__fastcall*)(ue::UObject* director,
                                                    const ue::UObject* actor,
                                                    ue::UObject* target);
using DirectorRedistributeFn = void(__fastcall*)(const ue::UObject* target, bool immediate,
                                                 std::uint8_t reason);

DirectorRegisterFn g_director_register = nullptr;
DirectorRemoveForTargetFn g_director_remove_for_target = nullptr;
DirectorRedistributeFn g_director_redistribute = nullptr;
StaticClassFn g_director_class = nullptr;

constexpr std::uint8_t kGlobalBehaviorAlerted = 3;

constexpr std::uint8_t kRoleChangeScript = 2;

ue::UObject* g_registered_partner = nullptr;
ue::UObject* g_registered_with_director = nullptr;
ue::UObject* g_registered_world = nullptr;

void ClearDirectorRegistration() {
    g_registered_partner = nullptr;
    g_registered_with_director = nullptr;
    g_registered_world = nullptr;
}

ue::UObject* FindDirector() {
    if (!g_get_all_actors || !g_director_class) return nullptr;
    ue::UObject* world = ue::GetWorld();
    if (!world) return nullptr;
    g_get_all_actors(world, g_director_class(), &g_actors);
    if (!g_actors.data || g_actors.num <= 0) return nullptr;
    return g_actors.data[0];
}

void ReleasePartnerFromDirector() {
    if (!g_registered_partner || !g_registered_with_director) {
        ClearDirectorRegistration();
        return;
    }

    const bool current_registration =
        g_registered_world && g_registered_world == ue::GetWorld() && TrackingIsCurrent();
    if (current_registration && g_director_remove_for_target &&
        ue::IsValidObject(g_registered_partner) &&
        ue::IsValidObject(g_registered_with_director)) {
        int removed = 0;
        for (int i = 0; i < g_tracked_count; ++i) {
            ue::UObject* candidate = g_tracked[i].actor;
            if (!candidate || !ue::IsValidObject(candidate)) continue;

            g_director_remove_for_target(g_registered_with_director, g_registered_partner,
                                         candidate);
            ++removed;
        }
        SC_LOG("director: released your partner target and %d enemy candidates", removed);
    }
    ClearDirectorRegistration();
}

void MaintainPartnerAsDirectorTarget(ue::UObject* partner) {
    if (!coop::Get().director_targets_partner) return;
    if (coop::Get().mode != coop::Mode::Coop) return;
    if (!g_director_register) return;

    static ue::UObject* seen_world = nullptr;
    static DWORD world_settled_at = 0;
    static bool warned_no_director = false;
    ue::UObject* world = ue::GetWorld();
    if (!world) return;
    const DWORD now = GetTickCount();
    if (world != seen_world) {
        seen_world = world;
        world_settled_at = now;
        warned_no_director = false;
        ClearDirectorRegistration();
        return;
    }
    if (now - world_settled_at < 5000) return;

    if (!partner || !ue::IsValidObject(partner)) {
        ReleasePartnerFromDirector();
        return;
    }
    if (partner != g_registered_partner) ReleasePartnerFromDirector();

    static DWORD next_attempt = 0;
    if (next_attempt != 0 && static_cast<LONG>(next_attempt - now) > 0) return;
    next_attempt = now + 2000;

    ue::UObject* director = FindDirector();
    if (!director) {
        if (!warned_no_director) {
            warned_no_director = true;
            SC_LOG("director: no AAIDirectorActor in this level -- roles cannot be allocated "
                   "to your partner here");
        }
        return;
    }

    int candidates = 0;
    for (int i = 0; i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        if (!entry.active || !entry.actor || !ue::IsValidObject(entry.actor)) continue;

        g_director_register(director, partner, kGlobalBehaviorAlerted, entry.actor);
        ++candidates;
    }
    if (candidates == 0) return;
    if (g_director_redistribute) g_director_redistribute(partner, true, kRoleChangeScript);

    if (partner != g_registered_partner || director != g_registered_with_director) {
        g_registered_partner = partner;
        g_registered_with_director = director;
        g_registered_world = world;
        SC_LOG("director: registered your partner as a combat TARGET with %d enemy candidates "
               "-- the roles line should stop reading all zeros for 'fighting your partner'",
               candidates);
    }
}

int EnumerateFighters(ue::UObject** out, int max_out) {
    if (!g_get_all_actors || !g_fighting_character_class) return 0;
    ue::UObject* world = ue::GetWorld();
    if (!world) return 0;

    g_get_all_actors(world, g_fighting_character_class(), &g_actors);
    if (!g_actors.data || g_actors.num <= 0) return 0;

    const int count = g_actors.num < max_out ? g_actors.num : max_out;
    for (int i = 0; i < count; ++i) out[i] = g_actors.data[i];
    return count;
}

int FindTracked(std::uint32_t hash) {
    for (int i = 0; i < g_tracked_count; ++i) {
        if (g_tracked[i].hash == hash || g_tracked[i].wire_hash == hash) return i;
    }
    return -1;
}

int FindTrackedBySource(std::uint32_t source_hash) {
    if (source_hash == 0) return -1;
    for (int i = 0; i < g_tracked_count; ++i) {
        if (g_tracked[i].source_hash == source_hash) return i;
    }
    return -1;
}

struct SourceLink {
    ue::UObject* actor = nullptr;
    std::uint32_t source_hash = 0;
};

int EnumerateSpawnerSources(SourceLink* out, int max_out) {
    if (!out || max_out <= 0 || !g_get_all_actors) return 0;
    ue::UObject* world = ue::GetWorld();
    if (!world) return 0;
    if (!g_ai_spawner_class_attempted) {
        g_ai_spawner_class_attempted = true;

        g_ai_spawner_class = ue::FindObjectByPath(L"/Script/Sifu.AISpawner");
        SC_LOG("enemies: AISpawner class %s", g_ai_spawner_class ? "resolved" : "MISSING");
    }
    if (!g_ai_spawner_class) return 0;

    g_get_all_actors(world, g_ai_spawner_class, &g_spawners);
    if (!g_spawners.data || g_spawners.num <= 0) return 0;

    int count = 0;
    for (int i = 0; i < g_spawners.num && count < max_out; ++i) {
        ue::UObject* spawner = g_spawners.data[i];
        if (!spawner) continue;

        struct SpawnedAIParams {
            ue::UObject* ReturnValue;
        } spawned_params = {};
        if (!ue::CallFunction(spawner, L"BPF_GetSpawnedAI", &spawned_params)) continue;
        ue::UObject* spawned = spawned_params.ReturnValue;
        if (!spawned) continue;
        char path[256] = {};
        if (!ue::GetObjectPathName(spawner, path, sizeof(path))) continue;
        out[count++] = {spawned, HashName(path)};
    }
    return count;
}

std::uint32_t SourceHashForActor(ue::UObject* actor, const SourceLink* links, int count) {
    for (int i = 0; i < count; ++i) {
        if (links[i].actor == actor) return links[i].source_hash;
    }
    return 0;
}

struct AppliedTotals {
    std::uint32_t hash = 0;
    float health = 0.f;
    float guard = 0.f;
};

AppliedTotals g_applied[net::kMaxTrackedEnemies * 2];
int g_applied_count = 0;

void ResetAppliedTotals() {
    g_applied_count = 0;
}

AppliedTotals* AppliedTotalsFor(std::uint32_t hash) {
    for (int i = 0; i < g_applied_count; ++i) {
        if (g_applied[i].hash == hash) return &g_applied[i];
    }
    if (g_applied_count >= static_cast<int>(sizeof(g_applied) / sizeof(g_applied[0]))) {
        return nullptr;
    }
    AppliedTotals& slot = g_applied[g_applied_count++];
    slot = {};
    slot.hash = hash;
    return &slot;
}

AppliedTotals AppliedTotalsValue(std::uint32_t hash) {
    for (int i = 0; i < g_applied_count; ++i) {
        if (g_applied[i].hash == hash) return g_applied[i];
    }
    return {};
}

void RefreshTracked(ue::UObject* player, ue::UObject* puppet) {

    const bool same_world = ue::GetWorld() == g_tracked_world;
    if (!same_world) ResetAppliedTotals();

    ue::UObject* fighters[net::kMaxTrackedEnemies * 2] = {};
    const int count = EnumerateFighters(fighters, net::kMaxTrackedEnemies * 2);
    SourceLink source_links[net::kMaxTrackedEnemies * 2] = {};
    const int source_count =
        EnumerateSpawnerSources(source_links, net::kMaxTrackedEnemies * 2);

    Tracked previous[net::kMaxTrackedEnemies];
    const int previous_count = g_tracked_count;
    for (int i = 0; i < previous_count; ++i) previous[i] = g_tracked[i];

    g_tracked_count = 0;
    for (int i = 0; i < count && g_tracked_count < net::kMaxTrackedEnemies; ++i) {
        ue::UObject* actor = fighters[i];
        if (!actor || actor == player || actor == puppet) continue;

        char name[sizeof(Tracked::name)] = {};
        if (!LeafName(actor, name, sizeof(name))) continue;

        Tracked& entry = g_tracked[g_tracked_count];
        entry = Tracked();
        entry.actor = actor;
        entry.attack_component = ResolveFighter(actor).attack;
        entry.ai_fighting = GetAIFightingComponent(actor);
        entry.source_hash = SourceHashForActor(actor, source_links, source_count);

        entry.runtime_named = SplitRuntimeSuffix(name, &entry.runtime_number);
        lstrcpynA(entry.name, name, sizeof(entry.name));
        ++g_tracked_count;
    }

    for (int i = 0; i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        if (!entry.runtime_named) {
            entry.hash = HashName(entry.name);
            entry.wire_hash = entry.hash;
            continue;
        }
        int ordinal = 0;
        for (int k = 0; k < g_tracked_count; ++k) {
            if (k == i || !g_tracked[k].runtime_named) continue;
            if (lstrcmpA(g_tracked[k].name, entry.name) != 0) continue;
            if (g_tracked[k].runtime_number > entry.runtime_number) ++ordinal;
        }
        entry.ordinal = ordinal;
        entry.hash = HashNameWithOrdinal(entry.name, ordinal);
        entry.wire_hash = entry.hash;
    }

    for (int i = 0; same_world && i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        for (int k = 0; k < previous_count; ++k) {
            if (previous[k].hash != entry.hash) continue;
            entry.wire_hash = previous[k].wire_hash;
            entry.ai_stopped = previous[k].ai_stopped;
            entry.last_ai_stop_ms = previous[k].last_ai_stop_ms;
            entry.was_down = previous[k].was_down;

            entry.died_locally = previous[k].died_locally;
            entry.death_anim_presented = previous[k].death_anim_presented;
            entry.death_repair_attempts = previous[k].death_repair_attempts;
            entry.next_death_repair = previous[k].next_death_repair;
            entry.parked = previous[k].parked;
            entry.present = previous[k].present;
            entry.missing_since = previous[k].missing_since;
            entry.next_presence_refresh = previous[k].next_presence_refresh;
            entry.active = previous[k].active;
            entry.death_announce_until = previous[k].death_announce_until;
            entry.last_local_health = previous[k].last_local_health;
            entry.last_local_guard = previous[k].last_local_guard;
            entry.reported_total = previous[k].reported_total;
            entry.reported_guard_total = previous[k].reported_guard_total;
            entry.host_applied = previous[k].host_applied;
            entry.host_guard_applied = previous[k].host_guard_applied;
            entry.last_host_location = previous[k].last_host_location;
            entry.last_host_motion_ms = previous[k].last_host_motion_ms;
            entry.have_host_motion = previous[k].have_host_motion;
            entry.ever_seen_from_host = previous[k].ever_seen_from_host;

            entry.peer_aggro_until = previous[k].peer_aggro_until;
            entry.last_peer_target_ms = previous[k].last_peer_target_ms;
            entry.next_hostility_ms = previous[k].next_hostility_ms;
            entry.hostile_confirmed = previous[k].hostile_confirmed;

            entry.local_brain = previous[k].local_brain;
            entry.not_ours_since = previous[k].not_ours_since;
            entry.ours_wanted_since = previous[k].ours_wanted_since;
            entry.owned_since = previous[k].owned_since;
            entry.down_owner_retargeted = previous[k].down_owner_retargeted;
            entry.last_esync_ms = previous[k].last_esync_ms;
            entry.last_replica_reaction_ms = previous[k].last_replica_reaction_ms;
            entry.pending_death_anim = previous[k].pending_death_anim;
            entry.pending_death_anim_ms = previous[k].pending_death_anim_ms;
            entry.local_reaction_motion_until =
                previous[k].local_reaction_motion_until;
            entry.revive_refused = previous[k].revive_refused;
            entry.host_target_flags = previous[k].host_target_flags;
            entry.mirrored_target = previous[k].mirrored_target;
            break;
        }
    }

    g_tracked_world = ue::GetWorld();
    coop::GetStats().enemies_known = g_tracked_count;
    static int last_source_count = -1;
    if (source_count != last_source_count) {
        last_source_count = source_count;
        SC_LOG("enemies: %d spawner identities bound to pool bodies", source_count);
    }

    int collisions = 0;
    for (int i = 0; i < g_tracked_count; ++i) {
        for (int k = i + 1; k < g_tracked_count; ++k) {
            if (g_tracked[i].hash != g_tracked[k].hash) continue;
            ++collisions;
            if (collisions <= 3) {
                SC_LOG("enemies: ID COLLISION %08X -- '%s' and '%s' cannot be told apart",
                       g_tracked[i].hash, g_tracked[i].name, g_tracked[k].name);
            }
        }
    }
    if (collisions > 0) {
        coop::ReportProblem("%d enemy id collisions -- co-op will misbehave", collisions);
    }
}

bool PresentClientReplicaDeath(ue::UObject* actor) {
    if (!actor) return false;

    struct Params {
        bool ClientReplicaFastDeath;
    } params = {true};
    return ue::CallFunction(actor, L"CLast Hitted", &params);
}

bool LaunchReplicatedImpact(Tracked& entry, float replicated_delta) {
    if (!coop::Get().mirror_hit_reactions || !entry.actor || entry.local_brain) return false;

    const DWORD now = GetTickCount();
    if (entry.last_replica_reaction_ms != 0 && now - entry.last_replica_reaction_ms < 80) {
        return false;
    }
    struct Params { bool ClientReplicaFastDeath; } params = {false};
    const bool presented = ue::CallFunction(entry.actor, L"CLast Hitted", &params);
    if (presented) entry.last_replica_reaction_ms = now;
    if (presented && (coop::Get().verbose_enemies || replicated_delta > 0.f)) {
        SC_LOG("reaction: replica hurt %.1f on %08X '%s'", replicated_delta,
               entry.hash, entry.name);
    }
    return presented;
}

void ApplyPeerDamage() {
    net::DamageReport reports[net::kMaxDamagePerPacket];
    const int count = net::GetEnemyDamage(reports, net::kMaxDamagePerPacket);
    if (count == 0) return;

    coop::Stats& stats = coop::GetStats();

    for (int i = 0; i < count; ++i) {
        const int index = FindTracked(reports[i].name_hash);
        if (index < 0) continue;
        Tracked& entry = g_tracked[index];

        AppliedTotals* applied = AppliedTotalsFor(reports[i].name_hash);
        if (!applied) continue;

        if (reports[i].total + 0.01f < applied->health) applied->health = 0.f;
        if (reports[i].guard_total + 0.01f < applied->guard) applied->guard = 0.f;

        const float delta = reports[i].total - applied->health;
        const float guard_delta = reports[i].guard_total - applied->guard;
        if (delta <= 0.01f && guard_delta <= 0.01f) continue;

        Fighter fighter = ResolveFighter(entry.actor);
        if (!fighter.health) continue;

        const bool already_dead = GetHealth(fighter) <= 0.5f || IsDead(fighter);
        if (already_dead) {
            applied->health = reports[i].total;
            applied->guard = reports[i].guard_total;
            entry.peer_aggro_until = 0;
            entry.last_peer_target_ms = 0;
            entry.host_target_flags = 0;
            entry.mirrored_target = nullptr;
            continue;
        }

        if (guard_delta > 0.01f && fighter.defense) {
            const float current_guard = GetGuard(fighter);
            SetGuard(fighter, current_guard - guard_delta);
            applied->guard = reports[i].guard_total;
        }
        if (delta <= 0.01f) {

            ue::UObject* peer = GetPuppet();
            if (GetHealth(fighter) > 0.5f && !IsDead(fighter) && peer &&
            ForceAttackTarget(entry, peer)) {
                entry.peer_aggro_until = GetTickCount() + 8000;
                entry.last_peer_target_ms = GetTickCount();
            }
            if (GetHealth(fighter) > 0.5f) LaunchReplicatedImpact(entry, guard_delta);
            continue;
        }

        const bool lethal = !already_dead && GetHealth(fighter) - delta <= 0.5f;

        ue::UObject* peer = GetPuppet();
        if (lethal) {
            ArmReplicatedKillInstigator(fighter.health, peer);
            ApplyDamage(fighter, delta);
            ClearReplicatedKillInstigator();
        } else {
            SetHealth(fighter, GetHealth(fighter) - delta);
        }
        applied->health = reports[i].total;

        if (GetHealth(fighter) > 0.5f) LaunchReplicatedImpact(entry, delta);

        if (GetHealth(fighter) <= 0.5f) {

            entry.was_down = true;

            SetDeathState(fighter);
            const bool presented = PresentClientReplicaDeath(entry.actor);
            entry.death_repair_attempts = presented ? 1 : 0;

            NotifyDownStateChanged(fighter, true);
            if (!presented || coop::Get().verbose_enemies) {
                SC_LOG("death: %s client-replica presentation=%d", entry.name,
                       presented ? 1 : 0);
            }
            if (coop::Get().verbose_enemies) {
                SC_LOG("enemies: %s killed by your partner -- forced down and presented",
                       entry.name);
            }
            FreezeDeadEnemy(entry, GetTickCount());
        }

        if (GetHealth(fighter) > 0.5f && !IsDead(fighter) && peer &&
            ForceAttackTarget(entry, peer)) {
            entry.peer_aggro_until = GetTickCount() + 8000;
            entry.last_peer_target_ms = GetTickCount();
            static bool first_aggro = true;
            if (first_aggro) {

                SC_LOG("targets: FIRST peer hit handed %s aggro to puppet -- role is now %s",
                       entry.name, combat_role::Name(ReadCombatRole(entry.ai_fighting)));
            }
            first_aggro = false;
        }

        stats.damage_applied_total += delta;

        static bool first = true;
        if (first) {
            first = false;
            SC_LOG("coop: FIRST peer damage applied (%.1f to %s) -- the joining player can "
                   "hurt enemies", delta, entry.name);
        }
        if (coop::Get().verbose_enemies) {
            SC_LOG("enemies: peer dealt %.1f to %s (total %.1f)", delta, entry.name,
                   reports[i].total);
        }
    }
}

void PublishEnemies() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* host_player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    ue::UObject* peer_player = GetPuppet();
    const DWORD now = GetTickCount();

    bool host_player_is_out = false;
    bool peer_player_is_out = false;

    MaintainPeerHostility(peer_player);
    MaintainPartnerAsDirectorTarget(peer_player);

    if (peer_player && host_player) {
        Fighter host_fighter = ResolveFighter(host_player);
        host_player_is_out =
            host_fighter.health && (GetHealth(host_fighter) <= 0.5f || IsDown(host_fighter));
        Fighter peer_fighter = ResolveFighter(peer_player);
        peer_player_is_out =
            peer_fighter.health &&
            (GetHealth(peer_fighter) <= 0.5f || IsDown(peer_fighter));
        static bool announced_host_down = false;
        static bool announced_peer_down = false;
        if (host_player_is_out && !peer_player_is_out) {
            int handed = 0;
            for (int i = 0; i < g_tracked_count; ++i) {
                Tracked& entry = g_tracked[i];
                if (!entry.active || !entry.actor) continue;
                Fighter enemy = ResolveFighter(entry.actor);
                if (enemy.health &&
                    (GetHealth(enemy) <= 0.5f || IsDead(enemy))) continue;
                if (ForceAttackTarget(entry, peer_player)) {
                    entry.peer_aggro_until = now + 3000;
                    entry.last_peer_target_ms = now;
                    ++handed;
                }
            }
            if (!announced_host_down) {
                announced_host_down = true;
                SC_LOG("targets: you are down -- handed %d enemies to your partner", handed);
            }
            announced_peer_down = false;
        } else if (coop::Get().retarget_from_down_peer && peer_player_is_out &&
                   !host_player_is_out) {
            int handed = 0;
            for (int i = 0; i < g_tracked_count; ++i) {
                Tracked& entry = g_tracked[i];
                if (!entry.active || !entry.actor) continue;
                Fighter enemy = ResolveFighter(entry.actor);
                if (enemy.health &&
                    (GetHealth(enemy) <= 0.5f || IsDead(enemy))) continue;

                entry.peer_aggro_until = 0;
                entry.last_peer_target_ms = 0;
                if (ForceAttackTarget(entry, host_player)) {
                    entry.host_target_flags = net::kEnemyTargetsHost;
                    ++handed;
                }
            }
            if (!announced_peer_down) {
                announced_peer_down = true;
                SC_LOG("targets: your partner is down -- handed %d enemies back to you",
                       handed);
            }
            announced_host_down = false;
        } else {
            announced_host_down = false;
            announced_peer_down = false;
        }
    }

    net::EnemyStateOut out[net::kMaxTrackedEnemies];
    int count = 0;
    int active = 0;

    for (int i = 0; i < g_tracked_count && count < net::kMaxTrackedEnemies; ++i) {
        Tracked& entry = g_tracked[i];

        ue::FVector location = {};
        if (!ue::GetActorLocation(entry.actor, &location)) continue;

        const bool was_active = entry.active;
        entry.active = !IsPooled(location);
        if (was_active && !entry.active) {

            entry.death_announce_until = now + 2000;
        }
        const bool announcing_death = !entry.active && entry.death_announce_until != 0 &&
                                     static_cast<LONG>(now - entry.death_announce_until) < 0;
        if (!entry.active && !announcing_death) entry.death_announce_until = 0;
        if (announcing_death) {
            net::EnemyStateOut& dead = out[count++];
            dead.name_hash = entry.hash;
            dead.source_hash = entry.source_hash;
            dead.x = location.X;
            dead.y = location.Y;
            dead.z = location.Z;
            dead.yaw = 0.f;
            dead.health = 0.f;
            dead.max_health = entry.max_health > 0.f ? entry.max_health : 1.f;
            dead.guard = 0.f;
            const AppliedTotals applied = AppliedTotalsValue(entry.hash);
            dead.damage_applied = applied.health;
            dead.guard_damage_applied = applied.guard;
            dead.flags = net::kEnemyActive | net::kEnemyDown | net::kEnemyDead;
            continue;
        }
        if (!entry.active) {

            continue;
        }

        ue::FRotator rotation = {};
        ue::GetActorRotation(entry.actor, &rotation);

        Fighter fighter = ResolveFighter(entry.actor);
        entry.health = GetHealth(fighter);
        entry.max_health = GetMaxHealth(fighter);
        const bool alive =
            fighter.health && entry.health > 0.5f && !IsDead(fighter);
        if (alive) {

            entry.was_down = false;

            ++active;
        } else {
            entry.peer_aggro_until = 0;
            entry.last_peer_target_ms = 0;
            entry.host_target_flags = 0;
        }

        if (fighter.health && !IsDown(fighter) && GetHealth(fighter) > 0.5f) {
            const DWORD presence_now = GetTickCount();
            if (entry.next_presence_refresh == 0 ||
                static_cast<LONG>(presence_now - entry.next_presence_refresh) >= 0) {
                entry.next_presence_refresh = presence_now + 500;

                SetActorCollisionEnabled(entry.actor, true);
                RegisterEnemyTargetable(entry.actor);
            }
        }

        net::EnemyStateOut& state = out[count++];
        state.name_hash = entry.hash;
        state.source_hash = entry.source_hash;
        state.x = location.X;
        state.y = location.Y;
        state.z = location.Z;
        state.yaw = rotation.Yaw;

        ue::FVector measured = {};
        if (GetActorVelocity(entry.actor, &measured)) {
            const float speed = sqrtf(measured.X * measured.X + measured.Y * measured.Y +
                                      measured.Z * measured.Z);
            if (std::isfinite(speed) && speed <= 3000.f) {
                state.velocity_x = measured.X;
                state.velocity_y = measured.Y;
                state.velocity_z = measured.Z;
            }
        } else if (entry.have_host_motion) {
            const DWORD elapsed_ms = now - entry.last_host_motion_ms;
            if (elapsed_ms > 0 && elapsed_ms <= 250) {
                const float seconds = static_cast<float>(elapsed_ms) / 1000.f;
                state.velocity_x = (location.X - entry.last_host_location.X) / seconds;
                state.velocity_y = (location.Y - entry.last_host_location.Y) / seconds;
                state.velocity_z = (location.Z - entry.last_host_location.Z) / seconds;
                const float speed = sqrtf(state.velocity_x * state.velocity_x +
                                          state.velocity_y * state.velocity_y +
                                          state.velocity_z * state.velocity_z);

                if (speed > 3000.f) state.velocity_x = state.velocity_y = state.velocity_z = 0.f;
            }
        }

        net::OwnedEnemy owned = {};

        const bool reaction_motion_here =
            entry.local_reaction_motion_until != 0 &&
            static_cast<LONG>(entry.local_reaction_motion_until - now) > 0;
        const bool peer_owned =
            entry.health > 0.5f && !IsDead(fighter) &&
            net::GetOwnedEnemy(entry.wire_hash ? entry.wire_hash : entry.hash, &owned);
        if (peer_owned) {

            const bool was_stopped = entry.ai_stopped;
            KeepClientBrainStopped(entry, now);
            if (!was_stopped && entry.ai_stopped) {
                SC_LOG("authority: %s actions handed to peer; host brain stopped", entry.name);
            }
            if (!reaction_motion_here) {
                const ue::FVector target = {owned.x, owned.y, owned.z};
                const ue::FRotator facing = {0.f, owned.yaw, 0.f};
                const ue::FVector velocity = {owned.velocity_x, owned.velocity_y,
                                              owned.velocity_z};
                DriveActorTo(entry.actor, target, facing, velocity);
                location = target;
                state.x = target.X;
                state.y = target.Y;
                state.z = target.Z;
                state.yaw = owned.yaw;
            }
        } else if (entry.ai_stopped) {

            const bool may_resume = entry.active && entry.health > 0.5f && !IsDead(fighter);
            if (may_resume && StartBrain(entry.actor)) {
                entry.ai_stopped = false;
                SC_LOG("authority: %s actions returned to host; host brain restarted",
                       entry.name);
            }
        }

        entry.last_host_location = location;
        entry.last_host_motion_ms = now;
        entry.have_host_motion = true;
        state.health = entry.health;
        state.max_health = entry.max_health;
        state.guard = GetGuard(fighter);
        const AppliedTotals applied = AppliedTotalsValue(entry.hash);
        state.damage_applied = applied.health;
        state.guard_damage_applied = applied.guard;

        state.time_dilation = GetActorTimeDilation(entry.actor);
        state.flags = net::kEnemyActive;
        if (reaction_motion_here) state.flags |= net::kEnemyReactionMotion;

        const bool peer_aggro_active =
            alive && peer_player && !peer_player_is_out && entry.peer_aggro_until &&
            static_cast<LONG>(entry.peer_aggro_until - now) > 0;
        if (peer_aggro_active) {
            if (now - entry.last_peer_target_ms >= 250) {
                ForceAttackTarget(entry, peer_player);
                entry.last_peer_target_ms = now;
            }
            entry.host_target_flags = net::kEnemyTargetsPeer;
        } else if (now - entry.last_target_sample_ms >= 50) {
            entry.last_target_sample_ms = now;
            entry.host_target_flags = HostTargetFlags(entry.attack_component, host_player,
                                                      peer_player);
        }
        state.flags |= entry.host_target_flags;

        if (IsDown(fighter)) state.flags |= net::kEnemyDown;
        if (entry.max_health > 0.f && entry.health <= 0.5f) state.flags |= net::kEnemyDead;
    }

    coop::GetStats().enemies_active = active;

    net::SendEnemyStates(out, count);
}

void AccumulateLocalDamage(Tracked& entry, const Fighter& fighter) {
    if (!fighter.health) return;
    const float health = GetHealth(fighter);
    const float guard = GetGuard(fighter);

    if (entry.last_local_health < 0.f || entry.last_local_guard < 0.f) {
        entry.last_local_health = health;
        entry.last_local_guard = guard;
        return;
    }

    const float health_drop = entry.last_local_health - health;
    const float guard_drop = entry.last_local_guard - guard;
    entry.last_local_health = health;
    entry.last_local_guard = guard;
    const bool useful = health_drop > 0.05f || guard_drop > 0.05f;
    if (health_drop > 0.05f) {
        entry.reported_total += health_drop;
        coop::GetStats().damage_reported_total += health_drop;
    }
    if (guard_drop > 0.05f) {
        entry.reported_guard_total += guard_drop;
    }
    if (!useful) return;

    static bool first = true;
    if (first) {
        first = false;
        SC_LOG("coop: FIRST local hit (health %.1f, guard %.1f to %s) -- reporting",
               health_drop > 0.f ? health_drop : 0.f,
               guard_drop > 0.f ? guard_drop : 0.f, entry.name);
    }
    if (coop::Get().verbose_enemies) {
        SC_LOG("enemies: local hit %s health=%.1f guard=%.1f totals=%.1f/%.1f", entry.name,
               health_drop, guard_drop, entry.reported_total, entry.reported_guard_total);
    }

}

void SendOwnedEnemies() {
    net::OwnedEnemy owned[net::kMaxOwnedEnemiesTotal];
    int count = 0;
    bool dropped_dead_owner = false;
    for (int i = 0; i < g_tracked_count && count < net::kMaxOwnedEnemiesTotal; ++i) {
        const Tracked& entry = g_tracked[i];
        if (!entry.local_brain || !entry.active || !entry.actor)
            continue;
        Fighter fighter = ResolveFighter(entry.actor);
        if (fighter.health && (GetHealth(fighter) <= 0.5f || IsDead(fighter))) {
            dropped_dead_owner = true;
            continue;
        }

        ue::FVector where = {};
        ue::FRotator facing = {};
        if (!ue::GetActorLocation(entry.actor, &where)) continue;
        ue::GetActorRotation(entry.actor, &facing);
        ue::FVector velocity = {};
        GetActorVelocity(entry.actor, &velocity);

        net::OwnedEnemy& out = owned[count++];
        out.name_hash = entry.wire_hash ? entry.wire_hash : entry.hash;
        out.x = where.X;
        out.y = where.Y;
        out.z = where.Z;
        out.yaw = facing.Yaw;
        out.velocity_x = velocity.X;
        out.velocity_y = velocity.Y;
        out.velocity_z = velocity.Z;
    }

    if (count > 0 || g_announce_empty_ownership || dropped_dead_owner) {
        net::SendOwnedEnemies(count > 0 ? owned : nullptr, count);
    }
}

void SendDamageReports() {
    net::DamageReport reports[net::kMaxDamagePerPacket];
    int count = 0;
    for (int i = 0; i < g_tracked_count && count < net::kMaxDamagePerPacket; ++i) {
        if (g_tracked[i].reported_total <= 0.f && g_tracked[i].reported_guard_total <= 0.f) continue;
        if (!g_tracked[i].seen_from_host) continue;

        reports[count].name_hash =
            g_tracked[i].wire_hash ? g_tracked[i].wire_hash : g_tracked[i].hash;
        reports[count].total = g_tracked[i].reported_total;
        reports[count].guard_total = g_tracked[i].reported_guard_total;
        ++count;
    }
    if (count == 0) return;
    net::SendEnemyDamage(reports, count);
    ++coop::GetStats().damage_reports;
}

void ApplyRemoteEnemies() {
    if (!net::HasEnemySweep()) return;

    if (!TrackingIsCurrent()) return;

    char local_level[192] = {};
    const char* peer_level = net::GetPeerLevel();
    if (!ue::GetCurrentLevelPath(local_level, sizeof(local_level)) ||
        !peer_level || !peer_level[0] || _stricmp(local_level, peer_level) != 0) {
        return;
    }
    g_had_host_sweep = true;

    net::EnemyStateOut states[net::kMaxTrackedEnemies];
    const int count = net::GetEnemyStates(states, net::kMaxTrackedEnemies);

    const coop::Config& config = coop::Get();
    coop::Stats& stats = coop::GetStats();
    ue::UObject* local_world = ue::GetWorld();
    ue::UObject* local_player =
        local_world ? ue::GetPlayerCharacter(local_world, 0) : nullptr;
    Fighter local_fighter = ResolveFighter(local_player);
    const bool local_player_is_out =
        local_fighter.health &&
        (GetHealth(local_fighter) <= 0.5f || IsDown(local_fighter));
    g_announce_empty_ownership = false;

    for (int i = 0; i < g_tracked_count; ++i) {
        g_tracked[i].seen_from_host = false;
        g_tracked[i].driven = false;
    }

    int driven = 0;
    int unmatched = 0;
    int alive_count = 0;

    for (int i = 0; i < count; ++i) {
        const net::EnemyStateOut& state = states[i];
        int index = FindTracked(state.name_hash);
        if (index < 0) index = FindTrackedBySource(state.source_hash);
        if (index < 0) {

            ++unmatched;
            static DWORD last_unmatched_log = 0;
            const DWORD now = GetTickCount();
            if (unmatched <= 3 && now - last_unmatched_log >= 2000) {
                last_unmatched_log = now;
                SC_LOG("enemies: host %08X source %08X has no local pool body", state.name_hash,
                       state.source_hash);
            }
            continue;
        }

        Tracked& entry = g_tracked[index];
        if (entry.wire_hash != state.name_hash) {
            SC_LOG("enemies: source %08X binds host %08X to local %08X (%s)",
                   state.source_hash, state.name_hash, entry.hash, entry.name);
            entry.wire_hash = state.name_hash;
        }
        const bool first_host_state = !entry.ever_seen_from_host;
        entry.seen_from_host = true;
        entry.ever_seen_from_host = true;
        entry.active = (state.flags & net::kEnemyActive) != 0;

        bool near_us = false;
        if (config.peer_fights_locally && entry.active) {
            ue::UObject* world = ue::GetWorld();
            ue::UObject* mine = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
            ue::UObject* theirs = GetPuppet();
            ue::FVector enemy_at = {};
            ue::FVector my_at = {};
            if (mine && ue::GetActorLocation(entry.actor, &enemy_at) &&
                ue::GetActorLocation(mine, &my_at)) {
                const float dxm = enemy_at.X - my_at.X;
                const float dym = enemy_at.Y - my_at.Y;
                const float to_me = sqrtf(dxm * dxm + dym * dym);

                constexpr float kEngagedWithin = 1200.f;
                if (to_me <= kEngagedWithin) {
                    ue::FVector their_at = {};
                    if (theirs && ue::GetActorLocation(theirs, &their_at)) {
                        const float dxt = enemy_at.X - their_at.X;
                        const float dyt = enemy_at.Y - their_at.Y;
                        const float to_them = sqrtf(dxt * dxt + dyt * dyt);
                        constexpr float kMargin = 300.f;
                        near_us = to_me + kMargin < to_them;
                    } else {
                        near_us = true;
                    }
                }
            }
        }

        const bool host_says_ours =
            config.peer_fights_locally &&
            (((state.flags & net::kEnemyTargetsPeer) != 0) || near_us);

        const bool host_says_dead = (state.flags & net::kEnemyDead) != 0 ||
                                    (state.max_health > 0.f && state.health <= 0.5f);
        const bool must_release = host_says_dead || !entry.active;

        const DWORD own_now = GetTickCount();
        if (host_says_ours && !must_release) {
            entry.not_ours_since = 0;
            if (entry.ours_wanted_since == 0) entry.ours_wanted_since = own_now;
        } else {
            entry.ours_wanted_since = 0;
            if (entry.local_brain && entry.not_ours_since == 0) {
                entry.not_ours_since = own_now;
            }
        }

        constexpr DWORD kClaimDebounceMs = 600;

        bool fights_us = entry.local_brain;
        if (!entry.local_brain && host_says_ours && !must_release) {
            fights_us = entry.ours_wanted_since != 0 &&
                        own_now - entry.ours_wanted_since >= kClaimDebounceMs;
        } else if (entry.local_brain && must_release) {
            fights_us = false;
        }

        if (fights_us && local_player_is_out && !entry.down_owner_retargeted) {
            ue::UObject* surviving_partner = GetPuppet();
            if (surviving_partner && ForceAttackTarget(entry, surviving_partner)) {
                entry.down_owner_retargeted = true;
            }
        } else if (!local_player_is_out) {
            entry.down_owner_retargeted = false;
        }

        if (fights_us != entry.local_brain) {
            entry.local_brain = fights_us;
            entry.owned_since = fights_us ? own_now : 0;
            if (fights_us) {
                if (StartBrain(entry.actor)) {
                    entry.ai_stopped = false;

                    WakeEnemyPerception(entry.ai_fighting, true);
                    SC_LOG("enemies: %s handed to local AI -- it is fighting you", entry.name);
                }
            } else {
                entry.ai_stopped = false;
                entry.not_ours_since = 0;
                SC_LOG("enemies: %s returned to the host's drive", entry.name);
            }
        }

        if (!fights_us && !host_says_dead && entry.active) {

            ApplyMirroredTarget(entry, state.flags);
        } else if (host_says_dead || !entry.active) {
            entry.host_target_flags = 0;
            entry.mirrored_target = nullptr;
        }

        const bool reaction_motion_here =
            entry.local_reaction_motion_until != 0 &&
            static_cast<LONG>(entry.local_reaction_motion_until - own_now) > 0;
        const bool local_ai = config.client_simulates_enemies || fights_us;

        if (host_says_dead) {
            FreezeDeadEnemy(entry, own_now);
        } else if (config.suppress_client_ai && !local_ai && !host_says_dead) {
            KeepClientBrainStopped(entry, GetTickCount());
        } else if (local_ai && entry.ai_stopped) {
            if (StartBrain(entry.actor)) {
                entry.ai_stopped = false;
                if (config.verbose_enemies) {
                    SC_LOG("enemies: %s thinking for itself here", entry.name);
                }
            }
        }

        const bool dead = (state.flags & net::kEnemyDead) != 0 ||
                          (state.max_health > 0.f && state.health <= 0.5f);
        const bool knocked_down = (state.flags & net::kEnemyDown) != 0;
        if (entry.active && !dead) ++alive_count;

        const bool should_be_present = entry.active && !dead;
        if (should_be_present && (!entry.present || entry.parked || first_host_state)) {
            SetActorPresent(entry.actor, true);
            RegisterEnemyTargetable(entry.actor);
            entry.present = true;
            entry.parked = false;
        }

        Fighter fighter = ResolveFighter(entry.actor);

        if (should_be_present && fighter.health && !IsDown(fighter)) {
            const DWORD presence_now = GetTickCount();
            if (entry.next_presence_refresh == 0 ||
                static_cast<LONG>(presence_now - entry.next_presence_refresh) >= 0) {
                entry.next_presence_refresh = presence_now + 500;

                SetActorCollisionEnabled(entry.actor, true);
                RegisterEnemyTargetable(entry.actor);
            }
        }
        const bool locally_dead_before_sync =
            fighter.health && (GetHealth(fighter) <= 0.5f || IsDead(fighter));

        if (locally_dead_before_sync && !entry.died_locally) {
            entry.died_locally = true;
            if (config.verbose_enemies) {
                SC_LOG("enemies: %s died HERE (%s) -- holding the corpse until the host agrees",
                       entry.name, entry.local_brain ? "our brain" : "host damage applied here");
            }
        }

        if (config.report_damage) AccumulateLocalDamage(entry, fighter);

        const bool host_caught_up = state.damage_applied + 0.05f >= entry.reported_total;
        if (config.sync_enemy_vitals && fighter.health) {

            if (host_caught_up) {
                const float local_health = GetHealth(fighter);

                if (state.health + 0.05f < local_health) {
                    const float replicated = local_health - state.health;
                    const bool replicated_lethal = state.health <= 0.5f;
                    if (replicated_lethal) {
                        ArmReplicatedKillInstigator(fighter.health, GetPuppet());
                        ApplyDamage(fighter, replicated);
                        ClearReplicatedKillInstigator();
                    } else {

                        SetHealth(fighter, state.health);
                    }

                    if (!replicated_lethal && config.mirror_hit_reactions) {
                        WakeEnemyPerception(entry.ai_fighting);
                        if (GetHealth(fighter) > 0.5f) {
                            LaunchReplicatedImpact(entry, replicated);
                        }
                    }
                    if (GetHealth(fighter) > state.health + 0.5f) {
                        SetHealth(fighter, state.health);
                    }
                } else if (state.health > local_health + 0.05f) {

                    const float max_health = GetMaxHealth(fighter);
                    const float reset_gap = max_health > 0.f ? max_health * 0.5f : 60.f;
                    if (state.health - local_health >= reset_gap || first_host_state) {
                        SetHealth(fighter, state.health);
                    }
                }

                if (state.guard + 0.05f < GetGuard(fighter)) SetGuard(fighter, state.guard);
                entry.last_local_guard = GetGuard(fighter);
                entry.host_guard_applied = state.guard_damage_applied;

                entry.last_local_health = GetHealth(fighter);
                entry.host_applied = state.damage_applied;
            }
        }

        entry.health = GetHealth(fighter);
        entry.max_health = GetMaxHealth(fighter);

        if (!entry.local_brain) {
            SetActorTimeDilation(entry.actor, state.time_dilation);
        }

        const bool stale_save_corpse = first_host_state && !dead;
        if (entry.died_locally && !dead &&
            (stale_save_corpse ||
             (state.health > 1.f && state.max_health > 0.f &&
              state.health >= state.max_health - 0.5f))) {
            entry.died_locally = false;
            entry.death_anim_presented = false;
            if (config.verbose_enemies) {
                SC_LOG("enemies: %s -- the host's 'alive' is real this time (%s)", entry.name,
                       stale_save_corpse ? "corpse from our own save, first host state"
                                         : "back from the pool at full health");
            }
        }

        if (!dead && locally_dead_before_sync && !entry.died_locally &&
            net::EnemySweepIsFresh()) {
            entry.was_down = false;
            if (GetHealth(fighter) <= 0.5f) {
                const float revive_health =
                    host_caught_up && state.health > 1.f ? state.health : 1.f;
                SetHealth(fighter, revive_health);
                entry.last_local_health = revive_health;
                entry.last_local_guard = GetGuard(fighter);
            }
            SetDown(fighter, false);

            NotifyDownStateChanged(fighter, false);
            SetActorPresent(entry.actor, true);
            RegisterEnemyTargetable(entry.actor);
            entry.present = true;
            entry.parked = false;
            entry.revive_refused = false;
            SC_LOG("enemies: revived and re-registered %s from client-only death", entry.name);
        } else if (!dead && locally_dead_before_sync) {

            if (!entry.revive_refused) {
                entry.revive_refused = true;
                SC_LOG("enemies: %s stays down -- %s", entry.name,
                       entry.died_locally
                           ? "we killed it, the host has not caught up yet"
                           : "the host's sweep is stale and its 'alive' cannot be trusted");
            }

            if (!entry.was_down) {
                entry.was_down = true;
                if (fighter.health && !entry.local_brain) {
                    SetDeathState(fighter);
                    NotifyDownStateChanged(fighter, true);
                    SC_LOG("death: %s presented locally (down=%d) ahead of the host's "
                           "confirmation",
                           entry.name, IsDown(fighter) ? 1 : 0);
                }
            }
        } else if (entry.was_down != dead) {
            entry.was_down = dead;

            const DWORD death_now = GetTickCount();
            const bool have_death_anim =
                entry.pending_death_anim && death_now - entry.pending_death_anim_ms <= 2000;

            if (dead && fighter.health) {
                ue::UObject* death_world = ue::GetWorld();
                ue::UObject* killer =
                    death_world ? ue::GetPlayerCharacter(death_world, 0) : nullptr;

                const float local_now = GetHealth(fighter);
                if (local_now > 0.5f) {
                    ArmReplicatedKillInstigator(fighter.health, killer);
                    ApplyDamage(fighter, local_now + 1.f);
                    ClearReplicatedKillInstigator();
                }

                bool killed = GetHealth(fighter) <= 0.5f;
                if (!killed) {
                    killed = KillWithAnimation(
                        fighter, killer, have_death_anim ? entry.pending_death_anim : nullptr);
                }

                SetDeathState(fighter);
                const bool replica_presented = PresentClientReplicaDeath(entry.actor);
                entry.death_repair_attempts = replica_presented ? 1 : 0;
                entry.next_death_repair = death_now + 750;

                NotifyDownStateChanged(fighter, true);

                if (have_death_anim) entry.death_anim_presented = true;
                SC_LOG("death: %s kill=%d anim=%d replica=%d health_comp=%d -> down=%d",
                       entry.name, killed ? 1 : 0, have_death_anim ? 1 : 0,
                       replica_presented ? 1 : 0, fighter.health ? 1 : 0,
                       IsDown(fighter) ? 1 : 0);
            }
            entry.pending_death_anim = nullptr;

            if (!dead) {
                SetDown(fighter, false);
                NotifyDownStateChanged(fighter, false);

                SetActorPresent(entry.actor, true);
                entry.present = true;
                entry.parked = false;
            }
            if (config.verbose_enemies) {
                SC_LOG("enemies: %s %s", entry.name, dead ? "DIED" : "recycled alive");
            }
        }

        if (dead && fighter.health && !IsDown(fighter) &&
            entry.death_repair_attempts < 3 &&
            (entry.next_death_repair == 0 ||
             static_cast<LONG>(own_now - entry.next_death_repair) >= 0)) {
            ++entry.death_repair_attempts;
            entry.next_death_repair = own_now + 750;
            const bool replica_presented = PresentClientReplicaDeath(entry.actor);
            SetDeathState(fighter);
            NotifyDownStateChanged(fighter, true);
            SC_LOG("death: %s repaired missed corpse edge attempt=%u replica=%d down=%d",
                   entry.name, static_cast<unsigned int>(entry.death_repair_attempts),
                   replica_presented ? 1 : 0, IsDown(fighter) ? 1 : 0);
        } else if (!dead) {
            entry.death_repair_attempts = 0;
            entry.next_death_repair = 0;
        }

        const bool host_reaction_motion =
            (state.flags & net::kEnemyReactionMotion) != 0;
        if (host_reaction_motion && !dead && config.sync_enemies &&
            !reaction_motion_here) {
            const ue::FVector target = {state.x, state.y, state.z};
            const ue::FRotator facing = {0.f, state.yaw, 0.f};
            const ue::FVector velocity = {state.velocity_x, state.velocity_y,
                                          state.velocity_z};
            DriveActorTo(entry.actor, target, facing, velocity);
            entry.driven = true;
            ++driven;
        } else if (local_ai && !dead && !knocked_down && config.sync_enemies) {
            ue::FVector here = {};
            if (ue::GetActorLocation(entry.actor, &here)) {
                const float dx = state.x - here.X;
                const float dy = state.y - here.Y;
                const float gap = sqrtf(dx * dx + dy * dy);
                constexpr float kTolerated = 400.f;
                if (gap > kTolerated) {
                    const ue::FVector target = {state.x, state.y, state.z};
                    const ue::FRotator facing = {0.f, state.yaw, 0.f};
                    DriveActorTo(entry.actor, target, facing, ue::FVector{});
                    if (config.verbose_enemies) {
                        SC_LOG("enemies: %s drifted %.0f units from the host -- pulled back",
                               entry.name, gap);
                    }
                }
            }
        }

        if (config.verbose_enemies && entry.active) {
            const DWORD esync_now = GetTickCount();
            if (esync_now - entry.last_esync_ms >= 1000) {
                entry.last_esync_ms = esync_now;

                const char* tgt = (state.flags & net::kEnemyTargetsPeer) ? "peer"
                                 : (state.flags & net::kEnemyTargetsHost) ? "host"
                                                                          : "none";

                SC_LOG("esync: %s fac=%d hp=%.0f/%.0f applied=%.0f/%.0f caught_up=%d down=%d "
                       "hostdown=%d dead=%d owned=%d tgt=%s",
                       entry.name, GetFaction(entry.actor), GetHealth(fighter), state.health, state.damage_applied,
                       entry.reported_total,
                       state.damage_applied + 0.05f >= entry.reported_total ? 1 : 0,
                       IsDown(fighter) ? 1 : 0, knocked_down ? 1 : 0, dead ? 1 : 0,
                       entry.local_brain ? 1 : 0, tgt);
            }
        }

        if (!dead && config.sync_enemies && !local_ai && !host_reaction_motion) {
            const ue::FVector target = {state.x, state.y, state.z};
            const ue::FRotator facing = {0.f, state.yaw, 0.f};
            const ue::FVector velocity = {state.velocity_x, state.velocity_y, state.velocity_z};
            DriveActorTo(entry.actor, target, facing, velocity);
            entry.driven = true;
            ++driven;
        }
    }

    constexpr DWORD kMissingGraceMs = 1500;
    const DWORD retire_now = GetTickCount();
    for (int i = 0; i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        if (!entry.ever_seen_from_host || entry.was_down) {
            entry.missing_since = 0;
            continue;
        }
        if (entry.seen_from_host) {
            entry.missing_since = 0;
            continue;
        }
        if (entry.missing_since == 0) {
            entry.missing_since = retire_now;
            continue;
        }
        if (retire_now - entry.missing_since < kMissingGraceMs) continue;
        entry.missing_since = 0;
        if (config.suppress_client_ai && !entry.ai_stopped) {
            if (StopBrain(entry.actor)) entry.ai_stopped = true;
        }
        entry.local_brain = false;
        entry.owned_since = 0;
        entry.ours_wanted_since = 0;
        entry.not_ours_since = 0;
        entry.active = false;
        SetActorPresent(entry.actor, false);
        entry.parked = true;
        entry.present = false;
        if (config.verbose_enemies) {
            SC_LOG("enemies: %s absent from host sweep -- parked without inventing death",
                   entry.name);
        }
    }

    if (config.park_extra_enemies && g_had_host_sweep) {
        for (int i = 0; i < g_tracked_count; ++i) {
            Tracked& entry = g_tracked[i];
            if (entry.seen_from_host || entry.parked) continue;
            if (entry.ever_seen_from_host) continue;

            ue::FVector location = {};
            if (!ue::GetActorLocation(entry.actor, &location)) continue;
            if (IsPooled(location)) continue;

            if (config.suppress_client_ai && !entry.ai_stopped) {
                if (StopBrain(entry.actor)) entry.ai_stopped = true;
            }
            SetActorPresent(entry.actor, false);
            entry.parked = true;
            entry.present = false;
            entry.last_local_health = -1.f;
            entry.last_local_guard = -1.f;
            entry.reported_total = 0.f;
            entry.reported_guard_total = 0.f;
            if (config.verbose_enemies) SC_LOG("enemies: parked %s", entry.name);
        }
    }
    if (unmatched > 0) g_refresh_requested = true;

    stats.enemies_driven = driven;
    stats.enemies_unmatched = unmatched;
    stats.enemies_active = alive_count;
}

}

void DumpEnemyTargets() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    ue::UObject* second = GetPuppet();

    int targeting_player = 0;
    int targeting_second = 0;
    int targeting_other = 0;
    int no_target = 0;

    const std::int32_t player_index = InternalIndexOf(player);
    const std::int32_t second_index = InternalIndexOf(second);

    for (int i = 0; i < g_tracked_count; ++i) {
        const Tracked& entry = g_tracked[i];
        if (!entry.active || !entry.attack_component) continue;
        Fighter fighter = ResolveFighter(entry.actor);
        if (fighter.health && (GetHealth(fighter) <= 0.5f || IsDead(fighter)))
            continue;

        const std::int32_t target = TargetIndexOf(entry.attack_component);
        if (target < 0) {
            ++no_target;
        } else if (target == player_index) {
            ++targeting_player;
        } else if (second && target == second_index) {
            ++targeting_second;
        } else {
            ++targeting_other;
        }
    }

    SC_LOG("targets: %d enemies on YOU, %d on the second player, %d elsewhere, %d idle%s",
           targeting_player, targeting_second, targeting_other, no_target,
           second ? "" : "  (no second player present)");

    int roles_on_player[combat_role::kCount] = {};
    int roles_on_second[combat_role::kCount] = {};
    const std::int32_t player_idx = InternalIndexOf(player);
    const std::int32_t second_idx = InternalIndexOf(second);
    for (int i = 0; i < g_tracked_count; ++i) {
        const Tracked& entry = g_tracked[i];
        if (!entry.active || !entry.ai_fighting) continue;
        Fighter fighter = ResolveFighter(entry.actor);
        if (fighter.health && (GetHealth(fighter) <= 0.5f || IsDead(fighter)))
            continue;
        const int role = ReadCombatRole(entry.ai_fighting);
        if (role < 0 || role >= combat_role::kCount) continue;
        const std::int32_t enemy_of = InternalIndexOf(ReadAIEnemy(entry.ai_fighting));
        if (enemy_of == player_idx) {
            ++roles_on_player[role];
        } else if (second && enemy_of == second_idx) {
            ++roles_on_second[role];
        }
    }
    SC_LOG("roles: fighting YOU direct=%d indirect=%d non=%d none=%d | "
           "fighting your partner direct=%d indirect=%d non=%d none=%d",
           roles_on_player[combat_role::kDirectOpponent],
           roles_on_player[combat_role::kIndirectOpponent],
           roles_on_player[combat_role::kNonOpponent], roles_on_player[combat_role::kNone],
           roles_on_second[combat_role::kDirectOpponent],
           roles_on_second[combat_role::kIndirectOpponent],
           roles_on_second[combat_role::kNonOpponent], roles_on_second[combat_role::kNone]);
}

void DumpRoster() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;

    SC_LOG("enemies: %d tracked (host sweep seen: %s)", g_tracked_count,
           g_had_host_sweep ? "yes" : "no");

    for (int i = 0; i < g_tracked_count; ++i) {
        const Tracked& entry = g_tracked[i];
        ue::FVector location = {};
        ue::GetActorLocation(entry.actor, &location);
        Fighter fighter = ResolveFighter(entry.actor);

        char origin[48] = {};
        if (entry.runtime_named) {
            wsprintfA(origin, " ord=%d rt=%u", entry.ordinal, entry.runtime_number);
        }
        SC_LOG("enemies:  [%02d] %08X %-46s hp=%.0f/%.0f %s%s%s(%.0f, %.0f, %.0f)%s", i,
               entry.hash, entry.name, GetHealth(fighter), GetMaxHealth(fighter),
               IsPooled(location) ? "POOLED " : "active ", entry.ai_stopped ? "ai-off " : "",
               entry.parked ? "parked " : "", location.X, location.Y, location.Z, origin);
    }
    if (player) {
        Fighter mine = ResolveFighter(player);
        SC_LOG("enemies:  YOU hp=%.0f/%.0f guard=%.0f faction=%d", GetHealth(mine),
               GetMaxHealth(mine), GetGuard(mine), GetFaction(player));
    }
}

void InitEnemies(std::uintptr_t base) {
    g_get_all_actors =
        reinterpret_cast<GetAllActorsFn>(base + offsets::UGameplayStatics_GetAllActorsOfClass);
    g_fighting_character_class =
        reinterpret_cast<StaticClassFn>(base + offsets::AFightingCharacter_StaticClass);
    SC_LOG("enemies: ready (INSERT lists the roster)");
    g_director_register =
        offsets::AAIDirectorActor_RegisterOrRemoveForTarget
            ? reinterpret_cast<DirectorRegisterFn>(
                  base + offsets::AAIDirectorActor_RegisterOrRemoveForTarget)
            : nullptr;
    g_director_remove_for_target =
        offsets::AAIDirectorActor_RemoveActorFromCombatRolesForTarget
            ? reinterpret_cast<DirectorRemoveForTargetFn>(
                  base + offsets::AAIDirectorActor_RemoveActorFromCombatRolesForTarget)
            : nullptr;
    g_director_redistribute =
        offsets::AAIDirectorActor_RequestCombatRoleRedistribution
            ? reinterpret_cast<DirectorRedistributeFn>(
                  base + offsets::AAIDirectorActor_RequestCombatRoleRedistribution)
            : nullptr;
    g_director_class =
        offsets::AAIDirectorActor_StaticClass
            ? reinterpret_cast<StaticClassFn>(base + offsets::AAIDirectorActor_StaticClass)
            : nullptr;
    SC_LOG("director: registration %s",
           g_director_register && g_director_class ? "available" : "UNAVAILABLE on this build");
    g_set_attack_target = offsets::UAttackComponent_SetTarget
        ? reinterpret_cast<SetAttackTargetFn>(base + offsets::UAttackComponent_SetTarget)
        : nullptr;
    g_get_targetable_actor_component = offsets::UTargetableActorHelper_GetTargetableActorComponent
        ? reinterpret_cast<GetTargetableActorComponentFn>(
              base + offsets::UTargetableActorHelper_GetTargetableActorComponent)
        : nullptr;
    g_register_targetable_actor = offsets::USCActorManager_RegisterTargetableActor
        ? reinterpret_cast<RegisterTargetableActorFn>(
              base + offsets::USCActorManager_RegisterTargetableActor)
        : nullptr;
    g_ai_fighting_class =
        offsets::UAIFightingComponent_StaticClass
            ? reinterpret_cast<StaticClassFn>(base + offsets::UAIFightingComponent_StaticClass)
            : nullptr;
}

void ForgetEnemyWorldObjects() {

    ClearDirectorRegistration();
    g_tracked_count = 0;
    g_tracked_world = nullptr;
    g_had_host_sweep = false;
    g_announce_empty_ownership = false;
    g_refresh_requested = true;
}

void NotifyPuppetWillBeDestroyed(ue::UObject* puppet) {

    if (puppet && puppet == g_registered_partner &&
        g_registered_world == ue::GetWorld() && TrackingIsCurrent()) {
        ReleasePartnerFromDirector();
    } else {
        ClearDirectorRegistration();
    }
}

ue::UObject* FindEnemyByHash(std::uint32_t hash) {

    if (!TrackingIsCurrent()) return nullptr;
    const int index = FindTracked(hash);
    return index < 0 ? nullptr : g_tracked[index].actor;
}

std::uint32_t EnemyHashForAttackComponent(const void* attack_component) {
    if (!attack_component || !TrackingIsCurrent()) return 0;
    for (int i = 0; i < g_tracked_count; ++i) {
        if (g_tracked[i].attack_component == attack_component) return g_tracked[i].hash;
    }
    return 0;
}

std::uint32_t EnemyHashForActor(const void* actor) {
    if (!actor || !TrackingIsCurrent()) return 0;
    for (int i = 0; i < g_tracked_count; ++i) {
        if (g_tracked[i].actor != actor) continue;
        return g_tracked[i].wire_hash ? g_tracked[i].wire_hash : g_tracked[i].hash;
    }
    return 0;
}

std::uint32_t EnemyHashForHealthComponent(const void* health_component) {
    if (!health_component || !TrackingIsCurrent()) return 0;
    for (int i = 0; i < g_tracked_count; ++i) {
        if (ResolveFighter(g_tracked[i].actor).health == health_component) {
            return g_tracked[i].wire_hash ? g_tracked[i].wire_hash : g_tracked[i].hash;
        }
    }
    return 0;
}

bool EnemyRunsLocalBrain(std::uint32_t hash) {
    const int index = FindTracked(hash);
    return index >= 0 && g_tracked[index].local_brain;
}

bool EnemyActionsAreLocallyAuthoritative(std::uint32_t hash, bool include_dead) {
    const int index = FindTracked(hash);
    if (index < 0) return false;

    const Tracked& entry = g_tracked[index];
    if (!entry.active) return false;
    Fighter fighter = ResolveFighter(entry.actor);
    if (!include_dead && fighter.health && (GetHealth(fighter) <= 0.5f || IsDead(fighter)))
        return false;
    if (net::GetRole() != net::Role::Host) return entry.local_brain;

    net::OwnedEnemy owned = {};
    const std::uint32_t wire_hash = entry.wire_hash ? entry.wire_hash : entry.hash;
    return !net::GetOwnedEnemy(wire_hash, &owned);
}

void NoteEnemyReactionOrder(const void* actor, unsigned int order_type) {
    if (!actor || !TrackingIsCurrent()) return;

    if (order_type != 12 && order_type != 27) return;
    for (int i = 0; i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        if (entry.actor != actor) continue;
        entry.local_reaction_motion_until = GetTickCount() + 1200;
        SC_LOG("motion: %s reaction owns transform for 1200ms (order=%u)", entry.name,
               order_type);
        return;
    }
}

bool EnemyMustRemainDead(const void* health_component) {
    if (!health_component || !TrackingIsCurrent()) return false;
    for (int i = 0; i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        if (!entry.was_down || !entry.actor) continue;
        Fighter fighter = ResolveFighter(entry.actor);
        if (fighter.health != health_component) continue;
        ue::FVector location = {};
        if (ue::GetActorLocation(entry.actor, &location) && IsPooled(location)) {
            return false;
        }
        return GetHealth(fighter) <= 0.5f || IsDead(fighter);
    }
    return false;
}

void NoteEnemyDeathAnimation(std::uint32_t hash, ue::UObject* animation) {
    if (!animation) return;
    const int index = FindTracked(hash);
    if (index < 0) return;
    Tracked& entry = g_tracked[index];
    entry.pending_death_anim = animation;
    entry.pending_death_anim_ms = GetTickCount();

}

bool ApplyMirroredEnemyTargetForAttack(std::uint32_t hash) {
    if (!TrackingIsCurrent()) return false;
    const int index = FindTracked(hash);
    if (index < 0) return false;
    Tracked& entry = g_tracked[index];
    Fighter fighter = ResolveFighter(entry.actor);
    if (!entry.active || entry.was_down ||
        (fighter.health && (GetHealth(fighter) <= 0.5f || IsDead(fighter)))) return false;
    return ApplyMirroredTarget(entry, entry.host_target_flags, true);
}

bool PrepareMirroredEnemyAttack(std::uint32_t hash, EnemyAttackContext* out) {
    if (!out) return false;
    *out = {};
    if (!TrackingIsCurrent()) return false;
    const int index = FindTracked(hash);
    if (index < 0) return false;

    Tracked& entry = g_tracked[index];
    Fighter fighter = ResolveFighter(entry.actor);
    if (!entry.active || entry.was_down ||
        (fighter.health && (GetHealth(fighter) <= 0.5f || IsDead(fighter)))) return false;

    if (net::GetRole() == net::Role::Host) {
        entry.host_target_flags = net::kEnemyTargetsPeer;
    }
    out->actor = entry.actor;
    out->attack_component = entry.attack_component;
    out->ai_fighting = entry.ai_fighting;
    if (ApplyMirroredTarget(entry, entry.host_target_flags, true)) {
        out->target = entry.mirrored_target;
    }
    return out->actor && out->attack_component && out->ai_fighting;
}

int GetEnemyRows(EnemyRow* out, int max_out) {
    if (!out || max_out <= 0) return 0;
    const int count = g_tracked_count < max_out ? g_tracked_count : max_out;
    for (int i = 0; i < count; ++i) {
        const Tracked& entry = g_tracked[i];
        out[i].hash = entry.hash;
        out[i].distance = entry.distance;
        out[i].health = entry.health;
        out[i].max_health = entry.max_health;
        out[i].active = entry.active;
        out[i].down = entry.was_down;
        out[i].driven = entry.driven;
        out[i].ai_stopped = entry.ai_stopped;
        lstrcpynA(out[i].name, entry.name, sizeof(out[i].name));
    }
    return count;
}

void TickEnemies() {
    static bool list_key = false;

    const bool wants_roster = KeyPressed(VK_INSERT, &list_key);

    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (!player) return;

    const DWORD now = GetTickCount();

    const bool world_changed = (world != g_tracked_world);
    if (world_changed) {
        g_tracked_count = 0;
        g_had_host_sweep = false;
        g_announce_empty_ownership = false;

        net::ResetEnemyReplication();
    }

    static ue::UObject* last_puppet = nullptr;
    ue::UObject* puppet = GetPuppet();

    static DWORD last_refresh = 0;
    if (world_changed || puppet != last_puppet || now - last_refresh > 2000 ||
        (g_refresh_requested && now - last_refresh > 250)) {
        last_refresh = now;
        last_puppet = puppet;
        RefreshTracked(player, puppet);
        g_refresh_requested = false;

        static ue::UObject* dumped_world = nullptr;
        if (coop::Get().verbose_enemies && g_tracked_count > 0 && dumped_world != world) {
            dumped_world = world;
            DumpRoster();
        }
    }

    if (wants_roster) {
        DumpRoster();
        DumpEnemyTargets();
    }

    if (GetPuppet()) {
        static DWORD last_targets = 0;
        if (now - last_targets > 5000) {
            last_targets = now;
            DumpEnemyTargets();
        }
    }

    static DWORD last_distance = 0;
    if (now - last_distance > 250) {
        last_distance = now;
        ue::FVector mine = {};
        if (ue::GetActorLocation(player, &mine)) {
            for (int i = 0; i < g_tracked_count; ++i) {
                ue::FVector location = {};
                if (!ue::GetActorLocation(g_tracked[i].actor, &location)) continue;
                const float dx = location.X - mine.X;
                const float dy = location.Y - mine.Y;
                g_tracked[i].distance = sqrtf(dx * dx + dy * dy);

                g_tracked[i].active = !IsPooled(location) && !g_tracked[i].parked;
            }
        }
    }

    if (!net::IsConnected() || !ActorsReady() || !CoopGameplayActive()) return;

    if (net::GetRole() == net::Role::Host) {
        ApplyPeerDamage();

        int hz = coop::Get().snapshot_hz;
        if (hz < 10) hz = 10;
        static DWORD last_publish = 0;
        if (now - last_publish >= static_cast<DWORD>(1000 / hz)) {
            last_publish = now;
            PublishEnemies();
        }
    } else {
        ApplyRemoteEnemies();

        static DWORD last_report = 0;
        static DWORD last_owned_publish = 0;
        if (coop::Get().report_damage && now - last_report >= 100) {
            last_report = now;
            SendDamageReports();
        }
        if (now - last_owned_publish >= 33) {
            last_owned_publish = now;
            SendOwnedEnemies();
        }
    }
}

}
