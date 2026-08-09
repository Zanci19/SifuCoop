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

GetAllActorsFn g_get_all_actors = nullptr;
StaticClassFn g_fighting_character_class = nullptr;

TArrayRaw g_actors;

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

    // UAttackComponent::BPF_UpdateLockMoveTarget(AActor* _currentAttacked).
    // This is the component's own target-update path, unlike writing a private
    // field or guessing an attack slot name.
    struct Params {
        ue::UObject* current_attacked;
    } params = {};
    params.current_attacked = desired;
    if (!ue::CallFunction(entry.attack_component, L"BPF_UpdateLockMoveTarget", &params)) {
        SC_LOG("targets: %s could not apply mirrored target", entry.name);
        return false;
    }

    entry.mirrored_target = desired;
    SC_LOG("targets: %s mirrored to %s", entry.name,
           flags == net::kEnemyTargetsHost ? "host puppet" : "local player");
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
        if (g_tracked[i].hash == hash) return i;
    }
    return -1;
}

struct AppliedTotal {
    std::uint32_t hash = 0;
    float total = 0.f;
};

AppliedTotal g_applied[net::kMaxTrackedEnemies * 2];
int g_applied_count = 0;

void ResetAppliedTotals() {
    g_applied_count = 0;
}

// returns the stored total for an id, creating a "zeroed" slot (null only if the ledger is full, which cannot happen for a real roster)
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

// report the figure back to peer
float AppliedTotalValue(std::uint32_t hash) {
    for (int i = 0; i < g_applied_count; ++i) {
        if (g_applied[i].hash == hash) return g_applied[i].total;
    }
    return 0.f;
}

void RefreshTracked(ue::UObject* player, ue::UObject* puppet) {
    if (ue::GetWorld() != g_tracked_world) ResetAppliedTotals();

    ue::UObject* fighters[net::kMaxTrackedEnemies * 2] = {};
    const int count = EnumerateFighters(fighters, net::kMaxTrackedEnemies * 2);

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
        entry.runtime_named = SplitRuntimeSuffix(name, &entry.runtime_number);
        lstrcpynA(entry.name, name, sizeof(entry.name));
        ++g_tracked_count;
    }
    for (int i = 0; i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        if (!entry.runtime_named) {
            entry.hash = HashName(entry.name);
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
    }

    for (int i = 0; i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        for (int k = 0; k < previous_count; ++k) {
            if (previous[k].hash != entry.hash) continue;
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
        }
    }

    g_tracked_world = ue::GetWorld();
    coop::GetStats().enemies_known = g_tracked_count;

    // check collisions so the figure doesnt freak out like me tryna get off the blanket at 2 am
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

// HOST
void ApplyPeerDamage() {
    net::DamageReport reports[net::kMaxDamagePerPacket];
    const int count = net::GetEnemyDamage(reports, net::kMaxDamagePerPacket);
    if (count == 0) return;

    coop::Stats& stats = coop::GetStats();

    for (int i = 0; i < count; ++i) {
        const int index = FindTracked(reports[i].name_hash);
        if (index < 0) continue;
        Tracked& entry = g_tracked[index];
        float* applied = AppliedTotalFor(reports[i].name_hash);
        if (!applied) continue;
        if (reports[i].total + 0.01f < *applied) *applied = 0.f;

        const float delta = reports[i].total - *applied;
        if (delta <= 0.01f) continue;
        *applied = reports[i].total;

        Fighter fighter = ResolveFighter(entry.actor);
        if (!fighter.health) continue;
        ApplyDamage(fighter, delta);

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
            // TODO: fix this fucking shit with the enemies dying and still standing
            entry.death_announce_until = now + 2000;
        }
        const bool announcing_death = !entry.active && entry.death_announce_until != 0 &&
                                     static_cast<LONG>(now - entry.death_announce_until) < 0;
        if (!entry.active && !announcing_death) entry.death_announce_until = 0;
        if (announcing_death) {
            net::EnemyStateOut& dead = out[count++];
            dead.name_hash = entry.hash;
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
        // TODO: refresh rate of puppet
        if (now - entry.last_target_sample_ms >= 50) {
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

// CLIENT

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
    // TODO: currently if the client fights an enemy, the enemy is assigned to them. what if they wombo-combo together or switch? fix that
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
        reports[count].name_hash = g_tracked[i].hash;
        reports[count].total = g_tracked[i].reported_total;
        ++count;
    }
    if (count == 0) return;
    net::SendEnemyDamage(reports, count);
    ++coop::GetStats().damage_reports;
}

void ApplyRemoteEnemies() {
    if (!net::HasEnemySweep()) return;
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
        const int index = FindTracked(state.name_hash);
        if (index < 0) {
            ++unmatched;
            continue;
        }

        Tracked& entry = g_tracked[index];
        const bool first_host_state = !entry.ever_seen_from_host;
        entry.seen_from_host = true;
        entry.ever_seen_from_host = true;
        entry.active = (state.flags & net::kEnemyActive) != 0;
        ApplyMirroredTarget(entry, state.flags);

        if (config.suppress_client_ai) KeepClientBrainStopped(entry, GetTickCount());

        const bool dead = (state.flags & net::kEnemyDead) != 0 ||
                          (state.max_health > 0.f && state.health <= 0.5f);
        const bool knocked_down = (state.flags & net::kEnemyDown) != 0;

        const bool should_be_present = entry.active && !dead;
        if (should_be_present && (!entry.present || entry.parked || first_host_state)) {
            SetActorPresent(entry.actor, true);
            entry.present = true;
            entry.parked = false;
        }

        Fighter fighter = ResolveFighter(entry.actor);

        if (config.report_damage) AccumulateLocalDamage(entry, fighter);

        if (config.sync_enemy_vitals && fighter.health) {
            const bool host_caught_up = state.damage_applied + 0.05f >= entry.reported_total;
            if (host_caught_up) {
                const float local_health = GetHealth(fighter);
                if (state.health + 0.05f < local_health) {
                    const bool lethal = dead || state.health <= 0.5f;
                    if (lethal || config.mirror_hit_reactions) {
                        ApplyDamage(fighter, local_health - state.health);
                        if (GetHealth(fighter) > state.health + 0.5f) {
                            SetHealth(fighter, state.health);
                        }
                    } else {
                        SetHealth(fighter, state.health);
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
        if (entry.was_down != dead) {
            entry.was_down = dead;
            SetDown(fighter, dead);
            if (config.verbose_enemies) {
                SC_LOG("enemies: %s %s", entry.name, dead ? "DIED" : "recycled alive");
            }
        }
        if (!dead && !knocked_down && !IsDown(fighter) && config.sync_enemies) {
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

    if (!net::IsConnected() || !ActorsReady()) return;

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
