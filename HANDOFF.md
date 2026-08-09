# SifuCoop — Full Project Handoff

*This document is a complete, self-contained briefing for an AI assistant taking over
development of an existing codebase. Read all of it before touching anything. The project's
defining value is **honesty about what is verified versus assumed** — preserve that. Do not
claim something works unless it was observed working; mark everything else clearly.*

---

## 1. What this is

**SifuCoop** is an unofficial two-player online co-op mod for the single-player game **Sifu**
(by Sloclap, built on Unreal Engine 4.26.2). Sifu ships with no multiplayer. This mod lets two
people fight the game's own enemies together: shared encounters, shared kills, shared progress
through a level, each player controlling their own character.

It is delivered as a single **`dsound.dll`** (a proxy DLL that forwards real dsound calls while
injecting the mod) plus a **`SifuCoop.ini`** config file, both dropped next to the game
executable. No game files are modified; uninstall = delete those two files.

- **Project source root:** `C:\Users\zanci\SifuCoop\`
- **Shippable package:** `C:\Users\zanci\SifuCoop\dist\` (also zipped to `C:\Users\zanci\SifuCoop-ship.zip`)
- **Game install (this machine, "desktop", the working one):**
  `C:\Program Files\Epic Games\Sifu\Sifu\Binaries\Win64\`
- **Game build fingerprint that offsets target:** Epic build, `TimeDateStamp=0x68400CB9`,
  `SizeOfImage=0x064DB000`, internal name `"epic"`. UE 4.26.2, Shipping config.

---

## 2. Core architecture and design decisions (these are settled — do not relitigate)

- **Host-authoritative for enemies.** One player ("host") is the source of truth for every
  enemy: AI, position, health, death. The other player ("joiner"/"client") has its copies of
  the enemies' AI switched off (`UBrainComponent::StopLogic` via reflection) and drives them
  from the host's packets.
- **Peer-authoritative for players.** Each machine resolves its OWN player's damage and reports
  the result. Nobody is ever told they were hit. This is deliberate: in a parry-timing game,
  the only correct place to judge a parry is the machine where the button was pressed. Your own
  inputs are never delayed or mispredicted.
- **Why no dedicated server:** Sifu's hitboxes, animation timing, parry windows, and AI all
  live inside the Windows game executable. Only a running Sifu can adjudicate a fight. A
  separate server could relay packets but never judge combat, and there is no headless build.
  This was re-examined and confirmed.
- **Enemy identity across machines:** FNV-1a hash of an enemy actor's **leaf object name**.
  Sifu pre-spawns every enemy into a pool with deterministic names
  (e.g. `BP_AICharacter_Grunt_M_HD4_Banker_03_C_0`), identical on both machines, so the same
  enemy hashes to the same ID on both. **Verified stable:** two independent game launches of the
  same level produced 62 enemies, 62 distinct hashes, identical name sets, every name → same ID.
- **The remote player is shown as a "puppet":** a spawned clone of the local player class,
  driven by the peer's snapshots. In co-op it is set to the SAME faction as the local player
  and made invincible (its death is decided on the peer's machine). In "Versus" mode it is
  hostile (the original sparring behavior, retained).

### User-locked gameplay decisions (from direct Q&A — implement to match these):
- **Death/aging:** exactly the singleplayer mechanic, per player. A dead player ages by their
  own death counter and gets back up where they fell; the other player keeps fighting. No shared
  restart, no revive system. (This already works because the mod never touches the death path.)
- **Progression:** each player keeps their OWN age and skills, but they fight enemies together.
  Independent by design.
- **Shrines/upgrades:** each player spends their own XP. No sharing.
- **Cutscenes:** explicitly out of scope (too much effort, not wanted).

---

## 3. Technical mechanism

- **Injection:** `dsound.dll` proxy. `DllMain` → `Bootstrap` thread waits for `GEngine`, then
  installs everything.
- **Build guard:** reads the running exe's PE `TimeDateStamp`/`SizeOfImage`; if it doesn't match
  a known build in `builds/*.json`, the mod refuses to hook anything and stays fully inert (logs
  `guard: UNKNOWN BUILD`). This is why it does nothing on a different Sifu version.
- **Offsets, two mechanisms, regenerated per build by `build.ps1`:**
  1. **Function RVAs** from the shipped PDB by mangled-name substring match — see the `WANTED`
     dict in `tools/pdbdump/pdbdump.py`, emitted into `src/core/offsets.g.h`.
  2. **Struct member offsets** recovered from Unreal's generated reflection tables (every
     reflected `UPROPERTY` records its byte offset; the PDB names the table entry) — see
     `WANTED_MEMBERS` in the same file, and `tools/pdbdump/structdump.py` for ad-hoc lookups.
     This is how `m_fHealth (+0x138)`, `m_fMaxHealth (+0x140)`, `m_fCurrentGuard (+0x160)`,
     `m_eFaction (+0x1530)`, the three component pointers on `AFightingCharacter`
     (`m_HealthComponent +0x1350`, `m_AttackComponent +0x1308`, `m_DefenseComponent +0x12A8`),
     and `m_DefaultCombo (+0x608)` were found exactly.
- **Hooks:** `UGameEngine::Tick` (vtable swap, slot found by scanning for the PDB address);
  several combat functions via **MinHook** inline hooks (`UOrderComponent::PlayOrder`,
  `PrepareToLaunchAttack`, `LaunchAttack`, `GeNextAttackID`, `MultiCastPlayOrder`); D3D11
  `Present` + `ResizeBuffers` + `SetCursorPos` + `ClipCursor` for the in-game overlay.
- **Reflection layer** (`src/ue/reflection.{h,cpp}`): `ue::CallFunction(obj, L"FuncName",
  &params)` calls any Blueprint-exposed `UFunction` by name via `ProcessEvent`. This is the
  SAFEST way to call into the game (no ABI guesswork). Prefer it over raw native calls.
- **Accessor layer** (`src/game/actors.{h,cpp}`): typed, cheap reads/writes of health, max
  health, guard, faction, death, components, damage, for any `AFightingCharacter`, using the
  member offsets (avoids per-frame `ProcessEvent`).
- **Network:** UDP, `src/net/`. Protocol is currently **v7**. Every packet carries a truncated
  **HMAC-SHA256** (8-byte tag) keyed by a shared passphrase; a session nonce from each side is
  mixed into the key (anti-replay). Unverified packets are dropped before any field is read.
  Peer address is pinned after handshake. Level paths are shape-validated before `OpenLevel`;
  all floats are NaN/range-checked before becoming positions/health. Crypto is self-contained
  (`src/net/crypto.cpp`, RFC 6234/2104) and **verified against published FIPS 180-4 / RFC 4231
  test vectors on every build — the build fails if any vector mismatches** (`tools/cryptotest/`).
- **Connectivity:** ZeroTier/VPN (recommended), host port-forward, or STUN + UDP hole-punching
  (STUN client on the game's own socket; "Find my public address" button in the overlay).
- **In-game overlay/menu (F1):** Dear ImGui drawn by hooking the D3D11 swap chain. Purely
  cosmetic. Can be disabled with `in_game_overlay=0` (it's the riskiest thing the mod does).
  Everything it configures is also settable directly in `SifuCoop.ini`, so F1 is never required.

### Config (`SifuCoop.ini`) — `[net]` and `[coop]` sections
`[net]`: `mode` (off/host/client), `host` (IPv4 of host, client only), `port`, `passphrase`
(must match on both machines; empty = no auth, only safe on VPN/LAN), plus optional
`punch`/`local_port`/`stun_server` for hole-punching.
`[coop]` toggles (all default ON unless noted): `versus` (0), `sync_enemies`,
`suppress_client_ai`, `sync_enemy_vitals`, `park_extra_enemies`, `echo_enemy_attacks`,
`echo_player_attacks` (**default 0 — see friendly-fire bug**), `report_damage`,
`mirror_peer_vitals`, `sync_montages`, `sync_run_state`, `fix_room_clear` (0), `auto_follow_level`,
`adaptive_interp`, `interp_delay_ms` (60), `snapshot_hz` (60), `in_game_overlay`, `selftest` (0),
`verbose_enemies` (0), `verbose_orders` (0).

---

## 4. File/directory map (source of truth)

```
src/
  dllmain.cpp            injection, bootstrap, build guard
  core/hooks.cpp         UGameEngine::Tick hook, per-frame OnFrame()
  core/log.cpp           SifuCoop.log (immediate-flush, timestamped)
  core/offsets.g.h       GENERATED offsets (do not hand-edit)
  ue/reflection.cpp      CallFunction, anim state read/apply, level travel, object-path identity
  game/actors.cpp        typed health/guard/faction/death/component accessors + pool helpers
  game/coop.cpp          Config + Stats structs, ini load/save, ReportProblem
  game/enemies.cpp       host-authoritative enemy sync (publish/apply/damage), id collision check
  game/puppet.cpp        remote-player puppet: spawn, drive, vitals, lobby, montage mirror
  game/orders.cpp        attack capture + replay (PrepareToLaunchAttack path)
  game/selftest.cpp      in-game harness: does a replayed enemy attack damage the local player?
  net/protocol.h         wire format (v7), packet structs, static_asserts
  net/session.cpp        socket, auth, STUN/punch, interpolation, all packet handlers
  net/crypto.cpp         SHA-256 + HMAC-SHA256 (verified against test vectors)
  ui/d3d_overlay.cpp     ImGui menu via swap-chain hook (Lobby/Levels/Sync/Tuning/Internet/Network/How-to)
  ui/overlay.cpp         fallback status window (when swap-chain hook unavailable)
tools/
  pdbdump/pdbdump.py     offset generator (WANTED funcs + WANTED_MEMBERS)
  pdbdump/structdump.py  ad-hoc struct member offset lookup
  pdbdump/query.py       batch regex search over ~906k PDB symbols (parses in ~2s)
  cryptotest/            crypto test-vector harness (run every build)
  crashinfo/crashinfo.py parses a UE4 minidump to attribute a crash to a module
testclient/testclient.cpp  a REAL protocol peer (fakes player 2 on one machine)
launcher/launcher.cpp   pre-launch setup GUI (host/join/address/passphrase)
build.ps1               regenerate offsets → run crypto vectors → compile dll+testclient+launcher → package to dist/
builds/epic.json        the "epic" build's offsets (data; commit these)
COOP-PLAN.md            engineering plan + phase status + research findings
SETUP.md                end-user install/connect/test guide
README.md, LICENSE, THIRD_PARTY.md, .gitignore
```

**Build:** `.\build.ps1 -NoGen` (fast, skips PDB pass) or `.\build.ps1` (regenerates offsets from
the PDB). Requires WinLibs g++ (MinGW UCRT) and Python 3. Compiles warning-clean under
`-Wall -Wextra`; treat any warning as a regression. Deploy: copy `build\dsound.dll` to the game's
`Win64` folder.

---

## 5. CURRENT STATE (as of this handoff — important, partially mid-stream)

- **Protocol is v9** (v8 → v9 added `kEnemyDead`; see §7b.2). The build is clean (no
  warnings) and passes crypto vectors, and the v9 dll IS deployed to the Epic game folder.
  **Both machines must be updated together** — a v8 peer and a v9 peer will refuse to talk
  and log "peer speaks protocol vN". `testclient.exe` is rebuilt from the same source by
  build.ps1.
- **The `steam` build definition is one offset short of `epic` (75 vs 76):** it predates
  `AFightingPlayerController_BPF_SetHUD`, so on a Steam machine the second player's HUD
  suppression degrades to a logged no-op until `build.ps1` is re-run there. Nothing else
  is affected.
- **`sync_montages` (default ON):** cosmetic animation mirroring — captures the local player's
  active `UAnimMontage` on change, sends its object path, and plays it on the peer's puppet as a
  PURE VISUAL (no hitbox). **Caveat discovered:** Sifu drives combat through "Orders", NOT
  montages (~2 montage changes in 2 minutes of fighting), so this carries dodges/traversal, NOT
  strikes. It does **not** solve "the remote player should visibly attack." Builds and is wired;
  unverified in a real 2-machine session.
- **`sync_run_state` / RunState packet (v6 scaffold):** transport exists for age / room-clear% /
  held-weapon path, but is **dormant** — nothing on the game side reads the local values or
  applies the peer's. `fix_room_clear` defaults off.
- **Jitter fix: NOT applied.** (See open issues.) The plan was: smooth the puppet's facing
  (interpolate yaw instead of snapping every frame in `DriveTo`) and raise the interpolation
  floor (`GetInterpolationDelayMs` currently clamps `delay < 30` → 30 on localhost, too tight).

---

## 6. What is VERIFIED (observed working on the live game)

- Build fingerprint matches; every function offset and member offset resolves at runtime.
- Player transform + vitals cross the wire; puppet spawns, is driven, shows peer down/up state.
- Enemy state sync: positions track, correct per-archetype health pools (60/100/120/210 seen),
  pooled enemies correctly excluded.
- Client damage reporting → host applies via `BPF_ApplyDamage` (cumulative-total, idempotent
  protocol). **BUT the one observed "kill" was against a phantom enemy reading `hp=0/0`
  (zero max health) — it was pooled, not killed. Killing a REAL enemy from co-op damage is
  NOT yet verified.**
- Authentication both directions: correct passphrase connects; wrong/empty passphrase is
  rejected (rate-limited log), verified live.
- Enemy ID stability across launches (see §2).
- STUN wire format independently confirmed against a real STUN server.
- Crypto: 18 published test vectors pass, enforced on every build.

---

## 7. BUGS FOUND AND FIXED (via live testing + minidump analysis)

1. **Enemy ID collision:** name buffer was `char[40]`; `..._Banker_03_C_0` is exactly 40 chars,
   so the trailing instance number was truncated and `_C_0`/`_C_1` hashed identically → two
   enemies shared one ID (driven to the same spot, damaged together), silently. Fixed → `char[96]`
   plus a per-refresh collision check that logs loudly.
2. **Death vs knockdown:** `IsDown()` is knockdown state, not death; enemies die at 0 health
   without it ever becoming true. The joiner was driving corpses. Fixed: treat `health<=0` as down.
3. **Dangling puppet pointer** across level changes (auto-follow made this routine). Fixed: drop
   the pointer on world change.
4. **Stale enemy table** read after its level was freed (attack hook fires on the engine's
   schedule). Fixed: refuse lookups against a table built in a non-current world.
5. **WSACleanup reference leak** on reconnect. Fixed.
6. **Damage-counter reset asymmetry** made an enemy's health visibly spring back after each hit.
   Fixed: both counters monotonic per body, reset together.
7. **D3D11 access-violation crash (`writing 0x...14A3`, faulting module `d3d11.dll`).** A
   minidump parser (`tools/crashinfo/crashinfo.py`) confirmed the faulting address was NOT inside
   the mod, but the root cause was the mod: the swap-chain `Present` hook fired for EVERY swap
   chain in the process (Sifu loads DLSS + the Epic overlay, each with its own), so it bound a
   render target from one chain while another was presenting. Fixed: pin `g_swap_chain` and pass
   all others straight through. (Note: this crash occurred once, at startup, right after
   force-killing the previous instance — driver state was likely also disturbed. The fix
   addresses a real latent bug with the right signature but was not reproduced/confirmed fixed.)

---

## 7b. THE 2026-08-08 PASS — five reported defects, what was changed, what is still unproven

All five came from a two-machine session. Every change below is **implemented and
compiles warning-clean; none of it has been seen running.** The diagnosis for each is
stated so the next session can tell "the fix did not work" from "the diagnosis was wrong".

1. **"Life from the client appears as mine."** Diagnosis: the second player is a real
   local player, so Sifu builds it a real HUD, and with splitscreen force-disabled that
   HUD draws into the *same* full-screen viewport as player one's — while the mod mirrors
   the peer's real health onto that body. Fix: hook `AFightingPlayerController::BPF_SetHUD`
   (new offset) and decline it for the second controller, taking the widget back out of
   the viewport. Toggle `hide_second_player_hud`. Two independent guards were added for
   the *other* possible cause — Sifu's game mode handing the second player player one's
   own character, which it is already known to do at creation and can plausibly redo on
   respawn/travel: `MaintainSecondPlayer` now detects and repairs a re-theft (after a
   1 s confirmation window, so an ordinary respawn is not "repaired"), and `ApplyPeerVitals`
   refuses any body controller 0 is possessing.
2. **"Cannot hit enemies the other player has touched."** Diagnosis, and this one is
   solid: `kEnemyDown` conflated *knockdown* with *death*. The joiner forced its copy
   through `InternalSetDownState(Down, force)` on every host knockdown and back through
   `(None, force)` on every recovery — and a body walked in and out of that state machine
   from outside comes back upright but no longer a valid hit target. Fix: new `kEnemyDead`
   wire flag; only death drives the local down path, knockdown replicates as position
   only. Two supporting fixes: presence (visibility + collision) is now tracked per enemy
   and repaired whenever it disagrees, instead of only being restored on the first host
   state; and the "left the host's fight" retirement now needs the body to be absent for
   1.5 s, so one dropped sweep no longer executes a live enemy.
3. **"The game starts when I load the level without pressing start."** Two causes
   addressed. The joiner used to call `OpenLevel` the instant a `LevelSync` arrived —
   `auto_follow_level` was only ever consulted on the *host*. Invites are now held and
   offered ("Join them" in F1 → Play); `auto_join_level` (default **0**) restores the old
   behaviour. Separately, a second local player is no longer registered outside a playable
   level (`second_player_in_gameplay_only`), because asking the engine for one in a menu
   is indistinguishable from a second pad pressing Start.
4. **"The peer's position updates about once a second."** Diagnosis: the clock offset was
   the minimum of (arrival − sent) over the *whole session*, corrected upward at 0.2 ms/s.
   Packets arrive in bursts; within a burst the newest packet yields the smallest observed
   value of the session, so the estimate latched onto it and every sample's timeline
   position was biased older by roughly the burst span. The render point then sat past the
   newest sample almost always, the interpolator was starved, and the starved branch
   *repeated its last output* — a character standing still, moving only when a burst
   happened to bracket the render point. Fixes: the offset is now a minimum over a rolling
   2 s window (a burst is forgotten in one window instead of held for the session), and
   starvation on the new side now dead-reckons from the newest sample along its reported
   velocity (capped at 200 ms) instead of freezing. **The heartbeat now prints `peerpos=N/s`**
   — snapshots only, separate from total `rx` — so the next session can settle this by
   reading one number instead of reasoning about it. If `peerpos` is near `snapshot_hz` and
   the character still stands still, the fault is in the drive, not the transport.
5. **"Animations do not play — neither the peer's nor the enemies'."** Two separate causes.
   (a) Every remote animation is rebuilt on a locally captured `FDelayedActionAttack`, and
   that template could only be captured from the *local player's own* attack — so the
   joining player had to throw a punch before any enemy would visibly swing, and the
   template is discarded on every respawn and level change. It is now captured from any
   local character (enemies swing constantly), with the player's still preferred.
   (b) `PumpRemoteOrders` *dropped* an echoed enemy attack whenever the host reported no
   target for it — and `BPF_GetTargetForAction` answers "none" for whole rooms in the live
   log, so enemy attacks essentially never played. A missing target now means "leave the
   local lock alone and play it anyway" rather than "discard".
   For the remote *player*: `remote_player_attacks` (default on) replays their swings, but
   only after the friendly relationship has been set **and read back out of the game**.
   Be clear what that proves — that Sifu is *storing* "these two are friendly", not that
   its melee code consults that map. If partners start damaging each other, set
   `remote_player_attacks=0` and they will move but not swing, which is the old behaviour.

**Also added:** "Teleport to partner" (F1 → Play, only when both report the same level) —
the play-testers' "wait for me at the boss" request, generalised so it needs no per-boss
scripting. And `sync_montages` is finally read from and written to the ini; it was
declared, documented and switchable but never actually loaded.

**What to check first, in this order:** `peerpos=N/s` in the heartbeat; whether an enemy
the other player knocked down is still hittable afterwards; whether enemy swings animate
on the joining screen from the first fight; whether the log line
`puppet: friendly relationship ... readback` says CONFIRMED; and whether loading a level on
the host now leaves the other player where they were.

---

## 8. OPEN BUGS / UNRESOLVED ISSUES (highest priority first)

1. **FRIENDLY FIRE — the puppet's replayed attacks are real, damaging hitboxes, and same-faction
   does NOT prevent them from hurting you in Sifu** (the game has no allies in singleplayer, so
   melee damage was never made faction-exempt). In testing, the remote player's puppet punched
   and killed the local player. **Mitigation shipped:** `echo_player_attacks` defaults to `0`, so
   in co-op the remote player's attacks are simply not replayed. **Consequence:** the remote
   player MOVES but does NOT visibly attack in co-op. This is the biggest felt gap. It is also
   conceptually correct that their attacks should be cosmetic (their damage already resolved on
   their machine), but there is no confirmed way to make a replayed attack animate WITHOUT a
   hitbox: montages don't carry Sifu attacks (they're Orders), and no settable "zero outgoing
   damage" field was found (`GetDamageMultiplier` is a computed getter). **This is the #1 thing to
   solve for co-op to feel right.** Candidate approaches, none verified: (a) drive attacks through
   the gameplay-ability system so selection is correct and possibly faction-aware; (b) find/repair
   Sifu's relationship/hit-detection so same-faction is damage-exempt; (c) briefly make the local
   player immune only to the puppet during its attack window (no per-instigator immunity API found).
2. **Real enemy death from co-op damage is UNVERIFIED** (the one "kill" was a phantom 0/0 enemy).
   Re-test the joiner killing a real, full-health enemy and confirm a death animation plays.
3. **Puppet jitter** ("twitches like on caffeine" while following). Root cause identified but fix
   NOT applied: (a) facing is snapped to the peer's yaw EVERY frame in `DriveTo`
   (`K2_SetActorRotation`), and the peer's yaw itself jitters; (b) the `testclient` bot's movement
   oscillates by design (approach/back-off around striking range), making it a poor smoothness
   source; (c) interpolation floor of 30 ms is too tight on localhost. Planned fix: interpolate
   the puppet's yaw toward target instead of snapping, and raise the interp floor. NOT DONE.
4. **Exact-strike fidelity:** the puppet reproduces the local player's LAST captured attack rather
   than the peer's chosen strike, because Sifu selects the move inside `UAttackAbility` /
   `GeNextAttackID` — BELOW the `PrepareToLaunchAttack` entry point the mod uses (the
   `GeNextAttackID` hook installs but fires zero times on that path). Deferred; no reachable
   entry point above the decision was found. Cosmetic (hits still land).

---

## 9. RESEARCHED but NOT (fully) IMPLEMENTED — findings from reading the game binary

- **Doors / room-clear:** there is NO `ADoor`/`AGate`/`ABarrier` actor to flip. Room completion
  is emergent from `AAIDirectorActor::OnEnemyDeathDetected` + `AThePlainesGameState::
  m_fRoomClearedLifePercent`. Since the mod already syncs enemy DEATH host-authoritatively, room
  clear SHOULD follow automatically on the joiner **provided its local game observes its enemies
  dying** — this is the single most important thing to verify on two machines. If it doesn't
  follow, the fix is to replicate the clear-percentage (RunState scaffold already carries it) and
  optionally nudge it via `fix_room_clear`, NOT to build a door system. Transport exists; read/apply
  is NOT wired.
- **Weapons:** `ABaseWeapon::BPF_AttachWeapon`, `BPF_DropWeapon`, `BPF_GetWeaponData` (returns a
  `UBaseWeaponData` asset whose path is portable). `AFightingCharacter` has `OnEquipWeapon`/
  `OnUnequipWeapon` delegates. Observing the local held weapon is feasible; making the puppet hold
  the same means spawning + attaching a weapon actor — genuinely risky, unverifiable without two
  machines. RunState scaffold carries a `weapon_path`; capture/apply NOT wired. Deferred as
  highest-effort/highest-risk. This is the biggest visual gap after remote attacks (right now a
  player holding a bat looks like they're punching).
- **Death/aging:** age + death counter live in a per-character `UStatsComponent`
  (`BPF_DecrementDeathCounter`, `BPF_GetAgeIncrement`, `BPF_ResetDeathCounter`). **The mod never
  touches it** (source-audited: `SetHealth`/`SetDown`/`ApplyDamage`/`SetFaction`/`SetInvincible`
  are only ever called on enemies and the puppet, never the local player). So aging is already
  fully independent per player — matches the user's requirement with no new code. The age-cap
  ending a run mid-session (a level reload) is the one unhandled edge (rare; would show as the
  peer's puppet freezing until they reappear).
- **Targeting/aggro:** `BPF_GetTargetForAction` reads an `AActor*`; `BPF_SetTargetForSlot` /
  `BPF_UpdateLockMoveTarget` take one. But this is LOW VALUE: the joiner's enemies are AI-stopped
  position-puppets with no combat target of their own, and which player the host's AI fights is
  already conveyed implicitly by where the enemy is driven. Deferred.
- **Shrines/progression:** independent by design (per-character `UStatsComponent`, never touched).
  Shrines are per-machine world actors with no shared mutable state. Believed safe; verify two
  players at one shrine don't stall each other.
- **Bosses:** enumerate as ordinary `AFightingCharacter`s (`BP_Fengjie_Base_C` was in the pool),
  so position/health/death replicate through the existing enemy path. The risk is separate scripted
  PHASE state driven on the host fighting the joiner's local script. Basic fight likely works;
  phase transitions are the highest desync risk. Unverified.

---

## 10. HOW TO TEST (the mod cannot be fully verified without this)

**Fake a second player on ONE machine** (the working desktop): set the game to host
(`mode=host`, a passphrase), load a level with enemies, then run the real protocol peer:
```
tools\testclient.exe 127.0.0.1 7777 damage <passphrase>
```
Modes: `bot` (walks up, attacks), `damage` (bot + drains the nearest enemy's health — watch it
die), `circle` (smooth orbit — use this to isolate jitter: if the puppet is smooth in circle mode,
the jitter was the bot's oscillation, not the mod), `idle`. It authenticates like a real client,
so everything goes over real UDP. It has no body, so it CANNOT test whether enemy attacks damage a
real player — use the self-test for that.

**Self-test** (`selftest=1` in ini): once you are standing in an active fight, the mod teleports an
enemy next to you and forces it to attack — under its own AI, then via the joiner's replay path —
and logs whether your health dropped. It answers "do replayed enemy attacks damage a player?" on
one machine. It refuses to run if there is no active (non-pooled) enemy nearby (it will not yank a
dormant pooled actor, which could crash). Set back to 0 for normal play.

**Session logging:** always-on heartbeat lines in `SifuCoop.log`
(`%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log`): `coop: === SESSION LIVE ===`, a 5-second
heartbeat (role, RTT, levels, enemies known/active/driven/unmatched, damage out/in, rejected), and
one-time `FIRST peer damage applied` / `FIRST local hit` markers. Put two machines' logs side by
side to see what each saw. `verbose_enemies`/`verbose_orders` add per-event detail.

**Two real machines:** connect via ZeroTier (both join one network at my.zerotier.com, tick the
Auth box for each device, use the `10.x` Managed IP), matching passphrase; host loads a level, the
joiner is auto-pulled in. Then check: see each other move; enemy positions/health/death agree; the
joiner can kill a real enemy; room clears on both; level transitions carry the joiner along.

---

## 11. ENVIRONMENT / LOGISTICS GOTCHAS

- **Two PCs.** The desktop runs the matching `"epic"` build. The **laptop runs a DIFFERENT Sifu
  build**, so the mod is INERT there (`guard: UNKNOWN BUILD` in the log). Fix: either update both
  to the same Sifu version, OR run `.\build.ps1` ON THE LAPTOP (needs its own
  `Sifu-Win64-Shipping.pdb` + g++ + Python) to add its fingerprint to `builds/` and produce a dll
  that serves both. Check the log line `guard: build 'epic' matched` vs `UNKNOWN BUILD` to know
  which case you're in.
- **F1 menu may not open on the laptop** (Lenovo function-row defaults to media keys — try Fn+F1,
  or toggle FnLock via Fn+Esc; also the swap-chain overlay can fail on hybrid Intel+NVIDIA GPUs).
  This does NOT block anything: everything is configurable in `SifuCoop.ini` (host/join/passphrase),
  so F1 is a convenience only.
- **Launching the game from here:** direct-launching `Sifu-Win64-Shipping.exe` repeatedly makes it
  exit at startup (needs the Epic launcher session). Launch via
  `com.epicgames.launcher://apps/d36336f190094951873ed6138ac208d8?action=launch&silent=true`.
  This is an environment quirk, NOT a mod bug (vanilla Sifu with the dll removed fails identically).
- Both machines must use the SAME protocol version (currently v7) and the SAME passphrase.

---

## 12. IMMEDIATE NEXT STEPS (recommended order)

1. **Redeploy the v7 dll** to the game folder and refresh the ship zip (built dll is v7, deployed
   is v6).
2. **Decide the friendly-fire / remote-attack model** (§8.1) — this is the core of "make co-op
   feel right." Until solved, remote players don't visibly attack in co-op.
3. **Apply the jitter fix** (§8.3) — smooth puppet yaw, raise interp floor. Low-risk, high felt
   value. Verify with `circle` mode.
4. **Verify a REAL enemy kill** and **room-clear following** on two machines (§8.2, §9).
5. Then: weapons visual (§9), boss phase behavior, wiring the RunState scaffold for age display.

**Discipline to preserve:** this codebase earns trust by never claiming unverified things work.
Build warning-clean, keep the crypto vectors passing, put anything risky/unverifiable behind a
default-off config toggle with an honest comment, and clearly separate "verified live" from
"implemented but untested" from "researched only" in every status you write.
```
