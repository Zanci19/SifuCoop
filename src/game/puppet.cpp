#include "puppet.h"

#include "../../third_party/minhook/include/MinHook.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../core/hooks.h"
#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../net/protocol.h"
#include "../net/session.h"
#include "../ue/reflection.h"
#include "../ui/overlay.h"
#include "actors.h"
#include "coop.h"
#include "enemies.h"
#include "orders.h"
#include "player2.h"

namespace sifucoop::game {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;
namespace net = sifucoop::net;
namespace ui = sifucoop::ui;
namespace coop = sifucoop::coop;

// UObjectBase::ClassPrivate. This is the one struct offset the mod takes on
// faith: the UObjectBase layout (vtable, ObjectFlags, InternalIndex,
// ClassPrivate, NamePrivate, OuterPrivate) is fixed for every non-editor UE4
// build, and there is no reflection call that yields an object's UClass without
// it. Everything else comes from the PDB or from Unreal's own property tables.
constexpr std::uintptr_t kClassPrivateOffset = 0x10;

using SpawnActorFn = ue::UObject*(__fastcall*)(void* world, void* uclass, const ue::FVector*,
                                               const ue::FRotator*, const void* params);

SpawnActorFn g_spawn_actor = nullptr;

// UPlayerAnim samples owner velocity here, before Sifu evaluates its locomotion graph.
using PlayerAnimUpdateFn = void(__fastcall*)(ue::UObject*, float);
using PlayerAnimSetSpeedStateFn = void(__fastcall*)(ue::UObject*, std::uint8_t);
using GetTargetableActorComponentFn = ue::UObject*(__fastcall*)(const ue::UObject*);
using RegisterTargetableActorFn = void(__fastcall*)(ue::UObject*);

PlayerAnimUpdateFn g_original_player_anim_update = nullptr;
PlayerAnimUpdateFn g_original_sc_anim_update = nullptr;
PlayerAnimSetSpeedStateFn g_set_player_anim_speed_state = nullptr;
GetTargetableActorComponentFn g_get_targetable_actor_component = nullptr;
RegisterTargetableActorFn g_register_targetable_actor = nullptr;
std::uintptr_t g_player_anim_update_target = 0;
std::uintptr_t g_sc_anim_update_target = 0;
ue::UObject* g_puppet_anim_instance = nullptr;
ue::FVector g_puppet_presentation_velocity = {};
bool g_have_puppet_presentation_velocity = false;

// Resolved on the game thread in DriveTo and consumed inside the animation
// update, which may not call reflection. Cleared whenever the puppet changes.
PresentationTargets g_puppet_presentation_targets;

// --- Driven-enemy locomotion, published where the graph can still see it -----
//
// Enemies were already given a presentation velocity and a speed band, but in
// DriveTo -- during UGameEngine::Tick. The character movement component ticks
// after that and applies braking friction to a body with no input, so by the
// time USCAnimInstance::NativeUpdateAnimation sampled the owner the velocity
// was back to zero and the graph played idle. That is the whole of "enemies
// chasing the host just slide" on the observing machine.
//
// The remote player had exactly this bug and it was fixed by moving the write
// to the last point before the graph samples. This is the same fix for enemies,
// with two differences that matter:
//
//   * m_vOwnerVelocity / m_fWantedSpeed / m_SpeedState are declared on
//     UPlayerAnim ONLY -- checked against the class property table. An enemy's
//     USCAnimInstance has no such fields, so writing those offsets there would
//     corrupt whatever it does keep. Only the class-agnostic movement and root
//     scene components are written.
//   * SpeedStateForSpeed keeps its hysteresis in function-level statics, which
//     one puppet can own but a room full of enemies cannot share. Each body
//     carries its own band and its own hold timer here.
//
// Resolution stays on the game thread: ResolvePresentationTargets calls
// K2_GetRootComponent, and reflection inside an animation update is what caused
// the Steam access violation. The hook only writes already-resolved pointers,
// guarded by the world they were resolved in.
struct EnemyPresentation {
    ue::UObject* anim_instance = nullptr;
    PresentationTargets targets;
    ue::UObject* world = nullptr;
    ue::FVector velocity = {};
    int band = 0;
    int candidate_band = 0;
    DWORD candidate_since = 0;
    DWORD until = 0;
};
constexpr int kEnemyPresentationSlots = 24;
EnemyPresentation g_enemy_presentation[kEnemyPresentationSlots];

// Sifu's own free-move bands out of Content/DB/Movement/BaseMovementDB. An AI
// anim instance exposes no thresholds to read, and the exact boundary matters
// far less than not being pinned at V0.
int EnemyBandForSpeed(float speed) {
    if (speed <= 20.f) return 0;
    if (speed < 240.f) return 1;
    if (speed < 475.f) return 2;
    return 3;
}

void ForgetEnemyPresentation() {
    for (EnemyPresentation& slot : g_enemy_presentation) slot = {};
}

// Called from DriveTo on the game thread, once per driven enemy per frame.
void NoteEnemyPresentation(ue::UObject* actor, const ue::FVector& velocity) {
    ue::UObject* anim_instance = ue::GetAnimInstance(actor);
    if (!anim_instance) return;

    const DWORD now = GetTickCount();
    EnemyPresentation* slot = nullptr;
    EnemyPresentation* free_slot = nullptr;
    for (EnemyPresentation& candidate : g_enemy_presentation) {
        if (candidate.anim_instance == anim_instance) {
            slot = &candidate;
            break;
        }
        if (!free_slot && (candidate.anim_instance == nullptr ||
                           static_cast<LONG>(now - candidate.until) >= 0)) {
            free_slot = &candidate;
        }
    }
    if (!slot) slot = free_slot;
    if (!slot) return;  // more driven bodies than slots: the rest keep the old path

    const float speed =
        sqrtf(velocity.X * velocity.X + velocity.Y * velocity.Y);
    const int wanted = EnemyBandForSpeed(speed);

    if (slot->anim_instance != anim_instance || slot->world != ue::GetWorld()) {
        *slot = {};
        slot->anim_instance = anim_instance;
        slot->targets = ResolvePresentationTargets(actor);
        slot->world = ue::GetWorld();
        slot->band = wanted;
        slot->candidate_band = wanted;
        slot->candidate_since = now;
    }
    slot->velocity = velocity;

    // Per-body hysteresis. A band recomputed from an instantaneous speed every
    // frame restarts the blend it just began, which is what "lifts a leg and
    // stops" looks like on the remote player -- enemies get the same guard.
    constexpr DWORD kBandSettleMs = 120;
    if (wanted != slot->candidate_band) {
        slot->candidate_band = wanted;
        slot->candidate_since = now;
    } else if (wanted != slot->band && now - slot->candidate_since >= kBandSettleMs) {
        slot->band = wanted;
    }
    slot->until = now + 400;
}
// The world those two component pointers were resolved in.
//
// They are raw pointers into components that a level change frees, and the
// animation hook runs on the ENGINE's schedule -- so between the world being
// torn down and TickPuppet noticing, the hook can fire and hand a freed
// movement component to SetSpeedState. That is a read through dead memory and
// it is the crash seen on a Steam restart: the faulting address was inside
// this dll for the first time. Comparing the world is a single pointer read
// through GWorld, which is cheap enough to do in the hook.
ue::UObject* g_puppet_targets_world = nullptr;

// Weak: the puppet can be destroyed by the game (level transition, respawn),
// so this is validated before use rather than trusted.
ue::UObject* g_puppet = nullptr;
ue::UObject* g_puppet_world = nullptr;
bool g_coop_started = false;
char g_announced_level[192] = {};
bool g_lobby_was_connected = false;

// Orders identify strikes before the animation becomes active. Keep this short
// window so TickPuppet samples the resulting montage on its next frames and
// sends it even when it is the same asset as the preceding strike.
DWORD g_cosmetic_attack_montage_until = 0;
DWORD g_puppet_cinematic_until = 0;
bool g_puppet_cinematic_needs_clear = false;

struct CosmeticCinematic {
    std::uint32_t actor_hash = 0;
    DWORD until = 0;
    ue::UObject* anim_instance = nullptr;
};
constexpr int kCosmeticCinematicCount = 16;
CosmeticCinematic g_enemy_cinematics[kCosmeticCinematicCount] = {};

bool g_arrival_teleport_pending = false;


// Follow mode: replay the local player's own movement onto the puppet on a
// delay. This is deliberately the same shape as the network path -- samples in,
// transforms out -- so it validates the drive mechanism before any sockets
// exist. If the puppet moves convincingly here, it will move convincingly from
// a peer's packets.
constexpr int kSampleCount = 256;  // ~4s at 60fps
constexpr int kFollowDelayFrames = 120;

struct Sample {
    ue::FVector location;
    ue::FRotator rotation;
    bool valid = false;
};

Sample g_samples[kSampleCount];
int g_sample_head = 0;
int g_samples_since_reset = 0;
bool g_follow_enabled = false;

// The pawn is destroyed and recreated on death/aging and level transitions.
// Replaying samples recorded by a previous pawn teleports the puppet to a
// stale position -- often in a different level entirely.
ue::UObject* g_last_player = nullptr;

ue::FVector g_last_local_location;
bool g_have_local_velocity = false;

// UPlayerAnim member offsets, all straight out of Unreal's generated property
// tables (tools/pdbdump/structdump.py UPlayerAnim). Named here rather than
// spelled inline so the reader can check them against that dump.
constexpr std::uintptr_t kAnimOwnerVelocity = 0x0FB4;        // m_vOwnerVelocity
constexpr std::uintptr_t kAnimOwnerVelocityLength = 0x0FC0;  // m_fOwnerVelocityLength
constexpr std::uintptr_t kAnimVelocityMaxV0 = 0x0FC4;        // m_fOwnerVelocityMaxForV0Anim
constexpr std::uintptr_t kAnimVelocityMaxV1 = 0x0FC8;        // ...V1Anim
constexpr std::uintptr_t kAnimVelocityMaxV2 = 0x0FCC;        // ...V2Anim
constexpr std::uintptr_t kAnimBlendspaceAngle = 0x0FD4;      // m_fBlendspaceAngle
constexpr std::uintptr_t kAnimWantedSpeed = 0x15E4;          // m_fWantedSpeed
constexpr std::uintptr_t kAnimLastActionAnim = 0x0E68;       // m_LastActionAnim
constexpr std::uintptr_t kAnimLastActionCursor = 0x0E74;     // m_fLastActionAnimCursor
constexpr std::uintptr_t kAnimMoveStatus = 0x1C29;           // m_MoveStatus
constexpr std::uintptr_t kAnimSpeedState = 0x1C2D;           // m_SpeedState
constexpr std::uintptr_t kAnimSpeedStateAlphaV0 = 0x1C44;    // m_fSpeedStateAlphaV0..V3

// USCAnimInstance offsets from the shipped PDB. The raw attack sequence is
// played in the Cinematic slot, but the slot contributes nothing unless these
// graph inputs select and blend it.
constexpr std::uintptr_t kAnimCinematicOverallWeight = 0x0378;
constexpr std::uintptr_t kAnimCinematicLayerCursor = 0x037C;

float ReadFloatAt(const std::uint8_t* bytes, std::uintptr_t offset) {
    float value = 0.f;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

void WriteCinematicWeight(std::uint8_t* bytes, float weight) {
    const float cinematic_layer = 0.f;
    std::memcpy(bytes + kAnimCinematicOverallWeight, &weight, sizeof(weight));
    std::memcpy(bytes + kAnimCinematicLayerCursor, &cinematic_layer, sizeof(cinematic_layer));
}

DWORD AnimationDeadline(ue::UObject* animation) {
    float seconds = ue::GetAnimationAssetLength(animation);
    if (seconds < 0.1f) seconds = 0.5f;
    if (seconds > 10.f) seconds = 10.f;
    return GetTickCount() + static_cast<DWORD>((seconds + 0.12f) * 1000.f);
}

void ArmEnemyCinematic(std::uint32_t actor_hash, DWORD until) {
    if (!actor_hash) return;
    const DWORD now = GetTickCount();
    for (CosmeticCinematic& active : g_enemy_cinematics) {
        if (active.actor_hash == actor_hash || !active.actor_hash || now >= active.until) {
            active = {actor_hash, until, nullptr};
            return;
        }
    }
    g_enemy_cinematics[0] = {actor_hash, until, nullptr};
}

void TickEnemyCinematics() {
    const DWORD now = GetTickCount();
    for (CosmeticCinematic& active : g_enemy_cinematics) {
        if (!active.actor_hash) continue;
        ue::UObject* actor = FindEnemyByHash(active.actor_hash);
        ue::UObject* anim_instance = actor ? ue::GetAnimInstance(actor) : nullptr;
        if (!anim_instance) {
            active = {};
            continue;
        }
        auto* bytes = reinterpret_cast<std::uint8_t*>(anim_instance);
        active.anim_instance = anim_instance;
        if (now < active.until) {
            WriteCinematicWeight(bytes, 1.f);
        } else {
            WriteCinematicWeight(bytes, 0.f);
            active = {};
        }
    }
}

// Which locomotion band a speed falls in, using Sifu's OWN thresholds.
//
// These were hardcoded as 18/280/600. Those numbers were measured off one
// character and the anim instance carries the real ones three floats after the
// velocity length, so read them: a build change or a different player state
// moves the boundaries, and picking a different band than the rest of the graph
// expects is exactly how you get a transition that starts and never finishes.
int RawSpeedStateForSpeed(const std::uint8_t* bytes, float speed) {
    float v0 = ReadFloatAt(bytes, kAnimVelocityMaxV0);
    float v1 = ReadFloatAt(bytes, kAnimVelocityMaxV1);
    float v2 = ReadFloatAt(bytes, kAnimVelocityMaxV2);
    if (!(v0 > 0.f && v1 > v0 && v2 > v1)) {
        // Sifu's own free-move speeds, read out of
        // Content/DB/Movement/BaseMovementDB: V1 100-130, V2 350, V3 600. The
        // old fallback was 18/280/600, measured off one character, which put
        // the V1/V2 boundary 70 units below where the game puts it.
        v0 = 20.f;
        v1 = 240.f;
        v2 = 475.f;
    }
    if (speed <= v0) return 0;
    if (speed < v1) return 1;
    if (speed < v2) return 2;
    return 3;
}

// The band, held steady enough for a blend to finish.
//
// BaseMovementDB gives these transitions real time: V0->V1 0.3 s, V1->V2 0.7 s,
// V0->V2 and V0->V3 a full second each. A band recomputed from an instantaneous
// speed every frame can change far faster than that, and each change restarts a
// blend from wherever the last one had reached -- so the character sits at the
// start of a transition indefinitely, which is what "lifts a leg and stops"
// looks like. Two guards: a band must be wanted continuously for a short while
// before it is adopted, and once adopted it is held for at least the blend it
// was given.
int SpeedStateForSpeed(const std::uint8_t* bytes, float speed) {
    static int held = 0;
    static int candidate = 0;
    static DWORD candidate_since = 0;
    static DWORD held_since = 0;

    const int raw = RawSpeedStateForSpeed(bytes, speed);
    const DWORD now = GetTickCount();
    if (held_since == 0) held_since = now;

    if (raw == held) {
        candidate = held;
        candidate_since = 0;
        return held;
    }
    if (raw != candidate) {
        candidate = raw;
        candidate_since = now;
        return held;
    }
    // Rising to a faster band is what the player sees first, so let it through
    // quickly; settling back down waits longer, because a momentary dip to zero
    // between two real samples is the exact failure this exists to absorb.
    const DWORD confirm_ms = raw > held ? 60u : 180u;
    const DWORD minimum_hold_ms = 150u;
    if (now - candidate_since < confirm_ms) return held;
    if (now - held_since < minimum_hold_ms) return held;

    held = candidate;
    held_since = now;
    candidate_since = 0;
    return held;
}

void __fastcall SCAnimUpdateHook(ue::UObject* anim_instance, float delta_seconds) {
    if (g_original_sc_anim_update) g_original_sc_anim_update(anim_instance, delta_seconds);

    // TickEnemyCinematics used to write this during UGameEngine::Tick. Enemy
    // NativeUpdateAnimation then reset the graph inputs before evaluation, so
    // PlaySlotAnimationAsDynamicMontage succeeded while the slot stayed at
    // zero weight. Restore it after Sifu's update, at the same pre-evaluation
    // boundary that fixed remote-player animations.
    const DWORD now = GetTickCount();
    for (CosmeticCinematic& active : g_enemy_cinematics) {
        if (active.anim_instance != anim_instance || now >= active.until) continue;
        WriteCinematicWeight(reinterpret_cast<std::uint8_t*>(anim_instance), 1.f);
        break;
    }

    // Re-publish this body's locomotion after Sifu's update and before the
    // graph evaluates, for the reason spelled out at EnemyPresentation. No
    // reflection here, and nothing written unless the pointers were resolved in
    // the world we are still in.
    for (EnemyPresentation& driven : g_enemy_presentation) {
        if (driven.anim_instance != anim_instance) continue;
        if (static_cast<LONG>(now - driven.until) >= 0) break;  // stale: leave it alone
        if (driven.world != ue::GetWorld()) break;
        WritePresentationVelocity(driven.targets, driven.velocity);
        SetMovementSpeedState(driven.targets.movement, driven.band);
        break;
    }
}

void __fastcall PlayerAnimUpdateHook(ue::UObject* anim_instance, float delta_seconds) {
    // Do not call reflection here: nested ProcessEvent in this traversal-
    // sensitive update caused the Steam AV. Everything below is either a memcpy
    // at a reflected offset or a native setter.
    const bool is_puppet_anim = anim_instance && anim_instance == g_puppet_anim_instance;
    const bool inject = is_puppet_anim && g_have_puppet_presentation_velocity;
    auto* bytes = reinterpret_cast<std::uint8_t*>(anim_instance);
    float speed = 0.f;
    if (inject) {
        speed = sqrtf(g_puppet_presentation_velocity.X *
                          g_puppet_presentation_velocity.X +
                      g_puppet_presentation_velocity.Y *
                          g_puppet_presentation_velocity.Y);

        // THE fix for "the remote player never plays a walk cycle".
        //
        // Presentation velocity was being published in DriveTo, during the game
        // tick. The character movement component then ticks, and a walking
        // character with no input has braking friction applied to it -- so by
        // the time the animation graph ran, the velocity it reads had already
        // been decelerated back to zero and the graph saw a standing character.
        // Everything after this point was compensation for that: force the
        // derived speed state, force the blend alphas, and hope the state
        // machine agrees. It half-worked, which is what "lifts a leg and stops"
        // looks like -- the start-step transition fires and then its own
        // conditions are false.
        //
        // NativeUpdateAnimation is the last point before the graph samples the
        // owner, so publish here instead. Sifu then computes velocity length,
        // speed band, blendspace angle, move status and every transition
        // condition itself, from consistent inputs, exactly as it does for a
        // real player.
        // Refuse to touch either component if the world has moved on since they
        // were resolved -- but never skip the engine's own update on the way
        // out. See g_puppet_targets_world.
        const bool targets_live = g_puppet_targets_world == ue::GetWorld();

        if (targets_live) {
            WritePresentationVelocity(g_puppet_presentation_targets,
                                      g_puppet_presentation_velocity);
        }

        // ...and the locomotion band, at ITS source.
        //
        // Injecting velocity was necessary but not sufficient: Sifu agreed about
        // the speed (the log read `ours 850, Sifu's 850`) and still played the
        // idle state, because the band is not derived from velocity by the
        // graph. It belongs to the movement component, and m_SpeedState below is
        // a copy NativeUpdateAnimation refreshes from there every frame. Writing
        // the copy was writing a mirror; this writes the thing being mirrored.
        if (targets_live) {
            SetMovementSpeedState(g_puppet_presentation_targets.movement,
                                  SpeedStateForSpeed(bytes, speed));
        }

        std::memcpy(bytes + kAnimOwnerVelocity, &g_puppet_presentation_velocity,
                    sizeof(g_puppet_presentation_velocity));
        std::memcpy(bytes + kAnimOwnerVelocityLength, &speed, sizeof(speed));
        std::memcpy(bytes + kAnimWantedSpeed, &speed, sizeof(speed));
    }
    if (g_original_player_anim_update) g_original_player_anim_update(anim_instance, delta_seconds);

    if (is_puppet_anim) {
        const DWORD now = GetTickCount();
        if (now < g_puppet_cinematic_until) {
            WriteCinematicWeight(bytes, 1.f);
            g_puppet_cinematic_needs_clear = true;
        } else if (g_puppet_cinematic_needs_clear) {
            WriteCinematicWeight(bytes, 0.f);
            g_puppet_cinematic_needs_clear = false;
        }
    }

    if (!inject) return;

    // Two separate questions now, and conflating them is what made the last
    // round look like no progress at all. Does Sifu agree about the SPEED, and
    // does the graph hold the right BAND? The first was already fixed by
    // publishing velocity here; the second is what left a character sprinting
    // at 850 units/s playing the idle state.
    const float native_speed = ReadFloatAt(bytes, kAnimOwnerVelocityLength);
    const float tolerance = speed * 0.25f > 8.f ? speed * 0.25f : 8.f;
    const bool native_agrees = fabsf(native_speed - speed) <= tolerance;

    const int wanted = SpeedStateForSpeed(bytes, speed);
    // FSpeedState is five bytes: a V0..V3 bitfield at +1 and ESpeedState at +4
    // (structdump.py FSpeedState). The enum is the one the graph switches on.
    const int graph_band = bytes[kAnimSpeedState + 4];
    const bool band_accepted = graph_band == wanted;

    static int last_mode = -1;
    const int mode = (native_agrees ? 1 : 0) | (band_accepted ? 2 : 0);
    if (mode != last_mode) {
        last_mode = mode;
        SC_LOG("puppet: locomotion speed %s, band %s (ours %.0f/V%d, Sifu's %.0f/V%d)",
               native_agrees ? "agreed" : "RECOMPUTED IDLE",
               band_accepted ? "held by the movement component"
                             : "REJECTED -- forcing the anim copy",
               speed, wanted, native_speed, graph_band);
    }

    // Periodic read-out of what the graph actually holds. This is the
    // instrument that was missing: every previous locomotion fix was judged by
    // watching the character, which cannot distinguish "wrong speed band" from
    // "right band, transition never completed".
    static DWORD last_dump = 0;
    const DWORD now = GetTickCount();
    if (speed > 18.f && now - last_dump >= 2000) {
        last_dump = now;
        const std::uint8_t* state_bytes = bytes + kAnimSpeedState;
        SC_LOG("puppet: anim speed=%.0f native=%.0f band=V%d state=%02X%02X%02X%02X%02X "
               "alphas %.2f/%.2f/%.2f/%.2f angle=%.0f movestatus=%02X%02X%02X%02X",
               speed, native_speed, wanted, state_bytes[0], state_bytes[1], state_bytes[2],
               state_bytes[3], state_bytes[4],
               ReadFloatAt(bytes, kAnimSpeedStateAlphaV0),
               ReadFloatAt(bytes, kAnimSpeedStateAlphaV0 + 4),
               ReadFloatAt(bytes, kAnimSpeedStateAlphaV0 + 8),
               ReadFloatAt(bytes, kAnimSpeedStateAlphaV0 + 12),
               ReadFloatAt(bytes, kAnimBlendspaceAngle), bytes[kAnimMoveStatus],
               bytes[kAnimMoveStatus + 1], bytes[kAnimMoveStatus + 2],
               bytes[kAnimMoveStatus + 3]);
    }

    // The movement component now owns this, so normally there is nothing to do.
    // Forcing the copy is kept only for the case where the band did not take --
    // a mirror write is better than idle, and it is what shipped before.
    if (band_accepted) return;

    const int state = wanted;

    if (g_set_player_anim_speed_state) {
        g_set_player_anim_speed_state(anim_instance, static_cast<std::uint8_t>(state));
    } else {
        std::uint8_t speed_state[5] = {};
        speed_state[1] = static_cast<std::uint8_t>(1u << state);  // m_bV0..m_bV3 bitfield
        speed_state[4] = static_cast<std::uint8_t>(state);
        std::memcpy(bytes + kAnimSpeedState, speed_state, sizeof(speed_state));
    }

    float speed_alphas[4] = {};
    speed_alphas[state] = 1.f;
    std::memcpy(bytes + kAnimSpeedStateAlphaV0, speed_alphas, sizeof(speed_alphas));

    // Transition-cache flags are one-shot graph results. Reasserting them each
    // frame restarted the start-step forever: the raised-leg freeze seen live.
    // Leave this cache entirely to Sifu's animation state machine.
}

void EnsurePlayerAnimHook() {
    static bool attempted = false;
    if (attempted || !IsOrderHookInstalled() || !g_player_anim_update_target) return;
    attempted = true;
    if (MH_CreateHook(reinterpret_cast<void*>(g_player_anim_update_target),
                      reinterpret_cast<void*>(&PlayerAnimUpdateHook),
                      reinterpret_cast<void**>(&g_original_player_anim_update)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(g_player_anim_update_target)) == MH_OK) {
        SC_LOG("puppet: UPlayerAnim pre-evaluation hook ACTIVE");
    } else {
        g_original_player_anim_update = nullptr;
        SC_LOG("puppet: UPlayerAnim pre-evaluation hook FAILED");
    }
}

void EnsureSCAnimHook() {
    static bool attempted = false;
    if (attempted || !IsOrderHookInstalled() || !g_sc_anim_update_target) return;
    attempted = true;
    if (MH_CreateHook(reinterpret_cast<void*>(g_sc_anim_update_target),
                      reinterpret_cast<void*>(&SCAnimUpdateHook),
                      reinterpret_cast<void**>(&g_original_sc_anim_update)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(g_sc_anim_update_target)) == MH_OK) {
        SC_LOG("enemy anim: USCAnimInstance pre-evaluation hook ACTIVE");
    } else {
        g_original_sc_anim_update = nullptr;
        SC_LOG("enemy anim: USCAnimInstance pre-evaluation hook FAILED");
    }
}

void ResetSamples() {
    for (Sample& sample : g_samples) sample.valid = false;
    g_sample_head = 0;
    g_samples_since_reset = 0;
}

void RecordSample(const ue::FVector& location, const ue::FRotator& rotation) {
    g_sample_head = (g_sample_head + 1) % kSampleCount;
    g_samples[g_sample_head] = {location, rotation, true};
    if (g_samples_since_reset < kSampleCount) ++g_samples_since_reset;
}

// Beyond this the character is so far out of position that walking would look
// worse than snapping (spawn, level change, a long stall).
constexpr float kSnapDistance = 600.f;
constexpr float kVerticalSnap = 250.f;


// Fraction of the remaining yaw error closed per frame. Low enough to smooth
// the peer's per-snapshot yaw wobble, high enough that a real turn resolves in
// a few frames. Applied along the shortest arc in DriveTo.
constexpr float kYawSmoothing = 0.25f;

// Fraction of remaining horizontal error closed by authoritative transform
// correction. Low enough to stay smooth (exponential decay, not a step), high
// enough that a replicated actor does not trail by hundreds of units before
// snapping.
constexpr float kPositionCorrection = 0.30f;

// Both smoothing factors above are written as "fraction closed in one frame at
// 60fps". Applied literally per frame they are not a rate at all but a rate per
// frame, so a 165fps machine closes the same error nearly three times faster in
// real time than a 60fps one. With the two players on exactly those refresh
// rates, each machine smooths the other's character differently -- and each
// sees the OTHER one as the jittery player, which is precisely what was
// reported.
//
// Converts one of those per-frame fractions into the equivalent for however
// long this frame actually lasted, so behaviour is identical at any frame rate.
float FrameRateAdjusted(float fraction_at_60) {
    const float dt = sifucoop::hooks::FrameDeltaSeconds();
    const float retained = powf(1.f - fraction_at_60, dt * 60.f);
    float alpha = 1.f - retained;
    if (alpha < 0.f) alpha = 0.f;
    if (alpha > 1.f) alpha = 1.f;
    return alpha;
}

// Position is sender-authoritative and corrected directly. Presentation
// velocity is restored after each transform so normal animation variables see
// locomotion. The player clone additionally gets the exact UPlayerAnim speed
// state in PlayerAnimUpdateHook.
void DriveTo(ue::UObject* target_actor, const ue::FVector& target,
             const ue::FRotator& rotation, const ue::FVector& reported_velocity, bool is_puppet) {
    // Never share a movement-failure latch between unrelated actors.

    ue::FVector current = {};
    if (!ue::GetActorLocation(target_actor, &current)) {
        TeleportActor(target_actor, target, rotation);
        return;
    }

    const float dx = target.X - current.X;
    const float dy = target.Y - current.Y;
    const float dz = target.Z - current.Z;
    const float distance = sqrtf(dx * dx + dy * dy);

    // How far the puppet is actually running behind, and how often it gives up
    // and teleports. Guessing at this from a description cost a whole test
    // round; the numbers say directly whether the drive is keeping up.
    //
    // Puppet only. Driven enemies come through this same function, and counting
    // them made the sample count exceed the frame rate several times over and
    // averaged the peer's character together with every enemy in the room --
    // which is not a number that means anything.
    static DWORD stats_since = 0;
    static double distance_sum = 0.0;
    static float distance_peak = 0.f;
    static int distance_samples = 0;
    static int snap_count = 0;
    static int correction_failed = 0;
    const DWORD stats_now = GetTickCount();
    if (stats_since == 0) stats_since = stats_now;
    if (is_puppet) {
    distance_sum += distance;
    if (distance > distance_peak) distance_peak = distance;
    ++distance_samples;
    if (stats_now - stats_since >= 5000) {
        // The fallback counters are process-wide (driven enemies use the same
        // path), so they are printed as a delta over this window.
        std::uint32_t fallbacks = 0;
        std::uint32_t hard = 0;
        GetTeleportFallbackCounts(&fallbacks, &hard);
        static std::uint32_t last_fallbacks = 0;
        static std::uint32_t last_hard = 0;
        SC_LOG("puppet: chase lag avg=%.0f peak=%.0f units over %d frames, %d snaps, "
               "%d corrections refused (%u swept aside, %u genuinely stuck)",
               distance_sum / (distance_samples > 0 ? distance_samples : 1), distance_peak,
               distance_samples, snap_count, correction_failed, fallbacks - last_fallbacks,
               hard - last_hard);
        last_fallbacks = fallbacks;
        last_hard = hard;
        stats_since = stats_now;
        distance_sum = 0.0;
        distance_peak = 0.f;
        distance_samples = 0;
        snap_count = 0;
        correction_failed = 0;
    }
    }

    // Facing is resolved first because the position correction below carries a
    // rotation with it, and both must use the same smoothed value. Interpolating
    // instead of snapping is what stops the peer's own yaw wobble arriving as a
    // twitch.
    ue::FRotator new_rotation = rotation;
    ue::FRotator current_rotation = {};
    if (ue::GetActorRotation(target_actor, &current_rotation)) {
        float delta = rotation.Yaw - current_rotation.Yaw;
        while (delta > 180.f) delta -= 360.f;
        while (delta < -180.f) delta += 360.f;
        new_rotation.Yaw = current_rotation.Yaw + delta * FrameRateAdjusted(kYawSmoothing);
    }

    // Too far to walk back convincingly, or a floor change: snap.
    if (distance > kSnapDistance || fabsf(dz) > kVerticalSnap) {
        if (is_puppet) ++snap_count;
        TeleportActor(target_actor, target, new_rotation);
        return;
    }

    // Sifu's spawned clone accepts AddMovementInput, but its player animation
    // graph never consumes that synthetic controller input. Worse, its movement
    // component integrates every late correction as physical acceleration. At
    // normal speed it continually overshoots and reverses; in Focus time the
    // target moves slowly enough to hide that loop. Drive the already
    // interpolated network transform directly instead. This keeps the capsule
    // grounded through K2_TeleportTo while avoiding movement-component chase
    // oscillation. Animation velocity is handled separately once the exact
    // player AnimBP input is identified.
    if (is_puppet) {
        const float alpha = FrameRateAdjusted(kPositionCorrection);
        ue::FVector corrected = {current.X + dx * alpha, current.Y + dy * alpha,
                                 current.Z + dz * alpha};
        // Animation follows the sender's measured velocity, not the correction
        // step used to close positional error. The latter spikes on arrival and
        // falls toward zero as the bodies converge even while the peer keeps
        // running; the log captured a false 3162-unit sprint from that bug.
        ue::FVector presentation_velocity = {reported_velocity.X, reported_velocity.Y, 0.f};
        float presentation_speed = sqrtf(presentation_velocity.X * presentation_velocity.X +
                                         presentation_velocity.Y * presentation_velocity.Y);
        constexpr float kMaximumPresentationSpeed = 850.f;
        if (presentation_speed > kMaximumPresentationSpeed) {
            const float scale = kMaximumPresentationSpeed / presentation_speed;
            presentation_velocity.X *= scale;
            presentation_velocity.Y *= scale;
        } else if (presentation_speed <= 18.f && distance > 35.f) {
            // One useful fallback during the first sample after a stall: show
            // the visible catch-up rather than sliding, but keep it bounded.
            const float frame_seconds = sifucoop::hooks::FrameDeltaSeconds();
            if (frame_seconds > 0.001f) {
                presentation_velocity = {(corrected.X - current.X) / frame_seconds,
                                         (corrected.Y - current.Y) / frame_seconds, 0.f};
                presentation_speed = sqrtf(presentation_velocity.X * presentation_velocity.X +
                                           presentation_velocity.Y * presentation_velocity.Y);
                if (presentation_speed > kMaximumPresentationSpeed) {
                    const float scale = kMaximumPresentationSpeed / presentation_speed;
                    presentation_velocity.X *= scale;
                    presentation_velocity.Y *= scale;
                }
            }
        }
        g_puppet_presentation_velocity = presentation_velocity;
        g_have_puppet_presentation_velocity = true;
        g_puppet_anim_instance = ue::GetAnimInstance(target_actor);
        // Re-resolved whenever the driven body changes. Both components are
        // rebuilt on respawn and travel, so this is never held across one.
        static ue::UObject* resolved_for = nullptr;
        if (resolved_for != target_actor) {
            resolved_for = target_actor;
            g_puppet_presentation_targets = ResolvePresentationTargets(target_actor);
            g_puppet_targets_world = ue::GetWorld();
        }
        if (!TeleportActor(target_actor, corrected, new_rotation)) ++correction_failed;
        WritePresentationVelocity(g_puppet_presentation_targets, presentation_velocity);
        return;
    }
    // Client enemy brains are deliberately stopped, so RequestDirectMove has
    // no consumer. The old code counted a dispatched request as "driven" even
    // when the actor never moved. Apply the host transform directly, then
    // restore root/component velocity after teleport so the AnimBP sees motion.
    {
        const float alpha = FrameRateAdjusted(kPositionCorrection);
        const ue::FVector corrected = {current.X + dx * alpha, current.Y + dy * alpha,
                                       current.Z + dz * alpha};
        ue::FVector presentation_velocity = {reported_velocity.X, reported_velocity.Y, 0.f};
        constexpr float kMaximumEnemyPresentationSpeed = 1200.f;
        const float speed = sqrtf(presentation_velocity.X * presentation_velocity.X +
                                  presentation_velocity.Y * presentation_velocity.Y);
        if (speed > kMaximumEnemyPresentationSpeed) {
            const float scale = kMaximumEnemyPresentationSpeed / speed;
            presentation_velocity.X *= scale;
            presentation_velocity.Y *= scale;
        }
        TeleportActor(target_actor, corrected, new_rotation);
        SetPresentationVelocity(target_actor, presentation_velocity);
        // ...and again from inside the animation update, which is the write
        // that actually survives to the graph. This one only makes the body
        // land correctly during the tick itself.
        NoteEnemyPresentation(target_actor, presentation_velocity);

        // Driven enemies need the locomotion band for exactly the same reason
        // the puppet does -- a body with no input computes V0 and slides -- but
        // they are not player characters, so they never reach the UPlayerAnim
        // hook where the puppet's is set. Set it here instead. An enemy gliding
        // around the room without a walk cycle is a large part of what reads as
        // "the enemies are not synced".
        //
        // No thresholds are available from an AI anim instance, so the player's
        // measured bands are used; the exact boundary matters far less than not
        // being pinned at idle.
        int band = 0;
        if (speed > 18.f) band = speed < 280.f ? 1 : (speed < 600.f ? 2 : 3);
        SetActorSpeedState(target_actor, band);
        return;
    }

}

// Local rehearsal of the network path: replay our own movement on a delay.
void DrivePuppetFromSamples(ue::UObject* puppet) {
    // Wait until the delay window is full of samples from the *current* pawn,
    // otherwise the first frames replay whatever the buffer happened to hold.
    if (g_samples_since_reset <= kFollowDelayFrames) return;

    int index = g_sample_head - kFollowDelayFrames;
    while (index < 0) index += kSampleCount;
    const Sample& sample = g_samples[index];
    if (!sample.valid) return;

    DriveTo(puppet, sample.location, sample.rotation, ue::FVector{}, true);
}

void* GetObjectClass(ue::UObject* object) {
    if (!object) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(object) +
                                     kClassPrivateOffset);
}

// A spawned clone of the player class runs *all* the player's logic, including
// the death sequence -- which is why killing it played the player death music
// and drove the player HUD.
//
// Invincibility is not a workaround, it is the correct end state: a puppet
// represents a player on another machine, and that machine is the only thing
// allowed to decide whether its player died. Locally the puppet must never
// resolve its own damage; it replays the peer's result instead. It still shows
// the peer's real health, so enemies attacking it are not attacking a target
// that visibly never suffers.
//
// Faction is what turns this from sparring into co-op. Sifu's AI picks targets
// by faction, so putting the puppet in the *same* faction as the local player
// makes the level's enemies treat both players as enemies and makes the players
// unable to hurt each other. The old behaviour -- opposite factions -- is what
// a versus match wants, and is kept for exactly that.
// A spawned player blueprint can inherit hidden state from a local-only setup path.
// The remote clone is never intentionally hidden while connected, so explicitly
// restore actor visibility after spawn and occasionally while it is driven.
bool EnsureRemoteVisible(ue::UObject* puppet, bool log_result) {
    if (!puppet) return false;
    struct HiddenParams {
        std::uint8_t bNewHidden[8];
    } show = {};
    const bool ok = ue::CallFunction(puppet, L"SetActorHiddenInGame", &show);
    if (log_result) SC_LOG("puppet: visibility restore %s", ok ? "requested" : "FAILED");
    return ok;
}

// The bystander-faction experiment is over: faction is not what Sifu's AI
// discriminates on.
//
// The idea was that putting the puppet in the enemies' faction would make them
// ignore it -- the same mechanism that stops one grunt punching another. It
// never fired, and the reason was invisible until `esync` was made to print
// `fac=`: every enemy reads faction 0, and so does the player. There is no
// "enemies' faction" to move the puppet into, because both sides are already in
// the same one. The retry loop that lived here searched every active enemy for
// a faction different from the player's, found none, and did nothing, forever.
//
// Removed rather than left inert, because a dead code path that looks like a
// working feature is how this question stayed open for three sessions. See
// HANDOFF -- the finding is recorded there so it is not tried a fourth time.

void ConfigureAsRemote(ue::UObject* puppet, ue::UObject* player) {
    const int player_faction = GetFaction(player);
    const bool coop_mode = coop::Get().mode == coop::Mode::Coop;

    if (player_faction >= 0) {
        // Faction 0 and 1 are the two sides; picking "the other one" rather
        // than a constant means this still works if the player's faction ever
        // differs by level. In co-op both players share the level's faction --
        // which, measurably, the enemies share too, so this decides nothing
        // about who they attack. It is kept because Versus does rely on it.
        const int target = coop_mode ? player_faction : (player_faction == 1 ? 0 : 1);
        SetFaction(puppet, target);
        SC_LOG("puppet: faction %d (you are %d) -- %s", target, player_faction,
               coop_mode ? "CO-OP, allied against the level" : "VERSUS, hostile to you");
    } else {
        SC_LOG("puppet: could not read your faction -- leaving the puppet's alone");
    }

    // The puppet needs normal WORLD collision. Turning collision off also turns
    // off the floor: gravity then drops the capsule until the vertical snap
    // guard teleports it back up, producing the visible floor-loop at frame rate.
    // Friendly faction/invincibility handle player combat; body overlap is the
    // acceptable trade-off for a grounded, stable remote character.
    struct CollisionParams {
        std::uint8_t bNewActorEnableCollision[8];
    } collision = {};
    collision.bNewActorEnableCollision[0] = 1;
    const bool collision_on =
        ue::CallFunction(puppet, L"SetActorEnableCollision", &collision);
    SC_LOG("puppet: world collision %s", collision_on ? "enabled" : "FAILED");

    // World collision keeps the floor solid. Pawn collision must be ignored:
    // two spawned character capsules otherwise block/push each other, freezing
    // Player 0 and producing the peer's small circular jitter.
    // ACharacter::GetCapsuleComponent is a native inline getter, not a
    // reflected UFunction. The old CallFunction attempt therefore always
    // failed, leaving the two player capsules blocking each other. Resolve the
    // engine CapsuleComponent class and use AActor::GetComponentByClass instead
    // (the same reflected route used by GetAnimInstance).
    struct ComponentParams {
        void* ComponentClass;
        ue::UObject* ReturnValue;
    } capsule_result = {};
    bool pawn_ignore = false;
    const bool want_pawn_ignore = coop::Get().puppet_ignores_pawn_collision;
    void* capsule_class = ue::FindObjectByPath(L"/Script/Engine.CapsuleComponent");
    capsule_result.ComponentClass = capsule_class;
    if (want_pawn_ignore && capsule_class &&
        ue::CallFunction(puppet, L"GetComponentByClass", &capsule_result) &&
        capsule_result.ReturnValue) {
        struct CollisionResponseParams {
            std::uint8_t Channel;
            std::uint8_t NewResponse;
        } response = {};
        response.Channel = 2;      // ECC_Pawn
        response.NewResponse = 0;  // ECR_Ignore
        pawn_ignore = ue::CallFunction(capsule_result.ReturnValue,
                                       L"SetCollisionResponseToChannel", &response);
    }
    SC_LOG("puppet: pawn collision %s%s",
           !want_pawn_ignore ? "left BLOCKING (puppet_ignores_pawn_collision=0)"
                             : (pawn_ignore ? "ignored" : "FAILED"),
           capsule_class || !want_pawn_ignore ? "" : " (CapsuleComponent class unavailable)");
    // Invincibility is normally correct -- only the machine that owns a player
    // decides whether they died -- but it is a switch now because an enemy may
    // decline to attack a target it cannot damage. See coop.h.
    const bool invincible = coop::Get().puppet_invincible;
    SetInvincible(puppet, invincible);
    if (!invincible) {
        SC_LOG("puppet: invincibility OFF (puppet_invincible=0) -- their own game still "
               "decides their health; this is only to see whether enemies will commit");
    }
    EnsureRemoteVisible(puppet, true);

    // UCharacterMovementComponent::TickComponent bails out early when the pawn
    // has no controller, so an unpossessed puppet never moves under its own
    // power and never accumulates velocity -- which is why it slid around in an
    // idle pose. Giving it a controller makes the movement component tick, so
    // AddMovementInput produces real motion and real locomotion animation.
    struct Empty {
    } none = {};
    if (ue::CallFunction(puppet, L"SpawnDefaultController", &none)) {
        SC_LOG("puppet: controller spawned (movement component will now tick)");
    } else {
        SC_LOG("puppet: SpawnDefaultController unavailable -- expect sliding");
    }

    // The controller we just handed it may carry Sifu's own AI. If it does, the
    // puppet runs a behaviour tree -- walking, turning, picking targets -- while
    // DriveTo is pushing it toward the peer's position at the same time. The two
    // fight every frame, and what that looks like is a character darting around
    // rather than following. The joining side already stops enemy brains for
    // exactly this reason; the puppet needs it too.
    //
    // Worth logging either way: StopBrain only reports true when it actually
    // found a brain to stop, so this line answers whether the puppet had AI.
    if (StopBrain(puppet)) {
        SC_LOG("puppet: AI brain STOPPED -- it had one, and it was fighting the drive");
    } else {
        SC_LOG("puppet: no AI brain found (the drive is the only thing moving it)");
    }
}

// --- Friendly-fire fix via Sifu's own relationship system (EXPERIMENTAL) ------
//
// Sifu stores relationships per actor in USocialComponent::m_Relationships, a
// TMap<AActor*, ERelationshipTypes>. Its melee hit-detection is believed to
// consult this (it is why two enemies of one faction never damage each other).
// If so, marking the puppet<->local-player relationship "friendly" makes the
// puppet's replayed swings pass through the local player the way one grunt's
// swing passes through another -- a per-instigator exemption the mod otherwise
// has no API for. All calls go through reflection (ProcessEvent), the safest
// layer; nothing here dereferences an unknown field.
//
// The friendly enum value is not hardcoded. It is discovered at runtime by
// reading the relationship between two live enemies (same faction => the
// friendly value), which is exact and survives any enum-ordering change. Until
// two enemies are in the scene to sample, application is simply deferred.

// The friendly enum value is not hardcoded, and it is not assumed to stick.
// Candidates are tried in turn and each is read back, so the value that
// survives is chosen by the game rather than by us.
namespace rel = sifucoop::game::relationship;

// Learn a friendly value from two distinct active enemies. Enemies share the
// level's hostile faction, so their relationship to each other is a value the
// game itself considers non-hostile. Returns kUnknown until two can be sampled.
int DiscoverFriendlyRelationValue() {
    EnemyRow rows[64];
    const int count = GetEnemyRows(rows, 64);
    ue::UObject* first = nullptr;
    for (int i = 0; i < count; ++i) {
        if (!rows[i].active || rows[i].down) continue;
        ue::UObject* actor = FindEnemyByHash(rows[i].hash);
        if (!actor) continue;
        if (!first) {
            first = actor;
            continue;
        }
        const int value = ReadRelationship(first, actor);
        if (value != rel::kUnknown) return value;
    }
    return rel::kUnknown;
}

ue::UObject* g_friendly_applied_for = nullptr;
bool g_friendly_verified = false;
int g_relationship_that_stuck = rel::kUnknown;
DWORD g_next_relationship_attempt = 0;
bool g_relationship_writes_land = false;

// Set both directions and report whether the game actually kept it.
bool TrySetRelationshipBothWays(ue::UObject* player, ue::UObject* puppet, int value,
                                int* out_player, int* out_puppet) {
    ue::UObject* player_social = GetSocialComponent(player);
    ue::UObject* puppet_social = GetSocialComponent(puppet);

    // Evidence, gathered once: if a write neither changes the readback nor
    // grows the relationship map, BPF_ServerChangeRelationship is a no-op on
    // this build and every retry below is wasted. Saying so in the log is worth
    // more than another silent failure -- this exact write has been reported as
    // succeeding, and read back as Neutral, in every session so far.
    // Sampled BEFORE the write, because a readback that equals what we asked
    // for is only evidence if it differed beforehand. The enemy-side user of
    // this same primitive reports success without this step, writing Fight and
    // reading back Fight on bodies that were plausibly Fight already -- so the
    // two halves of this mod reached opposite verdicts about one function.
    const int was_actor = ReadRelationship(player, puppet);
    const int was_comp = ReadRelationshipViaComponent(player, puppet);

    const int before = RelationshipMapSize(player_social);
    WriteRelationship(player_social, puppet, value);
    WriteRelationship(puppet_social, player, value);
    const int after = RelationshipMapSize(player_social);

    const int back_player = ReadRelationship(player, puppet);
    const int back_puppet = ReadRelationship(puppet, player);
    // The component owns the map the write goes into. When it disagrees with
    // the actor's virtual, IT is the one that saw the write.
    const int back_comp = ReadRelationshipViaComponent(player, puppet);
    if (out_player) *out_player = back_player;
    if (out_puppet) *out_puppet = back_puppet;

    const bool held = back_player == value && back_puppet == value;
    const bool held_component = back_comp == value;
    const bool map_grew = RelationshipMapProbeTrusted() && before >= 0 && after > before;
    if (held || held_component || map_grew) g_relationship_writes_land = true;

    // One line per distinct outcome rather than one line ever: the whole point
    // is to see the two getters disagree, and that only shows up as a pattern.
    static int last_shape = -1;
    const int shape = (was_actor + 1) * 100000 + (was_comp + 1) * 10000 +
                      (back_player + 1) * 1000 + (back_puppet + 1) * 100 +
                      (back_comp + 1) * 10 + (map_grew ? 1 : 0);
    if (shape != last_shape) {
        last_shape = shape;
        SC_LOG("relationship: player->puppet asked %d %s | actor %d -> %d | component %d -> %d "
               "| map %d -> %d (%s)",
               value, rel::Name(value), was_actor, back_player, was_comp, back_comp, before,
               after,
               RelationshipMapProbeTrusted() ? "probe trusted"
                                             : "probe UNVERIFIED -- count means nothing yet");
    }
    return held || held_component;
}

// Establish -- and KEEP -- a non-hostile relationship between the two players.
//
// Three things follow from a live session in which the setter reported success
// and the readback returned Neutral. Coop is tried FIRST, because Sifu ships a
// relationship type by that name and it is plainly the one this mod means.
// Every candidate is verified, so the value that survives is the game's choice.
// And it is RE-ASSERTED on a timer rather than applied once: something in the
// game recomputes these (ABaseCharacter::UpdateRelationshipToOtherCharacters
// exists), and a value that holds for one frame is worth nothing to a swing
// thrown ten seconds later. The old code set an "already applied" flag on its
// single attempt whether or not it worked, so it never tried again at all.
void MaintainFriendlyRelationship(ue::UObject* player, ue::UObject* puppet) {
    const coop::Config& config = coop::Get();
    // remote_player_attacks depends on this, so wanting remote attacks is
    // itself a reason to establish the exemption.
    if (!config.friendly_relationship && !config.remote_player_attacks) return;
    if (config.mode != coop::Mode::Coop) return;
    if (!player || !puppet) return;

    // A level restart tears both bodies down while this keeps asserting against
    // them, and Sifu's relationship multicast walks whatever actor it is given.
    // Forget everything learned about a puppet from a previous world rather
    // than carrying a stale verdict into the new one.
    static ue::UObject* applied_world = nullptr;
    ue::UObject* world = ue::GetWorld();
    if (applied_world != world) {
        applied_world = world;
        g_friendly_applied_for = nullptr;
        g_friendly_verified = false;
        g_relationship_that_stuck = rel::kUnknown;
        g_next_relationship_attempt = 0;
        return;
    }

    if (g_friendly_applied_for != puppet) {
        g_friendly_applied_for = puppet;
        g_friendly_verified = false;
        g_relationship_that_stuck = rel::kUnknown;
        g_next_relationship_attempt = 0;
    }

    const DWORD now = GetTickCount();
    if (g_next_relationship_attempt != 0 && now < g_next_relationship_attempt) return;
    // Confirmed pairs are re-checked lazily; unconfirmed ones retry briskly,
    // because until one sticks the remote player cannot swing at all.
    g_next_relationship_attempt = now + (g_friendly_verified ? 3000 : 1000);

    int back_player = rel::kUnknown;
    int back_puppet = rel::kUnknown;

    // Already found one the game keeps: just hold it there.
    if (g_relationship_that_stuck != rel::kUnknown) {
        const bool still = TrySetRelationshipBothWays(player, puppet, g_relationship_that_stuck,
                                                      &back_player, &back_puppet);
        if (still != g_friendly_verified) {
            g_friendly_verified = still;
            SC_LOG("puppet: relationship %s %s (readback %d/%d)",
                   rel::Name(g_relationship_that_stuck),
                   still ? "re-confirmed" : "STOPPED HOLDING -- searching again", back_player,
                   back_puppet);
        }
        if (!still) g_relationship_that_stuck = rel::kUnknown;
        return;
    }

    const int candidates[] = {rel::kCoop, rel::kAlly, DiscoverFriendlyRelationValue()};
    for (const int value : candidates) {
        if (value == rel::kUnknown || value < 0 || value >= rel::kCount) continue;
        if (!TrySetRelationshipBothWays(player, puppet, value, &back_player, &back_puppet)) {
            continue;
        }
        g_relationship_that_stuck = value;
        g_friendly_verified = true;
        SC_LOG("puppet: relationship %s STUCK -- readback %d/%d, remote attacks may play",
               rel::Name(value), back_player, back_puppet);
        return;
    }

    // Nothing held. Say so once rather than every second, and say what was
    // actually read, because that number is the whole diagnosis.
    //
    // Two claims were removed from this message because neither survived being
    // checked. It said the setter was "a no-op on this build":
    // USocialComponent::SetRelationship is a real unfolded function (one symbol
    // at its RVA) and is bound in both build tables, so the write is a direct
    // native call that lands somewhere -- what is in doubt is the READER, not
    // the writer. And it said remote attacks stay off: they do not, orders.cpp
    // gates those on the config flag alone. What actually degrades is friendly
    // fire between the two players, so that is what the diagnostic records.
    static int last_reported = -2;
    if (last_reported != back_player) {
        last_reported = back_player;
        SC_LOG("puppet: no relationship value read back on either getter (last %d/%d) -- "
               "the two players can hurt each other. "
               "0=Enemy 1=Fight 2=Object 3=Neutral 4=Coop 5=Ally",
               back_player, back_puppet);
    }
}

bool g_peer_was_down = false;

// True only when the puppet was created by a peer connecting, so a manual F9
// spawn is never removed by the connection logic.
bool g_puppet_auto_spawned = false;

void ApplyPeerVitals(ue::UObject* puppet, const net::PeerVitals& vitals) {
    if (!puppet) return;
    ue::UObject* world = ue::GetWorld();
    ue::UObject* local_player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (local_player && puppet == local_player) {
        static bool warned = false;
        if (!warned) SC_LOG("puppet: refused remote vitals on the local player");
        warned = true;
        return;
    }
    // Belt and braces on the same rule, from the other direction. Sifu's game
    // mode has been observed handing the second player the FIRST player's
    // character, and it does that on respawn and travel as well as at creation.
    // GetPlayerCharacter(0) can lag that by a frame, so also refuse any body
    // that controller 0 is currently possessing -- writing the peer's health
    // into your own character is precisely how their life bar ends up looking
    // like yours.
    if (puppet == PrimaryPlayerPawn()) {
        static bool warned_primary = false;
        if (!warned_primary) {
            warned_primary = true;
            SC_LOG("puppet: refused remote vitals -- that body belongs to controller 0");
        }
        return;
    }
    Fighter fighter = ResolveFighter(puppet);

    if (coop::Get().mirror_peer_vitals && vitals.max_health > 0.f) {
        SetHealth(fighter, vitals.health);
        SetGuard(fighter, vitals.guard);

        // Max health is on the wire and cannot be written: Sifu exposes
        // BPF_GetMaxHealth on both health components and no setter anywhere.
        // It does not need one -- max health is DERIVED from age, and the
        // puppet's age is now the peer's. So this one comparison answers a
        // question no other line can: whether writing the stats component
        // actually propagates to the stats that follow from it.
        //
        // Equal means the age write took and Sifu recomputed. Still differing
        // means BPF_SetCharacterAge stored a number nothing downstream reads,
        // and the partner's body will keep showing your health bar and your
        // face regardless of what the age says.
        const float puppet_max = GetMaxHealth(fighter);
        static float last_reported_gap = -1.f;
        const float gap = puppet_max - vitals.max_health;
        const float magnitude = gap < 0.f ? -gap : gap;
        if (last_reported_gap < 0.f || (magnitude - last_reported_gap > 1.f) ||
            (last_reported_gap - magnitude > 1.f)) {
            last_reported_gap = magnitude;
            SC_LOG("puppet: max health here %.0f, theirs %.0f -- %s", puppet_max,
                   vitals.max_health,
                   magnitude <= 1.f
                       ? "agreed, so the age write reached the stats derived from it"
                       : "still YOURS, so age alone does not drive it and their bar is wrong");
        }
    }

    if (vitals.is_down == g_peer_was_down) return;  // only on transitions
    g_peer_was_down = vitals.is_down;

    // SetIsDown alone only flipped a flag -- it fired correctly on every
    // transition but the puppet stayed standing. InternalSetDownState is what
    // drives the visible state machine; SetDown does both.
    SetDown(fighter, vitals.is_down);
    SC_LOG("puppet: peer %s", vitals.is_down ? "went DOWN" : "got back up");
}

// Just the leaf name, for display: the full package path is far too long for a
// status line.
const char* LevelLeaf(const char* path) {
    if (!path || !path[0]) return "?";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Sifu's hideout level picker is an intermediate UI world, not a playable
// level. Sending it to a peer opens a black, menu-only screen with a Continue
// prompt. Keep the peer in its current safe world until the host commits to a
// real destination, which will then be announced normally.
bool IsTransientSelectionLevel(const char* path) {
    if (!path || !path[0]) return true;
    static const char* const kNonGameplay[] = {"SelectHideoutLevel", "MainMenu", "Frontend",
                                               "Startup", "EntryLevel", "Hideout_0_Main"};
    for (const char* fragment : kNonGameplay) {
        if (strstr(path, fragment) != nullptr) return true;
    }
    return false;
}

// Creating a second local player is observable game input. It may happen only
// after the host explicitly started co-op and both machines report the same
// settled Story world. This prevents the host pawn theft/startup-text bug and
// prevents the joiner creating controller 1 in Hideout 0 while OpenLevel is
// still pending.
bool CoopBodiesMayExist() {
    if (!g_coop_started || !net::IsConnected()) return false;
    char level[192] = {};
    if (!ue::GetCurrentLevelPath(level, sizeof(level)) || IsTransientSelectionLevel(level)) {
        return false;
    }
    const char* peer_level = net::GetPeerLevel();
    return peer_level && peer_level[0] && _stricmp(level, peer_level) == 0;
}

// The host's most recent standing invite, held until the joining player takes
// it. Kept as a level path rather than a flag so it survives the retries the
// host sends and still names a destination when accepted seconds later.
char g_invite_level[192] = {};
bool g_have_invite = false;

// Saying no is an answer, not silence. Without this the host sees an invite
// that is simply never taken and cannot tell refusal from a peer stuck in a
// menu -- so the offer is cleared here AND reported back over the wire.
void DeclineInvite() {
    const std::uint32_t id = net::GetPendingInviteId();
    if (id == 0) return;
    net::SendInviteReply(id, false);
    net::ClearPendingInvite();
    g_have_invite = false;
    g_invite_level[0] = '\0';
    SC_LOG("lobby: invitation declined -- the host has been told");
}

void AcceptInvite() {
    if (!g_have_invite || !g_invite_level[0]) {
        SC_LOG("lobby: no invite to accept");
        coop::ReportProblem("no invite pending");
        return;
    }
    // Answer before clearing: the id lives with the pending offer, and
    // OpenLevel below tears this world down mid-call.
    const std::uint32_t invite_id = net::GetPendingInviteId();
    if (invite_id != 0) net::SendInviteReply(invite_id, true);
    g_have_invite = false;
    g_coop_started = true;
    // A level package alone is not enough: Sifu restores the joiner's saved
    // checkpoint inside that package. Arriving at the host's live transform
    // makes a previously-cleared local save join the host's current run.
    g_arrival_teleport_pending = true;
    SC_LOG("lobby: accepted -- travelling to '%s'", g_invite_level);
    ue::OpenLevel(g_invite_level);
}

// Put the local player where the peer is standing.
//
// This is the "meet me at the boss" request: rather than a scripted per-boss
// trigger, either player can close the gap at any point, which covers the same
// need (nobody starts a boss alone) without needing to know where any boss is.
// It uses the peer's live interpolated transform, so it lands next to them
// rather than at a hardcoded point, and it only works when both players report
// the same level -- teleporting into a level you are not in is not a teleport,
// it is a fall out of the world.
void TeleportToPeer(const char* current_level, bool have_level) {
    if (!net::IsConnected()) {
        coop::ReportProblem("not connected -- nobody to teleport to");
        return;
    }
    if (!have_level || !net::GetPeerLevel()[0] ||
        _stricmp(current_level, net::GetPeerLevel()) != 0) {
        SC_LOG("teleport: refused -- peer is in '%s', we are in '%s'", net::GetPeerLevel(),
               have_level ? current_level : "?");
        coop::ReportProblem("you are not in the same level as your partner");
        return;
    }

    ue::FVector where = {};
    ue::FRotator facing = {};
    ue::FVector unused = {};
    if (!net::GetPeerTransform(&where, &facing, &unused)) {
        coop::ReportProblem("no position from your partner yet");
        return;
    }

    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (!player) return;

    // Landed just behind them rather than inside them: two capsules asked to
    // occupy one point makes K2_TeleportTo refuse outright.
    const float yaw_radians = facing.Yaw * 3.14159265f / 180.f;
    where.X -= 150.f * cosf(yaw_radians);
    where.Y -= 150.f * sinf(yaw_radians);

    const bool ok = TeleportActor(player, where, facing);
    SC_LOG("teleport: to partner at (%.0f, %.0f, %.0f) -> %s", where.X, where.Y, where.Z,
           ok ? "arrived" : "REFUSED (no room there)");
    if (!ok) coop::ReportProblem("no room to land next to your partner");
}

// The joiner's save can open the correct map at a checkpoint the host has never
// reached. Once the host snapshot exists, land beside that host exactly once.
// This deliberately uses the same collision-aware route as the manual Lobby
// teleport, so failed placement remains a retry rather than a blind transform.
void ReconcileJoinerArrival(ue::UObject* player) {
    if (!g_arrival_teleport_pending || !player || !net::IsConnected() ||
        net::GetRole() != net::Role::Client) {
        return;
    }

    char current_level[192] = {};
    if (!ue::GetCurrentLevelPath(current_level, sizeof(current_level)) ||
        !net::GetPeerLevel()[0] || _stricmp(current_level, net::GetPeerLevel()) != 0) {
        return;
    }

    ue::FVector where = {};
    ue::FRotator facing = {};
    ue::FVector unused = {};
    if (!net::GetPeerTransform(&where, &facing, &unused)) return;

    const float yaw_radians = facing.Yaw * 3.14159265f / 180.f;
    where.X -= 150.f * cosf(yaw_radians);
    where.Y -= 150.f * sinf(yaw_radians);
    if (TeleportActor(player, where, facing)) {
        g_arrival_teleport_pending = false;
        SC_LOG("lobby: joiner reconciled to host at (%.0f, %.0f, %.0f)", where.X, where.Y,
               where.Z);
        return;
    }

    static DWORD last_refused_log = 0;
    const DWORD now = GetTickCount();
    if (now - last_refused_log >= 1000) {
        last_refused_log = now;
        SC_LOG("lobby: arrival teleport waiting for clear space near host");
    }
}

void InvitePeerHere(const char* current_level, bool have_level) {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (net::GetRole() != net::Role::Host) {
        SC_LOG("lobby: only the host can start (you are joining)");
        coop::ReportProblem("only the host can invite");
    } else if (!net::IsConnected()) {
        SC_LOG("lobby: nobody connected yet");
        coop::ReportProblem("nobody connected yet");
    } else if (!player || !have_level || IsTransientSelectionLevel(current_level)) {
        SC_LOG("lobby: no playable pawn/level yet -- press Story -> Continue first");
        coop::ReportProblem("press Story -> Continue before inviting");
    } else {
        g_coop_started = true;
        lstrcpynA(g_announced_level, current_level, sizeof(g_announced_level));
        net::BumpLevelRequest();
        net::SendLevelSync(current_level);
        SC_LOG("lobby: invited peer to '%s'", current_level);
    }
}

// The host re-invites whenever its own level changes, so the joiner follows
// through a whole playthrough instead of only the first room. Without this,
// every transition -- every death, every door -- would need a manual press.
void HostAnnounceLevelChanges(const char* current_level, bool have_level) {
    if (!g_coop_started || !coop::Get().auto_follow_level) return;
    if (!have_level || IsTransientSelectionLevel(current_level) ||
        net::GetRole() != net::Role::Host || !net::IsConnected()) return;
    if (_stricmp(g_announced_level, current_level) == 0) return;

    lstrcpynA(g_announced_level, current_level, sizeof(g_announced_level));
    net::BumpLevelRequest();
    net::SendLevelSync(current_level);
    SC_LOG("lobby: level changed to '%s' -- pulling the peer along", current_level);
}

// The joiner follows the host even without an explicit invite. An invite can be
// missed (UDP, or the joiner sitting in a menu at the time), and the symptom is
// two players in different levels seeing nothing at all, with no error -- which
// is indistinguishable from the mod being broken.
void JoinerFollowHostLevel(const char* current_level, bool have_level) {
    (void)current_level;
    (void)have_level;
    // Presence is display-only. Explicit, retried LevelSync invites do travel.
}

void PublishSyncRows() {
    EnemyRow rows[net::kMaxTrackedEnemies];
    const int count = GetEnemyRows(rows, net::kMaxTrackedEnemies);

    ui::SyncRow out[net::kMaxTrackedEnemies];
    for (int i = 0; i < count; ++i) {
        out[i].hash = rows[i].hash;
        out[i].distance = rows[i].distance;
        out[i].health = rows[i].health;
        out[i].max_health = rows[i].max_health;
        out[i].active = rows[i].active;
        out[i].down = rows[i].down;
        out[i].driven = rows[i].driven;
        out[i].ai_stopped = rows[i].ai_stopped;
        lstrcpynA(out[i].name, rows[i].name, sizeof(out[i].name));
    }
    ui::SetSyncRows(out, count);
}

// The lobby: connection state, where each player is, and the invite.
//
// It lives in the overlay rather than a separate app because only code inside
// the game can trigger a level load, and because alt-tabbing out of a fighting
// game to press Start would be worse than the problem it solves.
void UpdateLobby(ue::UObject* player) {
    char current_level[192] = {};
    const bool have_level = ue::GetCurrentLevelPath(current_level, sizeof(current_level));
    const bool connected = net::IsConnected();
    if (connected != g_lobby_was_connected) {
        g_lobby_was_connected = connected;
        g_coop_started = false;
        g_announced_level[0] = '\0';
        // An invite from a session that has ended is not an invite.
        g_have_invite = false;
        g_invite_level[0] = '\0';
        g_arrival_teleport_pending = false;
    }
    if (have_level && IsTransientSelectionLevel(current_level) && g_coop_started) {
        g_coop_started = false;
        g_announced_level[0] = '\0';
        SC_LOG("lobby: level picker reached -- co-op waits for the next Start action");
    }

    // Tell the peer where we are, about once a second.
    static DWORD last_presence = 0;
    const DWORD now = GetTickCount();
    if (net::IsConnected() && have_level && now - last_presence > 1000) {
        last_presence = now;
        net::SendLevelPresence(current_level);
    }


    HostAnnounceLevelChanges(current_level, have_level);
    JoinerFollowHostLevel(current_level, have_level);

    // Joiner: an invite is an OFFER, not an order.
    //
    // This used to call OpenLevel the instant a LevelSync arrived, which meant
    // the host loading any level tore the other player out of whatever they were
    // doing -- mid-fight, mid-menu, with no prompt and nothing pressed. That is
    // the reported "the game just starts when I load the level, without me
    // pressing start". The invite is now held and shown; `auto_join_level`
    // restores the old behaviour for anyone who wants it.
    char invited[192] = {};
    if (net::PopLevelSync(invited, sizeof(invited))) {
        if (IsTransientSelectionLevel(invited)) {
            g_coop_started = false;
            g_announced_level[0] = '\0';
            g_have_invite = false;
            SC_LOG("lobby: ignored host's transient level-picker world '%s'", invited);
        } else if (have_level && _stricmp(invited, current_level) == 0) {
            g_coop_started = true;
            g_have_invite = false;
            // Same package can still mean a different local checkpoint/run.
            // Reconcile the joiner to the host's live position once a peer
            // snapshot is available instead of leaving them in stale save data.
            g_arrival_teleport_pending = net::GetRole() == net::Role::Client;
            SC_LOG("lobby: already in '%s' -- arrival reconciliation queued", invited);
        } else {
            lstrcpynA(g_invite_level, invited, sizeof(g_invite_level));
            g_have_invite = true;
            if (coop::Get().auto_join_level) {
                SC_LOG("lobby: auto-joining '%s'", invited);
                AcceptInvite();
            } else {
                SC_LOG("lobby: host invited you to '%s' -- F1 -> Lobby -> Join to accept",
                       invited);
                coop::ReportProblem("host invited you to %s -- F1 to join", LevelLeaf(invited));
            }
        }
    }

    // Menu requests are executed HERE, on the game thread. The menu itself runs
    // on the render thread inside Present, where calling OpenLevel or spawning
    // an actor would race the engine's own use of those systems.
    ui::MenuRequests requests;
    if (ui::TakeMenuRequests(&requests)) {
        if (requests.disconnect_network) {
            net::DisconnectSession();
        } else if (requests.restart_network) {
            net::RestartSession();
        } else if (requests.apply_network) {
            net::Reconfigure(requests.host_mode, requests.address, requests.port,
                             requests.passphrase);
        }
        if (requests.discover_address) net::DiscoverPublicAddress();
        if (requests.save_config) coop::Save();
        if (requests.log_roster) DumpRoster();
        if (requests.spawn_puppet) SpawnPuppet();
        if (requests.despawn_puppet) DespawnPuppet();
        if (requests.invite_peer) InvitePeerHere(current_level, have_level);
        if (requests.accept_invite) AcceptInvite();
        if (requests.decline_invite) DeclineInvite();
        if (requests.teleport_to_peer) TeleportToPeer(current_level, have_level);
        if (requests.travel && requests.level[0]) {
            g_coop_started = true;
            lstrcpynA(g_announced_level, requests.level, sizeof(g_announced_level));
            // Invite first: the peer starts loading while we do, rather than
            // after we have already arrived.
            net::BumpLevelRequest();
            net::SendLevelSync(requests.level);
            SC_LOG("lobby: travelling to '%s' and inviting peer", requests.level);
            ue::OpenLevel(requests.level);
        }
    }

    // Status is refreshed about four times a second: often enough to feel live,
    // rarely enough that building it costs nothing.
    static int counter = 0;
    if (++counter % 15 != 0) return;

    PublishSyncRows();

    ui::MenuStatus status;
    status.connected = net::IsConnected();
    status.hosting = net::GetRole() == net::Role::Host;
    status.offline = net::GetRole() == net::Role::Offline;
    status.have_level = have_level;
    lstrcpynA(status.my_level, LevelLeaf(current_level), sizeof(status.my_level));
    lstrcpynA(status.peer_level, LevelLeaf(net::GetPeerLevel()), sizeof(status.peer_level));
    status.together = have_level && net::GetPeerLevel()[0] &&
                      _stricmp(current_level, net::GetPeerLevel()) == 0;

    Fighter mine = ResolveFighter(player);
    status.my_health = GetHealth(mine);
    status.my_max_health = GetMaxHealth(mine);
    status.my_guard = GetGuard(mine);
    status.faction_mine = GetFaction(player);

    net::PeerVitals vitals;
    status.peer_known = net::GetPeerVitals(&vitals);
    if (status.peer_known) {
        status.peer_health = vitals.health;
        status.peer_max_health = vitals.max_health;
        status.peer_guard = vitals.guard;
        status.peer_down = vitals.is_down;
    }

    status.puppet_alive = g_puppet != nullptr;
    if (g_puppet) status.faction_puppet = GetFaction(g_puppet);
    // The peer's answer, held briefly so it is readable rather than a flash.
    static bool answer_valid = false;
    static bool answer_accepted = false;
    static DWORD answer_until = 0;
    bool accepted_now = false;
    if (net::PopInviteReply(&accepted_now)) {
        answer_valid = true;
        answer_accepted = accepted_now;
        answer_until = GetTickCount() + 8000;
    } else if (answer_valid && static_cast<LONG>(GetTickCount() - answer_until) >= 0) {
        answer_valid = false;
    }
    status.invite_answer_valid = answer_valid;
    status.invite_answer_accepted = answer_accepted;

    status.invite_pending = g_have_invite;
    lstrcpynA(status.invite_level, LevelLeaf(g_invite_level), sizeof(status.invite_level));
    status.friendly_confirmed = g_friendly_verified;
    lstrcpynA(status.public_address, net::GetPublicAddress(), sizeof(status.public_address));
    lstrcpynA(status.detail, coop::GetStats().last_problem, sizeof(status.detail));
    ui::SetMenuStatus(status);

    // The one-line summary, for the window overlay and the menu header.
    char line[192] = {};
    if (status.invite_pending) {
        snprintf(line, sizeof(line),
                 "SifuCoop  your partner is in %s - F1 -> Lobby -> Join to go there",
                 status.invite_level);
    } else if (status.offline) {
        snprintf(line, sizeof(line), "SifuCoop  offline - open the menu with F1");
    } else if (!status.connected) {
        snprintf(line, sizeof(line), "SifuCoop  %s - waiting for peer...",
                 status.hosting ? "HOSTING" : "joining");
    } else if (status.together) {
        snprintf(line, sizeof(line), "SifuCoop  CONNECTED - together in %s  [peer %s, %dms]",
                 status.my_level, !status.peer_known ? "?" : (status.peer_down ? "DOWN" : "up"),
                 net::GetRoundTripMs());
    } else if (status.hosting) {
        snprintf(line, sizeof(line), "SifuCoop  peer in %s - syncing level",
                 status.peer_level);
    } else {
        snprintf(line, sizeof(line), "SifuCoop  host is in %s - following...",
                 status.peer_level);
    }
    ui::SetOverlayText(line);

    // Session heartbeat: one always-on line every few seconds while connected,
    // written to the log regardless of the verbose flags. It is the artifact
    // you read after a two-machine session -- put the two logs side by side and
    // it shows, per machine, whether they saw each other, whether enemies were
    // being driven, and whether damage was crossing in each direction. Without
    // it, confirming a remote session worked meant turning on verbose logging
    // and drowning in per-event spam.
    static DWORD last_heartbeat = 0;
    const coop::Stats& stats = coop::GetStats();
    static DWORD last_rx_count = 0;
    static DWORD last_dropped = 0;
    if (net::IsConnected() && now - last_heartbeat > 5000) {
        const DWORD elapsed = now - last_heartbeat;
        last_heartbeat = now;

        // Arrival rate and loss, which the heartbeat never showed. A puppet can
        // only be as smooth as the stream driving it: if snapshots are arriving
        // at a fraction of the sending rate, or sequence gaps are piling up, no
        // amount of interpolation on this side can hide it -- and it would look
        // exactly like the drive being at fault. This separates the two.
        const DWORD rx_now = stats.packets_received;
        const DWORD dropped_now = stats.packets_dropped;
        const DWORD rx_delta = rx_now - last_rx_count;
        const DWORD dropped_delta = dropped_now - last_dropped;
        last_rx_count = rx_now;
        last_dropped = dropped_now;
        const int rx_rate = elapsed > 0 ? static_cast<int>(rx_delta * 1000 / elapsed) : 0;

        // Position updates specifically. rx counts every packet type together,
        // so it could not answer "how often does the peer actually move" -- and
        // that was the exact question a whole round of guessing turned on. This
        // number should sit at snapshot_hz; anything near 1 means the peer is
        // not sending, and a healthy number here with a stationary character
        // means the fault is on the receiving side instead.
        static DWORD last_snapshots = 0;
        const DWORD snapshots_now = stats.snapshots_received;
        const int snapshot_rate =
            elapsed > 0 ? static_cast<int>((snapshots_now - last_snapshots) * 1000 / elapsed) : 0;
        last_snapshots = snapshots_now;

        SC_LOG("coop: %s rtt=%dms | levels me='%s' peer='%s' %s | enemies known=%d active=%d "
               "driven=%d unmatched=%d | dmg out=%.0f in=%.0f | attacks=%u | rejected=%u "
               // "seqgap" and not "lost": the sequence number is shared by every
               // packet type, so a snapshot following an enemy-state packet
               // looks like a gap without anything having been lost. Useful as a
               // relative trend, not as a loss figure -- reading it as loss
               // would send the next session chasing a network fault that is not
               // there.
               "| rx=%d/s peerpos=%d/s seqgap=%u (%u total)",
               net::GetRole() == net::Role::Host ? "HOST" : "JOIN", net::GetRoundTripMs(),
               status.my_level, status.peer_level, status.together ? "TOGETHER" : "apart",
               stats.enemies_known, stats.enemies_active, stats.enemies_driven,
               stats.enemies_unmatched, stats.damage_reported_total, stats.damage_applied_total,
               stats.attacks_echoed, stats.packets_rejected,
               rx_rate, snapshot_rate, dropped_delta, dropped_now);
    }
    // A one-time "we are actually connected" marker, so scanning the log for the
    // moment the session became live needs no arithmetic on timestamps.
    static bool announced_connected = false;
    if (net::IsConnected() && !announced_connected) {
        announced_connected = true;
        SC_LOG("coop: === SESSION LIVE as %s ===",
               net::GetRole() == net::Role::Host ? "HOST" : "JOINER");
    } else if (!net::IsConnected()) {
        announced_connected = false;
    }
}

}  // namespace

bool CoopGameplayActive() { return CoopBodiesMayExist(); }

void InitPuppet(std::uintptr_t base) {
    g_spawn_actor = reinterpret_cast<SpawnActorFn>(base + offsets::UWorld_SpawnActor_VecRot);
    g_player_anim_update_target = base + offsets::UPlayerAnim_NativeUpdateAnimation;
    g_sc_anim_update_target = offsets::USCAnimInstance_NativeUpdateAnimation
        ? base + offsets::USCAnimInstance_NativeUpdateAnimation
        : 0;
    g_set_player_anim_speed_state = offsets::UPlayerAnim_BPF_SetSpeedState
        ? reinterpret_cast<PlayerAnimSetSpeedStateFn>(
              base + offsets::UPlayerAnim_BPF_SetSpeedState)
        : nullptr;
    g_get_targetable_actor_component = offsets::UTargetableActorHelper_GetTargetableActorComponent
        ? reinterpret_cast<GetTargetableActorComponentFn>(
              base + offsets::UTargetableActorHelper_GetTargetableActorComponent)
        : nullptr;
    g_register_targetable_actor = offsets::USCActorManager_RegisterTargetableActor
        ? reinterpret_cast<RegisterTargetableActorFn>(
              base + offsets::USCActorManager_RegisterTargetableActor)
        : nullptr;



    SC_LOG("puppet: ready (use F1 -> Debug for diagnostics)");
}

ue::UObject* SpawnPlayerClone(const ue::FVector& location, const ue::FRotator& rotation) {
    ue::UObject* world = ue::GetWorld();
    if (!world || !g_spawn_actor) return nullptr;
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player) return nullptr;
    void* player_class = GetObjectClass(player);
    if (!player_class) return nullptr;
    alignas(16) unsigned char spawn_params[128] = {};
    return g_spawn_actor(world, player_class, &location, &rotation, spawn_params);
}

bool SpawnPuppet() {
    if (net::IsConnected() && !g_coop_started) {
        SC_LOG("puppet: refusing body spawn before host starts co-op in Story");
        coop::ReportProblem("host must press Story -> Continue, then invite");
        return false;
    }
    if (net::IsConnected() && !CoopBodiesMayExist()) {
        SC_LOG("puppet: body pending until both players occupy the same Story level");
        return false;
    }

    // Preferred path when enabled: ask the engine for a REAL second player
    // instead of spawning a clone to puppet. Everything downstream -- the drive,
    // vitals mirroring, level-change handling -- works on an actor pointer and
    // does not care which of the two produced it, so only this one place has to
    // know the difference.
    if (coop::Get().real_second_player) {
        if (ue::UObject* pawn = CreateSecondPlayer()) {
            g_puppet = pawn;
            g_peer_was_down = false;
            ResetSamples();
            g_follow_enabled = false;
            SC_LOG("puppet: using the REAL second player pawn %p (not a puppet clone)",
                   static_cast<void*>(pawn));
            return true;
        }
        // A real player can be temporarily pawnless during travel. A puppet
        // clone is not a valid substitute: maintenance will discard it, then
        // this retry path spawns another one every second. Leave the slot empty
        // until the persisted controller has been safely re-housed.
        SC_LOG("puppet: real second player pending -- no clone fallback during travel");
        return false;
    }

    if (g_puppet) {
        SC_LOG("puppet: one already exists at %p -- despawn it first",
               static_cast<void*>(g_puppet));
        return false;
    }

    ue::UObject* world = ue::GetWorld();
    if (!world) {
        SC_LOG("puppet: no world");
        return false;
    }

    // Never cached: the pawn is destroyed and recreated on death/aging and on
    // level transitions, so it is re-resolved at every use.
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player) {
        SC_LOG("puppet: no local player (not in a level?)");
        return false;
    }

    void* player_class = GetObjectClass(player);
    if (!player_class) {
        SC_LOG("puppet: could not read player UClass");
        return false;
    }

    ue::FVector location = {};
    ue::FRotator rotation = {};
    if (!ue::GetActorLocation(player, &location) || !ue::GetActorRotation(player, &rotation)) {
        SC_LOG("puppet: could not read player transform");
        return false;
    }

    // Two metres in front of the player, facing them.
    const float yaw_radians = rotation.Yaw * 3.14159265f / 180.f;
    location.X += 200.f * cosf(yaw_radians);
    location.Y += 200.f * sinf(yaw_radians);
    rotation.Yaw += 180.f;

    // FActorSpawnParameters zero-initialised: NAME_None, no template/owner/
    // instigator, default level, SpawnCollisionHandlingOverride = Undefined
    // (which defers to the class default). Oversized on purpose so we cannot
    // under-read the struct if its tail differs from what we assume.
    alignas(16) unsigned char spawn_params[128] = {};

    ue::UObject* spawned = g_spawn_actor(world, player_class, &location, &rotation, spawn_params);
    if (!spawned) {
        SC_LOG("puppet: SpawnActor returned null (collision handling, or the class "
               "refused to spawn)");
        coop::ReportProblem("could not spawn the remote character here");
        return false;
    }

    g_puppet = spawned;

    ue::FVector actual = {};
    ue::GetActorLocation(spawned, &actual);
    SC_LOG("puppet: SPAWNED %p at (%.1f, %.1f, %.1f)", static_cast<void*>(spawned), actual.X,
           actual.Y, actual.Z);

    ConfigureAsRemote(spawned, player);
    // A spawned clone is not a LocalPlayer and was absent from normal AI
    // candidate selection in live logs. Register its real target component
    // with Sifu's actor manager so enemies can acquire it.
    //
    // HOST ONLY, and this is what "the enemies want to fight the host no matter
    // what" turned out to be. The two machines want opposite things from this
    // body. On the host it stands in for the joining player, and enemies
    // acquiring it is how that player's fight begins -- the flag it produces is
    // what the joiner claims an enemy on. On the joining machine it stands in
    // for the HOST, whose fight is resolved on the host's own machine, so an
    // enemy that walks over to attack it accomplishes nothing and is stolen from
    // the player standing right there. The joiner's own census showed three or
    // four of five enemies doing exactly that, leaving one on the actual player.
    const bool attract_enemies = net::GetRole() != net::Role::Client;
    if (attract_enemies && g_get_targetable_actor_component && g_register_targetable_actor) {
        ue::UObject* targetable = g_get_targetable_actor_component(spawned);
        if (targetable) {
            g_register_targetable_actor(targetable);
            SC_LOG("puppet: targetable component registered for enemy AI");
        } else {
            SC_LOG("puppet: targetable component MISSING -- enemy AI cannot select peer");
        }
    } else if (!attract_enemies) {
        SC_LOG("puppet: NOT registered as a target -- your partner's body is a picture "
               "here, and their fight happens on their own machine");
    }
    g_peer_was_down = false;

    // Start from a clean buffer so the puppet does not immediately snap to
    // wherever you were two seconds ago.
    ResetSamples();

    // Follow mode is an offline rehearsal. With a peer connected the puppet
    // must hold its last known position when a packet is late, NOT fall back to
    // replaying our own movement -- that would look like the peer mimicking us.
    g_follow_enabled = !net::IsConnected();
    SC_LOG("puppet: follow mode %s", g_follow_enabled
                                         ? "ON (offline rehearsal, F8 toggles)"
                                         : "OFF (driven by peer)");
    return true;
}

void DriveActorTo(ue::UObject* actor, const ue::FVector& target,
                  const ue::FRotator& rotation, const ue::FVector& reported_velocity) {
    DriveTo(actor, target, rotation, reported_velocity, false);
}

ue::UObject* GetPuppet() {
    // Hooks may run before the next game-frame preflight during world teardown.
    return g_puppet_world == ue::GetWorld() ? g_puppet : nullptr;
}

bool FriendlyRelationshipVerified() { return g_friendly_verified; }

void DespawnPuppet() {
    if (!g_puppet) {
        SC_LOG("puppet: nothing to despawn");
        return;
    }
    // The combat director holds this exact actor as a target. Release it while
    // it is still live; attempting to validate/unregister it after destruction
    // caused the host restart UAF.
    NotifyPuppetWillBeDestroyed(g_puppet);

    // K2_DestroyActor takes no parameters, so reflection handles this without
    // any struct layout knowledge.
    if (SecondPlayerActive()) {
        // A real player is torn down through the engine, not destroyed as an
        // actor: its controller and local-player slot have to go too.
        RemoveSecondPlayer();
        SC_LOG("puppet: real second player removed");
    } else {
        const bool ok = ue::CallFunction(g_puppet, L"K2_DestroyActor", nullptr);
        SC_LOG("puppet: despawn %s (%p)", ok ? "requested" : "FAILED",
               static_cast<void*>(g_puppet));
    }
    g_puppet = nullptr;
    g_puppet_anim_instance = nullptr;
    g_have_puppet_presentation_velocity = false;
    g_puppet_presentation_targets = {};
    g_puppet_targets_world = nullptr;
    g_cosmetic_attack_montage_until = 0;
    g_puppet_cinematic_until = 0;
    g_puppet_cinematic_needs_clear = false;
    for (CosmeticCinematic& active : g_enemy_cinematics) active = {};
    ForgetEnemyPresentation();
    g_puppet_auto_spawned = false;
    g_friendly_applied_for = nullptr;
    g_friendly_verified = false;
}

void PreparePuppetLifecycle() {
    ue::UObject* world = ue::GetWorld();
    if (world == g_puppet_world) return;

    const bool had_cached_world = g_puppet_world != nullptr;
    g_puppet_world = world;

    // Enemy tracking contains the same old-world actor pointers and, on the
    // host, a combat-director registration for the puppet. At this point the
    // world is already different, so forgetting is the only safe operation.
    ForgetEnemyWorldObjects();

    g_puppet_anim_instance = nullptr;
    g_have_puppet_presentation_velocity = false;
    g_puppet_presentation_targets = {};
    g_puppet_targets_world = nullptr;
    g_cosmetic_attack_montage_until = 0;
    g_puppet_cinematic_until = 0;
    g_puppet_cinematic_needs_clear = false;
    for (CosmeticCinematic& active : g_enemy_cinematics) active = {};

    if (g_puppet) {
        SC_LOG("puppet: level changed -- forgetting the old remote character");
    } else if (had_cached_world) {
        SC_LOG("puppet: level changed -- old object caches cleared");
    }
    g_puppet = nullptr;
    g_puppet_auto_spawned = false;
    g_peer_was_down = false;
    g_friendly_applied_for = nullptr;
    g_friendly_verified = false;
    g_last_player = nullptr;
    g_have_local_velocity = false;
    ResetSamples();
    InvalidateAttackTemplate();
}

void NotifyLocalAttackForCosmetic() {
    // Player attacks can cross two game frames before their sequence/montage is
    // exposed. This remains short enough to avoid replaying stale combat, but
    // survives the real prepare-to-animation handoff on both storefront builds.
    g_cosmetic_attack_montage_until = GetTickCount() + 750;
}

void NotifyLocalSequenceSent() {
    // The exact raw sequence supersedes the montage fallback for this strike.
    g_cosmetic_attack_montage_until = 0;
}

void TickPuppet() {
    EnsurePlayerAnimHook();
    EnsureSCAnimHook();
    ue::UObject* world = ue::GetWorld();
    if (!world) return;
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player) return;

    if (player != g_last_player) {
        if (g_last_player) {
            SC_LOG("puppet: pawn changed %p -> %p (respawn or level change), samples reset",
                   static_cast<void*>(g_last_player), static_cast<void*>(player));
        }
        g_last_player = player;
        ResetSamples();
        // The captured attack template is a raw copy of a struct holding
        g_have_local_velocity = false;
        // pointers into the level we just left. Replaying it now would
        // dereference freed objects.
        InvalidateAttackTemplate();
    }

    ue::FVector location = {};
    ue::FRotator rotation = {};
    if (!ue::GetActorLocation(player, &location) || !ue::GetActorRotation(player, &rotation)) {
        return;
    }
    RecordSample(location, rotation);
    // Ask the movement component what our velocity is instead of inferring it
    // from how far the actor moved since the last frame.
    //
    // The inference is what put a strobing speed on the wire: the transform only
    // changes on frames where movement actually integrated, so at 165 fps
    // against a 60 Hz movement update roughly two frames in three repeat the
    // previous position and difference to exactly zero. The peer then receives
    // full speed, nothing, full speed, nothing -- and since BaseMovementDB gives
    // the V0->V1 blend 0.3 s and V0->V3 a full second, every flip restarts a
    // blend that never completes. The character never leaves the pose it started
    // in. It also explains why this looked one-directional: it depends on the
    // sender's frame rate against its movement tick, so the machine running
    // faster is the one whose character will not animate on the other screen.
    ue::FVector local_velocity = {};
    const float frame_seconds = sifucoop::hooks::FrameDeltaSeconds();
    const auto finite3 = [](const ue::FVector& v) {
        return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
    };
    if (!GetActorVelocity(player, &local_velocity) || !finite3(local_velocity)) {
        local_velocity = {};
        // Fallback for a build where the movement component is unavailable.
        if (g_have_local_velocity && frame_seconds > 0.001f && frame_seconds < 0.25f) {
            const float vx = (location.X - g_last_local_location.X) / frame_seconds;
            const float vy = (location.Y - g_last_local_location.Y) / frame_seconds;
            const float vz = (location.Z - g_last_local_location.Z) / frame_seconds;
            const float speed = sqrtf(vx * vx + vy * vy + vz * vz);
            // A spawn/travel correction is not a movement velocity. Do not turn
            // it into a multi-frame sprint on the peer's character.
            if (speed <= 3000.f) local_velocity = {vx, vy, vz};
        }
    }
    g_last_local_location = location;
    g_have_local_velocity = true;


    // Report our own transform and vitals. Each peer owns its own outcome: with
    // no server to arbitrate, the player who took the hit decides whether they
    // went down, and the other side reflects what was reported. That is not a
    // compromise -- it is the only arrangement in which a parry is judged on
    // the machine where the button was pressed.
    Fighter mine = ResolveFighter(player);
    net::LocalState local;
    local.location = location;
    local.rotation = rotation;
    local.velocity = local_velocity;
    local.in_level = true;
    if (mine.health) {
        local.health = GetHealth(mine);
        local.max_health = GetMaxHealth(mine);
        local.guard = GetGuard(mine);
        local.is_down = IsDown(mine);
        local.state_valid = true;
    }
    net::TickSession(local);

    // Everything the local player is ANIMATING, not only what they attacked
    // with.
    //
    // OrderAttack::OnStart carries strikes and nothing else, which is why the
    // partner still had no blocks, no parries, no hit reactions, no takedown
    // and no fall -- every one of those is a different order type, and most of
    // them do not override GetAnimPlayed at all, so there was nothing to send.
    //
    // UPlayerAnim::m_LastActionAnim is the animation the graph is playing for
    // the current action, whatever produced it, with its cursor beside it. One
    // edge-triggered read covers all of them at once. Player-only: the field is
    // declared on UPlayerAnim, and USCAnimInstance (the enemies' base) has no
    // equivalent -- their swings keep coming from the order path and their
    // deaths from the Kill hook.
    if (coop::Get().sync_montages && net::IsConnected()) {
        ue::UObject* anim_instance = ue::GetAnimInstance(player);
        if (anim_instance) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(anim_instance);
            ue::UObject* action = nullptr;
            std::memcpy(&action, bytes + kAnimLastActionAnim, sizeof(action));
            float cursor = 0.f;
            std::memcpy(&cursor, bytes + kAnimLastActionCursor, sizeof(cursor));

            // m_LastActionAnim is a LAST action, not a current one: it keeps
            // pointing at the asset long after the action has finished, and
            // repeating the same move never changes the pointer. Triggering on
            // the pointer alone therefore fired exactly once per session --
            // today's log has a single `action: sent`, for the death that ended
            // it, across three sessions of fighting.
            //
            // The cursor is what actually says an action started: it runs
            // forward while one plays and restarts when the next begins. Send on
            // either a new asset or a cursor that has gone backwards.
            static ue::UObject* last_action_sent = nullptr;
            static float last_cursor = 0.f;
            const bool restarted = cursor + 0.01f < last_cursor;
            const bool changed = action != last_action_sent;
            last_cursor = cursor;
            if (action && (changed || restarted)) {
                last_action_sent = action;
                char action_path[192] = {};
                if (ue::GetObjectPathName(action, action_path, sizeof(action_path))) {
                    // Sent on the raw-sequence channel so the receiver layers it
                    // through the Cinematic slot rather than replacing the
                    // locomotion graph.
                    net::SendAnimationSequence(action_path, 0,
                                               net::AnimationSemantic::Generic, cursor);
                    static unsigned int actions_sent = 0;
                    if (++actions_sent <= 5 || coop::Get().verbose_orders) {
                        SC_LOG("action: sent '%s' cursor=%.2f", action_path, cursor);
                    }
                }
            } else if (!action) {
                last_action_sent = nullptr;
            }
        }
    }

    // Cosmetic animation mirroring. Regular changes cover dodges/traversal.
    // An attack order also arms a short capture window: repeated punches often
    // reuse one montage asset, so an edge-trigger alone drops every strike after
    // the first. This only calls UAnimInstance::Montage_Play on the peer's
    // puppet; no UAttackComponent path is entered.
    if (coop::Get().sync_montages && net::IsConnected()) {
        static ue::UObject* last_sent = nullptr;
        const DWORD now = GetTickCount();
        const bool attack_pending = now < g_cosmetic_attack_montage_until;
        ue::AnimState anim = {};
        if (ue::ReadAnimState(player, &anim) && anim.montage &&
            (anim.montage != last_sent || attack_pending)) {
            last_sent = anim.montage;
            char path[192] = {};
            if (ue::GetObjectPathName(anim.montage, path, sizeof(path))) {
                net::SendMontageState(path, anim.position);
                if (attack_pending) {
                    g_cosmetic_attack_montage_until = 0;
                    static unsigned int cosmetic_sent = 0;
                    if (++cosmetic_sent <= 3 || coop::Get().verbose_orders) {
                        SC_LOG("attack: cosmetic montage sent");
                    }
                }
            }
        }
        if (g_cosmetic_attack_montage_until && now >= g_cosmetic_attack_montage_until) {
            g_cosmetic_attack_montage_until = 0;
            static DWORD last_unavailable_log = 0;
            if (now - last_unavailable_log >= 5000) {
                last_unavailable_log = now;
                SC_LOG("attack: cosmetic montage unavailable for this move");
            }
        }
    }
    // Real remote enemy orders run first so their local hitbox/parry windows are
    // active before the matching cosmetic sequence is layered onto the actor.
    PumpRemoteOrders();
    TickEnemyCinematics();

    // Drain a bounded burst. A single latest-value slot lost punches whenever
    // two attacks arrived in one frame; every attack start is an event.
    for (int event = 0; event < 8; ++event) {
        char path[192] = {};
        float position = 0.f;
        bool raw_sequence = false;
        std::uint32_t actor_hash = 0;
        net::AnimationSemantic semantic = net::AnimationSemantic::Generic;
        if (!net::PopMontageState(path, sizeof(path), &position, &raw_sequence,
                                  &actor_hash, &semantic)) {
            break;
        }
        const bool enemy_death = actor_hash != 0 && raw_sequence &&
                                 semantic == net::AnimationSemantic::Death;
        const bool peer_authored_death =
            enemy_death && net::GetRole() == net::Role::Host;
        if (peer_authored_death && !coop::Get().sync_enemy_death_animations) continue;

        ue::UObject* visual_actor = actor_hash ? FindEnemyByHash(actor_hash) : g_puppet;
        if (!visual_actor) continue;

        wchar_t wide[192] = {};
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 192);
        ue::UObject* animation = ue::FindObjectByPath(wide);
        if (!animation) continue;

        // A body running its own brain must not have the peer's Attack layered
        // over it -- it is animating that swing itself, and a second copy is out
        // of step. Explicit Death is allowed past this guard only so it can
        // pass the AnimSequence type gate and seed the pending-death ledger. The
        // stored sequence is never played here: enemies.cpp consumes it at the
        // authoritative dead edge and launches Sifu's kill path exactly once.
        // Because the semantic is on the wire, an Attack can never enter it.
        if (actor_hash && EnemyRunsLocalBrain(actor_hash) && !enemy_death) continue;

        // Second gate, on the receiving end, because the sender is not the only
        // thing that can be wrong and the cost of being wrong here is a heap
        // corruption inside FAnimMontageInstance::Advance rather than a missing
        // animation. PlaySlotAnimationAsDynamicMontage takes a UAnimSequenceBase;
        // a UPoseAsset reached it once and took the game down mid-fight.
        if (raw_sequence && !ue::ObjectClassIs(animation, "AnimSequence")) {
            static char refused_class[160] = {};
            char class_path[160] = {};
            if (ue::GetObjectClassPathName(animation, class_path, sizeof(class_path)) &&
                lstrcmpA(class_path, refused_class) != 0) {
                lstrcpynA(refused_class, class_path, sizeof(refused_class));
                SC_LOG("anim: refused a %s from the peer -- only AnimSequence may go through "
                       "the Cinematic slot",
                       class_path);
            }
            continue;
        }
        if (enemy_death) {
            NoteEnemyDeathAnimation(actor_hash, animation);
            SC_LOG("death: exact enemy sequence received actor=%08X", actor_hash);
            continue;
        }

        if (raw_sequence) {
            // Sifu keeps m_bIsDown set for the entire resurrection order and
            // clears it only when the stand-up sequence finishes. That is fine
            // on the owning machine, where the death state machine itself owns
            // the sequence. Here the sequence is layered through the AnimBP's
            // Cinematic slot instead, and the full-body Down branch masks that
            // slot while m_bIsDown is still true. The montage therefore runs
            // invisibly, then the body snaps upright when the peer's delayed
            // down=false snapshot arrives.
            //
            // Release only this puppet's presentation state immediately before
            // an actual player resurrection sequence. Keep g_peer_was_down true
            // until the real snapshot edge arrives: ApplyPeerVitals will not
            // reassert Down on the next frame, and it will still log/finalize
            // the authoritative transition at the end of the order.
            const bool peer_resurrection =
                actor_hash == 0 && strstr(path, "/Death/") && strstr(path, "resurrect_");
            if (peer_resurrection && g_peer_was_down) {
                SetDown(ResolveFighter(visual_actor), false);
                SC_LOG("puppet: released local down state for the incoming resurrection sequence");
            }

            if (ue::PlayAnimationAsset(visual_actor, animation, position)) {
                const DWORD until = AnimationDeadline(animation);
                ue::UObject* anim_instance = ue::GetAnimInstance(visual_actor);
                if (actor_hash == 0) {
                    g_puppet_anim_instance = anim_instance;
                    g_puppet_cinematic_until = until;
                    g_puppet_cinematic_needs_clear = true;
                    if (anim_instance) {
                        WriteCinematicWeight(reinterpret_cast<std::uint8_t*>(anim_instance), 1.f);
                    }
                } else {
                    // Only an explicit Death was recorded in the ledger above;
                    // all enemy raw events share this cosmetic lifetime path.
                    ArmEnemyCinematic(actor_hash, until);
                    if (anim_instance) {
                        WriteCinematicWeight(reinterpret_cast<std::uint8_t*>(anim_instance), 1.f);
                    }
                }
                SC_LOG("anim: cosmetic sequence playing actor=%08X semantic=%u", actor_hash,
                       static_cast<unsigned int>(semantic));
            } else {
                static DWORD last_failed_log = 0;
                const DWORD failed_now = GetTickCount();
                if (failed_now - last_failed_log >= 5000) {
                    last_failed_log = failed_now;
                    SC_LOG("anim: cosmetic sequence could not start actor=%08X semantic=%u",
                           actor_hash, static_cast<unsigned int>(semantic));
                }
            }
        } else if (actor_hash == 0) {
            ue::AnimState state;
            state.montage = animation;
            state.position = position;
            ue::ApplyAnimState(g_puppet, state);
        }
    }

    // Driven by the *current* connection state rather than the one-shot connect
    // event. Consuming the event was a bug: if the peer connected while we were
    // in a menu (no world, no pawn), the spawn failed, the event was gone, and
    // the puppet never appeared for the rest of the session.
    net::ConsumeConnectedEvent();
    if (net::ConsumeDisconnectedEvent()) {
        SC_LOG("net: peer left -- despawning puppet");
        DespawnPuppet();
    }

    // A connection is only transport. Do not create a clone/second controller
    // until the host has explicitly started co-op from a playable Story level.
    // Creating it in the front end is treated by Sifu as a second pad pressing
    // Start and can steal/block player one's body.
    if (CoopBodiesMayExist() && !g_puppet) {
        static DWORD last_attempt = 0;
        const DWORD now = GetTickCount();
        if (now - last_attempt > 1000) {  // retry, but do not spam
            last_attempt = now;
            if (SpawnPuppet()) g_puppet_auto_spawned = true;
        }
    } else if (!net::IsConnected() && g_puppet && g_puppet_auto_spawned) {
        // Only ever removes a puppet the connection created. Previously this
        // deleted ANY puppet whenever no peer was connected, which meant a
        // manual F9 spawn was destroyed on the very next frame -- so the
        // hotkey silently did nothing while hosting without a peer.
        SC_LOG("net: peer gone -- removing its puppet");
        DespawnPuppet();
    }

    // "The host could not move" watchdog.
    //
    // WASD going dead means one of two things, and they need different fixes:
    // controller 0 has no pawn at all, or it is possessing the wrong body --
    // the puppet is a clone of the player class, and Sifu's game mode has been
    // seen handing a second player the first player's character before. Reading
    // it is one comparison, and it turns "it froze" into a named cause.
    {
        ue::UObject* possessed = PrimaryPlayerPawn();
        static int last_state = -1;
        const int state = !possessed ? 0 : (possessed == g_puppet ? 1 : 2);
        if (state != last_state) {
            last_state = state;
            if (state == 0) {
                SC_LOG("input: controller 0 has NO pawn -- this is why movement is dead");
                coop::ReportProblem("you have no character: controller 0 lost its pawn");
            } else if (state == 1) {
                SC_LOG("input: controller 0 is possessing the PUPPET -- your input is "
                       "driving the remote player's body");
                coop::ReportProblem("your controller took over your partner's body");
            } else {
                SC_LOG("input: controller 0 possessing its own pawn (normal)");
            }
        }
    }

    UpdateLobby(player);
    ReconcileJoinerArrival(player);


    // The second player's pawn is rebuilt by the game mode on death, on aging and
    // on every level change, so it is re-resolved here rather than trusted. This
    // also creates the player in the first place once a peer is connected.
    if (coop::Get().real_second_player) {
        // Keep a manual debug body alive, but never revive a retired network
        // body just because its controller survived a disconnect.
        const bool maintain_real = CoopBodiesMayExist() ||
                                   (g_puppet && !g_puppet_auto_spawned);
        if (maintain_real) {
            ue::UObject* pawn = MaintainSecondPlayer();
            if (pawn && pawn != g_puppet) {
                g_puppet = pawn;
                // Only a CONNECTION-created player may be auto-removed when the
                // peer goes away. One created by hand with F4 must survive, or
                // the disconnect path below retires it the instant it appears --
                // which is what was retiring it every frame while offline.
                g_puppet_auto_spawned = net::IsConnected();
            } else if (!pawn && SecondPlayerActive()) {
                // Between pawns: hold nothing rather than drive a dead pointer.
                g_puppet = nullptr;
            }
        }
    }

    if (!g_puppet) return;

    // BP_TPSCharacter has local-only presentation branches. Reasserting once
    // per second catches a hidden-state change without touching transforms.
    if (!SecondPlayerActive()) {
        static DWORD last_visibility_restore = 0;
        const DWORD now = GetTickCount();
        if (now - last_visibility_restore >= 1000) {
            last_visibility_restore = now;
            EnsureRemoteVisible(g_puppet, false);
        }
    }
    net::PeerVitals vitals;
    if (net::GetPeerVitals(&vitals)) ApplyPeerVitals(g_puppet, vitals);

    // Best-effort, default-off: no-op once applied for this puppet.
    MaintainFriendlyRelationship(player, g_puppet);

    // A live peer always wins over the local follow-mode rehearsal.
    ue::FVector peer_location = {};
    ue::FRotator peer_rotation = {};
    ue::FVector peer_velocity = {};
    if (net::IsConnected() && net::GetPeerTransform(&peer_location, &peer_rotation, &peer_velocity)) {
        DriveTo(g_puppet, peer_location, peer_rotation, peer_velocity, true);
        return;
    }

    if (g_follow_enabled) DrivePuppetFromSamples(g_puppet);
}

}  // namespace sifucoop::game

