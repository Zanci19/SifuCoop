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






constexpr std::uintptr_t kClassPrivateOffset = 0x10;

using SpawnActorFn = ue::UObject*(__fastcall*)(void* world, void* uclass, const ue::FVector*,
                                               const ue::FRotator*, const void* params);

SpawnActorFn g_spawn_actor = nullptr;


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
int g_puppet_held_speed_band = 0;
int g_puppet_candidate_speed_band = 0;
int g_puppet_last_commanded_speed_band = -1;
DWORD g_puppet_band_candidate_since = 0;
DWORD g_puppet_band_held_since = 0;



PresentationTargets g_puppet_presentation_targets;



























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




int EnemyBandForSpeed(float speed) {
    if (speed <= 20.f) return 0;
    if (speed < 240.f) return 1;
    if (speed < 475.f) return 2;
    return 3;
}

void ForgetEnemyPresentation() {
    for (EnemyPresentation& slot : g_enemy_presentation) slot = {};
}


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
    if (!slot) return;

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




    constexpr DWORD kBandSettleMs = 120;
    if (wanted != slot->candidate_band) {
        slot->candidate_band = wanted;
        slot->candidate_since = now;
    } else if (wanted != slot->band && now - slot->candidate_since >= kBandSettleMs) {
        slot->band = wanted;
    }
    slot->until = now + 400;
}









ue::UObject* g_puppet_targets_world = nullptr;



ue::UObject* g_puppet = nullptr;
ue::UObject* g_puppet_world = nullptr;
bool g_coop_started = false;
char g_announced_level[192] = {};
bool g_lobby_was_connected = false;




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







constexpr int kSampleCount = 256;
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




ue::UObject* g_last_player = nullptr;

ue::FVector g_last_local_location;
bool g_have_local_velocity = false;




constexpr std::uintptr_t kAnimOwnerVelocity = 0x0FB4;
constexpr std::uintptr_t kAnimOwnerVelocityLength = 0x0FC0;
constexpr std::uintptr_t kAnimVelocityMaxV0 = 0x0FC4;
constexpr std::uintptr_t kAnimVelocityMaxV1 = 0x0FC8;
constexpr std::uintptr_t kAnimVelocityMaxV2 = 0x0FCC;
constexpr std::uintptr_t kAnimBlendspaceAngle = 0x0FD4;
constexpr std::uintptr_t kAnimWantedSpeed = 0x15E4;
constexpr std::uintptr_t kAnimLastActionAnim = 0x0E68;
constexpr std::uintptr_t kAnimLastActionCursor = 0x0E74;
constexpr std::uintptr_t kAnimMoveStatus = 0x1C29;
constexpr std::uintptr_t kAnimSpeedState = 0x1C2D;
constexpr std::uintptr_t kAnimSpeedStateAlphaV0 = 0x1C44;




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








int RawSpeedStateForSpeed(const std::uint8_t* bytes, float speed) {




    static bool announced = false;
    float v0 = ReadFloatAt(bytes, kAnimVelocityMaxV0);
    float v1 = ReadFloatAt(bytes, kAnimVelocityMaxV1);
    float v2 = ReadFloatAt(bytes, kAnimVelocityMaxV2);
    if (!(v0 > 0.f && v1 > v0 && v2 > v1)) {




        v0 = 20.f;
        v1 = 240.f;
        v2 = 475.f;
        if (!announced) {
            announced = true;
            SC_LOG("puppet: locomotion thresholds UNAVAILABLE on this anim instance -- "
                   "using the fallback %.0f/%.0f/%.0f, so any band disagreement with Sifu "
                   "is probably ours",
                   v0, v1, v2);
        }
    } else if (!announced) {
        announced = true;
        SC_LOG("puppet: locomotion thresholds read from the character: %.0f/%.0f/%.0f",
               v0, v1, v2);
    }
    if (speed <= v0) return 0;
    if (speed < v1) return 1;
    if (speed < v2) return 2;
    return 3;
}











int SpeedStateForSpeed(const std::uint8_t* bytes, float speed) {
    const int raw = RawSpeedStateForSpeed(bytes, speed);
    const DWORD now = GetTickCount();
    if (g_puppet_band_held_since == 0) g_puppet_band_held_since = now;

    if (raw == g_puppet_held_speed_band) {
        g_puppet_candidate_speed_band = g_puppet_held_speed_band;
        g_puppet_band_candidate_since = 0;
        return g_puppet_held_speed_band;
    }
    if (raw != g_puppet_candidate_speed_band) {
        g_puppet_candidate_speed_band = raw;
        g_puppet_band_candidate_since = now;
        return g_puppet_held_speed_band;
    }



    const DWORD confirm_ms = raw > g_puppet_held_speed_band ? 60u : 180u;
    const DWORD minimum_hold_ms = 150u;
    if (now - g_puppet_band_candidate_since < confirm_ms) return g_puppet_held_speed_band;
    if (now - g_puppet_band_held_since < minimum_hold_ms) return g_puppet_held_speed_band;

    g_puppet_held_speed_band = g_puppet_candidate_speed_band;
    g_puppet_band_held_since = now;
    g_puppet_band_candidate_since = 0;
    return g_puppet_held_speed_band;
}

void __fastcall SCAnimUpdateHook(ue::UObject* anim_instance, float delta_seconds) {
    if (g_original_sc_anim_update) g_original_sc_anim_update(anim_instance, delta_seconds);






    const DWORD now = GetTickCount();
    for (CosmeticCinematic& active : g_enemy_cinematics) {
        if (active.anim_instance != anim_instance || now >= active.until) continue;
        WriteCinematicWeight(reinterpret_cast<std::uint8_t*>(anim_instance), 1.f);
        break;
    }





    for (EnemyPresentation& driven : g_enemy_presentation) {
        if (driven.anim_instance != anim_instance) continue;
        if (static_cast<LONG>(now - driven.until) >= 0) break;
        if (driven.world != ue::GetWorld()) break;
        WritePresentationVelocity(driven.targets, driven.velocity);
        SetMovementSpeedState(driven.targets.movement, driven.band);
        break;
    }
}

void __fastcall PlayerAnimUpdateHook(ue::UObject* anim_instance, float delta_seconds) {



    const bool is_puppet_anim = anim_instance && anim_instance == g_puppet_anim_instance;
    const bool inject = is_puppet_anim && g_have_puppet_presentation_velocity;
    auto* bytes = reinterpret_cast<std::uint8_t*>(anim_instance);
    float speed = 0.f;
    if (inject) {
        speed = sqrtf(g_puppet_presentation_velocity.X *
                          g_puppet_presentation_velocity.X +
                      g_puppet_presentation_velocity.Y *
                          g_puppet_presentation_velocity.Y);






















        const bool targets_live = g_puppet_targets_world == ue::GetWorld();

        if (targets_live) {
            WritePresentationVelocity(g_puppet_presentation_targets,
                                      g_puppet_presentation_velocity);
        }


























        if (targets_live) {
            const int wanted_band = SpeedStateForSpeed(bytes, speed);
            if (wanted_band != g_puppet_last_commanded_speed_band) {
                SetMovementSpeedState(g_puppet_presentation_targets.movement,
                                      wanted_band);
                g_puppet_last_commanded_speed_band = wanted_band;
            }
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






    const float native_speed = ReadFloatAt(bytes, kAnimOwnerVelocityLength);
    const float tolerance = speed * 0.25f > 8.f ? speed * 0.25f : 8.f;
    const bool native_agrees = fabsf(native_speed - speed) <= tolerance;

    const int wanted = SpeedStateForSpeed(bytes, speed);


    const int graph_band = bytes[kAnimSpeedState + 4];
    const bool band_accepted = graph_band == wanted;

    static int last_mode = -1;
    const int mode = (native_agrees ? 1 : 0) | (band_accepted ? 2 : 0);
    if (mode != last_mode) {
        last_mode = mode;






        SC_LOG("puppet: locomotion speed %s, band %s (ours %.0f/V%d, Sifu's %.0f/V%d)",
               native_agrees ? "agreed" : "RECOMPUTED IDLE",
               band_accepted
                   ? "matches"
                   : (graph_band == 0 && speed > 25.f
                           ? "IDLE AT SPEED -- transition commanded"
                           : "transition commanded; graph blending"),
               speed, wanted, native_speed, graph_band);
    }





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





    if (band_accepted || graph_band != 0 || speed <= 25.f) return;

    const int state = wanted;

    if (g_set_player_anim_speed_state) {
        g_set_player_anim_speed_state(anim_instance, static_cast<std::uint8_t>(state));
    } else {
        std::uint8_t speed_state[5] = {};
        speed_state[1] = static_cast<std::uint8_t>(1u << state);
        speed_state[4] = static_cast<std::uint8_t>(state);
        std::memcpy(bytes + kAnimSpeedState, speed_state, sizeof(speed_state));
    }

    float speed_alphas[4] = {};
    speed_alphas[state] = 1.f;
    std::memcpy(bytes + kAnimSpeedStateAlphaV0, speed_alphas, sizeof(speed_alphas));




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
    g_puppet_held_speed_band = 0;
    g_puppet_candidate_speed_band = 0;
    g_puppet_last_commanded_speed_band = -1;
    g_puppet_band_candidate_since = 0;
    g_puppet_band_held_since = 0;
}

void RecordSample(const ue::FVector& location, const ue::FRotator& rotation) {
    g_sample_head = (g_sample_head + 1) % kSampleCount;
    g_samples[g_sample_head] = {location, rotation, true};
    if (g_samples_since_reset < kSampleCount) ++g_samples_since_reset;
}



constexpr float kSnapDistance = 600.f;
constexpr float kVerticalSnap = 250.f;





constexpr float kYawSmoothing = 0.25f;





constexpr float kPositionCorrection = 0.30f;











float FrameRateAdjusted(float fraction_at_60) {
    const float dt = sifucoop::hooks::FrameDeltaSeconds();
    const float retained = powf(1.f - fraction_at_60, dt * 60.f);
    float alpha = 1.f - retained;
    if (alpha < 0.f) alpha = 0.f;
    if (alpha > 1.f) alpha = 1.f;
    return alpha;
}





void DriveTo(ue::UObject* target_actor, const ue::FVector& target,
             const ue::FRotator& rotation, const ue::FVector& reported_velocity, bool is_puppet) {


    ue::FVector current = {};
    if (!ue::GetActorLocation(target_actor, &current)) {
        TeleportActor(target_actor, target, rotation);
        return;
    }

    const float dx = target.X - current.X;
    const float dy = target.Y - current.Y;
    const float dz = target.Z - current.Z;
    const float distance = sqrtf(dx * dx + dy * dy);









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





    ue::FRotator new_rotation = rotation;
    ue::FRotator current_rotation = {};
    if (ue::GetActorRotation(target_actor, &current_rotation)) {
        float delta = rotation.Yaw - current_rotation.Yaw;
        while (delta > 180.f) delta -= 360.f;
        while (delta < -180.f) delta += 360.f;
        new_rotation.Yaw = current_rotation.Yaw + delta * FrameRateAdjusted(kYawSmoothing);
    }


    if (distance > kSnapDistance || fabsf(dz) > kVerticalSnap) {
        if (is_puppet) ++snap_count;
        TeleportActor(target_actor, target, new_rotation);
        return;
    }










    if (is_puppet) {
        const float alpha = FrameRateAdjusted(kPositionCorrection);
        ue::FVector corrected = {current.X + dx * alpha, current.Y + dy * alpha,
                                 current.Z + dz * alpha};




        ue::FVector presentation_velocity = {reported_velocity.X, reported_velocity.Y, 0.f};
        float presentation_speed = sqrtf(presentation_velocity.X * presentation_velocity.X +
                                         presentation_velocity.Y * presentation_velocity.Y);
        constexpr float kMaximumPresentationSpeed = 850.f;
        if (presentation_speed > kMaximumPresentationSpeed) {
            const float scale = kMaximumPresentationSpeed / presentation_speed;
            presentation_velocity.X *= scale;
            presentation_velocity.Y *= scale;
        } else if (presentation_speed <= 18.f && distance > 35.f) {


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



        NoteEnemyPresentation(target_actor, presentation_velocity);











        int band = 0;
        if (speed > 18.f) band = speed < 280.f ? 1 : (speed < 600.f ? 2 : 3);
        SetActorSpeedState(target_actor, band);
        return;
    }

}


void DrivePuppetFromSamples(ue::UObject* puppet) {


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




















bool EnsureRemoteVisible(ue::UObject* puppet, bool log_result) {
    if (!puppet) return false;
    struct HiddenParams {
        std::uint8_t bNewHidden[8];
    } show = {};
    const bool ok = ue::CallFunction(puppet, L"SetActorHiddenInGame", &show);
    if (log_result) SC_LOG("puppet: visibility restore %s", ok ? "requested" : "FAILED");
    return ok;
}
















void ConfigureAsRemote(ue::UObject* puppet, ue::UObject* player) {
    const int player_faction = GetFaction(player);
    const bool coop_mode = coop::Get().mode == coop::Mode::Coop;

    if (player_faction >= 0) {





        const int target = coop_mode ? player_faction : (player_faction == 1 ? 0 : 1);
        SetFaction(puppet, target);
        SC_LOG("puppet: faction %d (you are %d) -- %s", target, player_faction,
               coop_mode ? "CO-OP, allied against the level" : "VERSUS, hostile to you");
    } else {
        SC_LOG("puppet: could not read your faction -- leaving the puppet's alone");
    }






    struct CollisionParams {
        std::uint8_t bNewActorEnableCollision[8];
    } collision = {};
    collision.bNewActorEnableCollision[0] = 1;
    const bool collision_on =
        ue::CallFunction(puppet, L"SetActorEnableCollision", &collision);
    SC_LOG("puppet: world collision %s", collision_on ? "enabled" : "FAILED");









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
        response.Channel = 2;
        response.NewResponse = 0;
        pawn_ignore = ue::CallFunction(capsule_result.ReturnValue,
                                       L"SetCollisionResponseToChannel", &response);
    }
    SC_LOG("puppet: pawn collision %s%s",
           !want_pawn_ignore ? "left BLOCKING (puppet_ignores_pawn_collision=0)"
                             : (pawn_ignore ? "ignored" : "FAILED"),
           capsule_class || !want_pawn_ignore ? "" : " (CapsuleComponent class unavailable)");



    const bool invincible = coop::Get().puppet_invincible;
    SetInvincible(puppet, invincible);
    if (!invincible) {
        SC_LOG("puppet: invincibility OFF (puppet_invincible=0) -- their own game still "
               "decides their health; this is only to see whether enemies will commit");
    }
    EnsureRemoteVisible(puppet, true);






    struct Empty {
    } none = {};
    if (ue::CallFunction(puppet, L"SpawnDefaultController", &none)) {
        SC_LOG("puppet: controller spawned (movement component will now tick)");
    } else {
        SC_LOG("puppet: SpawnDefaultController unavailable -- expect sliding");
    }










    if (StopBrain(puppet)) {
        SC_LOG("puppet: AI brain STOPPED -- it had one, and it was fighting the drive");
    } else {
        SC_LOG("puppet: no AI brain found (the drive is the only thing moving it)");
    }
}




















namespace rel = sifucoop::game::relationship;




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


bool TrySetRelationshipBothWays(ue::UObject* player, ue::UObject* puppet, int value,
                                int* out_player, int* out_puppet) {
    ue::UObject* player_social = GetSocialComponent(player);
    ue::UObject* puppet_social = GetSocialComponent(puppet);











    const int was_actor = ReadRelationship(player, puppet);
    const int was_comp = ReadRelationshipViaComponent(player, puppet);

    const int before = RelationshipMapSize(player_social);
    WriteRelationship(player_social, puppet, value);
    WriteRelationship(puppet_social, player, value);
    const int after = RelationshipMapSize(player_social);

    const int back_player = ReadRelationship(player, puppet);
    const int back_puppet = ReadRelationship(puppet, player);


    const int back_comp = ReadRelationshipViaComponent(player, puppet);
    if (out_player) *out_player = back_player;
    if (out_puppet) *out_puppet = back_puppet;

    const bool held = back_player == value && back_puppet == value;
    const bool held_component = back_comp == value;
    const bool map_grew = RelationshipMapProbeTrusted() && before >= 0 && after > before;
    if (held || held_component || map_grew) g_relationship_writes_land = true;



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












void MaintainFriendlyRelationship(ue::UObject* player, ue::UObject* puppet) {
    const coop::Config& config = coop::Get();


    if (!config.friendly_relationship && !config.remote_player_attacks) return;
    if (config.mode != coop::Mode::Coop) return;
    if (!player || !puppet) return;





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


    g_next_relationship_attempt = now + (g_friendly_verified ? 3000 : 1000);

    int back_player = rel::kUnknown;
    int back_puppet = rel::kUnknown;


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

    if (vitals.is_down == g_peer_was_down) return;
    g_peer_was_down = vitals.is_down;




    SetDown(fighter, vitals.is_down);















    const bool peer_is_dead = vitals.max_health > 0.f && vitals.health <= 0.5f;
    if (vitals.is_down && peer_is_dead) {
        NotifyDownStateChanged(fighter, true);
    } else if (!vitals.is_down) {



        NotifyDownStateChanged(fighter, false);
    }
    SC_LOG("puppet: peer %s%s", vitals.is_down ? "went DOWN" : "got back up",
           vitals.is_down && !peer_is_dead ? " (knockdown -- state only)" : "");
}



const char* LevelLeaf(const char* path) {
    if (!path || !path[0]) return "?";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}





bool IsTransientSelectionLevel(const char* path) {
    if (!path || !path[0]) return true;
    static const char* const kNonGameplay[] = {"SelectHideoutLevel", "MainMenu", "Frontend",
                                               "Startup", "EntryLevel", "Hideout_0_Main"};
    for (const char* fragment : kNonGameplay) {
        if (strstr(path, fragment) != nullptr) return true;
    }
    return false;
}






bool CoopBodiesMayExist() {
    if (!g_coop_started || !net::IsConnected()) return false;
    char level[192] = {};
    if (!ue::GetCurrentLevelPath(level, sizeof(level)) || IsTransientSelectionLevel(level)) {
        return false;
    }
    const char* peer_level = net::GetPeerLevel();
    return peer_level && peer_level[0] && _stricmp(level, peer_level) == 0;
}




char g_invite_level[192] = {};
bool g_have_invite = false;




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


    const std::uint32_t invite_id = net::GetPendingInviteId();
    if (invite_id != 0) net::SendInviteReply(invite_id, true);
    g_have_invite = false;
    g_coop_started = true;



    g_arrival_teleport_pending = true;
    SC_LOG("lobby: accepted -- travelling to '%s'", g_invite_level);
    ue::OpenLevel(g_invite_level);
}










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



    const float yaw_radians = facing.Yaw * 3.14159265f / 180.f;
    where.X -= 150.f * cosf(yaw_radians);
    where.Y -= 150.f * sinf(yaw_radians);

    const bool ok = TeleportActor(player, where, facing);
    SC_LOG("teleport: to partner at (%.0f, %.0f, %.0f) -> %s", where.X, where.Y, where.Z,
           ok ? "arrived" : "REFUSED (no room there)");
    if (!ok) coop::ReportProblem("no room to land next to your partner");
}





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





void JoinerFollowHostLevel(const char* current_level, bool have_level) {
    (void)current_level;
    (void)have_level;

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






void UpdateLobby(ue::UObject* player) {
    char current_level[192] = {};
    const bool have_level = ue::GetCurrentLevelPath(current_level, sizeof(current_level));
    const bool connected = net::IsConnected();
    if (connected != g_lobby_was_connected) {
        g_lobby_was_connected = connected;
        g_coop_started = false;
        g_announced_level[0] = '\0';

        g_have_invite = false;
        g_invite_level[0] = '\0';
        g_arrival_teleport_pending = false;
    }
    if (have_level && IsTransientSelectionLevel(current_level) && g_coop_started) {
        g_coop_started = false;
        g_announced_level[0] = '\0';
        SC_LOG("lobby: level picker reached -- co-op waits for the next Start action");
    }


    static DWORD last_presence = 0;
    const DWORD now = GetTickCount();
    if (net::IsConnected() && have_level && now - last_presence > 1000) {
        last_presence = now;
        net::SendLevelPresence(current_level);
    }


    HostAnnounceLevelChanges(current_level, have_level);
    JoinerFollowHostLevel(current_level, have_level);









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


            net::BumpLevelRequest();
            net::SendLevelSync(requests.level);
            SC_LOG("lobby: travelling to '%s' and inviting peer", requests.level);
            ue::OpenLevel(requests.level);
        }
    }



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








    static DWORD last_heartbeat = 0;
    const coop::Stats& stats = coop::GetStats();
    static DWORD last_rx_count = 0;
    static DWORD last_dropped = 0;
    if (net::IsConnected() && now - last_heartbeat > 5000) {
        const DWORD elapsed = now - last_heartbeat;
        last_heartbeat = now;






        const DWORD rx_now = stats.packets_received;
        const DWORD dropped_now = stats.packets_dropped;
        const DWORD rx_delta = rx_now - last_rx_count;
        const DWORD dropped_delta = dropped_now - last_dropped;
        last_rx_count = rx_now;
        last_dropped = dropped_now;
        const int rx_rate = elapsed > 0 ? static_cast<int>(rx_delta * 1000 / elapsed) : 0;







        static DWORD last_snapshots = 0;
        const DWORD snapshots_now = stats.snapshots_received;
        const int snapshot_rate =
            elapsed > 0 ? static_cast<int>((snapshots_now - last_snapshots) * 1000 / elapsed) : 0;
        last_snapshots = snapshots_now;

        SC_LOG("coop: %s rtt=%dms | levels me='%s' peer='%s' %s | enemies known=%d active=%d "
               "driven=%d unmatched=%d | dmg out=%.0f in=%.0f | attacks=%u | rejected=%u "






               "| rx=%d/s peerpos=%d/s seqgap=%u (%u total)",
               net::GetRole() == net::Role::Host ? "HOST" : "JOIN", net::GetRoundTripMs(),
               status.my_level, status.peer_level, status.together ? "TOGETHER" : "apart",
               stats.enemies_known, stats.enemies_active, stats.enemies_driven,
               stats.enemies_unmatched, stats.damage_reported_total, stats.damage_applied_total,
               stats.attacks_echoed, stats.packets_rejected,
               rx_rate, snapshot_rate, dropped_delta, dropped_now);
    }


    static bool announced_connected = false;
    if (net::IsConnected() && !announced_connected) {
        announced_connected = true;
        SC_LOG("coop: === SESSION LIVE as %s ===",
               net::GetRole() == net::Role::Host ? "HOST" : "JOINER");
    } else if (!net::IsConnected()) {
        announced_connected = false;
    }
}

}

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
    ue::FVector peer_velocity = {};
    bool have_peer_spawn = false;
    if (!ue::GetActorLocation(player, &location) || !ue::GetActorRotation(player, &rotation)) {
        SC_LOG("puppet: could not read player transform");
        return false;
    }




    have_peer_spawn = net::IsConnected() &&
                      net::GetPeerTransform(&location, &rotation, &peer_velocity);
    if (!have_peer_spawn) {
        const float yaw_radians = rotation.Yaw * 3.14159265f / 180.f;
        location.X += 200.f * cosf(yaw_radians);
        location.Y += 200.f * sinf(yaw_radians);
        rotation.Yaw += 180.f;
    }





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
    if (have_peer_spawn) {

        DriveTo(spawned, location, rotation, peer_velocity, true);
    }













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



    ResetSamples();




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

    return g_puppet_world == ue::GetWorld() ? g_puppet : nullptr;
}

bool FriendlyRelationshipVerified() { return g_friendly_verified; }

void DespawnPuppet() {
    if (!g_puppet) {
        SC_LOG("puppet: nothing to despawn");
        return;
    }



    NotifyPuppetWillBeDestroyed(g_puppet);



    if (SecondPlayerActive()) {


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



    g_cosmetic_attack_montage_until = GetTickCount() + 750;
}

void NotifyLocalSequenceSent() {

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

        g_have_local_velocity = false;


        InvalidateAttackTemplate();
    }

    ue::FVector location = {};
    ue::FRotator rotation = {};
    if (!ue::GetActorLocation(player, &location) || !ue::GetActorRotation(player, &rotation)) {
        return;
    }
    RecordSample(location, rotation);













    ue::FVector local_velocity = {};
    const float frame_seconds = sifucoop::hooks::FrameDeltaSeconds();
    const auto finite3 = [](const ue::FVector& v) {
        return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
    };
    if (!GetActorVelocity(player, &local_velocity) || !finite3(local_velocity)) {
        local_velocity = {};

        if (g_have_local_velocity && frame_seconds > 0.001f && frame_seconds < 0.25f) {
            const float vx = (location.X - g_last_local_location.X) / frame_seconds;
            const float vy = (location.Y - g_last_local_location.Y) / frame_seconds;
            const float vz = (location.Z - g_last_local_location.Z) / frame_seconds;
            const float speed = sqrtf(vx * vx + vy * vy + vz * vz);


            if (speed <= 3000.f) local_velocity = {vx, vy, vz};
        }
    }
    g_last_local_location = location;
    g_have_local_velocity = true;







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






    static float last_visual_health = -1.f;
    static float last_visual_guard = -1.f;
    static DWORD player_victim_visual_until = 0;
    if (local.state_valid) {
        const bool health_drop = last_visual_health >= 0.f &&
            local.health + 0.05f < last_visual_health;
        const bool guard_drop = last_visual_guard >= 0.f &&
            local.guard + 0.05f < last_visual_guard;
        if (health_drop || guard_drop) player_victim_visual_until = GetTickCount() + 250;
        last_visual_health = local.health;
        last_visual_guard = local.guard;
    } else {
        last_visual_health = -1.f;
        last_visual_guard = -1.f;
    }















    if (coop::Get().sync_montages && net::IsConnected()) {
        ue::UObject* anim_instance = ue::GetAnimInstance(player);
        if (anim_instance) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(anim_instance);
            ue::UObject* action = nullptr;
            std::memcpy(&action, bytes + kAnimLastActionAnim, sizeof(action));
            float cursor = 0.f;
            std::memcpy(&cursor, bytes + kAnimLastActionCursor, sizeof(cursor));











            static ue::UObject* last_action_sent = nullptr;
            static float last_cursor = 0.f;
            const bool restarted = cursor + 0.01f < last_cursor;
            const bool changed = action != last_action_sent;
            last_cursor = cursor;
            if (action && (changed || restarted)) {
                last_action_sent = action;
                char action_path[192] = {};
                if (ue::GetObjectPathName(action, action_path, sizeof(action_path))) {
                    const bool main_character_asset =
                        strncmp(action_path, "/Game/Animations/MainChar/", 26) == 0;
                    const bool paired_defender_asset =
                        strstr(action_path, "_defender") != nullptr ||
                        strstr(action_path, "_hitted") != nullptr ||
                        strstr(action_path, "_Hitted") != nullptr;
                    const bool player_was_just_hit = player_victim_visual_until != 0 &&
                        static_cast<LONG>(player_victim_visual_until - GetTickCount()) > 0;
                    const bool paired_player_reaction =
                        paired_defender_asset && player_was_just_hit;
                    if (!main_character_asset && !paired_player_reaction) {


                        static unsigned int refused_wrong_actor = 0;
                        if (++refused_wrong_actor <= 8 || coop::Get().verbose_orders) {
                            SC_LOG("action: refused non-player asset from player channel '%s'",
                                   action_path);
                        }
                    } else {
                        if (paired_player_reaction) {
                            SC_LOG("action: paired defender visual accepted for the hit player '%s'",
                                   action_path);
                        }


                        net::SendAnimationSequence(action_path, 0,
                                                   net::AnimationSemantic::Generic, cursor);
                        static unsigned int actions_sent = 0;
                        if (++actions_sent <= 5 || coop::Get().verbose_orders) {
                            SC_LOG("action: sent '%s' cursor=%.2f", action_path, cursor);
                        }
                    }
                }
            } else if (!action) {
                last_action_sent = nullptr;
            }
        }
    }






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


    PumpRemoteOrders();
    TickEnemyCinematics();



    for (int event = 0; event < 8; ++event) {
        char path[192] = {};
        float position = 0.f;
        net::AnimationAssetKind asset_kind = net::AnimationAssetKind::Montage;
        std::uint32_t actor_hash = 0;
        net::AnimationSemantic semantic = net::AnimationSemantic::Generic;
        if (!net::PopMontageState(path, sizeof(path), &position, &asset_kind,
                                  &actor_hash, &semantic)) {
            break;
        }
        const bool raw_sequence = asset_kind == net::AnimationAssetKind::Sequence;
        const bool pose_asset = asset_kind == net::AnimationAssetKind::PoseAsset;
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

        if (pose_asset) {
            if (actor_hash == 0 || semantic != net::AnimationSemantic::Reaction ||
                EnemyRunsLocalBrain(actor_hash)) {
                continue;
            }
            if (!ue::ObjectClassIs(animation, "PoseAsset")) {
                SC_LOG("reaction: refused non-PoseAsset payload for %08X", actor_hash);
                continue;
            }
            if (SetCurrentPoseAsset(visual_actor, animation)) {
                static unsigned int applied_pose_logs = 0;
                ++applied_pose_logs;
                if (applied_pose_logs <= 8 || coop::Get().verbose_orders) {
                    SC_LOG("reaction: observer pose applied to %08X '%s'", actor_hash, path);
                }
            }
            continue;
        }








        if (actor_hash && EnemyRunsLocalBrain(actor_hash) && !enemy_death) continue;






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

        }

        if (raw_sequence) {














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





    net::ConsumeConnectedEvent();
    if (net::ConsumeDisconnectedEvent()) {
        SC_LOG("net: peer left -- despawning puppet");
        DespawnPuppet();
    }





    if (CoopBodiesMayExist() && !g_puppet) {
        static DWORD last_attempt = 0;
        const DWORD now = GetTickCount();
        if (now - last_attempt > 1000) {
            last_attempt = now;
            if (SpawnPuppet()) g_puppet_auto_spawned = true;
        }
    } else if (!net::IsConnected() && g_puppet && g_puppet_auto_spawned) {




        SC_LOG("net: peer gone -- removing its puppet");
        DespawnPuppet();
    }








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





    if (coop::Get().real_second_player) {


        const bool maintain_real = CoopBodiesMayExist() ||
                                   (g_puppet && !g_puppet_auto_spawned);
        if (maintain_real) {
            ue::UObject* pawn = MaintainSecondPlayer();
            if (pawn && pawn != g_puppet) {
                g_puppet = pawn;




                g_puppet_auto_spawned = net::IsConnected();
            } else if (!pawn && SecondPlayerActive()) {

                g_puppet = nullptr;
            }
        }
    }

    if (!g_puppet) return;



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


    MaintainFriendlyRelationship(player, g_puppet);


    ue::FVector peer_location = {};
    ue::FRotator peer_rotation = {};
    ue::FVector peer_velocity = {};
    if (net::IsConnected() && net::GetPeerTransform(&peer_location, &peer_rotation, &peer_velocity)) {
        DriveTo(g_puppet, peer_location, peer_rotation, peer_velocity, true);
        return;
    }

    if (g_follow_enabled) DrivePuppetFromSamples(g_puppet);
}

}

