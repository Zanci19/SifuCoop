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

---

## 13. STATE AT 2026-08-10 07:40 (written while the owner was away)

Deployed to the **Epic** folder only. The Steam machine was unreachable (`Z:` not mounted),
so it is still on the previous build — **redeploy it before the next two-machine test**:

```
.\build.ps1 -GameDir "Z:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam -Deploy
```

### Fixed this pass

- **The action mirror fired once per session.** `UPlayerAnim::m_LastActionAnim` is a *last*
  action: it keeps pointing at its asset after the action ends, and a repeated move never
  changes the pointer, so an edge trigger on the pointer alone almost never fires. Today's
  log across three sessions of fighting contains exactly one `action: sent`. It now triggers
  on a new asset **or** a cursor that has gone backwards.
- **What that one line told us:** the asset was a death animation
  (`MC_man_barehands_death_high_east_FL_light_60fps`). So `m_LastActionAnim` carries deaths
  and takedowns — which is what the channel was added for — but does **not** appear to carry
  ordinary strikes. Those still come from the order path.

### New, default OFF: `peer_fights_locally`

The first route to "the client gets a real fight" that does not go through the host's
director. Only a `DirectOpponent` may swing; that ticket is allocated per target; the puppet
standing in for the joining player is not a target the host's director will ever allocate one
to. Forcing it there is what crashed in `AddRemoveCandidate` on death cleanup and parked four
of five enemies as `NonOpponent`.

On the joining machine that player **is** player zero — a legitimate target its own director
allocates attackers to normally. So for enemies the host reports with `kEnemyTargetsPeer`,
the joiner stops driving their transform and restarts their behaviour tree.

The cost: those bodies stop being position-authoritative, so the two machines will disagree
about where they are standing. That is the trade, and why it is off. Turn it on with
`peer_fights_locally=1` on the JOINING machine and watch for
`enemies: <name> handed to local AI`.

### Still open, with what is known

- **Enemy blocks and parries do not replicate.** `m_LastActionAnim` is declared on
  `UPlayerAnim`; `USCAnimInstance` (the enemies' base) has no equivalent — it was dumped and
  checked. It does have `m_ActionToActionBlendForRep` / `m_LocoToActionBlendForRep`, whose
  names suggest replication state; that is the first place to look next.
- **Hit reaction animations.** Needs `UHitComponent::BPF_GenerateFakeImpact`, whose
  `FHitRequest` reaches `FHitBox -> FHitboxDataRow -> TSet`. §7b already established those
  cannot be rebuilt from outside the owning process. The reachable half is done: the enemy
  registers being hit (`mirror_hit_reactions`, which until this week was an ini key read by
  nothing at all).
- **Corpses / death animation on the joiner** is implemented but **unverified**: no enemy
  died in any of the three logged sessions, so there is no evidence either way yet.

---

## 14. THE LISTEN SERVER IS NOT POSSIBLE — settled 2026-08-10, do not retry

`UWorld::Listen` is a **stub with no body** in the shipped executable. It was folded by
the linker's identical-code-folding into the same address as every other trivial
`return false;` function in the binary:

```
0x0085E7C0  shared by 82,450 symbols   <- UWorld::Listen
0x00A58420  unique                     <- UIpNetDriver::InitListen
0x01A33D60  unique                     <- AThePlainesGameMode::PostLogin
0x0394F630  unique                     <- UWorld::ServerTravel
0x019BC9E0  unique                     <- AFightingCharacter::GetLifetimeReplicatedProps
```

Count the symbols sharing an address: one means a real function, tens of thousands means
ICF collapsed an empty body. Everything *around* networking survived the shipping build —
the IP net driver can listen, the game mode can accept a login, characters really do declare
replicated properties — but the single function that turns a loaded map into a server does
nothing and returns false.

That is why every route failed identically, and why none of the fixes along the way changed
the outcome: `UEngine::LoadMap` calls `World->Listen(URL)`, gets false, and finishes loading
the map as an ordinary standalone level. Confirmed with the driver active, the port free
(7778), and the option verifiably delivered through `UGameplayStatics::OpenLevel`'s real
`Options` parameter. The map reloads every time; the world is never networked.

**The mistake to learn from:** `AFightingCharacter::GetLifetimeReplicatedProps` existing was
treated as strong evidence that native co-op was reachable. It is a real function and the
characters do replicate — but that says nothing about whether the server side survived
packaging. The inference chain skipped the one function that had to exist, and the check
that settled it (symbol count at the address) costs one `awk` line and should have been the
first thing done, not the last.

**The server half of UE4 networking is compiled out of this executable.** Two functions are
folded stubs, and they are exactly the two that live behind `#if WITH_SERVER_CODE`:

```
0x0085E5C0  11,097 symbols  <- UNetDriver::ServerReplicateActors   (folded stub)
0x0085E7C0  82,450 symbols  <- UWorld::Listen                      (folded stub)
0x0347B1D0  unique          <- UActorChannel::ReplicateActor
0x03664E20  unique          <- UNetDriver::TickFlush
0x0328BC50  unique          <- AActor::PreReplication
0x035D97F0  unique          <- UWorld::SpawnPlayActor
```

`ServerReplicateActors` is the function that walks the world's actors and sends their
replicated state to connected clients. It has no body. So even a hand-built listen server
would accept a connection and then replicate nothing, forever -- `ReplicateActor` being real
does not help, because it is only ever called *by* the function that was removed.

This closes the loophole below: there is nothing left to hand-build except UE4's replication
loop itself, from outside the process. And that is precisely what this mod already is -- a
hand-written stand-in for `ServerReplicateActors`, for the subset of actors that matter.
Rewriting it generically would be a bigger, worse version of what is already working.

**What would still be theoretically possible**, and is not recommended: hand-building the
listen server — `GEngine->CreateNamedNetDriver`, `UIpNetDriver::InitListen` (real), then
assigning the driver to the world and setting its net mode by hand. That reimplements
engine bookkeeping `LoadMap` normally does, needs an `FURL` built by hand, and a world
member offset that is not in the reflection tables. High risk, and it would have to be right
about several unverifiable things at once.

**Consequence:** the UDP mirror is the architecture, not a stopgap. Perfect enemy parity
between the two machines is not achievable — two simulations can be made to agree closely
and never exactly. Effort is better spent making the mirror feel good than chasing identity
it cannot reach.

`native_network` is back to 0. The code and the two menu buttons are left in place, behind
that switch, so nobody has to rediscover any of this.

---

## 15. THREE MORE SETTLED FACTS — 2026-08-11, from the live logs

Same rule as §14: each of these is one cheap check that should have been run before anything
was built on the assumption it disproves. They are recorded so the check is never run a fourth
time.

### `MultiCastPlayOrder` never fires. The FBuffer route does not exist.

The hook installs on both machines every session (`order: MultiCastPlayOrder hook ACTIVE`) and
has been called **zero times in every log ever recorded**:

```
grep -c multicast <both SifuCoop.log files>   ->  0
```

It is a multicast RPC. There is no net driver, so there is no multicast — the same reason
`UWorld::Listen` fails in §14. The comment in `orders.cpp` that calls the engine's serialised
`FBuffer` form "the correct target" for order replication is describing an unreachable path.
Byte-level `FNetOrderStruct` reconstruction is still abandoned for the reasons already given;
the difference is that there is now no engine-provided alternative either.

The same fact kills `MulticastChangeRelationship`, which is how `BPF_ServerChangeRelationship`
forwards. That is why the mod calls `USocialComponent::SetRelationship` natively instead — and
that function is real (see below).

### Faction is not what Sifu's AI discriminates on.

`esync`'s `fac=` field was added to settle exactly this, and it did, immediately: **every enemy
reads faction 0, and so does the player** (`enemies: YOU hp=91/114 guard=187 faction=0`).

There is no "enemies' faction" to move the puppet into. Both sides are already in the same one.
The bystander-faction retry loop searched every active enemy for a faction different from the
player's, found none, and did nothing — which is why commits `833fa6d` and `f280815` produced
no observable result of any kind, not even a failure.

`MaintainBystanderFaction` and the enemy-faction search in `ConfigureAsRemote` are **removed**.
`SetFaction` is kept for Versus, which does rely on the two sides differing. Whatever decides
that four of five enemies walk to the puppet on the joining machine, it is not faction.

### `ABaseCharacter::UpdateRelationshipToOtherCharacters` is a folded stub.

```
0085CA30   17,717 symbols   <- ABaseCharacter::UpdateRelationshipToOtherCharacters
01B779B0   1 symbol         <- USocialComponent::SetRelationship
01B5B7F0   1 symbol         <- USocialComponent::BPF_GetRelationship
019B4910   1 symbol         <- AFightingCharacter::BPF_GetRelationship
019953A0   1 symbol         <- ABaseCharacter::BPF_GetRelationship
```

Nothing recomputes relationships behind this mod's back. `MaintainFriendlyRelationship`'s
re-assert timer is defending against a function with no body; the comment claiming a value
"that holds for one frame is worth nothing" is wrong about the mechanism. The timer is cheap
and is left in place, but it is not the reason anything fails.

**And the setter is not a no-op.** `USocialComponent::SetRelationship` is real, unfolded, and
populated in both build tables (`epic 0x01B779B0`, `steam 0x019B7FB0`), and `WriteRelationship`
prefers that native pointer over reflection. The log line that has been claiming otherwise --
`relationship write did NOT reach the map: the setter is a no-op here` -- rested on two things
that were never checked:

- **The readback used the wrong getter.** `ReadRelationship` asks the *actor*, which for a
  fighter dispatches to `AFightingCharacter::BPF_GetRelationship` — a different function from
  `USocialComponent::BPF_GetRelationship`, which belongs to the class that owns
  `m_Relationships` and is where every write goes. There is now a component-side reader
  (`ReadRelationshipViaComponent`) and both are logged.
- **The map-size probe is a hardcoded, unverified offset** (`kSocialRelationshipsMap = 0x0318`).
  A wrong offset reading a plausible constant is indistinguishable from a map that never grows.
  `RelationshipMapProbeTrusted()` now gates quoting it: the count is evidence only once it has
  been observed to move.

And it contradicted the *other* user of the same primitive. `AssertHostileToward` reports
success (`targets: 5 of 5 active enemies hold 'Fight' toward your partner`) using the same
setter and the same getter. It never sampled before writing, so "reads back Fight after being
set to Fight" may be a tautology on bodies that were Fight already. Both sites now log
`before -> asked -> after` on both getters. **One test round decides which measurement was
lying**, and no more relationship work should be planned until it has.

### Hit reactions are Orders — the FHitRequest wall is not the only door

`OrderReaction` and `OrderTargetReactionBlendSpace` are real Order classes in the shipped
binary, with `GetSubType`, `IsAMovingOrder` and `GetNetOrderStructRaw`. Hit reactions therefore
arrive through the `PlayOrder` path this mod already hooks and already replays per enemy. The
1104-byte `FHitRequest` with unreconstructable `FWeakObjectPtr` serial numbers is the wall in
front of `BPF_GenerateFakeImpact` specifically — it is not the wall in front of reactions.

One number is missing: which `order_type` is a reaction. The `playorder:` hook used to log its
first 40 calls per process, which is the menu and a few steps and has never once covered a
fight. It now keeps a permanent per-type census (`orders: census type:total/player/hit`) and
logs every order for one second after the local player's health drops. **A type whose count
rises only inside the hit window is the reaction.** Get that number before writing any replay:
two guesses at this have already crashed the game.

---

## 16. THE CORPSE RACE, AND WHERE HIT REACTIONS ACTUALLY LIVE — 2026-08-11 (second pass)

Three symptoms were reported after §15 shipped: the observing machine still saw dead enemies
standing, enemies that had already been hit let attacks pass straight through, and enemies
never showed hurt animations. The first two are one bug. The third had been mis-scoped.

### One race behind both the standing corpses and the pass-through hits

Measured on the joiner, one body, half a second:

```
07:28:28.659  esync: 000_Receptionist_Right hp=35/35 owned=1 dead=0
07:28:28.961  enemies: revived and re-registered ... from client-only death
07:28:29.079  death: 000_Receptionist_Right kill=1 anim=0 health_comp=1 -> down=1
```

The joiner simulates the enemies fighting it, so **it reaches zero first** — it landed the
killing blow and Sifu started that body's death sequence locally. The host has not been told
yet; its sweep still says alive, and 300 ms later the revive branch acted on that: health back
to 1, `SetDown(false)`, re-registered as targetable. That **aborts the death animation already
playing** — the whole of `anim=0`. Then the host's death arrives 118 ms later and kills it a
second time, from outside, with no hit to choose an animation from.

Three forced state transitions on one body inside half a second. §7b already recorded what that
does: *a body walked in and out of that state machine from outside comes back upright but no
longer a valid hit target.* That is the pass-through report, same cause, and it explains why it
was always "enemies that had already been attacked".

The §15 freshness gate did not help, because the sweep was **fresh** — the host was talking, it
was simply a few hundred milliseconds behind. Freshness was the wrong question. The right one
is *whose kill was it.*

Fix: a body that dies here under a brain this machine is running latches `died_locally`, and no
host "alive" resurrects it. The latch clears when the host agrees (converged), or when the pool
lifts the body back at full health, which is the one case the revive was originally written
for. It survives a table rebuild, because a refresh in that half-second would otherwise drop it.

### Hit reactions: the player's already work; only enemies had no channel

This was never "hit reactions are unreachable". The **player's** reactions replicate today:

```
host   07:28:31.039  action: sent '.../MC_Man_Barehands_HitReaction_Strong_High_East'
joiner 07:28:31.476  attack: cosmetic sequence playing actor=00000000
```

`UPlayerAnim::m_LastActionAnim` carries them, and the existing channel delivers them. What has
no channel is an **enemy's** reaction, because that field is declared on `UPlayerAnim` (verified
against the property table: `Z_Construct_UClass_UPlayerAnim_Statics::NewProp_m_LastActionAnim`,
and nowhere else) while enemies animate through `USCAnimInstance`.

**`EOrderType` is now decoded.** 72 enumerators extracted from the exe's generated name table
(`0x04A1F350`–`0x04A1FB48`, epic), same method as `ERelationshipTypes`. The census answered on
its first run: **type 3 is `Hitted`** — 17 in one fight, 4 inside the second after the local
player's health dropped. It had been flowing through the `PlayOrder` hook the entire time,
unnamed and therefore invisible. Also now named: `12 Pushed`, `10 KnockedDown`, `11 Dizzy`,
`65 Deflected`, `2 ParryVictim`, `33 StructureBroken`, `58 HittedGeneric`. Every log line that
prints an order type now prints its name.

**`OrderBase::GetAnimPlayed` is a folded stub** — `0085E5C0`, 11,097 symbols. Only `OrderAttack`,
`OrderDodge`, `OrderFallOnSlope` and `OrderPlayAnim` override it, which is exactly why attacks
replicate and nothing else does. `OrderHitted` inherits the stub, so there is no accessor for
the reaction it chose. One `awk` line, no build round.

`OrderHitted::OnStart` is real and unique (epic `01ADF720`, steam `0191F950`) and now hooked.
`PlayOrder` knows *who* is reacting, `OnStart` knows *what* is being played, and neither can see
the other's half — so `PlayOrder` arms and `OnStart` collects, the same pairing `LaunchAttack`
already uses for `OrderAttack`. The sequence is harvested two ways: a bounded scan of the
order's first 0x120 bytes, every candidate put through `LooksLikeUObject` and then identified by
asking the game for its **class path** rather than trusting an offset; and failing that, the
anim instance's active montage. Neither dereferences anything unvalidated, and a miss logs
which of the two failed. `mirror_hit_reactions` gates it and is now **1** in both deployed inis.

If both routes come back empty, the log will say so per order type, and the next step is the
order's own layout — not another guess.

---

## 17. THE SCAN THAT CRASHED IT, AND WHY BOTH PLAYERS LOOKED THE SAME AGE

### Do not scan an order object for UObject pointers. It has none, and it crashes.

§16 shipped a bounded scan of `OrderHitted`'s first 0x120 bytes looking for the reaction's
`UAnimSequence`, with every candidate put through `LooksLikeUObject` and then identified by
asking the game for its class path. It crashed on the first punch of the session:

```
Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0xffffffffffffffff
  UObjectBaseUtility::GetPathName()   UObjectBaseUtility.cpp:51
  UObjectBaseUtility::GetPathName()   UObjectBaseUtility.cpp:58
  UObjectBaseUtility::GetPathName()   UObjectBaseUtility.cpp:44
  dsound                              <- the class-path check
  dsound                              <- OrderHittedOnStartHook
  OrderBase::Start()                  OrderBase.cpp:252
```

`LooksLikeUObject` only proves a pointer is *shaped* like a UObject: vtable in module range,
ClassPrivate readable. `GetPathName` then walks the Outer chain of whatever it is handed, and
three frames up that chain it read -1.

**The evidence against it was already in the log before it was written.** The order dump prints
`order: #6 order=... bytes=0 fnames=0 actors=0 uobjects=0` — orders carry no recognisable
UObject pointer in the scanned range at all. The scan could not have succeeded even if it had
survived. Checking that one existing line would have cost nothing; instead it cost the owner a
session. This is the §14 lesson exactly, and it was ignored while writing a section that quotes
§14.

The scan is deleted with a comment saying why. What replaced it asks only the **anim instance**
what it is playing, which is one reflection call on a live actor and the same call the mod
already makes on the local player every frame.

Two further corrections in the replacement, both learned here:

- `OrderHitted::OnStart` runs at the TOP of the order, before Sifu has put the reaction on the
  mesh, so sampling there returns the previous animation or nothing. It now arms a 400 ms
  capture window and samples afterwards -- the shape the attack path already uses.
- The window holds an actor **hash**, never an actor pointer. An enemy can be killed and
  recycled into the pool inside 400 ms, and "the anim hook holding freed components" is already
  in this project's fixed-bugs list. The hash is re-resolved every frame; if the body is gone
  the lookup simply stops returning it.

If the anim instance turns out to hold nothing during a reaction, the log now says so per order
type, and the next step is the order's own layout -- established by dumping it offline, not by
scanning it at runtime.

### Both players looked the same age because nothing ever consumed the peer's age

Reported: host at 48, joiner at 20; on the host both characters looked old, on the joiner both
looked young. Each machine was showing its partner at its OWN age.

The puppet is a clone of the local player, so it carries the local player's age, and Sifu ages a
character through `UStatsComponent` with appearance following from it. The peer's real age has
been on the wire and in the log the whole time -- `run: peer age=...` -- and §? said plainly
that peer run state was "intentionally not applied". That was right for room-clear and for the
weapon; it was wrong for age, which is a visible defect rather than a missing feature.

`UStatsComponent::BPF_SetCharacterAge(int)` is Blueprint-exposed (it has an exec thunk), so this
is reflection like everything around it -- no offset, no unknown field. It is written to the
**puppet only**: the local player's age is their own run and must never be written from the
network. `sync_peer_age`, default on.

What is not yet known is whether Sifu refreshes the model from the stats component on its own.
The log says which happened rather than assuming: if the body still looks your age after
`run: partner's body aged to N`, the setter landed but the mesh needs an explicit refresh, and
that is a different search.

---

## 18. WHAT THE 12:58 RUN SETTLED — 2026-08-11 (third pass)

No crash. Four reports, and the log answered three of them outright.

### `USCAnimInstance::m_CachedCurrentPoseAsset` exists. The earlier note was wrong.

The montage route shipped in §17 was measured dead in one session:
`reaction: Hitted on 48FCE144 -- nothing on its anim instance for 400ms`, over and over, across
four different enemies and five order types. Sifu does not play reactions as montages, so
`GetCurrentActiveMontage` was always going to return null for them.

It plays them as **pose assets**, and `USCAnimInstance` has the field. Its whole property table
is nine entries:

```
m_ActionToActionBlendForRep   m_ActionToLocoBlendForRep   m_CachedCurrentPoseAsset
m_LocoToActionBlendForRep     m_MirrorAnimDB              m_bIsInCinematic
m_fCinematicLayerTypesCursor  m_fCinematicOverallWeight   m_fPreviewCinematicLayerTypesCursor
```

`m_CachedCurrentPoseAsset` is the enemy equivalent of `UPlayerAnim::m_LastActionAnim`, and it
is what the earlier note -- *"USCAnimInstance (enemy anim base) has no current-action asset"*,
"it was dumped and checked" -- missed. Offset `0x390` on both builds, resolved from the PDB by
the build tool like every other member. The reaction capture now reads it instead of asking for
a montage.

That is three separate times now that a confidently-worded negative in this document turned out
to be an unverified one: `BPF_ServerChangeRelationship` in section 15, and this twice over.

### Costume: same root cause as age, same fix

The puppet is a **clone of the local player**, so it wears the local player's outfit -- which is
why both characters wore the machine owner's costume on both machines. `m_iOutfitIndex` on
`UPlayerFightingComponent` (offset `0x33C`) is read directly, because there is no BPF getter;
`BPF_SwapOutfit(int32, UMaterialInterface*, bool)` is the reflected write. Note the THIRD
parameter -- the decorated name is `QEAAXHPEAVUMaterialInterface@@_N@Z` -- a short parameter
frame here would corrupt ProcessEvent's stack.

`AFightingCharacter` has no BPF getter for the fighting component (the whole `BPF_` surface was
listed), so it is reached the way the capsule already is: `FindObjectByPath` the class, then
`GetComponentByClass`. Protocol bumped to **15** for the new wire field.

Confirmed working on the way: **the age write does propagate to derived stats.**
`run: partner's body aged to 28` was followed by
`puppet: max health here 108, theirs 108 -- agreed`. Writing the stats component from outside
is honoured by the things computed from it. Costume is not one of them, which is why it needs
its own channel.

### The targeting problem is now precisely located, and it is the ticket

Host census, the same second, over and over:

```
targets: 1 enemies on YOU, 4 on the second player, 0 elsewhere, 0 idle
roles:   fighting YOU direct=1 indirect=4 non=0 none=0 |
         fighting your partner direct=0 indirect=0 non=0 none=0
```

Four enemies are **aimed** at the partner and **none of them has any combat role toward it**.
Aim and permission are different things: only a `DirectOpponent` may swing, and the partner is
allocated nothing, ever. That is one mechanism behind three separate reports -- the partner
takes no damage, enemies ignore the partner, and enemies keep hitting a dead host.

The joiner's log says the same from its side: `orders: YOU took` fired **zero times** all
session, against six on the host. The joining player is untouchable, and on their own screen
four of five enemies are aimed at the host's puppet -- which until now was `puppet_invincible`,
an invincible decoy soaking most of the room. That is switched off in both inis.

`BPF_ForceEnemy` remains disabled and must stay so: it is the one thing that *does* grant the
ticket (measured: `fighting your partner direct=1`), and it crashes in
`AAIDirectorActor::OnDeathDetected -> RemoveActorFromSystems -> AddRemoveCandidate` at 0x108
when anything dies, because a spawned clone is not a candidate the director's bookkeeping can
survive removing.

**The next experiment is `real_second_player=1`, and the reasoning is now specific rather than
hopeful.** The crash is on *removal of a clone* from the director. A pawn created by
`UGameplayStatics::CreatePlayer` is a real `AFightingPlayerController` with a game-mode pawn --
exactly the kind of actor the director's bookkeeping is built to account for. It is currently
`0` on both machines. It is not switched on in this pass because it changes what the second
body IS, and that deserves its own run rather than being folded into four other changes.

Meanwhile a dead player now hands the room over with the peer-aggro lease that already exists
(`targets: you are down -- handed N enemies to your partner`). No ticket, no `BPF_ForceEnemy`,
so it steers them without permission to swing -- and if the roles census still reads
`direct=0` while that line is in the log, the ticket is conclusively the only blocker left.
