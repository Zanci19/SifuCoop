#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::net {

enum class Role { Offline, Host, Client };
enum class AnimationSemantic : std::uint8_t;

// Reads SifuCoop.ini beside the DLL and opens the socket. Never blocks.
bool StartSession();

// Human-readable reason for the most recent StartSession failure, or an empty
// string after a successful start. Bootstrap uses this before any gameplay/UI
// hooks exist, so a second host can fail loudly while leaving Sifu itself safe.
const char* GetStartFailure();

// Switches host/join, address or passphrase at runtime, so the in-game menu can
// change the connection without restarting the game. Writes the choice back to
// the ini so it survives a restart, then reopens the socket.
//
// Both players must use the same passphrase. An empty one still works and still
// authenticates -- it simply produces a key everybody else also has, which is
// acceptable on a private VPN and not acceptable on a forwarded port.
bool Reconfigure(bool host_mode, const char* address, int port, const char* passphrase);

// Reopen this machine's configured socket without closing Sifu. The peer receives
// a signed disconnect, then its periodic authenticated hello establishes a fresh key.
bool RestartSession();

// End the current session and leave networking offline until RestartSession or
// Save & Connect is used. The saved host/join settings remain unchanged.
void DisconnectSession();
void StopSession();

Role GetRole();
bool IsConnected();

// Called each frame. Pumps the socket and sends our snapshot at the configured
// rate. `health`, `max_health` and `guard` are display state for the peer's
// puppet; `state_valid` says whether they (and `is_down`) mean anything yet.
struct LocalState {
    ue::FVector location;
    ue::FRotator rotation;
    ue::FVector velocity;
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    bool state_valid = false;
    bool is_down = false;
    bool in_level = false;
};

void TickSession(const LocalState& local);

// Latest peer state, interpolated into the past by the configured delay.
// Returns false when no usable peer data exists yet.
bool GetPeerTransform(ue::FVector* location, ue::FRotator* rotation, ue::FVector* velocity);

// Peer's reported vitals. Returns false when the peer has not reported any yet,
// which is a different thing from "the peer is at zero health" -- conflating
// those two made every freshly connected peer appear dead.
struct PeerVitals {
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    bool is_down = false;
    bool in_level = false;
};

bool GetPeerVitals(PeerVitals* out);

// True on the frame the connection is established or lost, so the puppet can
// be spawned and removed automatically.
bool ConsumeConnectedEvent();
bool ConsumeDisconnectedEvent();

// "This character performed this move." Sent immediately rather than at
// snapshot rate: a dropped attack is far more visible than a dropped position
// update. `actor_hash` is 0 for our own player, or an enemy's name hash.
void SendOrderEvent(std::uint32_t actor_hash, std::uint32_t order_type,
                    std::int32_t attack_index, std::int32_t attack_depth);

// Pops one queued peer order, if any.
bool PopOrderEvent(std::uint32_t* actor_hash, std::uint32_t* order_type,
                   std::int32_t* attack_index, std::int32_t* attack_depth);

// Host -> joiner: "come to this level". Resent periodically so a peer that
// connects late, or is still in a menu, still receives it.
void SendLevelSync(const char* level_path);

// Advances the invite id, so the next SendLevelSync counts as a new request
// rather than a repeat of the last one.
void BumpLevelRequest();

// "I am in this level" -- informational only, never triggers a load. Lets each
// side show where the other is without pulling them anywhere.
void SendLevelPresence(const char* level_path);

// True once per *new* request. Repeats of one already handled are swallowed, so
// a re-sent invite cannot restart a level load that is already in progress.
bool PopLevelSync(char* out_level_path, int out_size);

// Joiner -> host: the answer to the invitation currently on offer. The id
// comes from GetPendingInviteId so a reply can never be credited to a newer
// invite than the one the player actually saw.
void SendInviteReply(std::uint32_t request_id, bool accepted);

// Host: pops the peer's most recent answer. False when nothing new arrived.
bool PopInviteReply(bool* out_accepted);

// The id of the invite currently offered to this machine, or 0 for none.
std::uint32_t GetPendingInviteId();

// Drops the offer without answering it -- used when the player declines, so a
// repeat of the same id is not re-offered.
void ClearPendingInvite();

// What level the peer last reported being in, for the lobby display.
const char* GetPeerLevel();

// --- Enemies ---------------------------------------------------------------

struct EnemyStateOut {
    std::uint32_t name_hash = 0;
    std::uint32_t source_hash = 0;
    float x = 0.f, y = 0.f, z = 0.f, yaw = 0.f;
    float velocity_x = 0.f, velocity_y = 0.f, velocity_z = 0.f;
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    float damage_applied = 0.f;
    float time_dilation = 1.f;
    std::uint8_t flags = 0;
};

// Host: publish the authoritative state of every active enemy. Chunked
// internally, so `count` may exceed what fits in one datagram.
void SendEnemyStates(const EnemyStateOut* entries, int count);

// Client: the most recent complete set received. Returns the count, filling
// `out` up to `max_out`.
int GetEnemyStates(EnemyStateOut* out, int max_out);

// Whether a complete sweep has ever arrived. A count of zero is ambiguous on
// its own -- it means either "the host's fight is over" or "the host has not
// told us anything yet", and those call for opposite behaviour.
bool HasEnemySweep();

// Whether the host's enemy set in front of us is CURRENT, not merely something
// we once received. Anything that overrides local state on the host's authority
// -- above all reviving a body because the host still calls it alive -- must
// ask this rather than HasEnemySweep().
bool EnemySweepIsFresh();

// A new UWorld owns a different enemy pool even when its package path is the
// same (restart/checkpoint reload). Drop completed/staging sweeps and damage
// ledgers before actors from that world are matched against network state.
void ResetEnemyReplication();

struct DamageReport {
    std::uint32_t name_hash = 0;
    float total = 0.f;
};

// Client: "these are my running damage totals". Idempotent; resending is the
// recovery mechanism, so this can be called on a timer and forgotten about.
void SendEnemyDamage(const DamageReport* entries, int count);

// AUTHORITY FOLLOWS THE FIGHT. The joining machine publishes the enemies that
// are fighting ITS player -- the ones running their own behaviour tree there --
// and the host displays those rather than its own drifting simulation of them.
// Health and death stay host-authoritative regardless; this is only about where
// a body is standing.
struct OwnedEnemy {
    std::uint32_t name_hash = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f;
    float velocity_x = 0.f, velocity_y = 0.f, velocity_z = 0.f;
};

void SendOwnedEnemies(const OwnedEnemy* entries, int count);

// False when the peer does not own this enemy, or has stopped saying so. The
// timeout matters: without it a body would freeze wherever the last packet left
// it when the peer died, disconnected or simply stopped fighting.
bool GetOwnedEnemy(std::uint32_t name_hash, OwnedEnemy* out);

// Host: the peer's latest totals. Returns the count written to `out`.
int GetEnemyDamage(DamageReport* out, int max_out);

// --- Run state (Phase D) -----------------------------------------------------
//
// Age, room-clear progress and held weapon, exchanged periodically so each
// player can see where the other is in the run. Informational only: each
// machine's own numbers stay authoritative for itself.

struct RunSnapshot {
    bool age_valid = false;
    int age = 0;
    bool room_clear_valid = false;
    float room_clear_percent = -1.f;
    bool has_weapon = false;
    char weapon_path[192] = {};
    bool outfit_valid = false;
    int outfit_index = -1;
};

// Either side: publish where this run stands. Cheap and slow (a couple of
// times a second), so it can just be fired on a timer and forgotten.
void SendRunState(const RunSnapshot& state);

// Peer's latest run state. Returns false until the first one has arrived.
bool GetPeerRunState(RunSnapshot* out);

// --- Reaching each other over the internet ---------------------------------

// Asks a STUN server what this machine looks like from outside, using the same
// socket the game plays on -- so the answer is the address a peer must actually
// send to, not merely this machine's public IP.
//
// Deliberately triggered by a button rather than run at startup: it sends a
// packet to a third party, and nobody should have their address handed to a
// server they did not ask about.
void DiscoverPublicAddress();

// "1.2.3.4:7777" once discovery has answered, or an empty string.
const char* GetPublicAddress();

// --- Link quality ----------------------------------------------------------

// Smoothed round-trip time in milliseconds, or -1 before the first reply.
// --- Remote animation (cosmetic) -------------------------------------------

// "My character just started this animation." Sent on change only, like order
// events. Purely visual on the receiving side: the montage is played on the
// peer's puppet without running the attack, so it cannot spawn a hitbox or
// damage anyone. Their damage already resolved on their own machine.
void SendMontageState(const char* montage_path, float position);
void SendAnimationSequence(const char* asset_path, std::uint32_t actor_hash,
                           AnimationSemantic semantic, float position = 0.f);

// Oldest pending animation from the peer, if any.
bool PopMontageState(char* out_path, int out_size, float* out_position, bool* out_raw_sequence,
                     std::uint32_t* out_actor_hash, AnimationSemantic* out_semantic);

int GetRoundTripMs();

// How far into the past the puppet is rendered. Derived from RTT when adaptive
// interpolation is on, otherwise the configured constant.
int GetInterpolationDelayMs();

}  // namespace sifucoop::net
