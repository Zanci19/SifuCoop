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
    // The AI's own fighting component: BPF_ForceEnemy / BPF_GetCurrentCombatRole
    // live here. Resolved once per table rebuild because GetComponentByClass is
    // a full ProcessEvent.
    ue::UObject* ai_fighting = nullptr;

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
    // Log dedup only: a refused revive would otherwise print at the snapshot
    // rate for as long as a dropped link lasts.
    bool revive_refused = false;
    // This body died on THIS machine, under a brain this machine was running.
    // Held until the host says dead too, because until then its sweep still
    // says alive and acting on that resurrects a corpse mid-death-animation.
    bool died_locally = false;
    // Whether this body has already been shown its death sequence, so a late
    // arrival is played once and a repeat is ignored.
    bool death_anim_presented = false;
    // Next time this body's presence is re-asserted rather than merely checked
    // against our own bookkeeping. See the refresh in ApplyRemoteEnemies.
    DWORD next_presence_refresh = 0;

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
    // Whether this enemy has been told the remote player is hostile, and when
    // to tell it again. See MaintainPeerHostility.
    DWORD next_hostility_ms = 0;
    bool hostile_confirmed = false;
    // Running its own behaviour tree locally rather than following the host.
    bool local_brain = false;
    // When the host first stopped saying this enemy fights the peer. Ownership
    // is held for a few seconds past that, so a flickering flag cannot thrash it.
    DWORD not_ours_since = 0;
    // Ownership hysteresis. Claiming used to be instantaneous while releasing
    // took three seconds, so a body released on a distance comparison was
    // re-claimed on the very next sweep. See the decision in ApplyRemoteEnemies.
    DWORD ours_wanted_since = 0;
    DWORD owned_since = 0;
    // Per-enemy throttle for the esync diagnostic.
    DWORD last_esync_ms = 0;
    // The exact death sequence the host's lethal hit selected, handed over by
    // the Kill hook through the animation channel. Sifu picks this per
    // archetype, direction and killing move, so it is the difference between a
    // body that falls the way it was hit and one that simply stops.
    ue::UObject* pending_death_anim = nullptr;
    DWORD pending_death_anim_ms = 0;
};

Tracked g_tracked[net::kMaxTrackedEnemies];
int g_tracked_count = 0;
bool g_announce_empty_ownership = false;

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

// UAttackComponent::m_Target, an FWeakObjectPtr {int32 ObjectIndex; int32 Serial},
// offset straight out of Unreal's property table for UAttackComponent. This is
// who the enemy has actually decided to fight.
//
// It replaces BPF_GetTargetForAction as the source of truth for that question.
// That call answered "nobody" for every enemy in the room, all session, in the
// same seconds the log recorded those enemies attacking -- so the mod's only
// instrument for "are enemies fighting my partner" was answering no
// unconditionally, and every judgement made with it was worthless.
constexpr std::uintptr_t kAttackComponentTarget = 0x06B4;

// UObjectBase::InternalIndex. Same standing as kClassPrivateOffset in puppet.cpp:
// the UObjectBase prefix is fixed for every non-editor UE4 build.
constexpr std::uintptr_t kInternalIndexOffset = 0x0C;

std::int32_t InternalIndexOf(const ue::UObject* object) {
    if (!object) return -1;
    std::int32_t index = -1;
    std::memcpy(&index, reinterpret_cast<const std::uint8_t*>(object) + kInternalIndexOffset,
                sizeof(index));
    return index;
}

// -1 when the enemy has no target. Comparing object indices rather than
// resolving the weak pointer keeps this reflection-free and safe to run over
// every enemy every second.
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

// --- Sifu's combat-role ticket system ---------------------------------------
//
// The right to attack is allocated centrally, per target: AAIDirectorActor runs
// a FAICombatRoleTicketManager for each thing being fought, and hands out roles
// from ESCAICombatRoles -- 0 None, 1 DirectOpponent, 2 IndirectOpponent,
// 3 NonOpponent (recovered from the exe's enumerator strings at 0x4A679E0).
// This is the machinery behind "only one or two enemies swing at you at a time".
//
// Only a DirectOpponent attacks. An IndirectOpponent closes, circles, pressures
// and defends -- which is an exact description of the reported symptom: enemies
// that engage the remote player, deflect everything, and never once swing back.
// Writing the attack component's target field, which is all the mod ever did, is
// downstream of this and creates no ticket.
//
// Every call below is Blueprint-exposed, so none of it depends on guessing an
// ABI. EGlobalBehaviors (0x49BBC70): 0 Idle, 1 Suspicious, 2 Surprised,
// 3 Alerted, 4 Abandoning, 5 Friendly, 6 Count, 7 None.
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
}  // namespace combat_role

constexpr std::uint8_t kBehaviorAlerted = 3;

StaticClassFn g_ai_fighting_class = nullptr;

// GetComponentByClass is a full ProcessEvent, so this is called once per enemy
// per table rebuild and cached on Tracked, never per frame.
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

// UAIFightingComponent::BPF_GetCurrentCombatRole() -> ESCAICombatRoles.
int ReadCombatRole(ue::UObject* ai_fighting) {
    if (!ai_fighting) return -1;
    struct Params {
        std::uint8_t ReturnValue;
    } params = {};
    if (!ue::CallFunction(ai_fighting, L"BPF_GetCurrentCombatRole", &params)) return -1;
    return params.ReturnValue;
}

// UAIFightingComponent::BPF_GetEnemy() -> AActor*.
ue::UObject* ReadAIEnemy(ue::UObject* ai_fighting) {
    if (!ai_fighting) return nullptr;
    struct Params {
        ue::UObject* ReturnValue;
    } params = {};
    if (!ue::CallFunction(ai_fighting, L"BPF_GetEnemy", &params)) return nullptr;
    return params.ReturnValue;
}

// UAIFightingComponent::BPF_ForceEnemy(AActor*, EGlobalBehaviors).
//
// The game's own "engage this actor" entry point. This is what should put the
// remote player into the director's ticket manager and let somebody be promoted
// to DirectOpponent -- the thing a raw target write never did.
// UAIFightingComponent::ActivateEnemyDetectionTimer() and
// BPF_ForceEnemyReactionBehavior(EGlobalBehaviors).
//
// Being hostile to somebody is not the same as having noticed them. The
// relationship work made enemies willing to fight the remote player; nothing
// ever told their perception he was there, so the first blow he landed
// registered as an ambush and they spent the fight on the back foot -- guarding
// against an opponent they had never registered engaging.
//
// Neither of these registers an actor anywhere, which is the whole reason they
// are used instead of BPF_ForceEnemy: no ticket manager entry is created, so
// there is nothing for the director to trip over when the body is destroyed.
// One takes no arguments at all and the other a single enum.
// `force_behaviour` is deliberately not the default.
//
// Forcing a global behaviour on the HOST's enemies every few seconds is the
// most invasive thing this file does to a machine whose fight is otherwise
// working, and the host losing WASD appeared in the same session it started
// doing that. Detection alone is enough for the thing this is for -- an enemy
// that has noticed the other player -- and it takes no arguments, so it cannot
// put a character into a state. The behaviour push is kept for the one place
// that genuinely needs it: the moment the joining machine restarts a brain and
// that brain must not open on a stranger.
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

// --- Making the remote player somebody worth attacking ----------------------
//
// The reported symptom is precise: enemies do not fight the remote player, and
// if the remote player fights them they only block and parry, never swing back.
// That is not a targeting failure -- they can see the puppet, it is registered
// with the targetable actor manager -- it is a disposition failure. Nothing in
// this mod has ever told an enemy that the puppet is hostile. Every relationship
// call it makes sets the two PLAYERS friendly to each other. An actor Sifu has
// no relationship entry for reads back as Neutral, and Neutral is exactly a
// character that will defend itself and never start anything.
//
// So say it. ERelationshipTypes has two hostile values and it is not obvious
// from the outside which one the combat code consults, so try `Fight` (being in
// a fight right now) before `Enemy` (a standing disposition), keep whichever the
// game actually stores, and re-asserts on a timer because something in Sifu
// recomputes these.
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
        // Sample first. This function has been reporting success since the day
        // it was written -- "5 of 5 active enemies hold 'Fight'" -- while the
        // player-side user of the SAME setter and the SAME getter reported that
        // the setter was a no-op. Both cannot be true. If an enemy already reads
        // Fight toward a player-class pawn before we touch it, this test proves
        // nothing about the write, and that is very likely what has been
        // happening: it is the one measurement here that was never controlled.
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

// Reflection is not free -- this is a ProcessEvent per enemy -- so only a few
// enemies are handled per tick and each is left alone for seconds afterwards.
void MaintainPeerHostility(ue::UObject* peer) {
    if (!peer || g_hostility_hopeless) return;
    if (coop::Get().mode != coop::Mode::Coop) return;
    // Never assert a relationship against a table built for a level we have
    // left. These pointers are freed by the transition, and the relationship
    // multicast dereferences whatever it is handed.
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
        // Wake perception on the same cadence, not only once the remote player
        // has already hit something. Detection that starts on the first blow is
        // detection that arrives too late: every opening strike counted as an
        // ambush, which in Sifu breaks structure outright and leaves the enemy
        // open to an instant takedown. Being aware of him beforehand is what
        // turns that into an ordinary exchange.
        if (held) WakeEnemyPerception(entry.ai_fighting);
        entry.next_hostility_ms = now + (held ? 5000 : 1000);
        if (held == entry.hostile_confirmed) continue;
        entry.hostile_confirmed = held;
    }

    // If nothing at all will hold after a fair number of tries, stop burning
    // reflection calls on it every second and say so once. The same setter is
    // what the player-side friendly relationship depends on, so this is one
    // fact about the build, not two separate mysteries.
    if (g_hostile_value == relationship::kUnknown && ++g_hostility_attempts > 200) {
        g_hostility_hopeless = true;
        SC_LOG("targets: no hostile relationship value would stick on any enemy -- "
               "BPF_ServerChangeRelationship does not appear to work on this build, so "
               "enemies will keep ignoring your partner until they are hit");
        coop::ReportProblem("enemies cannot be told your partner is an enemy");
    }

    // A rolling count, because "did this work" is otherwise invisible.
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
    // Reads m_Target, the same field the targets census reads -- and that
    // difference was the whole of "the peer never gets a real fight".
    //
    // This used to probe BPF_GetTargetForAction across eight action slots. The
    // census was moved off that call precisely because it answers "nobody" for
    // enemies that are demonstrably mid-swing, and the two then disagreed
    // flatly: the census reported an enemy on the second player in 24 of 38
    // samples while this function put the flag on the wire almost never. The
    // joining machine keys ownership off that flag, so it claimed an enemy only
    // when the eight-second aggro lease forced the flag by hand -- twice in a
    // whole session. Everything downstream followed: no local fight, no position
    // authority, nothing to publish back.
    const std::int32_t target = TargetIndexOf(attack_component);
    if (target < 0) return 0;
    if (target == InternalIndexOf(host_player)) return net::kEnemyTargetsHost;
    if (peer_player && target == InternalIndexOf(peer_player)) return net::kEnemyTargetsPeer;
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

void RegisterEnemyTargetable(ue::UObject* actor) {
    if (!actor || !g_get_targetable_actor_component || !g_register_targetable_actor) return;
    ue::UObject* targetable = g_get_targetable_actor_component(actor);
    if (targetable) g_register_targetable_actor(targetable);
}

// Map a player identity from the wire to the equivalent body on this machine.
// On the joiner, the host is the puppet and the peer is the physical player.
// On the host those identities are the other way around. This used to assume
// it only ran on the joiner; bidirectional enemy action ownership makes the
// host an observer for peer-owned enemies too.
bool ApplyMirroredTarget(Tracked& entry, std::uint8_t flags) {
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
           desired == local ? "local player" : "remote puppet");
    return true;
}

bool ForceAttackTarget(Tracked& entry, ue::UObject* desired) {
    if (!entry.attack_component || !desired || !g_set_attack_target) return false;

    // BPF_ForceEnemy WAS called here, and it is disabled deliberately.
    //
    // It worked, in the narrow sense: the roles line went from all-zero to
    // `fighting your partner direct=1`, so the combat-role ticket theory is
    // right and the call does reach the director. Two things came with it.
    //
    // It crashes. Registering the puppet as a ticket target means the director
    // tries to unregister it when anything dies, and that path dereferences
    // through a null:
    //   FAICombatRoleTicketManager::AddRemoveCandidate  AICombatRolesTicketManager.cpp:428
    //   AAIDirectorActor::RemoveActorFromSystems        AIDirectorActor.cpp:813
    //   AAIDirectorActor::OnDeathDetected               AIDirectorActor.cpp:719
    // reading address 0x108. A spawned player clone is not a candidate the
    // director's bookkeeping can survive removing.
    //
    // And it made the fight worse: with the puppet registered, the same reading
    // showed the host at `direct=0 non=3` -- four of five enemies parked as
    // NonOpponent, fighting nobody at all. Granting a ticket to a body the
    // director cannot fully account for takes the room out of combat rather
    // than sharing it.
    //
    // ...and the prerequisite that note asked for was implemented as the
    // experiment below. It is now OFF: the combined path allocated roles once,
    // then crashed both machines during later all-target redistribution.
    //
    // Disassembly after that crash found the argument error. The director method
    // calls SetTarget(manager, target), then AddCandidate(manager, fourth arg).
    // The old call passed the partner as BOTH arguments, putting the partner in
    // its own candidate list instead of putting enemies there. RequestEvaluation
    // later received a null manager (`this + 0x248` produced the exact 0x24c
    // fault address). MaintainPartnerAsDirectorTarget now registers each live
    // enemy as the candidate; this ForceEnemy path remains a separate, disabled
    // experiment.
    //
    //
    // Why BOTH halves are needed, which took far too long to see. There are two
    // separate fields and the two census lines read one each:
    //   - the ATTACK COMPONENT's target, set by g_set_attack_target below, is
    //     what `targets:` counts;
    //   - the AI's own enemy, read by ReadAIEnemy, is what `roles:` counts, and
    //     it is set by nothing in this mod except the call below.
    // So every fix so far moved `targets` to the partner while `roles` stayed at
    // zero, and the enemies duly walked over to the partner and stood there. Aim
    // without permission. Registering the target creates the ticket manager;
    // this call is what puts the enemy into it.
    if (coop::Get().force_enemy_engage && entry.ai_fighting &&
        ReadAIEnemy(entry.ai_fighting) != desired) {
        ForceEnemy(entry.ai_fighting, desired, kBehaviorAlerted);
    }

    // Safe half of the same idea: wake the enemy's perception so the remote
    // player is somebody it has noticed rather than somebody who keeps
    // appearing out of nowhere.
    WakeEnemyPerception(entry.ai_fighting);

    g_set_attack_target(entry.attack_component, desired);
    struct Params {
        ue::UObject* current_attacked;
    } params = {};
    params.current_attacked = desired;
    ue::CallFunction(entry.attack_component, L"BPF_UpdateLockMoveTarget", &params);
    // This used to return true unconditionally, which made
    // "FIRST peer hit handed enemy aggro to puppet" a line that proved nothing
    // -- it was logged whether or not the write took. Read it back instead.
    return TargetIndexOf(entry.attack_component) == InternalIndexOf(desired);
}
// --- Making the partner a TARGET the director knows about --------------------
//
// This is the measured cause of "enemies never attack my partner", and of the
// two reports that follow from it -- the partner takes no damage, and a dead
// player's attackers keep working on the corpse instead of switching over.
//
// The host census says it in one line, every sample:
//
//   targets: 0 enemies on YOU, 3 on the second player, 0 elsewhere, 0 idle
//   roles:   fighting YOU direct=1 indirect=1 |
//            fighting your partner direct=0 indirect=0 non=0 none=0
//
// Three enemies AIMED at the partner. All four role counters for the partner at
// zero -- not "they hold NonOpponent", not "they hold None", but no entry of any
// kind. Aim and permission are different things: only a DirectOpponent may
// swing, roles come out of a ticket manager AAIDirectorActor keeps PER TARGET,
// and nothing ever asked it to keep one for the puppet. Pointing enemies at a
// body the director has never heard of is why every targeting fix so far moved
// the `targets` line and never the `roles` one.
//
// RegisterOrRemoveFromCombatRoleTicketManagerForTarget is the director's own
// front door for exactly this, and it has never been called. Public, one symbol
// at its RVA, resolved on both builds.
//
// It also explains the BPF_ForceEnemy crash rather than repeating it. That call
// handed out a ticket for a target the director had no manager for, so removal
// on death walked a null:
//   FAICombatRoleTicketManager::AddRemoveCandidate  ...:428
//   AAIDirectorActor::RemoveActorFromSystems        ...:813
//   AAIDirectorActor::OnDeathDetected               ...:719
// Registering through the front door is what creates the bookkeeping that the
// removal path expects to find. The matching unregister below is mandatory for
// the same reason: leaving a destroyed puppet registered is that crash again.
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

// EGlobalBehaviors, from the exe's enumerator table:
// 0 Idle, 1 Suspicious, 2 Surprised, 3 Alerted, 4 Abandoning, 5 Friendly.
// Alerted is "there is a fight on and this is part of it".
constexpr std::uint8_t kGlobalBehaviorAlerted = 3;
// ESCAICombatRolesChangeReason: 2 is Script, which is exactly what this is.
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

// Not optional. An unregistered teardown is the crash quoted above.
void ReleasePartnerFromDirector() {
    if (!g_registered_partner || !g_registered_with_director) {
        ClearDirectorRegistration();
        return;
    }

    // Kismet IsValid is not a guard for an arbitrary cached pointer: invoking
    // it already dereferences the UObject. The 22:11 restart crash was exactly
    // this call receiving a puppet destroyed on disconnect. Only call the
    // native while every pointer is explicitly owned by the current world.
    const bool current_registration =
        g_registered_world && g_registered_world == ue::GetWorld() && TrackingIsCurrent();
    if (current_registration && g_director_remove_for_target) {
        int removed = 0;
        for (int i = 0; i < g_tracked_count; ++i) {
            ue::UObject* candidate = g_tracked[i].actor;
            if (!candidate) continue;
            // The native order is (manager target, candidate), as confirmed by
            // RemoveActorFromCombatRolesForTarget's GetTicketManager/RemoveCandidate calls.
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

    // The settle rule the relationship writes learned the hard way: a world
    // still being built has half-constructed AI structures, and this walks them.
    static ue::UObject* seen_world = nullptr;
    static DWORD world_settled_at = 0;
    ue::UObject* world = ue::GetWorld();
    if (!world) return;
    const DWORD now = GetTickCount();
    if (world != seen_world) {
        seen_world = world;
        world_settled_at = now;
        ClearDirectorRegistration();  // that world's director went with it
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
        static bool warned = false;
        if (!warned) {
            warned = true;
            SC_LOG("director: no AAIDirectorActor in this level -- roles cannot be allocated "
                   "to your partner here");
        }
        return;
    }

    int candidates = 0;
    for (int i = 0; i < g_tracked_count; ++i) {
        Tracked& entry = g_tracked[i];
        if (!entry.active || !entry.actor || !ue::IsValidObject(entry.actor)) continue;
        // Native semantics: target=the actor being fought, fourth argument=the
        // hostile candidate eligible for a role against that target.
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
        entry.ai_fighting = GetAIFightingComponent(actor);
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
            // Must survive a rebuild, or a table refresh in the half-second
            // between our kill and the host's confirmation drops the latch and
            // the corpse is resurrected after all.
            entry.died_locally = previous[k].died_locally;
            entry.death_anim_presented = previous[k].death_anim_presented;
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
            // These two were written AFTER the break, so they never ran. The
            // table is rebuilt whenever any enemy goes unmatched, which is
            // routine mid-fight -- and every rebuild silently threw away the
            // eight-second window in which an enemy is held on the remote
            // player after their damage lands, along with the timestamp that
            // paces the re-assertion. The aggro handoff was being cancelled
            // almost as fast as it was granted.
            entry.peer_aggro_until = previous[k].peer_aggro_until;
            entry.last_peer_target_ms = previous[k].last_peer_target_ms;
            entry.next_hostility_ms = previous[k].next_hostility_ms;
            entry.hostile_confirmed = previous[k].hostile_confirmed;
            // Without this a rebuild forgets the body is running its own brain,
            // and the very next frame stops it again -- the handover would last
            // until the first unmatched enemy and no longer.
            entry.local_brain = previous[k].local_brain;
            entry.not_ours_since = previous[k].not_ours_since;
            entry.ours_wanted_since = previous[k].ours_wanted_since;
            entry.owned_since = previous[k].owned_since;
            entry.last_esync_ms = previous[k].last_esync_ms;
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

// A replicated health decrease has no FHitRequest on this machine, so
// BPF_ApplyDamage changes the number without starting the victim's hit order.
// AFightingCharacter::BPF_LaunchImpact is the small, Blueprint-facing way back
// into that state machine.  Its exact reflected parameter names come from the
// shipped PDB: (_fDamage, _bLethal, _fStunTime).
//
// Damage stays zero deliberately.  The cumulative damage ledger immediately
// below is authoritative and has already applied the real delta; charging it a
// second time here would both double the hit and feed a phantom delta back to
// the host.  0.25 s is the conservative 15-frame stun used by the extracted
// Grunt hit-box data.  A failure to resolve the UFunction is harmless and is
// named in the log.  mirror_hit_reactions=0 disables this probe without a
// rebuild.
bool LaunchReplicatedImpact(Tracked& entry, float replicated_delta) {
    if (!coop::Get().mirror_hit_reactions || !entry.actor) return false;

    struct Params {
        float fDamage = 0.f;
        bool bLethal = false;
        std::uint8_t padding[3] = {};
        float fStunTime = 0.25f;
    } params;
    static_assert(sizeof(Params) == 12, "BPF_LaunchImpact parameter layout changed");

    const bool launched = ue::CallFunction(entry.actor, L"BPF_LaunchImpact", &params);
    static unsigned int attempts = 0;
    if (++attempts <= 8 || coop::Get().verbose_enemies) {
        // `owner` is the field that decides whether this line means anything.
        // A reaction on a body we are simulating would have happened without
        // us; only the OBSERVED ones test whether the replicated impact works,
        // and the census cannot tell the two apart on its own.
        SC_LOG("reaction: replicated %.1f damage on %08X '%s' [%s] -- zero-damage fake "
               "impact %s (stun=%.2f)",
               replicated_delta, entry.hash, entry.name,
               entry.local_brain ? "we simulate it" : "OBSERVED",
               launched ? "LAUNCHED" : "UNAVAILABLE", params.fStunTime);
    }
    return launched;
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

        // The ledger is committed only once the hit has actually gone in.
        // Writing it first meant a body whose health component could not be
        // resolved that frame recorded the peer's damage as applied and then
        // silently dropped it: the peer's total never mentions that delta
        // again, so the hit is gone for good.
        Fighter fighter = ResolveFighter(entry.actor);
        if (!fighter.health) continue;
        ApplyDamage(fighter, delta);
        *applied = reports[i].total;
        if (GetHealth(fighter) > 0.5f) LaunchReplicatedImpact(entry, delta);

        // BPF_ApplyDamage is a health path: it reaches zero and kills the body,
        // but it never went through the hit that would have chosen a death
        // animation, so the corpse stays on its feet. That is the other half of
        // "the one who did not land the killing blow never sees it fall" -- the
        // joining side had this fixed, the host had the same hole for the
        // damage its peer reported.
        //
        // No animation to hand over here, so this is the plain version: put it
        // down. A body that falls generically beats one that stands.
        if (GetHealth(fighter) <= 0.5f && !IsDown(fighter)) {
            SetDown(fighter, true);
            // The host has the same problem the joiner had, for a sharper
            // reason: by the time the partner kills this body, the host has
            // ALREADY stopped its brain to hand the fight over. The 05:11 log
            // shows it plainly --
            //   05:11:10  authority: ...Receptionist_Left actions handed to peer;
            //             host brain stopped
            //   05:11:13  enemies: ...Receptionist_Left killed by your partner
            //             -- forced down
            // -- and a body with a stopped brain cannot play its own death.
            // SetDown alone marks it and leaves it standing, so the host sees
            // the corpse the joiner is looking at from the other side of the
            // same bug. Run the presentation here too.
            NotifyDownStateChanged(fighter, true);
            if (coop::Get().verbose_enemies) {
                SC_LOG("enemies: %s killed by your partner -- forced down and presented",
                       entry.name);
            }
        }

        // Raw replicated health damage has no instigator, so Sifu perception
        // never learns who hit it. Hand aggro to the host's peer body and keep
        // it alive briefly; normal AI can then select/attack that body.
        ue::UObject* peer = GetPuppet();
        if (peer && ForceAttackTarget(entry, peer)) {
            entry.peer_aggro_until = GetTickCount() + 8000;
            entry.last_peer_target_ms = GetTickCount();
            static bool first_aggro = true;
            if (first_aggro) {
                // The role is the part that matters: aggro without a
                // DirectOpponent ticket is an enemy that will follow your
                // partner around deflecting and never swing.
                SC_LOG("targets: FIRST peer hit handed %s aggro to puppet -- role is now %s",
                       entry.name, combat_role::Name(ReadCombatRole(entry.ai_fighting)));
            }
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

    // Before publishing anything: make sure the level's enemies have been told
    // the remote player is an enemy. Without this they are Neutral toward the
    // puppet, which is a character they will defend themselves against and
    // never attack.
    MaintainPeerHostility(peer_player);
    MaintainPartnerAsDirectorTarget(peer_player);

    // A dead player is not a fight. Hand the room to the partner.
    //
    // Reported: "when host was dead, enemies still attacked at him, didn't want
    // to attack peer". They had no reason to switch -- nothing here ever told
    // them the body they were aimed at had stopped being a threat, so they kept
    // working on a corpse while the surviving player stood untouched.
    //
    // This reuses the peer-aggro lease that already runs after a peer's hit
    // lands, and nothing else: no BPF_ForceEnemy, no director ticket. That
    // matters, because forcing a ticket is the thing that crashes in
    // AAIDirectorActor::OnDeathDetected and it is documented at ForceAttackTarget
    // below. So this steers them and cannot grant them permission to swing --
    // if the roles census still reads `fighting your partner direct=0` while
    // this is active, the ticket really is the only remaining blocker and that
    // is worth knowing precisely.
    if (peer_player && host_player) {
        Fighter host_fighter = ResolveFighter(host_player);
        const bool host_is_out =
            host_fighter.health && (GetHealth(host_fighter) <= 0.5f || IsDown(host_fighter));
        Fighter peer_fighter = ResolveFighter(peer_player);
        const bool peer_is_out =
            peer_fighter.health &&
            (GetHealth(peer_fighter) <= 0.5f || IsDown(peer_fighter));
        static bool announced_host_down = false;
        static bool announced_peer_down = false;
        if (host_is_out && !peer_is_out) {
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
        } else if (coop::Get().retarget_from_down_peer && peer_is_out && !host_is_out) {
            int handed = 0;
            for (int i = 0; i < g_tracked_count; ++i) {
                Tracked& entry = g_tracked[i];
                if (!entry.active || !entry.actor) continue;
                Fighter enemy = ResolveFighter(entry.actor);
                if (enemy.health &&
                    (GetHealth(enemy) <= 0.5f || IsDead(enemy))) continue;
                // Cancel the peer-hit lease first. Otherwise the publish loop
                // below reasserts the corpse as target again on the same frame.
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
        // Same correction as the player snapshot: ask the movement component
        // rather than differencing the transform. GetTickCount only advances
        // every ~15.6 ms, so on a fast machine several sweeps in a row see the
        // same timestamp AND the same position, and publish a velocity of zero
        // between real samples. The joiner's locomotion band then flips faster
        // than BaseMovementDB's blends can resolve and the enemy slides.
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
                // Travel/pool corrections are not locomotion; never turn one
                // into a multi-frame sprint on the joining machine.
                if (speed > 3000.f) state.velocity_x = state.velocity_y = state.velocity_z = 0.f;
            }
        }
        // If the PEER owns this enemy, its version wins on this screen too.
        //
        // The joining machine simulates the enemies fighting it, because that is
        // the only place they can be given a real fight. Without this the host
        // kept its own drifting copy and the two players were watching different
        // enemies in different places. Ownership is only about position: the
        // host still decides health and death, and it publishes what it sees
        // here so the joiner's own copy keeps its authority over nothing but the
        // transform.
        net::OwnedEnemy owned = {};
        const bool peer_owned =
            net::GetOwnedEnemy(entry.wire_hash ? entry.wire_hash : entry.hash, &owned);
        if (peer_owned) {
            // Transform ownership without action ownership left this body with
            // two brains: the joiner chose the attack the peer saw, while the
            // host chose a different attack for its observer view. Stop the
            // host copy and let the incoming OrderEvent drive its real local
            // attack order. KeepClientBrainStopped retries because level logic
            // can restart a stopped behavior tree behind us.
            const bool was_stopped = entry.ai_stopped;
            KeepClientBrainStopped(entry, now);
            if (!was_stopped && entry.ai_stopped) {
                SC_LOG("authority: %s actions handed to peer; host brain stopped", entry.name);
            }
            const ue::FVector target = {owned.x, owned.y, owned.z};
            const ue::FRotator facing = {0.f, owned.yaw, 0.f};
            const ue::FVector velocity = {owned.velocity_x, owned.velocity_y,
                                          owned.velocity_z};
            DriveActorTo(entry.actor, target, facing, velocity);
            location = target;  // publish where it actually is, not where we had it
            state.x = target.X;
            state.y = target.Y;
            state.z = target.Z;
            state.yaw = owned.yaw;
        } else if (entry.ai_stopped) {
            // OwnedEnemy expires after 500 ms. Resume a LIVE enemy so a dropped
            // lease cannot freeze the fight, but never restart a corpse. The
            // latest two-machine log showed this branch running 0.1--0.5 s
            // after each peer kill, interrupting the fall and leaving the host
            // looking at an upright, motionless dead body.
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
        state.damage_applied = AppliedTotalValue(entry.hash);
        // Hit-stop, so the observer's copy stutters on the same frames this one
        // does. Read from the actor rather than inferred from orders: freeze
        // frames come from several different order types and the value is the
        // thing that actually matters.
        state.time_dilation = GetActorTimeDilation(entry.actor);
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

// The other half of authority following the fight.
//
// Enemies running their own behaviour tree on THIS machine are the ones fighting
// this player, and their position here is the real one -- the host is simulating
// a copy that drifts. Publish them so the host can display ours instead of its
// own. Health and death are untouched by this and stay host-authoritative; all
// this decides is where a body is standing.
void SendOwnedEnemies() {
    net::OwnedEnemy owned[net::kMaxOwnedEnemiesPerPacket];
    int count = 0;
    for (int i = 0; i < g_tracked_count && count < net::kMaxOwnedEnemiesPerPacket; ++i) {
        const Tracked& entry = g_tracked[i];
        if (!entry.local_brain || !entry.active || !entry.actor) continue;

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
    // Normally an empty set can expire through the short lease. When this
    // player is down, explicitly repeat an empty authoritative set so the host
    // drops every old owner immediately even if one UDP packet is lost.
    if (count > 0 || g_announce_empty_ownership) {
        net::SendOwnedEnemies(count > 0 ? owned : nullptr, count);
    }
}

void SendDamageReports() {
    net::DamageReport reports[net::kMaxDamagePerPacket];
    int count = 0;
    for (int i = 0; i < g_tracked_count && count < net::kMaxDamagePerPacket; ++i) {
        if (g_tracked[i].reported_total <= 0.f) continue;
        if (!g_tracked[i].seen_from_host) continue;  // not in the host's fight
        // Fallback to match the two sibling call sites (the owned-enemy publish
        // and the host-state lookup). The seen_from_host gate above implies a
        // wire hash was bound, so this is latent rather than live -- but three
        // places deriving the same id two different ways is how a live one
        // starts.
        reports[count].name_hash =
            g_tracked[i].wire_hash ? g_tracked[i].wire_hash : g_tracked[i].hash;
        reports[count].total = g_tracked[i].reported_total;
        ++count;
    }
    if (count == 0) return;
    net::SendEnemyDamage(reports, count);
    ++coop::GetStats().damage_reports;
}

void ApplyRemoteEnemies() {
    if (!net::HasEnemySweep()) return;

    // Every actor pointer below belongs to the world the table was built in, and
    // a restart frees all of them. The level-path check further down is not
    // enough on its own -- restarting the SAME level produces a new UWorld with
    // the same package path, which is exactly the case the refresh code already
    // warns about. Without this the pass ran over freed bodies and crashed
    // inside this dll on restart, with nothing but our own frames on the stack.
    if (!TrackingIsCurrent()) return;

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
    ue::UObject* local_world = ue::GetWorld();
    ue::UObject* local_player =
        local_world ? ue::GetPlayerCharacter(local_world, 0) : nullptr;
    Fighter local_fighter = ResolveFighter(local_player);
    const bool local_player_is_out =
        local_fighter.health &&
        (GetHealth(local_fighter) <= 0.5f || IsDown(local_fighter));
    g_announce_empty_ownership =
        config.retarget_from_down_peer && local_player_is_out;

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
        // Enemies the host says are fighting THIS player are handed back to
        // their own brains, when that has been asked for. The host's director
        // will never allocate an attacker to the puppet standing in for this
        // player, so the only place a real fight can happen for them is here,
        // where they are player zero and a legitimate target like any other.
        // Ownership is LATCHED, and that is the whole of three separate bugs.
        //
        // kEnemyTargetsPeer is sampled from the host's AI several times a second
        // and flickers -- an enemy mid-swing routinely reads as targeting nobody.
        // Deriving ownership straight from it meant the brain was restarted and
        // stopped again over and over, and with it the drive authority and the
        // position publisher. That is exactly "peer can sometimes attack, often
        // not" and "positions are sometimes synced, often not": nothing was
        // broken, everything was oscillating.
        //
        // It is also the likeliest source of the director crash. Sifu's own
        // SuspiciousBTService, in a tree WE restarted, calls SwitchToAlerted,
        // which has the director redistribute combat roles, which walks the
        // ticket manager's weak pointers. Starting and stopping the same brain a
        // few times a second is our doing, and it leaves that bookkeeping
        // half-built.
        //
        // So: claimed the moment the host says this enemy is fighting the peer,
        // and released only after it has said otherwise for a while -- or at
        // once when the body dies or leaves the fight, because then the host's
        // death handling has to own it again immediately.
        // Claimed on OUR OWN judgement, not only on the host's word.
        //
        // The host's census reports up to five enemies fighting the peer while
        // this side claimed none of them, and the flag has to survive a sample,
        // a packet and a 50 ms window to get here. This machine does not need to
        // be told: it can see where the enemy is standing, where its own player
        // is, and where the puppet standing in for the host is. An enemy clearly
        // nearer to us than to them is ours to simulate, and that is the same
        // judgement the host makes for its own side.
        //
        // The margin matters. Without it a body midway between the two players
        // would change hands every few frames, which is the thrash the latch
        // exists to prevent. The host's flag is kept as an additional trigger,
        // so nothing that used to claim an enemy stops claiming it.
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
                // Out of fighting range of us at all: never ours.
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
                        near_us = true;  // no puppet to compare against yet
                    }
                }
            }
        }

        const bool host_says_ours =
            config.peer_fights_locally &&
            (((state.flags & net::kEnemyTargetsPeer) != 0) || near_us);
        // `dead` proper is decided further down; this needs the same answer
        // earlier, and it is the same two facts off the same packet.
        const bool host_says_dead = (state.flags & net::kEnemyDead) != 0 ||
                                    (state.max_health > 0.f && state.health <= 0.5f);
        const bool must_release = host_says_dead || !entry.active ||
                                  (config.retarget_from_down_peer && local_player_is_out);

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

        // OWNERSHIP MUST BE STICKY. This is the systemic fault behind most of
        // what still felt broken.
        //
        // Measured over 90 s of one fight: 17 handoffs across 5 enemies, one
        // body changing hands 5 times. Every handoff starts or stops a
        // behaviour tree, which interrupts whatever order was playing, changes
        // who the body is aiming at, and swaps it between self-moving and
        // teleport-driven. A body mid-handoff is neither properly simulated nor
        // properly driven, and that is what "the other player's attacks phase
        // through that enemy, and its attacks phase through him" describes.
        //
        // The old rule made this inevitable: claiming was instantaneous while
        // releasing took three seconds, so a body released on a distance
        // comparison was re-claimed on the very next sweep. Both inputs flap by
        // nature -- `near_us` is a 300-unit margin between two players standing
        // in the same room, and the host's targets-peer flag follows its own
        // AI's retargeting.
        //
        // So: a claim must be wanted continuously before it is taken, and a body
        // once claimed is held for a minimum spell before any distance or
        // targeting opinion can take it away. Neither delay applies to
        // must_release -- death, deactivation and a downed player are facts, not
        // opinions, and those still release immediately.
        constexpr DWORD kClaimDebounceMs = 600;
        constexpr DWORD kReleaseAfterMs = 4000;
        constexpr DWORD kMinimumOwnershipMs = 5000;

        bool fights_us = entry.local_brain;
        if (!entry.local_brain && host_says_ours && !must_release) {
            fights_us = entry.ours_wanted_since != 0 &&
                        own_now - entry.ours_wanted_since >= kClaimDebounceMs;
        } else if (entry.local_brain && must_release) {
            fights_us = false;
        } else if (entry.local_brain && entry.not_ours_since != 0 &&
                   own_now - entry.not_ours_since >= kReleaseAfterMs &&
                   (entry.owned_since == 0 ||
                    own_now - entry.owned_since >= kMinimumOwnershipMs)) {
            fights_us = false;
        }

        if (fights_us != entry.local_brain) {
            entry.local_brain = fights_us;
            entry.owned_since = fights_us ? own_now : 0;
            if (fights_us) {
                if (StartBrain(entry.actor)) {
                    entry.ai_stopped = false;
                    // Hand it a fight it already knows about. Otherwise the
                    // first thing its restarted brain sees is a player who
                    // appeared from nowhere, and Sifu treats that as an ambush:
                    // structure broken outright and wide open to a takedown.
                    WakeEnemyPerception(entry.ai_fighting, /*force_behaviour=*/true);
                    SC_LOG("enemies: %s handed to local AI -- it is fighting you", entry.name);
                }
            } else {
                entry.ai_stopped = false;  // make the next stop actually run
                entry.not_ours_since = 0;
                SC_LOG("enemies: %s returned to the host's drive", entry.name);
            }
        }

        // Only force a mirrored target onto a body that is NOT thinking for
        // itself. Overwriting the target of an enemy running its own behaviour
        // tree fights that tree every frame, and an enemy whose lock keeps being
        // yanked swings where it was told to rather than where its opponent is
        // -- which is what "their attacks morph through the client" looks like.
        if (!fights_us) {
            // Applied before this frame's queued attack events run: it swaps
            // host/peer perspective as documented in protocol.h, so a swing
            // meant for the joining player cannot land on their host puppet.
            ApplyMirroredTarget(entry, state.flags);
        }

        // A body with no brain cannot animate, so give every enemy its brain.
        //
        // This is what "on the client they stand there, do not react to hits and
        // do not fall over" comes down to. Sifu animates through Orders and
        // Orders come from the brain; a stopped one leaves the body with no
        // mechanism to play a reaction or a death at all, and no field to mirror
        // instead -- USCAnimInstance carries no current-action asset the way the
        // player's animation instance does. The enemies this side already claims
        // animate correctly, and the only difference is that they have a brain.
        const bool local_ai = config.client_simulates_enemies || fights_us;

        if (config.suppress_client_ai && !local_ai) {
            KeepClientBrainStopped(entry, GetTickCount());
        } else if (local_ai && entry.ai_stopped) {
            if (StartBrain(entry.actor)) {
                entry.ai_stopped = false;
                if (config.verbose_enemies) {
                    SC_LOG("enemies: %s thinking for itself here", entry.name);
                }
            }
        }

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
            RegisterEnemyTargetable(entry.actor);
            entry.present = true;
            entry.parked = false;
        }

        Fighter fighter = ResolveFighter(entry.actor);

        // ...and the test above only ever catches OUR bookkeeping changing.
        //
        // The comment above is right about the symptom and wrong about the
        // sufficiency. `entry.present` records what we last asked for, not what
        // the body actually has, and Sifu retires and restores collision and
        // targetable registration on its own -- around knockdowns, deaths,
        // pooling and the resurrection sequence. Whenever it does that behind
        // us, our flag still reads "present", the transition never fires, and
        // the repair never runs. The body stays alive, visible, and untouchable
        // for the rest of the fight.
        //
        // Reported exactly that way: an enemy one player is fighting cannot be
        // hit by the other, and it persists after that enemy dies. The bodies
        // that change hands are the ones whose liveness churns, which is why it
        // is always those.
        //
        // So re-assert rather than infer. Idempotent reflected calls, twice a
        // second, and only for a body that is supposed to be standing and
        // fighting -- never for one that is down, where Sifu's reduced collision
        // is deliberate and putting it back would be the bug.
        if (should_be_present && fighter.health && !IsDown(fighter)) {
            const DWORD presence_now = GetTickCount();
            if (entry.next_presence_refresh == 0 ||
                static_cast<LONG>(presence_now - entry.next_presence_refresh) >= 0) {
                entry.next_presence_refresh = presence_now + 500;
                SetActorPresent(entry.actor, true);
                RegisterEnemyTargetable(entry.actor);
            }
        }
        const bool locally_dead_before_sync =
            fighter.health && (GetHealth(fighter) <= 0.5f || IsDead(fighter));

        // A body we are simulating that reaches zero here has been KILLED here,
        // by this player, and Sifu has already started its death sequence for
        // it. Latch that, because for the next few hundred milliseconds the
        // host will still be saying "alive" -- it has not been told yet -- and
        // acting on that is what has been destroying every corpse.
        //
        // Measured, 2026-08-11:
        //   07:28:28.659  hp=35/35 owned=1 alive
        //   07:28:28.961  revived and re-registered from client-only death
        //   07:28:29.079  death: kill=1 anim=0 -> down=1
        // Killed locally, resurrected by us 300 ms later, killed again by the
        // host's decree 118 ms after that. Three state transitions on one body
        // inside half a second. The revive sets health back to 1 and calls
        // SetDown(false), which aborts the death animation already playing --
        // that is the whole of `anim=0` and of the standing corpses. And a body
        // walked in and out of Sifu's down-state machine from outside comes
        // back upright but no longer a valid hit target, which the handoff
        // already documented for knockdowns: that is the "attacks go straight
        // through enemies that were already attacked" report, same cause.
        // ...and `entry.local_brain` was wrong here, which is why this only ever
        // half-worked.
        //
        // A body dies locally for two reasons, not one. It can be killed by this
        // player under our own brain -- which is what the note above describes
        // -- or it can reach zero because we applied the HOST's authoritative
        // damage through Sifu's own damage path, which is every enemy the host
        // kills while we are the observer. The second kind never latched, so the
        // revive below stood it straight back up, and the two symptoms that
        // produced are exactly what was reported after the last build: the death
        // animation plays and the body immediately gets up, and a body walked
        // back out of the down-state machine is upright but no longer a valid
        // hit target, so attacks pass through it.
        //
        // The reason the host still says "alive" for a moment is ordinary: our
        // ApplyDamage lands before the host's next sweep carries kEnemyDead. The
        // race is the same one; only the population was too narrow.
        //
        // Waking a pooled body is still honoured -- that is the recycle branch
        // further down, which requires full health rather than merely "alive".
        if (locally_dead_before_sync && !entry.died_locally) {
            entry.died_locally = true;
            if (config.verbose_enemies) {
                SC_LOG("enemies: %s died HERE (%s) -- holding the corpse until the host agrees",
                       entry.name, entry.local_brain ? "our brain" : "host damage applied here");
            }
        }

        // Order matters here. Local damage has to be read *before* the host's
        // health is written on top, or the write itself would be mistaken for
        // a hit -- or worse, our own hit would be erased before it was ever
        // reported and the enemy would be unkillable from this side.
        if (config.report_damage) AccumulateLocalDamage(entry, fighter);

        const bool host_caught_up = state.damage_applied + 0.05f >= entry.reported_total;
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
                    const float replicated = local_health - state.health;
                    ApplyDamage(fighter, replicated);
                    // BPF_ApplyDamage is a health path, not an impact, so the
                    // body loses health without ever registering that something
                    // hit it.
                    //
                    // The host already does this for damage the JOINER dealt.
                    // Doing it only there was half a fix: the reported symptom
                    // is symmetric -- "the observing machine doesn't show
                    // enemies' hurt animations" -- and the observer of the
                    // host's fight is this side. Same zero-damage impact, same
                    // reasoning: the health delta above is authoritative and has
                    // already been charged, so this adds the reaction and no
                    // second hit.
                    if (config.mirror_hit_reactions) {
                        WakeEnemyPerception(entry.ai_fighting);
                        if (GetHealth(fighter) > 0.5f) {
                            LaunchReplicatedImpact(entry, replicated);
                        }
                    }
                    if (GetHealth(fighter) > state.health + 0.5f) {
                        SetHealth(fighter, state.health);  // guarded fallback
                    }
                } else if (state.health > local_health + 0.05f) {
                    // Upward corrections are for a body that was RESET, not for
                    // ordinary disagreement mid-fight.
                    //
                    // This used to heal on any difference, and that is why one
                    // grunt absorbed 252 damage from the joining player against
                    // a pool of roughly 100-220 before it would die: every hit
                    // landed, and the next sweep put the health back. From the
                    // player's side that is exactly "I cannot hurt the enemies
                    // the host has already touched".
                    //
                    // The host's number is legitimately stale-high whenever this
                    // side is a beat ahead of it, and host_caught_up cannot see
                    // that, because it only accounts for damage the host has
                    // applied on OUR behalf -- not for the host's own hits still
                    // in flight. A genuine reset is a body returning to the pool
                    // at full health, which is a large jump; an in-fight
                    // difference is small. Only the large one is honoured, and
                    // the small one is left for the host to catch up on.
                    const float max_health = GetMaxHealth(fighter);
                    const float reset_gap = max_health > 0.f ? max_health * 0.5f : 60.f;
                    if (state.health - local_health >= reset_gap || first_host_state) {
                        SetHealth(fighter, state.health);
                    }
                }
                // Guard follows the same rule as health: take the host's value
                // when it is LOWER, never raise it.
                //
                // Writing it unconditionally is why the joining player could not
                // hurt anything. Sifu spends an attack on guard before it
                // touches health, and this restored the host's guard on every
                // single frame -- so the remote player's hits were absorbed by a
                // gauge that refilled faster than it could be emptied, health
                // never moved, and with nothing to report the host was never
                // told they had landed a blow. The joiner's whole session shows
                // `dmg out=0` and no first-hit line at all.
                if (state.guard + 0.05f < GetGuard(fighter)) SetGuard(fighter, state.guard);
                // What the body ACTUALLY has now, not what we asked for.
                //
                // This recorded state.health, and ApplyDamage does not land on
                // that number: Sifu runs the amount through armour and
                // multipliers, so the real health ends up lower. Next frame
                // AccumulateLocalDamage compares the actual value against this
                // higher one, reads the difference as a fresh hit by THIS
                // player, and reports it. The host applies that phantom damage,
                // publishes an even lower number, and the whole thing goes round
                // again.
                //
                // That loop is the 252 damage one grunt absorbed against a pool
                // of 240, and it is why the joining player's attacks start
                // passing through an enemy the host has also hit: the local copy
                // reaches zero long before the host's does, and a body that is
                // dead here has no collision left to hit.
                entry.last_local_health = GetHealth(fighter);
                entry.host_applied = state.damage_applied;
            }
        }

        entry.health = GetHealth(fighter);
        entry.max_health = GetMaxHealth(fighter);

        // Hit-stop, so this copy stutters on the same frames the owner's does.
        // Only for bodies this machine is presenting rather than simulating: an
        // enemy running its own brain here produces its own freeze frames, and
        // writing the owner's on top would fight them.
        if (!entry.local_brain) {
            SetActorTimeDilation(entry.actor, state.time_dilation);
        }

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
        //
        // Only on a CURRENT sweep. "The host says this body is alive" is a
        // statement about the moment the host said it, and the state in front
        // of us keeps saying it long after the host has stopped talking. When
        // the link drops mid-fight the last word is always "alive" -- so a body
        // the joining player has just killed is resurrected, dies again a
        // second later under the same local blows, and is resurrected again.
        // Four of those in seven seconds are in the 2026-08-11 log, immediately
        // after the host process quit, and a body oscillating between dead and
        // revived has no stable collision: that is a strong candidate for the
        // long-standing "attacks phase through" report.
        //
        // Fresh sweep, real revival. Stale sweep, the body stays dead and the
        // host repairs it when it comes back.
        // A genuine recycle: the pool has lifted this body back into the fight
        // with a full health bar. That is the one case where a host "alive"
        // against a local corpse is real rather than merely early, and it is
        // what the revive below was written for.
        //
        // ...and the FIRST authoritative state for a body is the other one.
        // Widening the latch to cover host-applied deaths would otherwise trap a
        // corpse restored from the joiner's own save: it is already lying down
        // when the level loads, the host has it alive at whatever health its own
        // run reached, and that is a disagreement about the past rather than a
        // race about the present. There is no local death to protect there,
        // because nothing in this session killed it.
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
        // Deliberately NOT cleared when the host agrees.
        //
        // Clearing on `dead` made the latch flap: it was set at the top of this
        // loop, cleared here, and set again on the next sweep, ~80 times a
        // second for as long as the corpse existed. The 04:56 log is one enemy
        // repeating "died HERE" every 12 ms. The latch only ever gates a
        // REVIVAL, and a revival requires !dead, so holding it while dead costs
        // nothing and the flap costs a flooded log.
        //
        // It is released by the two branches above -- a genuine pool recycle at
        // full health, or the first authoritative state for a body -- which are
        // the only two ways a dead body legitimately comes back.

        if (!dead && locally_dead_before_sync && !entry.died_locally &&
            net::EnemySweepIsFresh()) {
            entry.was_down = false;
            if (GetHealth(fighter) <= 0.5f) {
                const float revive_health =
                    host_caught_up && state.health > 1.f ? state.health : 1.f;
                SetHealth(fighter, revive_health);
                entry.last_local_health = revive_health;
            }
            SetDown(fighter, false);
            // ...and RUN the stand-up, not just clear the flag.
            //
            // Reported: "after revive collisions are effed up, all goes
            // through". SetDown changes the state machine;
            // UCharacterHealthComponent::OnCharacterStandsUp is what Sifu runs
            // at the END of a get-up to put the body back into the fight, and
            // on a machine with no replication nothing else ever calls it. A
            // body revived without it is upright in state, has never re-entered
            // its own combat bookkeeping, and is exactly as hittable as a
            // corpse. SetActorEnableCollision below restores the actor's
            // collision; this restores the character's.
            NotifyDownStateChanged(fighter, false);
            SetActorPresent(entry.actor, true);
            RegisterEnemyTargetable(entry.actor);
            entry.present = true;
            entry.parked = false;
            entry.revive_refused = false;
            SC_LOG("enemies: revived and re-registered %s from client-only death", entry.name);
        } else if (!dead && locally_dead_before_sync) {
            // Once per body, not once per sweep: a dropped link, or a host that
            // is simply a few hundred milliseconds behind, would otherwise print
            // this at the snapshot rate.
            if (!entry.revive_refused) {
                entry.revive_refused = true;
                SC_LOG("enemies: %s stays down -- %s", entry.name,
                       entry.died_locally
                           ? "we killed it, the host has not caught up yet"
                           : "the host's sweep is stale and its 'alive' cannot be trusted");
            }

            // PRESENT the death here, and only then claim the edge.
            //
            // Setting was_down without presenting is the bug the 04:56 log
            // caught: `hp=0/0 dead=1 hostdown=1 down=0`, held for the rest of
            // the fight. Claiming the edge suppressed the host's later
            // confirmation -- which is the branch that calls SetDown and
            // NotifyDownStateChanged -- so the corpse never entered Sifu's
            // down state at all. A body at zero health that is not down has not
            // retired its collision and is not a corpse: it neither lies down
            // nor can be hit, which is exactly "they don't stay down" and
            // "everything goes through" reported together.
            //
            // Do both, once. The host's edge below then finds the body already
            // down and its `if (!IsDown)` guard makes it a no-op, so the fall
            // that is playing is never interrupted by a second forced state.
            if (!entry.was_down) {
                entry.was_down = true;
                if (fighter.health) {
                    if (!IsDown(fighter)) SetDown(fighter, true);
                    NotifyDownStateChanged(fighter, true);
                    SC_LOG("death: %s presented locally (down=%d) ahead of the host's "
                           "confirmation",
                           entry.name, IsDown(fighter) ? 1 : 0);
                }
            }
        } else if (entry.was_down != dead) {
            entry.was_down = dead;
            // A death is not a knockdown, and InternalSetDownState only knows
            // how to do the second one. Forcing it from outside puts the body
            // in the down state without any of Sifu's death sequence, so the
            // enemy simply stopped where it was -- standing, upright, dead.
            //
            // Let the body die of its injuries instead: run its own health path
            // past zero and the game plays the fall it would have played if the
            // killing blow had landed here. Only when it is not already dead
            // locally, because a corpse must not be killed twice.
            // Prefer Sifu's own kill path, with the animation the host's lethal
            // hit selected. ApplyDamage reaches zero health but leaves the
            // choice of death animation to a path that never ran on this
            // machine, which is why the body stopped upright instead of falling
            // the way it was hit.
            const DWORD death_now = GetTickCount();
            const bool have_death_anim =
                entry.pending_death_anim && death_now - entry.pending_death_anim_ms <= 2000;
            // Always Sifu's own kill path, with the chosen animation when the
            // host managed to send one and without when it did not.
            //
            // The old shape only reached Kill if an animation had arrived, and
            // otherwise tried ApplyDamage -- which was skipped in turn whenever
            // health had already been synced to zero, so the common case ran
            // neither and the body was left standing while SetDown put it in a
            // knockdown pose. Five deaths in six went that way.
            if (dead && fighter.health) {
                ue::UObject* death_world = ue::GetWorld();
                ue::UObject* killer =
                    death_world ? ue::GetPlayerCharacter(death_world, 0) : nullptr;
                // DAMAGE first, Kill only as a fallback -- the opposite of
                // what this did, and the reason bodies stood up.
                //
                // On the host these same enemies fall correctly, and nothing
                // there forces anything: they simply take a lethal hit and Sifu
                // runs its whole death sequence off the damage. Forcing Kill
                // here announced the death and skipped that sequence, which is
                // why every measurement read kill=1 down=1 and the body was
                // still on its feet. Even OnRepSetIsDown could not rescue it,
                // because there was no death animation in flight for it to
                // present.
                //
                // Now that the joining machine simulates these enemies for real,
                // it can kill them the same way the host does.
                const float local_now = GetHealth(fighter);
                if (local_now > 0.5f) ApplyDamage(fighter, local_now + 1.f);

                bool killed = GetHealth(fighter) <= 0.5f;
                if (!killed) {
                    killed = KillWithAnimation(
                        fighter, killer, have_death_anim ? entry.pending_death_anim : nullptr);
                }
                // The measurement came back "kill=1 ... -> down=0" for every
                // death: Sifu's own Kill runs, returns success, and leaves the
                // body upright. So Kill decides that a character is dead; it is
                // the down-state machine that puts one on the floor, and taking
                // that call out of this path -- on the grounds that it produced
                // a knockdown pose rather than a death -- removed the only thing
                // that was laying anyone down. A knockdown pose is wrong; a
                // corpse standing up is worse.
                // Last resort only. If Sifu's own death sequence took the body
                // down, forcing the state on top of it interrupts the animation
                // that is already playing.
                if (!IsDown(fighter)) SetDown(fighter, true);
                // And then PLAY it. The flag was already being set correctly
                // -- every death measured down=1 -- and the body stood there
                // anyway, because the callback that turns that flag into a
                // fall is the one UE runs on a replication client, and there
                // is no replication here to run it.
                NotifyDownStateChanged(fighter, true);
                // Claimed, so a sequence that arrives after this edge is played
                // once by NoteEnemyDeathAnimation rather than twice here.
                if (have_death_anim) entry.death_anim_presented = true;
                SC_LOG("death: %s kill=%d anim=%d health_comp=%d -> down=%d", entry.name,
                       killed ? 1 : 0, have_death_anim ? 1 : 0, fighter.health ? 1 : 0,
                       IsDown(fighter) ? 1 : 0);
            }
            entry.pending_death_anim = nullptr;
            // Only for a REVIVAL now. Asserting the down state on a death used
            // to be the belt-and-braces that quietly became the only thing
            // running, and a forced knockdown is exactly the pose that reads as
            // "dead but still standing".
            if (!dead) {
                SetDown(fighter, false);
                NotifyDownStateChanged(fighter, false);
                // Give it its collision back, unconditionally.
                //
                // Sifu retires a dying body's collision, and our own bookkeeping
                // never noticed: entry.present was still true from before the
                // death, so the restore below -- which only fires when we think
                // presence is missing -- was skipped. The body came back, was
                // visible, was a valid target, and every attack went straight
                // through it. Asking for presence again costs two reflected
                // calls on an edge that happens once per revival.
                SetActorPresent(entry.actor, true);
                entry.present = true;
                entry.parked = false;
            }
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
        // A body running its own brain must not be dragged along the host's
        // transform stream frame by frame: the two fight each other and it
        // slides through its own attacks.
        //
        // But letting it drift without limit is the other complaint -- the two
        // machines end up disagreeing about where an enemy stands by metres. So
        // it is left alone while it stays near where the host has it, and pulled
        // back only when the gap becomes larger than a fight can explain. Far
        // enough apart and they are not describing the same enemy any more,
        // which is worse than a correction.
        if (local_ai && !dead && !knocked_down && config.sync_enemies) {
            ue::FVector here = {};
            if (ue::GetActorLocation(entry.actor, &here)) {
                const float dx = state.x - here.X;
                const float dy = state.y - here.Y;
                const float gap = sqrtf(dx * dx + dy * dy);
                constexpr float kTolerated = 400.f;  // roughly two strides
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

        // One line per active enemy, once a second. This function has now eaten
        // three fixes that each looked right in the file and changed nothing on
        // screen, so it gets an instrument rather than a fourth guess. Every
        // field here is already computed above.
        // Rate-limited PER ENEMY. A single shared timer was one static shared by
        // the whole loop, so exactly one enemy was ever printed -- the first to
        // pass the gate each second, which was a dead civilian for a whole
        // session while the four that mattered went unlogged. An instrument that
        // reports one row of a table is not an instrument.
        if (config.verbose_enemies && entry.active) {
            const DWORD esync_now = GetTickCount();
            if (esync_now - entry.last_esync_ms >= 1000) {
                entry.last_esync_ms = esync_now;
                // `tgt` is the one field that separates "the host never told us"
                // from "we were told and did not claim it". The host's own
                // census reports up to five enemies on the second player while
                // the client claims none, and only this says which half is
                // lying.
                const char* tgt = (state.flags & net::kEnemyTargetsPeer) ? "peer"
                                 : (state.flags & net::kEnemyTargetsHost) ? "host"
                                                                          : "none";
                // fac is here because the faction experiment produced no result
                // at all: the puppet was never moved out of the players' faction
                // because no enemy was ever found in a DIFFERENT one. Either
                // Sifu's enemies share faction 0 with the players -- in which
                // case faction is not what its AI discriminates on and that
                // whole approach is dead -- or GetFaction does not answer for
                // them. One number settles it.
                SC_LOG("esync: %s fac=%d hp=%.0f/%.0f applied=%.0f/%.0f caught_up=%d down=%d "
                       "hostdown=%d dead=%d owned=%d tgt=%s",
                       entry.name, GetFaction(entry.actor), GetHealth(fighter), state.health, state.damage_applied,
                       entry.reported_total,
                       state.damage_applied + 0.05f >= entry.reported_total ? 1 : 0,
                       IsDown(fighter) ? 1 : 0, knocked_down ? 1 : 0, dead ? 1 : 0,
                       entry.local_brain ? 1 : 0, tgt);
            }
        }

        // A STUCK down state is what stopped the driving, permanently.
        //
        // The gate below used to include IsDown(fighter), and one session's
        // numbers say what that cost: driven fell 3 -> 1 -> 0 while active
        // stayed at 6, and never recovered, with "DIED: 6, recycled alive: 0" in
        // the same log. Bodies go down under their own state machine here --
        // sync_enemy_vitals feeds the host's damage through Sifu's real
        // ApplyDamage, so this copy stages and falls by itself -- and nothing
        // ever brought one back up, because SetDown(false) only runs on a
        // host-driven dead->alive edge that never arrives for a body Sifu
        // floored on its own.
        //
        // Two changes. Repair the state when the host says this enemy is alive
        // and upright and the local copy disagrees, the same shape as the
        // existing stale-save revival. And keep driving a floored body's
        // position regardless: a knocked-down enemy still has somewhere it is
        // supposed to BE, and refusing to move it is what made one bad beat
        // permanent. Only the down/death state machine is left alone.
        if (!dead && !knocked_down && IsDown(fighter) && config.sync_enemies) {
            SetDown(fighter, false);
            entry.was_down = false;
            NotifyDownStateChanged(fighter, false);
            if (config.verbose_enemies) {
                SC_LOG("enemies: %s was floored locally but the host has it up -- restored",
                       entry.name);
            }
        }

        if (!dead && config.sync_enemies && !local_ai) {
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

    const std::int32_t player_index = InternalIndexOf(player);
    const std::int32_t second_index = InternalIndexOf(second);

    for (int i = 0; i < g_tracked_count; ++i) {
        const Tracked& entry = g_tracked[i];
        if (!entry.active || !entry.attack_component) continue;

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

    // Targeting is not permission. Sifu decides separately WHO MAY SWING, by
    // handing out combat roles per target, and only a DirectOpponent attacks.
    // This line is the difference between "they ignore my partner" and "they
    // engage my partner and are not allowed to hit him" -- two complaints that
    // look identical on screen and have nothing in common underneath.
    int roles_on_player[combat_role::kCount] = {};
    int roles_on_second[combat_role::kCount] = {};
    const std::int32_t player_idx = InternalIndexOf(player);
    const std::int32_t second_idx = InternalIndexOf(second);
    for (int i = 0; i < g_tracked_count; ++i) {
        const Tracked& entry = g_tracked[i];
        if (!entry.active || !entry.ai_fighting) continue;
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
    // The old world has already gone; never attempt reflected validity checks
    // or native unregister calls through its objects. Emptying the lookup table
    // also makes every asynchronous attack/death hook refuse stale actors.
    ClearDirectorRegistration();
    g_tracked_count = 0;
    g_tracked_world = nullptr;
    g_had_host_sweep = false;
    g_announce_empty_ownership = false;
    g_refresh_requested = true;
}

void NotifyPuppetWillBeDestroyed(ue::UObject* puppet) {
    // Despawn calls this before K2_DestroyActor/RemoveSecondPlayer, while the
    // exact registered body is still live. Any mismatch is a stale cache and
    // must only be forgotten.
    if (puppet && puppet == g_registered_partner &&
        g_registered_world == ue::GetWorld() && TrackingIsCurrent()) {
        ReleasePartnerFromDirector();
    } else {
        ClearDirectorRegistration();
    }
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
// Remember the death sequence the host's lethal hit chose, so the death edge
// can hand it straight back to Sifu's own kill path a moment later. The
// animation always arrives before the sweep that reports the body dead --
// UHealthComponent::Kill sends it, and the enemy is only published as dead on
// the following sweep.
// True when this body is thinking for itself on this machine. Its swings are
// its own, so the host's echoed attack for it must not be replayed on top --
// that is two attacks for one, out of step with each other and with the host.
bool EnemyRunsLocalBrain(std::uint32_t hash) {
    const int index = FindTracked(hash);
    return index >= 0 && g_tracked[index].local_brain;
}

bool EnemyActionsAreLocallyAuthoritative(std::uint32_t hash) {
    const int index = FindTracked(hash);
    if (index < 0) return false;

    const Tracked& entry = g_tracked[index];
    if (net::GetRole() != net::Role::Host) return entry.local_brain;

    net::OwnedEnemy owned = {};
    const std::uint32_t wire_hash = entry.wire_hash ? entry.wire_hash : entry.hash;
    return !net::GetOwnedEnemy(wire_hash, &owned);
}

void NoteEnemyDeathAnimation(std::uint32_t hash, ue::UObject* animation) {
    if (!animation) return;
    const int index = FindTracked(hash);
    if (index < 0) return;
    Tracked& entry = g_tracked[index];
    entry.pending_death_anim = animation;
    entry.pending_death_anim_ms = GetTickCount();

    // The sequence can arrive AFTER the body is already down.
    //
    // Measured on the joiner, 2026-08-12:
    //   04:28:12.790  death: ... kill=1 anim=0 health_comp=1 -> down=1
    //   04:28:13.998  death: exact enemy sequence received actor=844037D6
    // 1.2 s apart. Health reconciliation reaches zero from the enemy sweep,
    // while the death animation is a separate event on a separate path -- so
    // the death edge ran with nothing to play (`anim=0`) and the ledger was
    // filled a moment too late to be read.
    //
    // Claim it here instead of discarding it. The body is already in Sifu's
    // down state; this only layers the sequence the killing machine actually
    // chose over a fall that has already begun, which is the difference between
    // a corpse that dropped the way it was hit and a generic one.
    if (!entry.death_anim_presented && entry.was_down) {
        Fighter fighter = ResolveFighter(entry.actor);
        if (fighter.health && (GetHealth(fighter) <= 0.5f || IsDead(fighter))) {
            entry.death_anim_presented = true;
            if (ue::PlayAnimationAsset(entry.actor, animation, 0.f)) {
                SC_LOG("death: %s late sequence layered onto the corpse", entry.name);
            }
        }
    }
}

bool ApplyMirroredEnemyTargetForAttack(std::uint32_t hash) {
    if (!TrackingIsCurrent()) return false;
    const int index = FindTracked(hash);
    if (index < 0) return false;
    return ApplyMirroredTarget(g_tracked[index], g_tracked[index].host_target_flags);
}

bool PrepareMirroredEnemyAttack(std::uint32_t hash, EnemyAttackContext* out) {
    if (!out) return false;
    *out = {};
    if (!TrackingIsCurrent()) return false;
    const int index = FindTracked(hash);
    if (index < 0) return false;

    Tracked& entry = g_tracked[index];
    // An enemy OrderEvent received by the host can only originate from the
    // joiner's locally-owned fight. Its target on this screen is therefore the
    // remote puppet, even if the host brain's old target field went stale before
    // its ownership packet arrived.
    if (net::GetRole() == net::Role::Host) {
        entry.host_target_flags = net::kEnemyTargetsPeer;
    }
    out->actor = entry.actor;
    out->attack_component = entry.attack_component;
    out->ai_fighting = entry.ai_fighting;
    if (ApplyMirroredTarget(entry, entry.host_target_flags)) {
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
        g_announce_empty_ownership = false;
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
            SendOwnedEnemies();
        }
    }
}

}  // namespace sifucoop::game
