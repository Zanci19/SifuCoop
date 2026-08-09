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

    // Legacy experimental setting retained for config migration; custom UDP mirror is the supported transport.
    bool native_network = false;

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

    // Use a REAL second player body for the remote peer. This is required for
    // host AI to select and fight that peer; a spawned clone is not a player to
    // Sifu's targeting system.
    //
    // The puppet is not a player as far as Sifu is concerned, which is the root
    // of three separate complaints at once: enemies only ever fight the host,
    // the joining player's hits register erratically, and the remote character
    // cannot perform its own moves. A real PlayerController with a game-mode
    // spawned pawn is a genuine target, has ordinary hitboxes, and owns its
    // moveset. Sifu ships the whole path -- a community split-screen mod uses
    // the same call -- but whether its game mode will hand out a second player
    // is unknown until it is asked.
    bool real_second_player = false;

    // Whether to force the viewport out of splitscreen when the second player is
    // created. On paper this is what we want -- the other player is on another
    // machine, so half a screen showing their camera is wasted. In practice it
    // is also a prime suspect for the camera freezing on the level's start view
    // when a second player joins, so it is a switch rather than a decision.
    bool second_player_disable_splitscreen = true;

    // The second player is a REAL local player, so Sifu builds it a HUD -- and
    // with splitscreen force-disabled that HUD is drawn into the same
    // full-screen viewport as yours. Since the mod also mirrors the peer's real
    // health onto that body, the result is the remote player's life bar sitting
    // on your screen looking like yours. Suppressing the second player's HUD is
    // the fix; it is a switch because it is done by declining Sifu's own
    // BPF_SetHUD call for that controller, and a build that does not expose the
    // symbol simply keeps the old behaviour.
    bool hide_second_player_hud = true;

    // Never hand the engine a second player while the local player is sitting
    // in a menu, the hideout level picker, or any other non-gameplay world.
    // Registering a second local player there is indistinguishable, to Sifu,
    // from someone pressing Start on a second pad.
    bool second_player_in_gameplay_only = true;

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

    // Replay the peer's attacks on their body so they visibly fight, but only
    // once Sifu's own relationship system has been set to friendly between the
    // two players AND that value has been read back out of the game. Until the
    // readback agrees, the replay stays off, because a replayed swing is a real
    // hitbox and an unenforced exemption means the remote player kills their own
    // co-op partner.
    //
    // Be clear about what the readback proves: that Sifu is STORING "these two
    // are friendly", not that its melee code consults that map. The latter is
    // still an inference (it is why two grunts of one faction do not hurt each
    // other) and is what a two-machine session has to confirm. If partners start
    // damaging each other, set this to 0 -- the remote player then moves but
    // does not swing, which is the previous, safe behaviour.
    //
    // echo_player_attacks above is the manual override and ignores the check.
    bool remote_player_attacks = true;

    // Session flow.
    bool auto_follow_level = true;    // the host keeps pulling the joiner along
    // Whether the JOINER acts on a level invite without being asked.
    //
    // Default OFF, and this is a behaviour change: it used to travel the instant
    // an invite arrived, which meant loading a level on the host yanked the
    // other player out of whatever they were doing with no prompt -- "the game
    // just starts without me pressing start". The invite is now held, shown in
    // the overlay, and joined when the joining player says so.
    bool auto_join_level = false;
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
    // Position updates specifically, so "how often does the peer move" can be
    // read off the heartbeat instead of inferred from total packet traffic.
    std::uint32_t snapshots_received = 0;
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

// True only when the option was present at process startup. This prevents a
// live custom-mirror session from switching to the engine path after it has
// already created its synthetic local player.
bool NativeNetworkActive();
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

