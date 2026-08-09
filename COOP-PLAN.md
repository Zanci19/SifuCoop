# Sifu Co-op — full two-player playthrough

The goal is two people playing Sifu together against the game's own enemies: shared
encounters, shared kills, shared progress through a level. Sparring is a subset that
already worked and is kept as *Versus* mode.

---

## Phase C/D subsystem status (researched against the shipped PDB)

Seven further subsystems were investigated by reading the game binary directly. The
finding that shaped all of them: **the host-authoritative-enemy design makes several of
these either automatic or nearly pointless, and turns the rest into small additions rather
than new systems.** Honest status of each:

**Death, aging, respawn — done, by not touching it.** Age and the death counter live in a
per-character `UStatsComponent` (`BPF_DecrementDeathCounter`, `BPF_GetAgeIncrement`,
`BPF_ResetDeathCounter`). The mod never touches that component, and a source audit confirms
`SetHealth`/`SetDown`/`ApplyDamage` are only ever called on enemies and on the puppet, never
on the local player. So each player ages independently in their own game, exactly as
singleplayer, and the mod only *reads* the local player's `IsDown()` to send it and applies
the peer's to the puppet. Die → age → get up in place already replicates as fall-then-rise.
The one unhandled edge is the age cap ending a run mid-session (a level reload), which is
rare and remains the open D2 question; it will surface as the peer's puppet freezing until
they reappear.

**Room-clear and doors — no door to flip; it is emergent.** There is no `ADoor`/`AGate`/
`ABarrier` class to force. Room completion is driven by the AI director's
`OnEnemyDeathDetected` and `AThePlainesGameState::m_fRoomClearedLifePercent` — i.e. by
enemies dying, which the mod already synchronises host-authoritatively. So a room the host
clears should clear on the joiner too, *provided the joiner's own game observes its local
enemies dying* (the mod calls the down-state machine on them, which should feed the
director). This is the single most important thing to verify on a real two-machine run; if
it does not follow automatically, the fix is to replicate the game-state clear percentage
rather than to build a door system. **Status: likely automatic, unverified.**

> **Community input (Sifu modding Discord, 2026-08-07).** From Bondi, who has worked on
> Sifu's doors before: *removing* a door is not the hard part — **the door is what streams
> the next sub-level in.** (It also explains the stutter when opening a big door.) That
> changes the shape of the problem in a way worth writing down: a door is not a barrier to
> unlock, it is a level-streaming trigger, so two players either pass through it together
> or the one left behind is standing in a sub-level the other has already unloaded. If
> room-clear does *not* follow automatically, the answer is still not "build a door
> system" — it is to make sure both players cross a streaming boundary together, which is
> what the new **Teleport to partner** action (§7b) exists to make possible without any
> per-door work.
>
> Two other suggestions from the same conversation, both recorded rather than actioned:
> release for the **arenas first** (one map, no story-mode streaming, so none of the above
> applies) as a demo while story mode is still unreliable; and in-engine cutscenes let a
> second player walk around during them, which is funny rather than useful — cutscenes
> remain explicitly out of scope per the user's own decision in §2.

**Shrines and progression — already independent, by design.** Because `UStatsComponent` is
per-character and never touched, each player spends their own XP and keeps their own skills
with no work. Shrines are world actors each machine simulates separately; there is no shared
mutable state for two players to corrupt. **Status: believed to work with no new code;
verify that two players at one shrine do not stall each other.**

**Bosses — ordinary characters the enemy sync already covers.** The bosses enumerate as
`AFightingCharacter`s (e.g. `BP_Fengjie_Base_C` already appears in the tracked pool), so
their position, health and death replicate through the existing enemy path. The risk is
separate scripted *phase* state driven on the host fighting the joiner's local script.
**Status: basic fight likely works; phase transitions unverified and the highest desync
risk.**

**Enemy targeting — mostly implicit, low value to replicate.** Client-side enemies have
their AI stopped and are driven by position, so they have no combat target of their own to
synchronise. Which player the host's AI is fighting is already conveyed implicitly by where
it drives the enemy. Explicit sync (`BPF_GetTargetForAction` reads an `AActor*`,
`BPF_SetTargetForSlot`/`BPF_UpdateLockMoveTarget` take one) would only correct facing and
the lock-on reticle — polish, deferred. **Status: deferred as low value.**

**Weapons — the one real gap that needs risky, untestable work.** `ABaseWeapon` has
`BPF_AttachWeapon`, `BPF_DropWeapon` and `BPF_GetWeaponData` (returns a `UBaseWeaponData`
asset whose path is portable). Observing which weapon the local player holds is feasible;
making a remote character hold the same means spawning and attaching a weapon actor, which
is genuinely risky and cannot be verified without two machines. **Status: API located,
implementation deferred as the highest-effort / highest-risk item.**

**Exact-strike fidelity — no clean entry point found.** See the dedicated section below; the
investigation stands, and nothing in the symbol survey turned up a reachable launch point
above the ability-system decision. **Status: remains deferred.**

---

## Architecture decisions

**Host-authoritative for enemies.** One player's game is the source of truth for every
enemy: their AI, position, health and death. The other player's enemies are puppets with
their AI switched off.

**Peer-authoritative for players.** Nobody can tell you that you were hit. Each machine
resolves its own player's damage and reports the result. In a game decided by parry
timing this is not a compromise — it is the only arrangement where a parry is judged on
the machine where the button was pressed.

**Why not a dedicated server.** A separate server process cannot adjudicate Sifu combat —
the hitboxes, animation timings, parry windows and AI live inside the Windows executable.
Only a running Sifu can decide whether a hit landed. A headless instance is impossible
too: this is a Client target with server code compiled out, and it needs a renderer. A
Linux box can usefully serve as a **relay** (NAT traversal, later matchmaking) but never
as authority. This was re-examined and the conclusion did not change.

**Latency split.** Each machine always owns its own player, so your own moves are instant
and never mispredicted. Only enemy reactions wait on the host. The interpolation buffer is
now sized from measured RTT and jitter rather than a fixed 60 ms.

**Damage across the wire is a running total, not an event.** The joining player reports
"I have dealt N to this enemy in total"; the host applies the difference and echoes back
what it has accounted for. A lost packet costs nothing — the next one carries the whole
story — and a duplicate applies nothing twice. The echo is also the gate that stops the
host's older, higher health value from visibly healing an enemy the client just hit.

**Death and aging: per player.** Each player keeps their own age and counter, as in
singleplayer. Open question when we get there: what happens when one player hits the age
cap mid-run while the other has not.

---

## STATUS

Phase A is complete and Phase B's essential half is complete and **verified against the
live game**. What follows is what was actually observed, not what was intended.

| Item | State |
|---|---|
| A1 enemy identity (name hash) | **done** |
| A2 client AI suppression | **done** |
| A3 enemy transform replication | **done, verified** |
| A4 enemy orders/attacks | **done** — timing right, exact strike approximate |
| A5 enemy health and death | **done, verified** |
| B1 client hit reporting | **done, verified end to end** |
| B2 aggro/targeting | **done via faction** — enemies treat both players as enemies |
| B3 latency tuning | **done** — RTT/jitter measured, buffer sized from it |
| C1 spawn/wave agreement | **partial** — the joiner shows exactly the host's set |
| C2 room clear, doors | not started |
| C3 environment, weapons | not started |
| D1 level transitions | **done** — the joiner follows the host automatically |
| D2 death/aging/respawn | per player, untested together |
| D3 shrines and upgrades | not started |
| D4 cutscenes and bosses | not started |
| D5 save/progression ownership | not started |

### Verified against a running Sifu (Hideout 4, single machine + protocol bot)

- Build fingerprint matched; every new offset resolved: `health=+0x138 maxhealth=+0x140
  guard=+0x160 combo=+0x608`.
- Real player vitals over the wire: `hp=35/120`, matching the save.
- Eight active enemies published with correct per-archetype health pools (60, 100, 120,
  210), positions tracking their patrols, and the ~55 pooled ones correctly excluded.
- **A joining peer killed a real enemy.** Reported totals 48 → 90 → 138; the host's copy
  went 100 → 52 → 10 → 0, stopped moving, and was recycled into the pool. That is
  `BPF_ApplyDamage`, the cumulative-total protocol, the ack echo and the game's own death
  path, all working together.
- Puppet spawned as `faction 0 (you are 0) — CO-OP, allied against the level`.
- Level auto-announce, peer down/up transitions, and clean reconnect after a timeout.

**Still never tested with two real game clients.** Everything above was exercised against
a bot that speaks the protocol. The paths a bot cannot reach are: enemy attack echo
landing on a real second player, both players damaging the same enemy simultaneously, and
anything involving two real pawns in one room.

### What the live tests changed

Five real defects were found by running it rather than by reading it. Listed because each
one is a class of mistake, not a typo:

1. **Enemy ids collided.** The name buffer was `char[40]`, and
   `BP_AICharacter_Grunt_M_HD4_Banker_03_C_0` is exactly 40 characters — so the trailing
   instance number was truncated away and `_C_0` and `_C_1` hashed identically. Two enemies
   shared one id, meaning every packet about either addressed both: they would have been
   driven to the same place and damaged together. Invisible except as "those two enemies
   move as one". The buffer is now 96 bytes, and a collision check runs on every roster
   rebuild and says so loudly, because this failure mode is otherwise silent.
2. **Enemies die without ever setting the down flag.** `IsDown()` tracks the knockdown
   state; an enemy at zero health simply dies. The joining side was reading only the flag,
   so it would have kept driving a corpse around the room. Zero health now counts as down.
3. **Enemies leaving the host's set were treated as "never activated"** and hidden, which
   would have made every kill end with the body blinking out. They are now put down and left
   for the local game to clean up; only enemies the host never had are hidden.
4. **The puppet pointer dangled across a level change.** The actor is destroyed with the
   level and nothing says so, so every subsequent frame dereferenced freed memory. This was
   survivable while changing level took a deliberate keypress; automatic level following put
   it on the ordinary path through a playthrough.
5. **The enemy table could be read after the level it described was gone.** The attack hook
   fires on the engine's schedule, not ours, so a peer's packet naming an enemy during a
   transition resolved to a freed actor. Lookups now refuse to answer from a table built in
   a world that is no longer current.

An earlier version also retired the client's damage total the moment the host caught up.
That made the two counters incomparable from then on, so the host's stale-but-larger total
satisfied the test immediately after every new hit and the enemy's health visibly sprang
back before settling. Both counters now run monotonically for one body's lifetime and reset
together. The general lesson: never reset one side of a comparison without the other.

### Enemy identity is stable — measured, not assumed

The whole pairing scheme rests on both machines deriving the same id for the same enemy.
Two independent launches of the same level produced **62 enemies, 62 distinct ids, identical
name sets, and every name mapping to the same id**. Pool contents are deterministic.

Note what this does *not* require: that both machines activate the same pool instance at the
same spawn point. They need not. The host says "the enemy named X is here"; the joiner finds
*its* X and drives it there. Pairing by name rather than by spawn point is what makes the
scheme indifferent to activation order, which is the one thing that genuinely does vary.

### Security

Every packet carries a truncated HMAC-SHA256 keyed by a shared passphrase, with session
nonces from both sides mixed into the key so a recorded session cannot be replayed into a
later one. Unverified packets are dropped before a field is read; once a peer is accepted,
only that address is listened to. Level paths are shape-checked before reaching `OpenLevel`,
and every float is checked for NaN before it becomes a position or a health value — a NaN
position propagates into the movement component and stays there, because every later
comparison against it is false.

The SHA-256 and HMAC implementations are checked against the published FIPS 180-4 and RFC
4231 vectors on every build, and the build fails if any vector does not match. This is the
one part of the mod where "it appears to work" proves nothing: a broken HMAC either rejects
everything, which is obvious, or accepts everything, which is invisible and defeats the
entire point.

---

## Move selection — investigated hard, still not solved

Exact attack replication remains blocked, for a structural reason established by ruling
out four approaches with evidence:

1. Stamping the attack id at `+0x50` of `FDelayedActionAttack` — no effect.
2. Swapping the combo tree at `+0x48` — no effect. (`+0x48` is the *character's* tree.)
3. Overriding `FComboTransitions::GeNextAttackID` — the hook installed correctly and fired
   **zero times** across 21 replays of the entry point we use.
4. Implicitly: the struct is an *output*. `+0x50` is written after the decision.

The real chain is

    GetBestAttackAction -> InputAction
    TryLaunchNewAttack  -> GeNextAttackID(combo, transition) -> attack id
    PrepareToLaunchAttack(delayed action carrying that id) -> LaunchAttack -> PlayOrder

Selection happens in `UAttackAbility`, driven by the gameplay-ability system. Our replay
enters at `PrepareToLaunchAttack`, *below* the decision, so the component reuses whatever
it last resolved.

**What shipped instead.** The replay now uses the *target's own* combo asset, read from
`UAttackComponent::m_DefaultCombo`, so an enemy swings with an enemy's moveset and the
wire never carries an asset path. Both screens agree that a character attacked and when.
Which strike comes out may differ.

**Remaining options, neither cheap:**

- Keep the puppet's AI controller active so attacks trigger naturally, and override
  selection via the `GeNextAttackID` hook (which does fire on that path). Gives correct
  moves but AI-driven *timing* — the two screens would agree on what was thrown and
  disagree slightly on when.
- Construct the ability call directly: `TryLaunchNewAttack` is private and takes
  `FSCGameplayAbilityActorInfo&` plus a spec handle. Building those by hand is the same
  class of work that crashed the game repeatedly earlier.

**Still recommend deferring.** Attack fidelity is polish; a playable shared fight is not,
and it no longer depends on this.

---

## Friendly fire — Sifu's relationship system (candidate (b), now reachable)

The #1 co-op felt gap: the puppet's replayed attacks are real hitboxes and same *faction*
does not exempt them, so the remote player's puppet can punch and kill you. Mitigation to
date: `echo_player_attacks=0`, so the remote player moves but never visibly attacks.

**Found in the PDB (verified against the shipped binary, not the game):** Sifu has a real
per-actor relationship system, all Blueprint-exposed and therefore reflection-callable — no
new offsets, no ABI guessing:

- `USocialComponent::m_Relationships` is a **`TMap<AActor*, ERelationshipTypes>`** — the
  relationship is stored **per actor**, which is exactly the per-instigator exemption the
  earlier handoff said had no API. (`m_CoopGroup`, `m_InFightPlayers` also exist — Sifu
  carries an internal co-op/social framework.)
- `enum ERelationshipTypes { Ally, Enemy, Neutral }` (`Z_Construct_UEnum_Sifu_ERelationshipTypes`).
- `ABaseCharacter::BPF_GetSocialComponent() -> USocialComponent*` — reach the component from
  any character by reflection.
- `ABaseCharacter::BPF_GetRelationship(AActor*) -> ERelationshipTypes` — read a relationship.
- `USocialComponent::BPF_ServerChangeRelationship(AActor*, ERelationshipTypes)` — set one.
  (Native `USocialComponent::SetRelationship(AActor*, ERelationshipTypes)` also exists.)

**Hypothesis:** melee hit-detection consults this map (it is the most likely reason two
enemies of one faction never damage each other). If true, setting the puppet<->local-player
relationship to the friendly value makes the puppet's swings pass through you the way one
grunt's swing passes through another — and `echo_player_attacks` can then be turned back on
so the remote player visibly attacks with no friendly fire.

**What shipped (behind `friendly_relationship`, default OFF — see `src/game/puppet.cpp`
`MaintainFriendlyRelationship`):** each frame, once per puppet, the mod discovers the
friendly enum value at runtime (the relationship between two live same-faction enemies —
exact, no hardcoded number, survives any enum reordering), then calls
`BPF_ServerChangeRelationship` in both directions via each character's social component. If
fewer than two enemies are in the scene it simply defers. All reflection; nothing risky.

**UNVERIFIED.** Whether the melee path actually gates on this map is the open question, and
it cannot be answered without running the game. Single-machine test: `friendly_relationship=1`,
spawn a puppet (F9), replay an attack on it (F6) while standing next to it, and watch whether
your health drops — that is the same setup that first exposed the friendly-fire bug. Two open
risks: (a) `BPF_ServerChangeRelationship` is a Server RPC — in standalone SP the local player
is the authority so its `_Implementation` should run locally, but confirm; (b) the melee path
may key off something other than this map, in which case the value is set but ignored.

---

## How the offsets are obtained

Two mechanisms, both regenerated per build by `build.ps1`:

- **Function addresses** come from the shipped PDB by mangled-name substring match.
- **Struct member offsets** come from Unreal's own generated reflection tables. Every
  reflected property records its byte offset inside the owning type, and the PDB names
  each table entry, so `tools/pdbdump` reads them straight out of the executable. This
  replaced hex-diffing struct dumps at runtime and is how `m_fHealth`, `m_fCurrentGuard`,
  `m_eFaction` and the component pointers were found — exactly, in one pass.

  Bitfields are the exception: they carry a `SetBitFunc` rather than an offset, so a bool
  like `m_bIsDown` cannot be located this way. Nothing needs one — `IsDown()` and
  `SetIsDown()` cover the only case.

`tools/pdbdump/structdump.py` exposes the same machinery for exploration:

```bash
python structdump.py <exe> <pdb> UHealthComponent FNetOrderStructAttack
```

---

## Phase C — Encounters and rooms

**C1. Spawn triggers and waves.** Partly addressed: the joiner shows exactly the set the
host has activated, hiding anything its own game spawned independently. What is *not*
done is suppressing the joiner's spawn logic at source, so this is a correction rather
than a prevention.

**C2. Room-clear conditions, doors and barriers** agreeing on both machines. Instrumented,
not yet forced. There is NO door/barrier actor (researched); room completion is emergent
from `AAIDirectorActor::OnEnemyDeathDetected` + `AThePlainesGameState::m_fRoomClearedLifePercent`
(member at +0x39C, read via `UGameplayStatics::GetGameState` + a ThePlaines class guard). The
mod now reads the local percentage and sends it, and logs the peer's, so a two-machine session
can show whether the joiner's room clears on its own once its synced enemies die (the expected
behaviour, since enemy death is already host-authoritative). Applying the host's percentage into
the local game (`fix_room_clear`) is deliberately still dormant — writing a value with
unverified semantics/direction is a guess until that test is run. See `src/game/runstate.cpp`.

**C3. Environmental interactions** — thrown objects, weapons, destructibles. Not started.

---

## Phase D — A full playthrough

**D1. Level transitions together.** Done. The host announces every level change and the
joiner follows, with a fallback that notices a persistent disagreement and travels anyway
— an invite can be missed, and two players in different levels see nothing at all with no
error, which is indistinguishable from the mod being broken.

**D2. Death, aging and respawn**, per player, including the age-cap question above. Aging is
already independent (the mod never touches the local `UStatsComponent`). The run-state channel
now also reads the local age (`BPF_GetStatsComponent` -> `BPF_GetCharacterAge`) and sends it, and
logs the peer's, so each player can see the other's age. Purely informational — nothing applies
the peer's age locally, by design.

**D3. Shrines, upgrades and skill unlocks** — two players sharing or splitting a shrine.

**D4. Cutscenes and bosses** — likely the hardest, since bosses are heavily scripted.

**D5. Save and progression ownership** — whose run is it, and what the joiner keeps.

**Realistically:** D3–D5 are where "months" turns into "unknown". Each may be a day or a
fortnight, and some may need a different approach entirely.

---

## Immediate next step

**Play it with two people.** Every remaining unknown is now behind that door, and the
step-by-step list in `SETUP.md` is written to isolate a failure to one layer. The two most
likely disappointments, in order:

1. **Enemy attacks not landing on the joining player.** The echo makes the enemy swing;
   whether that swing's hitbox actually damages the local player is the one link a bot
   cannot test. If it does not, the fallback is host-authoritative player damage — the
   host already knows what its enemies did to the puppet, and the same running-total
   mechanism that carries enemy damage would carry it back.
2. **Enemies fighting over position** when the host's AI targets the puppet while the
   joiner's local physics push their copy elsewhere. The symptom would be jitter, and the
   first thing to try is a longer interpolation delay.

After that, C2 (room clear and doors) is the next thing that stops a level from being
completable together.
