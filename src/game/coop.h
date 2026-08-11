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
    // Wake a driven enemy's perception when the host's damage lands on it, so
    // it registers being attacked instead of only losing health.
    //
    // This does NOT reproduce Sifu's hit reaction animation, and the ini key
    // pretended otherwise for a long time -- it was written to the file and
    // read by nothing at all. A real reaction needs UHitComponent::
    // BPF_GenerateFakeImpact, whose FHitRequest reaches FHitBox ->
    // FHitboxDataRow -> TSet; the order work already established that those
    // cannot be rebuilt from outside the process that owns them. Until the
    // reaction is carried as an Order, this is the honest half: the enemy
    // notices.
    bool mirror_hit_reactions = true;
    // Show your partner at THEIR age rather than yours. Their age is already on
    // the wire; this is the only thing that consumes it. Written to the puppet
    // only -- your own age is your own run and is never touched from the network.
    bool sync_peer_age = true;
    // Register the partner with AAIDirectorActor as a combat TARGET, so its
    // ticket manager can allocate DirectOpponent to enemies aimed at them.
    // Without it they are aimed and never permitted to swing.
    bool director_targets_partner = true;

    // Let the JOINING machine's own AI fight its player, for the enemies the
    // host says are already targeting them.
    //
    // This is the only route to "the client gets a real fight" that does not go
    // through the host's director. Only a DirectOpponent is allowed to swing,
    // that ticket is allocated per target, and the host's puppet is not a player
    // its director will ever allocate one to -- forcing it there is what crashed
    // on death cleanup and parked the rest of the room as NonOpponent.
    //
    // On the joining machine, though, that player IS player zero: a legitimate
    // target the local director allocates attackers to normally. So for enemies
    // the host reports as fighting the peer, the joiner stops driving their
    // transform and lets their own behaviour tree run. It fits what this project
    // already decided -- each player resolves their own damage, because only a
    // running Sifu can adjudicate its own hitboxes and parry windows.
    //
    // DEFAULT ON, after a session that showed the alternative is not a fight at
    // all: replayed enemy swings morph straight through the joining player, and
    // every blow they land counts as an ambush that breaks structure outright.
    //
    // The cost is real and worth stating: an enemy fighting the joining player
    // is no longer position-authoritative from the host, so the two machines
    // will disagree about exactly where it stands. That is the right way round.
    // Each player already resolves their own damage in this design, and the
    // machine that must see an enemy accurately is the one it is fighting --
    // enemies fighting the host stay host-driven, so nothing about the host's
    // own fight changes. Set peer_fights_locally=0 to go back to a body that
    // stands in the right place and cannot fight.
    // Let the joining machine SIMULATE every enemy, not just the ones fighting
    // it -- host still owns health and death.
    //
    // This is the answer to "on the client they stand there, do not react to
    // hits and do not fall over when killed". Sifu animates a character through
    // Orders, and Orders come from its brain. suppress_client_ai stops that
    // brain, so those bodies have no mechanism to play a hit reaction or a death
    // at all; they move only because this mod writes their position and speed
    // by hand. The proof is already in the game: the enemies the joiner claims,
    // and therefore gives a brain back to, animate perfectly.
    //
    // There is nothing to mirror instead. USCAnimInstance -- the enemies'
    // animation base -- carries no current-action asset the way UPlayerAnim does,
    // so there is no equivalent of the player's action channel to build.
    //
    // With this on, every enemy thinks for itself on both machines and the host
    // stays authoritative for health, damage and death. Position is corrected on
    // drift rather than driven every frame, because driving a body that is
    // walking under its own power makes it slide through its own attacks.
    bool client_simulates_enemies = true;

    bool peer_fights_locally = true;

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

    // Two rule-outs for "enemies engage the remote player but never swing at
    // him". Both are switches rather than fixes because each is a guess that one
    // run settles, and each has a real cost if it turns out to be wrong.
    //
    // The puppet is invincible because the machine that owns that player is the
    // only thing allowed to decide whether they died. But if Sifu's attack
    // selection asks whether a target can actually be damaged, an invincible
    // one is not worth attacking. Turning this off costs nothing on this
    // machine -- the peer's health is still mirrored from their own game -- so
    // it is safe to try.
    // Call UAIFightingComponent::BPF_ForceEnemy to push the remote player into
    // Sifu's combat-role ticket system.
    //
    // DEFAULT OFF BECAUSE IT CRASHES THE GAME. It does work -- with it on, the
    // roles readout went from all-zero to `fighting your partner direct=1`, so
    // the diagnosis is right and this is the correct entry point. But a spawned
    // player clone is not a candidate the director can survive unregistering:
    // when anything dies, AAIDirectorActor::OnDeathDetected ->
    // RemoveActorFromSystems -> FAICombatRoleTicketManager::AddRemoveCandidate
    // dereferences a null. It also parked four of five enemies as NonOpponent,
    // taking them out of the fight with the host as well.
    //
    // Kept as a switch so the finding is not lost. Do not turn it on except to
    // study that crash.
    // Sets the AI's OWN enemy, which is what grants a combat role -- the attack
    // component's target only decides where they walk. Requires the partner to
    // be registered with the director first (director_targets_partner), which is
    // what its earlier crash was missing.
    bool force_enemy_engage = true;

    bool puppet_invincible = true;
    // The puppet ignores pawn collision so two player capsules do not shove
    // each other around. If an enemy's attack-reach test traces against pawn
    // collision, it can never confirm it can reach him and will not commit.
    // Turning this off may bring back the pushing, so try it second.
    bool puppet_ignores_pawn_collision = true;

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

