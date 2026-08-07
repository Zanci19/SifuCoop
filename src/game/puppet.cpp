#include "puppet.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

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

bool g_spawn_key_was_down = false;
bool g_despawn_key_was_down = false;
bool g_follow_key_was_down = false;

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

// Steering rather than teleporting is what produces locomotion animation.
//
// Teleporting sets position directly, so the movement component never
// accumulates velocity, so the anim graph sees speed 0 and plays idle -- the
// character slid around in a standing pose. AddMovementInput goes through the
// movement component instead, which produces real velocity and therefore real
// walking and running animation. It also requires the driven character to have
// a controller, or the movement component never ticks at all.
void DriveTo(ue::UObject* target_actor, const ue::FVector& target,
             const ue::FRotator& rotation) {
    static bool movement_available = true;

    ue::FVector current = {};
    if (!movement_available || !ue::GetActorLocation(target_actor, &current)) {
        TeleportActor(target_actor, target, rotation);
        return;
    }

    const float dx = target.X - current.X;
    const float dy = target.Y - current.Y;
    const float dz = target.Z - current.Z;
    const float distance = sqrtf(dx * dx + dy * dy);

    // Too far to walk back convincingly, or a floor change: snap.
    if (distance > kSnapDistance || fabsf(dz) > kVerticalSnap) {
        TeleportActor(target_actor, target, rotation);
        return;
    }

    if (distance > kArrivedDistance) {
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

        if (!ue::CallFunction(target_actor, L"AddMovementInput", &move)) {
            movement_available = false;
            SC_LOG("puppet: AddMovementInput unavailable -- falling back to teleport");
            TeleportActor(target_actor, target, rotation);
            return;
        }
    }

    // Facing is set directly: it is not physically simulated, and letting the
    // movement component turn the character would lag behind the peer's aim.
    //
    // Snapping straight to the peer's yaw every frame is what made the puppet
    // twitch: the peer's reported yaw jitters a few degrees per snapshot, and
    // the puppet mirrored every wobble instantly. Instead we interpolate the
    // yaw toward the target along the shortest arc, which absorbs that jitter
    // while still tracking real turns closely (the factor is high enough that a
    // genuine 90-degree turn completes in a handful of frames).
    ue::FRotator new_rotation = rotation;
    ue::FRotator current_rotation = {};
    if (ue::GetActorRotation(target_actor, &current_rotation)) {
        float delta = rotation.Yaw - current_rotation.Yaw;
        while (delta > 180.f) delta -= 360.f;
        while (delta < -180.f) delta += 360.f;
        new_rotation.Yaw = current_rotation.Yaw + delta * kYawSmoothing;
    }

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

    DriveTo(puppet, sample.location, sample.rotation);
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

// Learn the "friendly" enum value from two distinct active enemies. Enemies
// share the level's hostile faction, so their relationship to each other is the
// friendly value we want to copy onto the puppet pair. Returns -1 until two
// enemies can be sampled.
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

// Applied once per puppet instance; re-applied if the puppet actor changes.
ue::UObject* g_friendly_applied_for = nullptr;

void MaintainFriendlyRelationship(ue::UObject* player, ue::UObject* puppet) {
    if (!coop::Get().friendly_relationship) return;
    if (coop::Get().mode != coop::Mode::Coop) return;
    if (!player || !puppet) return;
    if (g_friendly_applied_for == puppet) return;  // already done for this puppet

    const int friendly = DiscoverFriendlyRelationValue();
    if (friendly == kRelationUnknown) return;  // not enough enemies yet; retry later

    ue::UObject* player_social = GetSocialComponent(player);
    ue::UObject* puppet_social = GetSocialComponent(puppet);
    const bool a = SetRelationship(player_social, puppet, friendly);
    const bool b = SetRelationship(puppet_social, player, friendly);

    if (a || b) {
        g_friendly_applied_for = puppet;
        SC_LOG("puppet: friendly relationship set (value=%d, player<-%s puppet<-%s) -- "
               "UNVERIFIED that melee gates on this",
               friendly, a ? "ok" : "FAIL", b ? "ok" : "FAIL");
    } else {
        SC_LOG("puppet: friendly relationship could not be applied "
               "(BPF_ServerChangeRelationship unavailable)");
        g_friendly_applied_for = puppet;  // do not retry every frame if unsupported
    }
}

bool g_peer_was_down = false;

// True only when the puppet was created by a peer connecting, so a manual F9
// spawn is never removed by the connection logic.
bool g_puppet_auto_spawned = false;

void ApplyPeerVitals(ue::UObject* puppet, const net::PeerVitals& vitals) {
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

bool KeyPressed(int vkey, bool* was_down) {
    const bool down = (GetAsyncKeyState(vkey) & 0x8000) != 0;
    const bool pressed = down && !*was_down;
    *was_down = down;
    return pressed;
}

// Just the leaf name, for display: the full package path is far too long for a
// status line.
const char* LevelLeaf(const char* path) {
    if (!path || !path[0]) return "?";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void InvitePeerHere(const char* current_level, bool have_level) {
    if (net::GetRole() != net::Role::Host) {
        SC_LOG("lobby: only the host can start (you are joining)");
        coop::ReportProblem("only the host can invite");
    } else if (!net::IsConnected()) {
        SC_LOG("lobby: nobody connected yet");
        coop::ReportProblem("nobody connected yet");
    } else if (!have_level) {
        SC_LOG("lobby: not in a level yet -- load one first");
        coop::ReportProblem("load a level before inviting");
    } else {
        net::BumpLevelRequest();
        net::SendLevelSync(current_level);
        SC_LOG("lobby: invited peer to '%s'", current_level);
    }
}

// The host re-invites whenever its own level changes, so the joiner follows
// through a whole playthrough instead of only the first room. Without this,
// every transition -- every death, every door -- would need a manual press.
void HostAnnounceLevelChanges(const char* current_level, bool have_level) {
    static char announced[192] = {};
    if (!have_level || net::GetRole() != net::Role::Host || !net::IsConnected()) return;
    if (_stricmp(announced, current_level) == 0) return;

    lstrcpynA(announced, current_level, sizeof(announced));
    net::BumpLevelRequest();
    net::SendLevelSync(current_level);
    SC_LOG("lobby: level changed to '%s' -- pulling the peer along", current_level);
}

// The joiner follows the host even without an explicit invite. An invite can be
// missed (UDP, or the joiner sitting in a menu at the time), and the symptom is
// two players in different levels seeing nothing at all, with no error -- which
// is indistinguishable from the mod being broken.
void JoinerFollowHostLevel(const char* current_level, bool have_level) {
    if (!coop::Get().auto_follow_level) return;
    if (net::GetRole() != net::Role::Client || !net::IsConnected()) return;

    const char* peer_level = net::GetPeerLevel();
    if (!peer_level[0]) return;
    if (have_level && _stricmp(peer_level, current_level) == 0) return;

    // Only after the disagreement has persisted: the host reports presence
    // about once a second, and both sides are briefly out of step during any
    // normal transition.
    static char pending[192] = {};
    static DWORD since = 0;
    const DWORD now = GetTickCount();

    if (_stricmp(pending, peer_level) != 0) {
        lstrcpynA(pending, peer_level, sizeof(pending));
        since = now;
        return;
    }
    if (now - since < 3000) return;
    since = now;

    SC_LOG("lobby: following host into '%s'", peer_level);
    ue::OpenLevel(peer_level);
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

    // Tell the peer where we are, about once a second.
    static DWORD last_presence = 0;
    const DWORD now = GetTickCount();
    if (net::IsConnected() && have_level && now - last_presence > 1000) {
        last_presence = now;
        net::SendLevelPresence(current_level);
    }

    static bool invite_key_was_down = false;
    if (KeyPressed(VK_F2, &invite_key_was_down)) InvitePeerHere(current_level, have_level);

    HostAnnounceLevelChanges(current_level, have_level);
    JoinerFollowHostLevel(current_level, have_level);

    // Joiner: act on an explicit invite, unless we are already there.
    char invited[192] = {};
    if (net::PopLevelSync(invited, sizeof(invited))) {
        if (have_level && _stricmp(invited, current_level) == 0) {
            SC_LOG("lobby: already in '%s'", invited);
        } else {
            SC_LOG("lobby: travelling to '%s'", invited);
            ue::OpenLevel(invited);
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
        if (requests.travel && requests.level[0]) {
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
    lstrcpynA(status.public_address, net::GetPublicAddress(), sizeof(status.public_address));
    lstrcpynA(status.detail, coop::GetStats().last_problem, sizeof(status.detail));
    ui::SetMenuStatus(status);

    // The one-line summary, for the window overlay and the menu header.
    char line[192] = {};
    if (status.offline) {
        snprintf(line, sizeof(line), "SifuCoop  offline - open the menu with F1");
    } else if (!status.connected) {
        snprintf(line, sizeof(line), "SifuCoop  %s - waiting for peer...",
                 status.hosting ? "HOSTING" : "joining");
    } else if (status.together) {
        snprintf(line, sizeof(line), "SifuCoop  CONNECTED - together in %s  [peer %s, %dms]",
                 status.my_level, !status.peer_known ? "?" : (status.peer_down ? "DOWN" : "up"),
                 net::GetRoundTripMs());
    } else if (status.hosting) {
        snprintf(line, sizeof(line), "SifuCoop  peer in %s - F2 brings them here",
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
    if (net::IsConnected() && now - last_heartbeat > 5000) {
        last_heartbeat = now;
        SC_LOG("coop: %s rtt=%dms | levels me='%s' peer='%s' %s | enemies known=%d active=%d "
               "driven=%d unmatched=%d | dmg out=%.0f in=%.0f | attacks=%u | rejected=%u",
               net::GetRole() == net::Role::Host ? "HOST" : "JOIN", net::GetRoundTripMs(),
               status.my_level, status.peer_level, status.together ? "TOGETHER" : "apart",
               stats.enemies_known, stats.enemies_active, stats.enemies_driven,
               stats.enemies_unmatched, stats.damage_reported_total, stats.damage_applied_total,
               stats.attacks_echoed, stats.packets_rejected);
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
    SC_LOG("puppet: ready (F9 spawn, F10 despawn, F8 follow, F6/F7 replay)");
}

bool SpawnPuppet() {
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
                  const ue::FRotator& rotation) {
    DriveTo(actor, target, rotation);
}

ue::UObject* GetPuppet() { return g_puppet; }

void DespawnPuppet() {
    if (!g_puppet) {
        SC_LOG("puppet: nothing to despawn");
        return;
    }
    // K2_DestroyActor takes no parameters, so reflection handles this without
    // any struct layout knowledge.
    const bool ok = ue::CallFunction(g_puppet, L"K2_DestroyActor", nullptr);
    SC_LOG("puppet: despawn %s (%p)", ok ? "requested" : "FAILED",
           static_cast<void*>(g_puppet));
    g_puppet = nullptr;
    g_puppet_auto_spawned = false;
    g_friendly_applied_for = nullptr;
}

void TickPuppet() {
    if (KeyPressed(VK_F9, &g_spawn_key_was_down)) SpawnPuppet();
    if (KeyPressed(VK_F10, &g_despawn_key_was_down)) DespawnPuppet();

    // Not F11: Windows and the game both claim it for fullscreen, so a toggle
    // bound there silently never fires.
    static bool replay_puppet_key = false;
    static bool replay_enemy_key = false;
    if (KeyPressed(VK_F6, &replay_puppet_key)) ReplayAttackOnPuppet();
    if (KeyPressed(VK_F7, &replay_enemy_key)) ReplayAttackOnNearestEnemy();

    if (KeyPressed(VK_F8, &g_follow_key_was_down)) {
        g_follow_enabled = !g_follow_enabled;
        SC_LOG("puppet: follow mode %s", g_follow_enabled ? "ON" : "OFF");
    }

    ue::UObject* world = ue::GetWorld();
    if (!world) return;
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player) return;

    // The puppet is destroyed along with the level it was spawned into, and
    // nothing tells us -- so the pointer has to be dropped when the world
    // changes or every subsequent frame dereferences freed memory. This was
    // survivable while a level change needed a deliberate keypress; now that
    // the joiner follows the host automatically it is on the ordinary path
    // through a playthrough, and it must not be a crash.
    //
    // Deliberately not DespawnPuppet(): there is nothing left to destroy, and
    // calling into the corpse is exactly what we are avoiding.
    static ue::UObject* puppet_world = nullptr;
    if (world != puppet_world) {
        puppet_world = world;
        if (g_puppet) {
            SC_LOG("puppet: level changed -- forgetting the old remote character");
            g_puppet = nullptr;
            g_puppet_auto_spawned = false;
            g_peer_was_down = false;
            g_friendly_applied_for = nullptr;
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

    // Report our own transform and vitals. Each peer owns its own outcome: with
    // no server to arbitrate, the player who took the hit decides whether they
    // went down, and the other side reflects what was reported. That is not a
    // compromise -- it is the only arrangement in which a parry is judged on
    // the machine where the button was pressed.
    Fighter mine = ResolveFighter(player);
    net::LocalState local;
    local.location = location;
    local.rotation = rotation;
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

    if (net::IsConnected() && !g_puppet) {
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

    if (!g_puppet) return;

    net::PeerVitals vitals;
    if (net::GetPeerVitals(&vitals)) ApplyPeerVitals(g_puppet, vitals);

    // Best-effort, default-off: no-op once applied for this puppet.
    MaintainFriendlyRelationship(player, g_puppet);

    // A live peer always wins over the local follow-mode rehearsal.
    ue::FVector peer_location = {};
    ue::FRotator peer_rotation = {};
    if (net::IsConnected() && net::GetPeerTransform(&peer_location, &peer_rotation)) {
        DriveTo(g_puppet, peer_location, peer_rotation);
        return;
    }

    if (g_follow_enabled) DrivePuppetFromSamples(g_puppet);
}

}  // namespace sifucoop::game

