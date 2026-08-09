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

namespace sifucoop::game {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;
namespace net = sifucoop::net;
namespace coop = sifucoop::coop;

// TArray<T>: {T* Data; int32 Num; int32 Max}. Reused across calls rather than
// declared fresh each time -- GetAllActorsOfClass resets it and reuses the
// allocation, so one array means one allocation for the whole session instead
// of leaking the game's memory on every poll.
struct TArrayRaw {
    ue::UObject** data = nullptr;
    std::int32_t num = 0;
    std::int32_t max = 0;
};

using GetAllActorsFn = void(__fastcall*)(const ue::UObject* world_context, void* actor_class,
                                         TArrayRaw* out_actors);
using StaticClassFn = void*(__fastcall*)();

using SetAttackTargetFn = void(__fastcall*)(ue::UObject*, ue::UObject*);
GetAllActorsFn g_get_all_actors = nullptr;
StaticClassFn g_fighting_character_class = nullptr;

SetAttackTargetFn g_set_attack_target = nullptr;
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

// Everything the co-op layer needs to remember about one enemy between frames.
// Deliberately a flat array indexed the same way on both sides, keyed by name
// hash -- the pool makes those identical across machines.
struct Tracked {
    std::uint32_t hash = 0;
    // Host wire id. Usually equals hash; after a saved room restored a local
    // pool body under another runtime name, source_hash binds it to this id.
    std::uint32_t wire_hash = 0;
    std::uint32_t source_hash = 0;
    ue::UObject* actor = nullptr;
    ue::UObject* attack_component = nullptr;

    // Wide enough for the longest name Sifu actually produces, with room to
    // spare. This was char[40], which is exactly the length of
    // "BP_AICharacter_Grunt_M_HD4_Banker_03_C_0" -- so the trailing instance
    // number was truncated away and _C_0 and _C_1 became the same string. The
    // id is a hash of this name, so two separate enemies shared one id, and
    // every packet about either of them addressed both.
    char name[96] = {};

    // Set when the leaf name carried a UE4 runtime instance number, which is
    // stripped off `name` before hashing because it differs per machine. The
    // number itself is kept only to order same-named actors by spawn time.
    bool runtime_named = false;
    std::uint32_t runtime_number = 0;
    int ordinal = 0;

    // --- Client-side ---
    bool ai_stopped = false;
    DWORD last_ai_stop_ms = 0;
    // "We have put this body down because the HOST said it died." Deliberately
    // not set for an ordinary knockdown -- see the kEnemyDown comment in
    // protocol.h.
    bool was_down = false;
    bool parked = false;          // we hid it because the host has not activated it
    // Whether this body currently has collision and is visible, as far as WE
    // last told it. Tracked rather than inferred: an enemy that reaches the
    // fight with collision still switched off is a live opponent you cannot
    // hit, and nothing else in the loop would ever notice.
    bool present = true;
    bool seen_from_host = false;  // present in the most recent host sweep
    bool ever_seen_from_host = false;
    // First tick at which the host stopped publishing this body. One dropped
    // sweep must not be read as "it died" -- see the retirement loop.
    DWORD missing_since = 0;
    float last_local_health = -1.f;
    float reported_total = 0.f;  // running damage we have told the host about
    float host_applied = 0.f;    // how much of that the host says it has applied

    // --- Host-side ---
    // (How much of the peer's total has been applied lives in the ledger above,
    //  not here: a field on this struct is lost every time the table rebuilds.)

    // Frames of "this one is dead" still owed to the peer. An enemy that dies is
    // recycled into the pool almost immediately, and a pooled enemy is not
    // published at all -- so the joining side was simply never told, and its own
    // copy stood there indefinitely, upright and untouched, long after the host
    // had killed it. The death has to outlive the body.
    DWORD death_announce_until = 0;

    // --- Display ---
    float distance = 0.f;
    float health = 0.f;
    float max_health = 0.f;
    bool active = false;
    bool driven = false;
    // Host-only motion sample. Enemy packets need a velocity as well as a
    // location or the joiner's movement component repeatedly falls to idle
    // between otherwise healthy 60 Hz snapshots.
    ue::FVector last_host_location = {};
    DWORD last_host_motion_ms = 0;
    bool have_host_motion = false;


    // The host's current target, represented with the wire flags already
    // reserved in protocol.h. On the client this is translated to that
    // machine's corresponding player before an echoed enemy attack is launched.
    std::uint8_t host_target_flags = 0;
    ue::UObject* mirrored_target = nullptr;
    DWORD last_target_sample_ms = 0;
    DWORD peer_aggro_until = 0;
    DWORD last_peer_target_ms = 0;
};

Tracked g_tracked[net::kMaxTrackedEnemies];
int g_tracked_count = 0;

// The world the tracked actor pointers belong to. A level change frees every
// one of them, and the hooks that look enemies up (the attack hook especially)
// fire on their own schedule rather than ours -- so the table has to be able to
// say "these pointers are from a level that no longer exists" rather than
// relying on being refreshed first.
ue::UObject* g_tracked_world = nullptr;
bool g_had_host_sweep = false;
bool g_refresh_requested = false;

bool TrackingIsCurrent() {
    return g_tracked_count > 0 && g_tracked_world != nullptr &&
           g_tracked_world == ue::GetWorld();
}

// Read the attack component's selected target through its reflected Blueprint
// accessor. The layout is from the shipped PDB and contains no game-object
// field offset guesses.
ue::UObject* ReadAttackTarget(ue::UObject* attack_component, std::uint8_t action_type) {
    if (!attack_component) return nullptr;
    struct Params {
        std::uint8_t action_type;
        std::uint8_t force_out_of_date;
        std::uint8_t pad[6];
        ue::UObject* ReturnValue;
    } params = {};
    params.action_type = action_type;
    if (!ue::CallFunction(attack_component, L"BPF_GetTargetForAction", &params)) return nullptr;
    return params.ReturnValue;
}

constexpr std::uint8_t kEnemyTargetMask =
    net::kEnemyTargetsHost | net::kEnemyTargetsPeer;

std::uint8_t HostTargetFlags(ue::UObject* attack_component, ue::UObject* host_player,
                             ue::UObject* peer_player) {
    // The target API is action-specific. Passing the zero-initialised enum only
    // asks for one action slot, which the log proved was idle even while the
    // enemy was attacking. Probe the compact action enum instead of inventing a
    // private offset or guessing a "current target" field. The getter is
    // read-only; invalid enum values simply have no target.
    for (std::uint8_t action_type = 0; action_type < 8; ++action_type) {
        const ue::UObject* target = ReadAttackTarget(attack_component, action_type);
        if (target == host_player) return net::kEnemyTargetsHost;
        if (target && peer_player && target == peer_player) return net::kEnemyTargetsPeer;
    }
    return 0;
}

void KeepClientBrainStopped(Tracked& entry, DWORD now) {
    // Level scripts can restart an AI brain after it was initially stopped.
    // A host-authoritative replica must never resume local decision making, or
    // its own AI will fight the network driver and create a private encounter.
    constexpr DWORD kRetryMs = 750;
    if (entry.ai_stopped && now - entry.last_ai_stop_ms < kRetryMs) return;
    entry.last_ai_stop_ms = now;
    if (StopBrain(entry.actor)) entry.ai_stopped = true;
}

// Map the host's target to the equivalent body on the joining machine. The host
// player is represented by this client's puppet, while the host's remote-player
// body is represented by this client's physical player.
bool ApplyMirroredTarget(Tracked& entry, std::uint8_t flags) {
    flags &= kEnemyTargetMask;
    entry.host_target_flags = flags;
    if (flags == 0 || (flags == kEnemyTargetMask) || !entry.attack_component) return false;

    ue::UObject* world = ue::GetWorld();
    ue::UObject* desired = nullptr;
    if (flags == net::kEnemyTargetsHost) {
        desired = GetPuppet();
    } else if (world) {
        desired = ue::GetPlayerCharacter(world, 0);
    }
    if (!desired) return false;

    if (entry.mirrored_target == desired) return true;

    // Lock-move target is only locomotion assistance; it does not change the
    // attack component's actual target. The old code therefore aimed real
    // replayed hitboxes at stale/null actors. Set both through Sifu's APIs.
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
           flags == net::kEnemyTargetsHost ? "host puppet" : "local player");
    return true;
}

bool ForceAttackTarget(Tracked& entry, ue::UObject* desired) {
    if (!entry.attack_component || !desired || !g_set_attack_target) return false;
    g_set_attack_target(entry.attack_component, desired);
    struct Params {
        ue::UObject* current_attacked;
    } params = {};
    params.current_attacked = desired;
    ue::CallFunction(entry.attack_component, L"BPF_UpdateLockMoveTarget", &params);
    return true;
}
// Every fighting character in the level, players included.
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

// Active runtime enemies are born from placed AISpawners. Their runtime actor
// names are per-process; their spawner paths are part of the map and stable.
// BPF_GetSpawnedAI also returns the pool body while it is parked, which is the
// critical case when the joiner cleared the room in a previous run.
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
        // Do not call this class through a hand-derived Steam RVA. The two
        // shipped executables are not address-compatible, and a bad native
        // call corrupts the traversal system before our first status log.
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
        // BPF_GetSpawnedAI has no inputs and one object return value. Calling
        // it through ProcessEvent is build-independent and correctly dispatches
        // a Blueprint override if Sifu supplies one.
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

// Rebuilt periodically rather than every frame: enumeration is not free, and
// the pool means the set of actors is fixed for the level anyway. Per-enemy
// bookkeeping is carried across a refresh by hash, so a rebuild does not
// forget how much damage has been reported.
// How much of the peer's cumulative damage total has already been applied, kept
// OUTSIDE the tracked table and keyed by enemy id.
//
// This used to be a field on the tracked entry, and that was wrong in a way that
// only a two-machine session could show. The table is rebuilt whenever the world
// changes, the puppet changes, or two seconds pass, and anything that loses the
// applied total does not cause a small error: the peer's next report states its
// ENTIRE running total, so a forgotten total re-applies all of it, and keeps
// re-applying it at frame rate. A peer that had dealt 95 damage drove the host's
// "damage applied" counter to 56,000 in under a minute -- 40 applications a
// second, exactly the host's frame rate -- which wiped out the host's enemies,
// left it reporting no active enemies at all, and so made the joining side park
// every enemy it had: frozen, unhittable, turning on the spot.
//
// Keyed by id and never rebuilt, so no amount of table churn can lose it. Only a
// level change clears it, which is correct: that is genuinely a new set of
// bodies.
struct AppliedTotal {
    std::uint32_t hash = 0;
    float total = 0.f;
};

AppliedTotal g_applied[net::kMaxTrackedEnemies * 2];
int g_applied_count = 0;

void ResetAppliedTotals() {
    g_applied_count = 0;
}

// Returns the stored total for an id, creating a zeroed slot on first sight.
// Null only if the ledger is full, which cannot happen for a real roster.
float* AppliedTotalFor(std::uint32_t hash) {
    for (int i = 0; i < g_applied_count; ++i) {
        if (g_applied[i].hash == hash) return &g_applied[i].total;
    }
    if (g_applied_count >= static_cast<int>(sizeof(g_applied) / sizeof(g_applied[0]))) {
        return nullptr;
    }
    AppliedTotal& slot = g_applied[g_applied_count++];
    slot.hash = hash;
    slot.total = 0.f;
    return &slot.total;
}

// Read-only lookup, for reporting the figure back to the peer without creating
// a ledger slot for an enemy nobody has damaged.
float AppliedTotalValue(std::uint32_t hash) {
    for (int i = 0; i < g_applied_count; ++i) {
        if (g_applied[i].hash == hash) return g_applied[i].total;
    }
    return 0.f;
}

void RefreshTracked(ue::UObject* player, ue::UObject* puppet) {
    // A new level is a new set of bodies, so what was applied to the old ones
    // means nothing. Any other rebuild must leave the ledger alone.
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

    // Identity is built in three passes rather than one, because an enemy's id
    // can depend on the other enemies present: a runtime-spawned actor is
    // identified by its name plus its ordinal within the group that shares that
    // name, and the ordinal is not knowable until the whole set is enumerated.
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
        entry.source_hash = SourceHashForActor(actor, source_links, source_count);
        // Strips UE4's per-process instance number, which is what stopped two
        // machines agreeing on any runtime-spawned enemy.
        entry.runtime_named = SplitRuntimeSuffix(name, &entry.runtime_number);
        lstrcpynA(entry.name, name, sizeof(entry.name));
        ++g_tracked_count;
    }

    // Ordinal within the set sharing a stripped name. Ordered by the runtime
    // number descending: UE4's counter walks downwards, so the largest number
    // was handed out first, and both machines spawn a group in the same order.
    // That makes the ordinal agree across machines where the raw number never
    // could. Actors without a runtime suffix keep ordinal 0 and are left alone.
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

    // Per-enemy bookkeeping survives a rebuild, so damage already reported is
    // not counted twice when the table is refreshed mid-fight.
    for (int i = 0; same_world && i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        for (int k = 0; k < previous_count; ++k) {
            if (previous[k].hash != entry.hash) continue;
            entry.wire_hash = previous[k].wire_hash;
            entry.ai_stopped = previous[k].ai_stopped;
            entry.last_ai_stop_ms = previous[k].last_ai_stop_ms;
            entry.was_down = previous[k].was_down;
            entry.parked = previous[k].parked;
            entry.present = previous[k].present;
            entry.missing_since = previous[k].missing_since;
            entry.active = previous[k].active;
            entry.death_announce_until = previous[k].death_announce_until;
            entry.last_local_health = previous[k].last_local_health;
            entry.reported_total = previous[k].reported_total;
            entry.host_applied = previous[k].host_applied;
            entry.last_host_location = previous[k].last_host_location;
            entry.last_host_motion_ms = previous[k].last_host_motion_ms;
            entry.have_host_motion = previous[k].have_host_motion;
            entry.ever_seen_from_host = previous[k].ever_seen_from_host;
            break;
            entry.peer_aggro_until = previous[k].peer_aggro_until;
            entry.last_peer_target_ms = previous[k].last_peer_target_ms;
        }
    }

    g_tracked_world = ue::GetWorld();
    coop::GetStats().enemies_known = g_tracked_count;
    static int last_source_count = -1;
    if (source_count != last_source_count) {
        last_source_count = source_count;
        SC_LOG("enemies: %d spawner identities bound to pool bodies", source_count);
    }

    // Two enemies sharing an id is fatal to everything downstream -- every
    // packet about one addresses both, so they are driven to the same place and
    // damaged together -- and it is completely silent. It happened, from a name
    // buffer one byte too short, and the symptom was a pair of enemies moving
    // as one. Cheap to check over a set this size; never worth not knowing.
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

// --- HOST -------------------------------------------------------------------

void ApplyPeerDamage() {
    net::DamageReport reports[net::kMaxDamagePerPacket];
    const int count = net::GetEnemyDamage(reports, net::kMaxDamagePerPacket);
    if (count == 0) return;

    coop::Stats& stats = coop::GetStats();

    for (int i = 0; i < count; ++i) {
        const int index = FindTracked(reports[i].name_hash);
        if (index < 0) continue;
        Tracked& entry = g_tracked[index];

        // Held in the ledger above rather than on the entry, so rebuilding the
        // tracked table cannot forget it and re-apply the peer's whole total.
        float* applied = AppliedTotalFor(reports[i].name_hash);
        if (!applied) continue;

        // A total that went backwards means the peer restarted its count for
        // this body -- the enemy was recycled through the pool. Restarting ours
        // too is what keeps a reused enemy damageable; without it the peer
        // could never again exceed the old high-water mark and every hit it
        // landed on that body would be silently discarded.
        if (reports[i].total + 0.01f < *applied) *applied = 0.f;

        const float delta = reports[i].total - *applied;
        if (delta <= 0.01f) continue;
        *applied = reports[i].total;

        Fighter fighter = ResolveFighter(entry.actor);
        if (!fighter.health) continue;
        ApplyDamage(fighter, delta);

        // Raw replicated health damage has no instigator, so Sifu perception
        // never learns who hit it. Hand aggro to the host's peer body and keep
        // it alive briefly; normal AI can then select/attack that body.
        ue::UObject* peer = GetPuppet();
        if (peer && ForceAttackTarget(entry, peer)) {
            entry.peer_aggro_until = GetTickCount() + 8000;
            entry.last_peer_target_ms = GetTickCount();
            static bool first_aggro = true;
            if (first_aggro) SC_LOG("targets: FIRST peer hit handed enemy aggro to puppet");
            first_aggro = false;
        }

        stats.damage_applied_total += delta;
        // The first time the peer's damage lands on an enemy is proof the
        // joining player can actually fight -- logged once, always, because it
        // is exactly what a two-machine test is trying to confirm.
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
            // Just left the fight -- almost always because it died. Announce it
            // for a while rather than letting it vanish from the stream.
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
            dead.damage_applied = AppliedTotalValue(entry.hash);
            dead.flags = net::kEnemyActive | net::kEnemyDown | net::kEnemyDead;
            continue;
        }
        if (!entry.active) {
            // Parked in the pool: not in the fight, so not replicated.
            //
            // Deliberately does NOT clear the applied total any more. Doing it
            // here meant a single frame in which the body read as pooled threw
            // the accounting away, and the peer's next report -- being a running
            // total -- then re-applied everything it had ever dealt. Recycling
            // is already handled where it belongs, by the rule that a total
            // moving backwards means the peer restarted its own count.
            continue;
        }
        ++active;

        ue::FRotator rotation = {};
        ue::GetActorRotation(entry.actor, &rotation);

        Fighter fighter = ResolveFighter(entry.actor);
        entry.health = GetHealth(fighter);
        entry.max_health = GetMaxHealth(fighter);

        net::EnemyStateOut& state = out[count++];
        state.name_hash = entry.hash;
        state.source_hash = entry.source_hash;
        state.x = location.X;
        state.y = location.Y;
        state.z = location.Z;
        state.yaw = rotation.Yaw;
        if (entry.have_host_motion) {
            const DWORD elapsed_ms = now - entry.last_host_motion_ms;
            if (elapsed_ms > 0 && elapsed_ms <= 250) {
                const float seconds = static_cast<float>(elapsed_ms) / 1000.f;
                state.velocity_x = (location.X - entry.last_host_location.X) / seconds;
                state.velocity_y = (location.Y - entry.last_host_location.Y) / seconds;
                state.velocity_z = (location.Z - entry.last_host_location.Z) / seconds;
                const float speed = sqrtf(state.velocity_x * state.velocity_x +
                                          state.velocity_y * state.velocity_y +
                                          state.velocity_z * state.velocity_z);
                // Travel/pool corrections are not locomotion; never turn one
                // into a multi-frame sprint on the joining machine.
                if (speed > 3000.f) state.velocity_x = state.velocity_y = state.velocity_z = 0.f;
            }
        }
        entry.last_host_location = location;
        entry.last_host_motion_ms = now;
        entry.have_host_motion = true;
        state.health = entry.health;
        state.max_health = entry.max_health;
        state.guard = GetGuard(fighter);
        state.damage_applied = AppliedTotalValue(entry.hash);
        state.flags = net::kEnemyActive;
        // Damage replication has no instigator stimulus. While a peer-hit aggro
        // lease is active, reassert the real target at 4 Hz and publish the
        // matching flag. Outside that lease, sample normal host AI state.
        const bool peer_aggro_active =
            peer_player && entry.peer_aggro_until &&
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
        // Two separate facts, and they were being sent as one. IsDown() is the
        // knockdown/stagger state an enemy enters and leaves repeatedly in a
        // normal fight; death is health reaching zero, which IsDown() never
        // reports. The joining side needs to tell them apart, because only one
        // of the two may drive its local death path.
        if (IsDown(fighter)) state.flags |= net::kEnemyDown;
        if (entry.max_health > 0.f && entry.health <= 0.5f) state.flags |= net::kEnemyDead;
    }

    coop::GetStats().enemies_active = active;
    // Always sent, including when empty: an empty sweep is how the client
    // learns a fight has ended and stops driving bodies back into the room.
    net::SendEnemyStates(out, count);
}

// --- CLIENT -----------------------------------------------------------------

// Watches for health that dropped without the host having said so: that is our
// own player's hit landing on a driven enemy, and it is the only evidence the
// host will ever get that the joining player is fighting at all.
void AccumulateLocalDamage(Tracked& entry, const Fighter& fighter) {
    if (!fighter.health) return;
    const float health = GetHealth(fighter);

    if (entry.last_local_health < 0.f) {
        entry.last_local_health = health;
        return;
    }

    const float drop = entry.last_local_health - health;
    entry.last_local_health = health;
    if (drop <= 0.05f) return;  // healed, reset, or noise

    entry.reported_total += drop;
    coop::Stats& stats = coop::GetStats();
    stats.damage_reported_total += drop;
    // First local hit on a driven enemy: proof this side is landing hits that
    // will be reported to the host. Logged once, always.
    static bool first = true;
    if (first) {
        first = false;
        SC_LOG("coop: FIRST local hit on a driven enemy (%.1f to %s) -- reporting to host",
               drop, entry.name);
    }
    if (coop::Get().verbose_enemies) {
        SC_LOG("enemies: dealt %.1f to %s locally (total %.1f)", drop, entry.name,
               entry.reported_total);
    }
}

void SendDamageReports() {
    net::DamageReport reports[net::kMaxDamagePerPacket];
    int count = 0;
    for (int i = 0; i < g_tracked_count && count < net::kMaxDamagePerPacket; ++i) {
        if (g_tracked[i].reported_total <= 0.f) continue;
        if (!g_tracked[i].seen_from_host) continue;  // not in the host's fight
        reports[count].name_hash = g_tracked[i].wire_hash;
        reports[count].total = g_tracked[i].reported_total;
        ++count;
    }
    if (count == 0) return;
    net::SendEnemyDamage(reports, count);
    ++coop::GetStats().damage_reports;
}

void ApplyRemoteEnemies() {
    if (!net::HasEnemySweep()) return;

    // Enemy ids are only meaningful inside one map. The live log proved the
    // old loop applied Hideout 4's sweep while the joiner was still in
    // Hideout 0, stopped every local brain, and parked the whole Wuguan roster.
    // Besides breaking that room immediately, those pool/death transitions can
    // be saved and explain why a later join starts with enemies already dead.
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

    for (int i = 0; i < g_tracked_count; ++i) {
        g_tracked[i].seen_from_host = false;
        g_tracked[i].driven = false;
    }

    int driven = 0;
    int unmatched = 0;

    for (int i = 0; i < count; ++i) {
        const net::EnemyStateOut& state = states[i];
        int index = FindTracked(state.name_hash);
        if (index < 0) index = FindTrackedBySource(state.source_hash);
        if (index < 0) {
            // Normal briefly after a level load. A persistent miss now records
            // both ids, so a missing pool body is diagnosable rather than silent.
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
        // Apply the target before this frame's queued attack events run. This
        // swaps host/peer perspective as documented in protocol.h, so a swing
        // meant for the joining player cannot land on their host puppet.
        ApplyMirroredTarget(entry, state.flags);

        // Stop its brain once. Repeating every frame would be wasted work and
        // would fight the engine if it ever legitimately restarts logic.
        if (config.suppress_client_ai) KeepClientBrainStopped(entry, GetTickCount());

        // Death and knockdown are now distinct on the wire. Everything below
        // needs both, so decide them once, up front.
        const bool dead = (state.flags & net::kEnemyDead) != 0 ||
                          (state.max_health > 0.f && state.health <= 0.5f);
        const bool knocked_down = (state.flags & net::kEnemyDown) != 0;

        // A live enemy must be visible and collidable, every frame, without
        // exception. The old condition only restored presence on the first host
        // state or straight out of the parked set, so a body that reached the
        // fight any other way -- or that was parked, unparked and re-parked
        // across a sweep gap -- kept the collision it was hidden with. That is
        // exactly the reported symptom: an enemy standing there, alive, that
        // punches pass straight through, and only ever the ones the other
        // player had already touched (they are the ones whose liveness changed
        // hands). Repairing it whenever it disagrees costs one reflected call
        // on the transition and nothing at all on the frames in between.
        const bool should_be_present = entry.active && !dead;
        if (should_be_present && (!entry.present || entry.parked || first_host_state)) {
            SetActorPresent(entry.actor, true);
            entry.present = true;
            entry.parked = false;
        }

        Fighter fighter = ResolveFighter(entry.actor);
        const bool locally_dead_before_sync =
            fighter.health && GetHealth(fighter) <= 0.5f;

        // Order matters here. Local damage has to be read *before* the host's
        // health is written on top, or the write itself would be mistaken for
        // a hit -- or worse, our own hit would be erased before it was ever
        // reported and the enemy would be unkillable from this side.
        if (config.report_damage) AccumulateLocalDamage(entry, fighter);

        if (config.sync_enemy_vitals && fighter.health) {
            // Only accept the host's number once it accounts for everything we
            // have told it about. Until then its value is stale-high and would
            // visibly heal an enemy we just hit.
            // Both counters run monotonically for as long as this body is in
            // the fight, and both restart together when it leaves. That
            // matters: an earlier version retired the client's total the
            // instant the host caught up, which made the two numbers
            // incomparable from then on -- the host's stale-but-larger total
            // satisfied this test immediately after every new hit, so the
            // enemy's health visibly sprang back up before settling. Never
            // reset one side of a comparison without the other.
            const bool host_caught_up = state.damage_applied + 0.05f >= entry.reported_total;
            if (host_caught_up) {
                const float local_health = GetHealth(fighter);
                // Directly writing a lower health value bypasses Sifu's hit and
                // death state machine.  The enemy then becomes untouchable at
                // zero health while still standing upright on the joiner.  Use
                // the same damage function as a normal hit for decreases so
                // the game plays its reaction/death and retires collision; a
                // direct write remains only for upward corrections (pool reset
                // or a stale client copy).
                if (state.health + 0.05f < local_health) {
                    ApplyDamage(fighter, local_health - state.health);
                    if (GetHealth(fighter) > state.health + 0.5f) {
                        SetHealth(fighter, state.health);  // guarded fallback
                    }
                } else if (state.health > local_health + 0.05f) {
                    SetHealth(fighter, state.health);
                }
                SetGuard(fighter, state.guard);
                entry.last_local_health = state.health;
                entry.host_applied = state.damage_applied;
            }
        }

        entry.health = GetHealth(fighter);
        entry.max_health = GetMaxHealth(fighter);

        // ONLY death drives the local down-state machine.
        //
        // This used to fire on knockdowns too, and that is the bug behind
        // "enemies the other player touched cannot be hit any more". Every time
        // the host staggered an enemy, this side forced its copy through
        // InternalSetDownState(Down, force), and every time it recovered,
        // through InternalSetDownState(None, force). A body walked in and out of
        // that state machine from outside comes back upright but no longer a
        // valid hit target -- alive, standing, and untouchable, which is exactly
        // what was reported. Knockdown now replicates as position and animation
        // only; the local body's own state machine is left alone.
        //
        // The reverse transition is still honoured, because it means something
        // different: a body that was DEAD and is alive again was recycled out of
        // the pool, and it has to be taken back out of the death state.
        // A host-live enemy can correspond to a corpse restored from the
        // joiner's previous save. A new Tracked entry starts with was_down=false,
        // so the old edge-only test never called SetDown(false): health rose,
        // but the actor stayed in Sifu's dead state. First authoritative live
        // state now explicitly revives such a body after restoring its health.
        if (!dead && first_host_state && locally_dead_before_sync) {
            entry.was_down = false;
            SetDown(fighter, false);
            SetActorPresent(entry.actor, true);
            entry.present = true;
            entry.parked = false;
            SC_LOG("enemies: revived %s from the joiner's stale save", entry.name);
        } else if (entry.was_down != dead) {
            entry.was_down = dead;
            SetDown(fighter, dead);
            if (config.verbose_enemies) {
                SC_LOG("enemies: %s %s", entry.name, dead ? "DIED" : "recycled alive");
            }
        }

        // Neither a corpse nor a floored body is walking anywhere; driving one
        // would drag it around underneath its own animation.
        //
        // Both sides of the question are asked, because the two can disagree by
        // a beat. The host's knockdown flag is the authority on what happened,
        // but the LOCAL body has its own knockdown too: sync_enemy_vitals feeds
        // the host's damage through Sifu's real ApplyDamage, so this copy stages
        // and falls under its own state machine. That is also why forcing the
        // down state from outside was never necessary -- the local game was
        // already going to play it.
        if (!dead && !knocked_down && !IsDown(fighter) && config.sync_enemies) {
            const ue::FVector target = {state.x, state.y, state.z};
            const ue::FRotator facing = {0.f, state.yaw, 0.f};
            const ue::FVector velocity = {state.velocity_x, state.velocity_y, state.velocity_z};
            DriveActorTo(entry.actor, target, facing, velocity);
            entry.driven = true;
            ++driven;
        }
    }

    // An enemy the host had and then dropped is finished: it died, or the
    // encounter ended and it went back to the pool. Either way it must stop
    // standing here as a live opponent our own player could still swing at --
    // that enemy no longer exists in the fight both players are sharing.
    //
    // Held for a moment first. A sweep is chunked, and a lost chunk means the
    // whole generation is discarded rather than promoted -- but the host's own
    // publish list is capped, so a crowded room can legitimately drop a body
    // from one generation and carry it in the next. Executing an enemy on the
    // strength of a single missing sweep turns a dropped packet into a
    // permanently dead-but-standing opponent.
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
        entry.was_down = true;
        entry.reported_total = 0.f;
        entry.last_local_health = -1.f;
        Fighter fighter = ResolveFighter(entry.actor);
        const float local_health = GetHealth(fighter);
        if (local_health > 0.5f) ApplyDamage(fighter, local_health);
        if (GetHealth(fighter) > 0.5f) SetHealth(fighter, 0.f);
        SetDown(fighter, true);
        if (config.verbose_enemies) SC_LOG("enemies: %s left the host's fight", entry.name);
    }

    // Anything the host has not activated must not be standing in our level:
    // its AI is stopped, so it would loiter as an inert obstacle, and if the AI
    // were left running it would be a second, private fight.
    //
    // Enemies the host *did* have and then dropped are left alone. They died,
    // or the host pooled them; either way we already put them down and the
    // local game will clean the body up the way it normally does. Hiding them
    // instead would make every kill end with the corpse blinking out.
    if (config.park_extra_enemies && g_had_host_sweep) {
        for (int i = 0; i < g_tracked_count; ++i) {
            Tracked& entry = g_tracked[i];
            if (entry.seen_from_host || entry.parked) continue;
            if (entry.ever_seen_from_host) continue;

            ue::FVector location = {};
            if (!ue::GetActorLocation(entry.actor, &location)) continue;
            if (IsPooled(location)) continue;  // already parked by the game itself

            if (config.suppress_client_ai && !entry.ai_stopped) {
                if (StopBrain(entry.actor)) entry.ai_stopped = true;
            }
            SetActorPresent(entry.actor, false);
            entry.parked = true;
            entry.present = false;
            entry.last_local_health = -1.f;
            entry.reported_total = 0.f;
            if (config.verbose_enemies) SC_LOG("enemies: parked %s", entry.name);
        }
    }
    if (unmatched > 0) g_refresh_requested = true;

    stats.enemies_driven = driven;
    stats.enemies_unmatched = unmatched;
    stats.enemies_active = count;
}

}  // namespace

// Who each enemy has actually decided to fight.
//
// This is the question the whole second-player experiment exists to answer: a
// puppet is not a player, so Sifu's AI never chose it, and every enemy fought
// the host no matter what the joining player did. A real PlayerController pawn
// should be electable like any other player -- and rather than squint at the
// screen, this reads it straight from each enemy's attack component.
//
//   UAttackComponent::BPF_GetTargetForAction(_eActionType @0x00,
//                                            _bForceOutOfDate @0x01) -> AActor* @0x08
void DumpEnemyTargets() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    ue::UObject* second = GetPuppet();

    int targeting_player = 0;
    int targeting_second = 0;
    int targeting_other = 0;
    int no_target = 0;

    for (int i = 0; i < g_tracked_count; ++i) {
        const Tracked& entry = g_tracked[i];
        if (!entry.active || !entry.attack_component) continue;

        struct Params {
            std::uint8_t action_type;
            std::uint8_t force_out_of_date;
            std::uint8_t pad[6];
            ue::UObject* ReturnValue;
        } params = {};
        if (!ue::CallFunction(entry.attack_component, L"BPF_GetTargetForAction", &params)) {
            continue;
        }

        if (!params.ReturnValue) {
            ++no_target;
        } else if (params.ReturnValue == player) {
            ++targeting_player;
        } else if (second && params.ReturnValue == second) {
            ++targeting_second;
        } else {
            ++targeting_other;
        }
    }

    SC_LOG("targets: %d enemies on YOU, %d on the second player, %d elsewhere, %d idle%s",
           targeting_player, targeting_second, targeting_other, no_target,
           second ? "" : "  (no second player present)");
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
        // The name printed is the *stripped* one -- the portable identity. For a
        // runtime-spawned enemy the discarded instance number is shown too, so
        // two machines' rosters can be compared: the hash and ordinal must
        // agree, the raw number will not.
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
    g_set_attack_target = offsets::UAttackComponent_SetTarget
        ? reinterpret_cast<SetAttackTargetFn>(base + offsets::UAttackComponent_SetTarget)
        : nullptr;
}

ue::UObject* FindEnemyByHash(std::uint32_t hash) {
    // Both lookups refuse to answer from a table built in a level we have since
    // left. The actors in it are freed, and handing one back would be a
    // use-after-free the moment a peer's packet named an enemy during a
    // transition -- which auto-follow makes a routine event, not a rare one.
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
bool ApplyMirroredEnemyTargetForAttack(std::uint32_t hash) {
    if (!TrackingIsCurrent()) return false;
    const int index = FindTracked(hash);
    if (index < 0) return false;
    return ApplyMirroredTarget(g_tracked[index], g_tracked[index].host_target_flags);
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
    // INSERT rather than a function key: the Epic overlay owns F3, and function
    // keys generally are contested by overlays and the game itself.
    const bool wants_roster = KeyPressed(VK_INSERT, &list_key);

    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (!player) return;

    const DWORD now = GetTickCount();

    // A new level invalidates every cached actor pointer and every flag derived
    // from one. Doing this unconditionally (not only while connected) means a
    // disconnect mid-level cannot leave stale pointers behind.
    const bool world_changed = (world != g_tracked_world);
    if (world_changed) {
        g_tracked_count = 0;
        g_had_host_sweep = false;
        // A restart may construct a new UWorld with the same package path.
        // Path equality cannot make an old completed sweep safe to reuse.
        net::ResetEnemyReplication();
    }

    // The puppet is a spawned clone of the player class, so it enumerates as a
    // fighting character. Refreshing the moment it appears keeps it from being
    // replicated as an enemy for the two seconds until the next poll -- which
    // would have the host publishing its peer's own character back at them.
    static ue::UObject* last_puppet = nullptr;
    ue::UObject* puppet = GetPuppet();

    // Rate-limited even when the table is empty. Keying the "refresh now" case
    // off an empty table meant a level with no fighting characters in it -- a
    // hub, a menu -- ran a full actor enumeration every single frame forever.
    static DWORD last_refresh = 0;
    if (world_changed || puppet != last_puppet || now - last_refresh > 2000 ||
        (g_refresh_requested && now - last_refresh > 250)) {
        last_refresh = now;
        last_puppet = puppet;
        RefreshTracked(player, puppet);
        g_refresh_requested = false;

        // The whole pairing scheme rests on both machines deriving the same id
        // for the same enemy, and the only way to check that is to compare the
        // two rosters. Dumped once per level so the comparison is possible
        // after the fact, from a log, without anyone having to reproduce a
        // desync live.
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

    // Also on a slow timer while a second player exists, so the answer shows up
    // in the log without anyone having to remember to press a key mid-fight.
    if (GetPuppet()) {
        static DWORD last_targets = 0;
        if (now - last_targets > 5000) {
            last_targets = now;
            DumpEnemyTargets();
        }
    }

    // Distances and liveness are display-only, so they refresh at the rate a
    // human can read rather than the rate the game runs at -- and they refresh
    // whether or not a peer is connected, so the overlay's table is useful for
    // working out what a level contains before anyone else joins.
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
                // A parked enemy is still standing at a real position, so the
                // pool test alone would report it as part of the fight.
                g_tracked[i].active = !IsPooled(location) && !g_tracked[i].parked;
            }
        }
    }

    // A socket connection is only a lobby connection. Publishing/applying here
    // before Start/Join used to synchronize Hideout 0 and stale save worlds,
    // killing or parking enemies before either player had begun co-op.
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

        // Damage is reported on a timer rather than per hit. Each report states
        // the full running total, so one every 100 ms both coalesces a flurry
        // of hits and repairs any that were lost on the way.
        static DWORD last_report = 0;
        if (coop::Get().report_damage && now - last_report >= 100) {
            last_report = now;
            SendDamageReports();
        }
    }
}

}  // namespace sifucoop::game
