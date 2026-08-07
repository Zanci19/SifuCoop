#pragma once

#include <cstdint>

// Runtime configuration and live telemetry, shared between the game thread
// (which acts on it) and the overlay's render thread (which displays and edits
// it).
//
// Everything the co-op layer does that could plausibly misbehave on a machine
// nobody has tested is a switch here, defaulted to on but reachable in two
// keystrokes from inside the game. That is deliberate: a feature that turns out
// to crash or feel wrong should cost a checkbox, not a rebuild.
//
// Fields are plain scalars written by one thread and read by the other. On
// x86-64 aligned word-sized loads and stores do not tear, and nothing here
// needs two fields to change together, so no lock is warranted -- adding one
// would mean the render thread could block the game thread.

namespace sifucoop::coop {

enum class Mode : int {
    Coop = 0,    // both players allied; enemies fight them both
    Versus = 1,  // the original sparring behaviour: players hostile to each other
};

struct Config {
    Mode mode = Mode::Coop;

    // Phase A -- enemies exist and agree on both screens.
    bool sync_enemies = true;         // drive client enemies from the host
    bool suppress_client_ai = true;   // StopLogic on the client's copies
    bool sync_enemy_vitals = true;    // health and guard follow the host
    bool park_extra_enemies = true;   // hide enemies the host has not activated
    bool echo_enemy_attacks = true;   // replay the host's enemy attacks (A4)

    // Replaying the REMOTE PLAYER's attacks on their puppet is off by default,
    // and deliberately so: the replay produces a real, damaging hitbox, and
    // same-faction does not stop it from hurting you in Sifu -- so with this on,
    // your co-op partner's swings kill you. It is also conceptually wrong:
    // their attack already resolved on their own machine, so on yours it should
    // be cosmetic, not a second live attack. Until an animation-only path
    // exists, the safe behaviour is to not perform it at all -- the remote
    // player still moves, they just do not swing on your screen. Turn this on
    // only to reproduce the friendly-fire bug, or in Versus mode where a
    // remote player damaging you is the whole point.
    bool echo_player_attacks = false;

    // EXPERIMENTAL, UNVERIFIED, default off. Attempts the principled fix for the
    // friendly-fire bug: Sifu keeps a per-actor relationship map
    // (USocialComponent::m_Relationships, a TMap<AActor*, ERelationshipTypes>),
    // and its hit-detection is believed to consult it -- which is why enemies of
    // one faction do not damage each other. Setting the puppet<->local-player
    // relationship to the friendly value should make the puppet's swings pass
    // through the local player the same way one grunt's swing passes through
    // another. If it works, echo_player_attacks can be turned on so the remote
    // player visibly attacks WITHOUT the friendly-fire. Reflection-only, per
    // actor, so nothing else is affected. NOT confirmed on a live game yet:
    // whether the melee path actually gates on this map is the open question.
    // Test on one machine: F9 to spawn a puppet, F6 to replay an attack on it
    // while standing next to it, and watch whether your health drops.
    bool friendly_relationship = false;

    // Mirror the peer''s montage onto their puppet as a pure visual. Sifu drives
    // combat through Orders and barely uses montages, so this mostly carries
    // dodges and traversal -- cheap, additive, and off nothing when idle.
    bool sync_montages = true;

    // Phase B -- both players can actually fight.
    bool report_damage = true;        // client tells the host what it hit
    bool mirror_peer_vitals = true;   // the puppet shows the peer's real health

    // Phase D -- run state, informational by default.
    bool sync_run_state = true;       // exchange age / room-clear / held weapon
    bool fix_room_clear = false;      // nudge the local room-clear % to the host's
    // fix_room_clear is a real (if optional) mutation of the joiner's own game
    // state, so it is off until a player asks for it -- singleplayer behaviour
    // is preserved by default and the toggle is a checkbox away in Tuning.

    // Session flow.
    bool auto_follow_level = true;    // the joiner follows the host's level
    bool adaptive_interp = true;      // size the interpolation buffer from RTT

    int interp_delay_ms = 60;         // used when adaptive_interp is off
    int snapshot_hz = 60;

    // The in-game menu draws by hooking the swap chain, which is the single
    // riskiest thing this mod does to a process it does not own -- and it is
    // entirely cosmetic. Anyone whose game misbehaves graphically must be able
    // to switch it off without a rebuild and keep playing, so this is read
    // before any graphics hook is installed. Everything else stays available
    // through SifuCoop.ini and the hotkeys.
    bool in_game_overlay = true;

    // Diagnostics, off by default: these log per event and get noisy fast.
    bool verbose_enemies = false;
    bool verbose_orders = false;

    // Runs the in-game harness that answers whether a replayed enemy attack
    // actually damages the local player. It teleports an enemy next to you and
    // forces it to attack, so it is not something to leave on.
    bool selftest = false;
};

// Live counters. Written by the game and network threads, read by the overlay.
struct Stats {
    int rtt_ms = -1;              // -1 until the first pong comes back
    int rtt_jitter_ms = 0;
    std::uint32_t packets_sent = 0;
    std::uint32_t packets_received = 0;
    std::uint32_t packets_dropped = 0;   // gaps in the peer's sequence numbers
    std::uint32_t packets_rejected = 0;  // failed authentication -- wrong key, or a stranger
    std::uint32_t bytes_per_second_in = 0;
    std::uint32_t bytes_per_second_out = 0;

    int enemies_known = 0;        // fighting characters in the level, minus us
    int enemies_active = 0;       // actually in the fight rather than pooled
    int enemies_driven = 0;       // matched to a host entry this frame
    int enemies_unmatched = 0;    // host sent an id we do not have locally

    std::uint32_t attacks_echoed = 0;
    std::uint32_t damage_reports = 0;
    float damage_reported_total = 0.f;
    float damage_applied_total = 0.f;

    // Last thing that went wrong, so the overlay can show it without the user
    // having to alt-tab to a log file.
    char last_problem[160] = {};
};

Config& Get();
Stats& GetStats();

// Reads SifuCoop.ini next to the executable. Missing keys keep their defaults,
// so an ini written by an older build still works.
void Load();
void Save();

// Fills `out` with the full path of SifuCoop.ini. Shared so the session and the
// config cannot disagree about which file they are reading.
void IniPath(char* out, int out_size);

// Records a one-line problem for the overlay. Rate-limited by the caller.
void ReportProblem(const char* format, ...);

}  // namespace sifucoop::coop

