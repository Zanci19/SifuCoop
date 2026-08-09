#include "puppet.h"

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
#include "native_net.h"
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

// Weak: the puppet can be destroyed by the game (level transition, respawn),
// so this is validated before use rather than trusted.
ue::UObject* g_puppet = nullptr;
bool g_coop_started = false;
char g_announced_level[192] = {};
bool g_lobby_was_connected = false;


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

// Twice corrected. 12 units was too tight -- one frame of movement overshot it
// and the puppet oscillated. 45 was too loose -- it stopped dead, waited for the
// target to pull away, then lurched, which read as stop-go motion.
//
// The deadzone is now small and the input is proportional with a *low* floor, so
// movement shrinks smoothly as it closes rather than being cut off at a
// threshold. A high floor is what forces the overshoot the deadzone then has to
// absorb; with a low one it converges instead of bouncing.
constexpr float kArrivedDistance = 8.f;
constexpr float kSlowdownDistance = 90.f;
constexpr float kMinimumScale = 0.06f;

// Fraction of the remaining yaw error closed per frame. Low enough to smooth
// the peer's per-snapshot yaw wobble, high enough that a real turn resolves in
// a few frames. Applied along the shortest arc in DriveTo.
constexpr float kYawSmoothing = 0.25f;

// Fraction of the remaining horizontal error closed outright each frame, on top
// of the movement input. Low enough to stay smooth (it is exponential decay, not
// a step), high enough that the puppet stops trailing a running peer by
// hundreds of units and then teleporting.
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

// Steering rather than teleporting is what produces locomotion animation.
//
// Teleporting sets position directly, so the movement component never
// accumulates velocity, so the anim graph sees speed 0 and plays idle -- the
// character slid around in a standing pose. AddMovementInput goes through the
// movement component instead, which produces real velocity and therefore real
// walking and running animation. It also requires the driven character to have
// a controller, or the movement component never ticks at all.
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
        SC_LOG("puppet: chase lag avg=%.0f peak=%.0f units over %d frames, %d snaps, "
               "%d corrections refused",
               distance_sum / (distance_samples > 0 ? distance_samples : 1), distance_peak,
               distance_samples, snap_count, correction_failed);
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

    // Native movement consumes a velocity rather than a controller input. It is
    // the path Sifu's fighting movement component uses to update AnimBP speed.
    constexpr float kCatchupSeconds = 0.12f;
    constexpr float kMaximumDriveSpeed = 1200.f;

    // Keep following the host's real motion even after the positional error is
    // small. Previously this was initialised to zero and only received the
    // remote velocity once the error exceeded 8 units. At 60 Hz that threshold
    // is crossed every few frames, making an otherwise caught-up character
    // alternately run and idle (the visible "morphing" report).
    ue::FVector desired_velocity = {reported_velocity.X, reported_velocity.Y, 0.f};
    if (distance > kArrivedDistance) {
        desired_velocity.X += dx / kCatchupSeconds;
        desired_velocity.Y += dy / kCatchupSeconds;
    }
    const float desired_speed = sqrtf(desired_velocity.X * desired_velocity.X +
                                      desired_velocity.Y * desired_velocity.Y);
    if (desired_speed > kMaximumDriveSpeed) {
        const float scale = kMaximumDriveSpeed / desired_speed;
        desired_velocity.X *= scale;
        desired_velocity.Y *= scale;
    }

    const bool direct_steering =
        is_puppet ? false : RequestDirectMove(target_actor, desired_velocity);
    if (!direct_steering && distance > kArrivedDistance) {
        struct MoveParams {
            ue::FVector WorldDirection;
            float ScaleValue;
            bool bForce;
        } move = {};
        move.WorldDirection = {dx / distance, dy / distance, 0.f};
        // Proportional approach: full input while far, easing towards a floor
        // as it arrives. A constant scale is what made it overshoot and bounce.
        float scale = distance / kSlowdownDistance;
        if (scale > 1.f) scale = 1.f;
        if (scale < kMinimumScale) scale = kMinimumScale;
        move.ScaleValue = scale;
        move.bForce = true;

        if (!ue::CallFunction(target_actor, L"AddMovementInput", &move) && is_puppet) ++correction_failed;

    }

    // A network player must actually be walked by its movement component.  A
    // per-frame transform write makes the visual location correct, but resets
    // the component velocity to zero, which leaves the character gliding in
    // the idle pose.  AddMovementInput above is therefore the authoritative
    // movement path for a puppet too; reserve teleports for the large-error
    // guard above.  Rotation still needs to be applied here because movement
    // input is in world space and does not turn the actor by itself.
    if (is_puppet) {
        struct RotationParams {
            ue::FRotator NewRotation;
            bool bTeleportPhysics;
            bool ReturnValue;
        } rot = {};
        rot.NewRotation = new_rotation;
        if (!ue::CallFunction(target_actor, L"K2_SetActorRotation", &rot)) ++correction_failed;
        return;
    }

    // Never transform-correct an active enemy here: direct movement preserves
    // collision sweeps and hit detection. The large-gap guard above is enough.

    // Arrived: hold position, and only keep the facing up to date.
    struct RotationParams {
        ue::FRotator NewRotation;
        bool bTeleportPhysics;
        bool ReturnValue;
    } rot = {};
    rot.NewRotation = new_rotation;
    ue::CallFunction(target_actor, L"K2_SetActorRotation", &rot);
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
void ConfigureAsRemote(ue::UObject* puppet, ue::UObject* player) {
    const int player_faction = GetFaction(player);
    const bool coop_mode = coop::Get().mode == coop::Mode::Coop;

    if (player_faction >= 0) {
        // Faction 0 and 1 are the two sides; picking "the other one" rather
        // than a constant means this still works if the player's faction ever
        // differs by level.
        const int target = coop_mode ? player_faction : (player_faction == 1 ? 0 : 1);
        SetFaction(puppet, target);
        SC_LOG("puppet: faction %d (you are %d) -- %s", target, player_faction,
               coop_mode ? "CO-OP, allied against the level" : "VERSUS, hostile to you");
    } else {
        SC_LOG("puppet: could not read your faction -- leaving the puppet's alone");
    }

    // A remote peer can occupy the same world-space point as the local player.
    // Disable only this cosmetic peer actor's physical blocking so an overlap
    // cannot make the engine reject its next interpolated teleport. Enemy AI
    // still receives an actor target and attacks are mirrored explicitly.
    struct CollisionParams {
        std::uint8_t bNewActorEnableCollision[8];
    } collision = {};
    const bool collision_off =
        ue::CallFunction(puppet, L"SetActorEnableCollision", &collision);
    SC_LOG("puppet: peer collision %s", collision_off ? "disabled" : "NOT disabled");
    SetInvincible(puppet, true);

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

// BPF_GetRelationship(AActor* Actor) -> ERelationshipTypes (returned as a byte).
constexpr int kRelationUnknown = -1;
int ReadRelationship(ue::UObject* from_actor, ue::UObject* to_actor) {
    if (!from_actor || !to_actor) return kRelationUnknown;
    struct Params {
        ue::UObject* Actor;
        std::uint8_t ReturnValue;
    } params = {};
    params.Actor = to_actor;
    if (!ue::CallFunction(from_actor, L"BPF_GetRelationship", &params)) {
        return kRelationUnknown;
    }
    return params.ReturnValue;
}

// ERelationshipTypes, recovered from the shipped executable's own generated
// enumerator-name table (contiguous 0x20-spaced strings at 0x04A420D0, in
// declaration order):
//
//   0 Enemy   1 Fight   2 Object   3 Neutral   4 Coop   5 Ally   6 Count   7 None
//
// Both halves of this were confirmed against a live two-machine session before
// being relied on: sampling two live enemies returned 5 (Ally, which is exactly
// what two same-faction characters should be), and reading back the relationship
// between the two players returned 3 (Neutral, which is exactly what two players
// standing in the hideout should be). A table that predicts both observations is
// not a guess.
//
// The headline is the fourth entry. **Sifu ships a `Coop` relationship type.**
// The game already has a name for what this mod is trying to express, so that is
// what gets asked for first -- ahead of Ally, which is merely "these two are on
// the same side".
constexpr int kRelationCoop = 4;
constexpr int kRelationAlly = 5;
constexpr int kRelationCount = 6;

// Learn the "friendly" enum value from two distinct active enemies. Enemies
// share the level's hostile faction, so their relationship to each other is the
// friendly value we want to copy onto the puppet pair. Returns -1 until two
// enemies can be sampled.
//
// Kept as a LAST resort rather than the primary source. It was the primary
// source, and a live session showed why that was weak: it can only answer while
// two enemies happen to be active, and what it learns is Ally, not the Coop
// value the game actually has for this.
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
        if (value != kRelationUnknown) return value;
    }
    return kRelationUnknown;
}

// USocialComponent* ABaseCharacter::BPF_GetSocialComponent().
ue::UObject* GetSocialComponent(ue::UObject* character) {
    if (!character) return nullptr;
    struct Params {
        ue::UObject* ReturnValue;
    } params = {};
    if (!ue::CallFunction(character, L"BPF_GetSocialComponent", &params)) return nullptr;
    return params.ReturnValue;
}

// USocialComponent::BPF_ServerChangeRelationship(AActor* Actor, ERelationshipTypes).
bool SetRelationship(ue::UObject* social, ue::UObject* toward, int value) {
    if (!social || !toward || value < 0) return false;
    struct Params {
        ue::UObject* Actor;
        std::uint8_t eRelation;
    } params = {};
    params.Actor = toward;
    params.eRelation = static_cast<std::uint8_t>(value);
    return ue::CallFunction(social, L"BPF_ServerChangeRelationship", &params);
}

ue::UObject* g_friendly_applied_for = nullptr;
bool g_friendly_verified = false;
int g_relationship_that_stuck = kRelationUnknown;
DWORD g_next_relationship_attempt = 0;

// Set both directions to `value` and report whether the game actually kept it.
bool TrySetRelationshipBothWays(ue::UObject* player, ue::UObject* puppet, int value,
                                int* out_player, int* out_puppet) {
    SetRelationship(GetSocialComponent(player), puppet, value);
    SetRelationship(GetSocialComponent(puppet), player, value);
    const int back_player = ReadRelationship(player, puppet);
    const int back_puppet = ReadRelationship(puppet, player);
    if (out_player) *out_player = back_player;
    if (out_puppet) *out_puppet = back_puppet;
    return back_player == value && back_puppet == value;
}

// Establish -- and KEEP -- a non-hostile relationship between the two players.
//
// Rewritten after a live session showed the first version failing silently in a
// way only a readback could reveal: it set the value sampled from two enemies
// (5 = Ally), the setter reported success, and reading it back returned 3
// (Neutral). The write did not take, so remote attacks correctly stayed off --
// but the reason was invisible until the enum was decoded.
//
// Three changes follow from that. Coop is tried FIRST, because the game has a
// relationship type by that name and it is plainly the one this mod means.
// Candidates are tried in turn and each is verified, so the value that survives
// is chosen by the game rather than by us. And it is RE-ASSERTED on a timer
// rather than applied once: `ABaseCharacter::UpdateRelationshipToOtherCharacters`
// exists, which means something recomputes these, and a value that holds for one
// frame is worth nothing to a swing thrown ten seconds later.
void MaintainFriendlyRelationship(ue::UObject* player, ue::UObject* puppet) {
    const coop::Config& config = coop::Get();
    // remote_player_attacks depends on this, so wanting remote attacks is
    // itself a reason to establish the exemption.
    if (!config.friendly_relationship && !config.remote_player_attacks) return;
    if (config.mode != coop::Mode::Coop) return;
    if (!player || !puppet) return;

    if (g_friendly_applied_for != puppet) {
        g_friendly_applied_for = puppet;
        g_friendly_verified = false;
        g_relationship_that_stuck = kRelationUnknown;
        g_next_relationship_attempt = 0;
    }

    const DWORD now = GetTickCount();
    if (g_next_relationship_attempt != 0 && now < g_next_relationship_attempt) return;
    // Confirmed pairs are re-checked lazily; unconfirmed ones retry briskly,
    // because until one sticks the remote player cannot swing at all.
    g_next_relationship_attempt = now + (g_friendly_verified ? 3000 : 1000);

    int back_player = kRelationUnknown;
    int back_puppet = kRelationUnknown;

    // Already found one the game keeps: just hold it there.
    if (g_relationship_that_stuck != kRelationUnknown) {
        const bool still = TrySetRelationshipBothWays(player, puppet,
                                                      g_relationship_that_stuck,
                                                      &back_player, &back_puppet);
        if (still != g_friendly_verified) {
            g_friendly_verified = still;
            SC_LOG("puppet: relationship %d %s (readback %d/%d)", g_relationship_that_stuck,
                   still ? "re-confirmed" : "STOPPED HOLDING -- searching again",
                   back_player, back_puppet);
        }
        if (!still) g_relationship_that_stuck = kRelationUnknown;
        return;
    }

    const int candidates[] = {kRelationCoop, kRelationAlly, DiscoverFriendlyRelationValue()};
    for (const int value : candidates) {
        if (value == kRelationUnknown || value < 0 || value >= kRelationCount) continue;
        if (!TrySetRelationshipBothWays(player, puppet, value, &back_player, &back_puppet)) {
            continue;
        }
        g_relationship_that_stuck = value;
        g_friendly_verified = true;
        SC_LOG("puppet: relationship %d (%s) STUCK -- readback %d/%d, remote attacks may play",
               value, value == kRelationCoop ? "Coop" : (value == kRelationAlly ? "Ally" : "?"),
               back_player, back_puppet);
        return;
    }

    // Nothing held. Say so once rather than every second, and say what was
    // actually read, because that number is the whole diagnosis.
    static int last_reported = -2;
    if (last_reported != back_player) {
        last_reported = back_player;
        SC_LOG("puppet: no relationship value would stick (last readback %d/%d) -- "
               "remote attacks stay OFF. 0=Enemy 1=Fight 2=Object 3=Neutral 4=Coop 5=Ally",
               back_player, back_puppet);
        coop::ReportProblem("could not turn friendly fire off -- "
                            "your partner will move but not swing");
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
    return path && strstr(path, "SelectHideoutLevel") != nullptr;
}

// The host's most recent standing invite, held until the joining player takes
// it. Kept as a level path rather than a flag so it survives the retries the
// host sends and still names a destination when accepted seconds later.
char g_invite_level[192] = {};
bool g_have_invite = false;

bool g_clear_menus_after_travel = false;
int g_clear_menus_delay = 0;

void ClearLingeringMenus() {
    ue::UObject* world = ue::GetWorld();
    if (!world) return;
    ue::UObject* controller = PrimaryPlayerController();
    if (!controller) return;

    struct Empty {
    } none = {};
    const bool cleared = ue::CallFunction(controller, L"BPF_ClearMenuStack", &none);
    const bool focused = ue::CallFunction(controller, L"GiveFocusToGameViewport", &none);
    SC_LOG("lobby: dismissed leftover front-end menus (clear=%s focus=%s)",
           cleared ? "ok" : "unavailable", focused ? "ok" : "unavailable");
}

void AcceptInvite() {
    if (!g_have_invite || !g_invite_level[0]) {
        SC_LOG("lobby: no invite to accept");
        coop::ReportProblem("no invite pending");
        return;
    }
    g_have_invite = false;
    g_coop_started = true;
    g_clear_menus_after_travel = true;
    g_clear_menus_delay = 0;
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

bool PeerInSameLevel() {
    if (!net::IsConnected()) return false;
    const char* peer = net::GetPeerLevel();
    char mine[192] = {};
    const bool have_mine = ue::GetCurrentLevelPath(mine, sizeof(mine));
    const bool same = have_mine && peer && peer[0] && !IsTransientSelectionLevel(mine) &&
                      _stricmp(mine, peer) == 0;

    static bool last_state = false;
    static DWORD last_log = 0;
    const DWORD now = GetTickCount();
    if (!same && (last_state || now - last_log > 10000)) {
        last_log = now;
        SC_LOG("lobby: holding the remote player back -- you are in '%s', partner reports "
               "'%s'", have_mine ? LevelLeaf(mine) : "?", (peer && peer[0]) ? LevelLeaf(peer) : "?");
    }
    last_state = same;
    return same;
}

void InvitePeerHere(const char* current_level, bool have_level) {
    if (net::GetRole() != net::Role::Host) {
        SC_LOG("lobby: only the host can start (you are joining)");
        coop::ReportProblem("only the host can invite");
    } else if (!net::IsConnected()) {
        SC_LOG("lobby: nobody connected yet");
        coop::ReportProblem("nobody connected yet");
    } else if (!have_level || IsTransientSelectionLevel(current_level)) {
        SC_LOG("lobby: not in a level yet -- load one first");
        coop::ReportProblem("load a level before inviting");
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
            SC_LOG("lobby: already in '%s'", invited);
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
        if (requests.apply_network) {
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
        if (requests.teleport_to_peer) TeleportToPeer(current_level, have_level);
        if (requests.native_start) {
            if (!coop::NativeNetworkActive()) {
                coop::ReportProblem("enable Engine networking, save, then restart first");
            } else if (requests.host_mode) {
                native_net::HostCurrentLevel(requests.port);
            } else {
                native_net::JoinHost(requests.address, requests.port);
            }
        }
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

void InitPuppet(std::uintptr_t base) {
    g_spawn_actor = reinterpret_cast<SpawnActorFn>(base + offsets::UWorld_SpawnActor_VecRot);
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

ue::UObject* GetPuppet() { return g_puppet; }

bool FriendlyRelationshipVerified() { return g_friendly_verified; }

void DespawnPuppet() {
    if (!g_puppet) {
        SC_LOG("puppet: nothing to despawn");
        return;
    }
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
    g_puppet_auto_spawned = false;
    g_friendly_applied_for = nullptr;
    g_friendly_verified = false;
}

void TickPuppet() {
    ue::UObject* world = ue::GetWorld();
    if (!world) return;
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player) return;
    // Native networking uses real AThePlainesGameMode-spawned pawns. Never
    // create or drive a local imitation beside them.
    if (coop::NativeNetworkActive()) {
        // The overlay runs on the render thread, so requests must still be
        // consumed here on the game thread. Previously this early return
        // skipped the queue entirely, leaving the Engine Host/Join buttons
        // apparently inert.
        ui::MenuRequests requests;
        if (ui::TakeMenuRequests(&requests)) {
            if (requests.save_config) coop::Save();
            if (requests.native_start) {
                if (requests.host_mode) {
                    native_net::HostCurrentLevel(requests.port);
                } else {
                    native_net::JoinHost(requests.address, requests.port);
                }
            }
        }
        native_net::TickNativeCoop();
        return;
    }

    // The puppet is destroyed along with the level it was spawned into, and
    // nothing tells us -- so the pointer has to be dropped when the world
    // changes or every subsequent frame dereferences freed memory. This was
    // survivable while a level change needed a deliberate keypress; now that
    // the joiner follows the host automatically it is on the ordinary path
    // through a playthrough, and it must not be a crash.
    //
    // Deliberately not DespawnPuppet(): there is nothing left to destroy, and
    // calling into the corpse is exactly what we are avoiding.
    if (g_clear_menus_after_travel && ue::GetPlayerCharacter(world, 0)) {
        if (++g_clear_menus_delay >= 120) {
            g_clear_menus_after_travel = false;
            g_clear_menus_delay = 0;
            ClearLingeringMenus();
        }
    }

    static ue::UObject* puppet_world = nullptr;
    if (world != puppet_world) {
        puppet_world = world;
        if (g_puppet) {
            SC_LOG("puppet: level changed -- forgetting the old remote character");
            g_puppet = nullptr;
            // Do not touch the cached controller here. This callback happens
            // while Unreal is tearing the old world down, so even asking it for
            // K2_GetPawn can enter ProcessEvent with a freed outer. The normal
            // maintenance path resolves controller 1 from the settled new world
            // after a short guard interval.
            g_puppet_auto_spawned = false;
            g_peer_was_down = false;
            g_friendly_applied_for = nullptr;
    g_friendly_verified = false;
        }
    }

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
    ue::FVector local_velocity = {};
    const float frame_seconds = sifucoop::hooks::FrameDeltaSeconds();
    if (g_have_local_velocity && frame_seconds > 0.001f && frame_seconds < 0.25f) {
        const float vx = (location.X - g_last_local_location.X) / frame_seconds;
        const float vy = (location.Y - g_last_local_location.Y) / frame_seconds;
        const float vz = (location.Z - g_last_local_location.Z) / frame_seconds;
        const float speed = sqrtf(vx * vx + vy * vy + vz * vz);
        // A spawn/travel correction is not a movement velocity. Do not turn it
        // into a multi-frame sprint on the peer's character.
        if (speed <= 3000.f) local_velocity = {vx, vy, vz};
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

    // Cosmetic animation mirroring, edge-triggered on montage change.
    //
    // Sifu drives combat through Orders and barely uses montages -- measured at
    // two montage changes across two minutes of fighting -- so this carries
    // dodges and traversal rather than strikes. It is cheap and additive: when
    // nothing is playing, nothing is sent.
    if (coop::Get().sync_montages && net::IsConnected()) {
        static ue::UObject* last_sent = nullptr;
        ue::AnimState anim = {};
        if (ue::ReadAnimState(player, &anim) && anim.montage != last_sent) {
            last_sent = anim.montage;
            if (anim.montage) {
                char path[192] = {};
                if (ue::GetObjectPathName(anim.montage, path, sizeof(path))) {
                    net::SendMontageState(path, anim.position);
                }
            }
        }
    }

    // Apply the peer's animation to their puppet as a PURE VISUAL: it animates
    // the motion without running the attack, so no hitbox is spawned and it
    // cannot damage anyone. Their damage already resolved on their machine.
    if (g_puppet) {
        char path[192] = {};
        float position = 0.f;
        if (net::PopMontageState(path, sizeof(path), &position)) {
            wchar_t wide[192] = {};
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 192);
            if (ue::UObject* montage = ue::FindObjectByPath(wide)) {
                ue::AnimState state;
                state.montage = montage;
                state.position = position;
                ue::ApplyAnimState(g_puppet, state);
            }
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

    if (net::IsConnected() && PeerInSameLevel() && !g_puppet) {
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

    UpdateLobby(player);

    // Everything the peer did since the last frame: their own moves onto the
    // puppet, and the host's enemy swings onto our driven enemies.
    PumpRemoteOrders();

    // The second player's pawn is rebuilt by the game mode on death, on aging and
    // on every level change, so it is re-resolved here rather than trusted. This
    // also creates the player in the first place once a peer is connected.
    if (coop::Get().real_second_player) {
        if (SecondPlayerActive() || (net::IsConnected() && PeerInSameLevel())) {
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

