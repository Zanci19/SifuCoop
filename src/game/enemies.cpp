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

    // --- Client-side ---
    bool ai_stopped = false;
    bool was_down = false;
    bool parked = false;          // we hid it because the host has not activated it
    bool seen_from_host = false;  // present in the most recent host sweep
    bool ever_seen_from_host = false;
    float last_local_health = -1.f;
    float reported_total = 0.f;  // running damage we have told the host about
    float host_applied = 0.f;    // how much of that the host says it has applied

    // --- Host-side ---
    float peer_applied = 0.f;    // how much of the peer's total we have applied

    // --- Display ---
    float distance = 0.f;
    float health = 0.f;
    float max_health = 0.f;
    bool active = false;
    bool driven = false;
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

bool TrackingIsCurrent() {
    return g_tracked_count > 0 && g_tracked_world != nullptr &&
           g_tracked_world == ue::GetWorld();
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

// Rebuilt periodically rather than every frame: enumeration is not free, and
// the pool means the set of actors is fixed for the level anyway. Per-enemy
// bookkeeping is carried across a refresh by hash, so a rebuild does not
// forget how much damage has been reported.
void RefreshTracked(ue::UObject* player, ue::UObject* puppet) {
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
        const std::uint32_t hash = HashName(name);

        Tracked& entry = g_tracked[g_tracked_count];
        entry = Tracked();
        entry.hash = hash;
        entry.actor = actor;
        entry.attack_component = ResolveFighter(actor).attack;
        lstrcpynA(entry.name, name, sizeof(entry.name));

        for (int k = 0; k < previous_count; ++k) {
            if (previous[k].hash != hash) continue;
            entry.ai_stopped = previous[k].ai_stopped;
            entry.was_down = previous[k].was_down;
            entry.parked = previous[k].parked;
            entry.last_local_health = previous[k].last_local_health;
            entry.reported_total = previous[k].reported_total;
            entry.host_applied = previous[k].host_applied;
            entry.peer_applied = previous[k].peer_applied;
            entry.ever_seen_from_host = previous[k].ever_seen_from_host;
            break;
        }
        ++g_tracked_count;
    }

    g_tracked_world = ue::GetWorld();
    coop::GetStats().enemies_known = g_tracked_count;

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

        // A total that went backwards means the peer restarted its count for
        // this body -- the enemy was recycled through the pool. Restarting ours
        // too is what keeps a reused enemy damageable; without it the peer
        // could never again exceed the old high-water mark and every hit it
        // landed on that body would be silently discarded.
        if (reports[i].total + 0.01f < entry.peer_applied) entry.peer_applied = 0.f;

        const float delta = reports[i].total - entry.peer_applied;
        if (delta <= 0.01f) continue;
        entry.peer_applied = reports[i].total;

        Fighter fighter = ResolveFighter(entry.actor);
        if (!fighter.health) continue;
        ApplyDamage(fighter, delta);

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
    net::EnemyStateOut out[net::kMaxTrackedEnemies];
    int count = 0;
    int active = 0;

    for (int i = 0; i < g_tracked_count && count < net::kMaxTrackedEnemies; ++i) {
        Tracked& entry = g_tracked[i];

        ue::FVector location = {};
        if (!ue::GetActorLocation(entry.actor, &location)) continue;

        entry.active = !IsPooled(location);
        if (!entry.active) {
            // Parked in the pool: not in the fight, so not replicated. Its
            // damage accounting restarts with the body, in step with the peer.
            entry.peer_applied = 0.f;
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
        state.health = entry.health;
        state.max_health = entry.max_health;
        state.guard = GetGuard(fighter);
        state.damage_applied = entry.peer_applied;
        state.flags = net::kEnemyActive;
        if (IsDown(fighter)) state.flags |= net::kEnemyDown;
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
            // Normal for a moment after a level load, and permanent if the two
            // machines somehow disagree about the level's contents.
            ++unmatched;
            continue;
        }

        Tracked& entry = g_tracked[index];
        entry.seen_from_host = true;
        entry.ever_seen_from_host = true;
        entry.active = (state.flags & net::kEnemyActive) != 0;

        // Stop its brain once. Repeating every frame would be wasted work and
        // would fight the engine if it ever legitimately restarts logic.
        if (config.suppress_client_ai && !entry.ai_stopped) {
            if (StopBrain(entry.actor)) {
                entry.ai_stopped = true;
            }
        }

        if (entry.parked) {
            SetActorPresent(entry.actor, true);
            entry.parked = false;
        }

        Fighter fighter = ResolveFighter(entry.actor);

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
                SetHealth(fighter, state.health);
                SetGuard(fighter, state.guard);
                entry.last_local_health = state.health;
                entry.host_applied = state.damage_applied;
            }
        }

        entry.health = GetHealth(fighter);
        entry.max_health = GetMaxHealth(fighter);

        // Zero health counts as down even when the flag is clear. Testing
        // against a live level showed enemies going to 0 and being recycled
        // into the pool without IsDown() ever becoming true -- that flag tracks
        // the knockdown state, and an enemy at zero health simply dies. Reading
        // only the flag left the joining side driving a corpse around.
        const bool down = (state.flags & net::kEnemyDown) != 0 ||
                          (state.max_health > 0.f && state.health <= 0.5f);
        if (entry.was_down != down) {
            entry.was_down = down;
            SetDown(fighter, down);
            if (config.verbose_enemies) {
                SC_LOG("enemies: %s %s", entry.name, down ? "DOWN" : "up");
            }
        }

        // A downed enemy is not walking anywhere; driving it would drag the
        // body around while its death animation plays.
        if (!down && config.sync_enemies) {
            const ue::FVector target = {state.x, state.y, state.z};
            const ue::FRotator facing = {0.f, state.yaw, 0.f};
            DriveActorTo(entry.actor, target, facing);
            entry.driven = true;
            ++driven;
        }
    }

    // An enemy the host had and then dropped is finished: it died, or the
    // encounter ended and it went back to the pool. Either way it must stop
    // standing here as a live opponent our own player could still swing at --
    // that enemy no longer exists in the fight both players are sharing.
    for (int i = 0; i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        if (!entry.ever_seen_from_host || entry.seen_from_host || entry.was_down) continue;
        entry.was_down = true;
        entry.reported_total = 0.f;
        entry.last_local_health = -1.f;
        SetDown(ResolveFighter(entry.actor), true);
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
            entry.last_local_health = -1.f;
            entry.reported_total = 0.f;
            if (config.verbose_enemies) SC_LOG("enemies: parked %s", entry.name);
        }
    }

    stats.enemies_driven = driven;
    stats.enemies_unmatched = unmatched;
    stats.enemies_active = count;
}

}  // namespace

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
        SC_LOG("enemies:  [%02d] %08X %-46s hp=%.0f/%.0f %s%s%s (%.0f, %.0f, %.0f)", i,
               entry.hash, entry.name, GetHealth(fighter), GetMaxHealth(fighter),
               IsPooled(location) ? "POOLED " : "active ", entry.ai_stopped ? "ai-off " : "",
               entry.parked ? "parked " : "", location.X, location.Y, location.Z);
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
    if (world_changed || puppet != last_puppet || now - last_refresh > 2000) {
        last_refresh = now;
        last_puppet = puppet;
        RefreshTracked(player, puppet);

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

    if (wants_roster) DumpRoster();

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
