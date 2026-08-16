# Enemy sync bug investigation

**Date:** 2026-08-12
**Scope:** Identify causes and prescribe fixes. **Do not treat this file as implemented work.**
**Protocol:** v17 (UDP mirror; no UE net driver).
**Primary code:** `src/game/enemies.cpp`, `src/game/puppet.cpp`, `src/game/orders.cpp`, `src/game/actors.cpp`, `src/net/protocol.h`, `src/game/coop.h`
**Companion memory:** `AI-HANDOFF.md` (settled facts, unsafe switches, log prefixes)

---

## How authority works (needed to read every bug)

| Domain | Who owns it |
|---|---|
| Enemy **health / death** | Host always |
| Enemy **position / attack selection** | Machine that owns that fight (`peer_fights_locally`) |
| Observer copy of a host fight | Brain stopped (`suppress_client_ai`); transform driven from wire; attacks usually cosmetic |
| Local player hits / parries | Owning machine only |

There is **no** native replication. Everything is hand-mirrored. Observer enemies are puppets unless the joiner claims them.

**Two different “target” fields (central finding, `AI-HANDOFF` §5):**

| Log census | Field | Writer |
|---|---|---|
| `targets:` | `UAttackComponent::m_Target` | `ForceAttackTarget` / `g_set_attack_target` |
| `roles:` | AI combat role | `ForceEnemy` only — **unsafe, forced off** |

Only **`DirectOpponent`** may swing. Aim without role → walk up, stand, deflect, kick corpses.

**Config trap:** `coop.h` defaults several live-needed switches to `false` (`mirror_hit_reactions`, `retarget_from_down_peer`, `observer_cosmetic_enemy_attacks_only`, `sync_enemy_death_animations`). Desired live values are in `AI-HANDOFF` §9. A machine on code defaults will still show several of these bugs even if code “has a fix.”

---

## Bug 1 — Attacks phase through after an enemy died and revived

### Symptom
Enemy was killed (or reached zero), then came back into the fight (pool recycle or false revive). Later swings pass through the body as if collision / targetability never returned.

### Root cause
Bodies that die under local simulation or host-applied damage reach zero **before** the host’s next `EnemyState` sweep carries `kEnemyDead`. Older sync treated host “alive” as authority and ran:

1. `SetHealth` upward
2. `SetDown(false)` — aborts death presentation
3. No stand-up / combat re-entry

That walks the body through `InternalSetDownState` from outside Sifu’s normal combat bookkeeping. Result: upright, often present visually, **not a valid hit target**. Documented in-code at `ApplyRemoteEnemies` (~1984–2015, ~2152–2182) and `AI-HANDOFF` §6.4.

A second path: applying host health via raw writes (instead of `ApplyDamage`) zeroed HP without retiring collision, so the local corpse was already untouchable while the host still thought it was alive.

### Current mitigation in tree (unconfirmed in play)
- `died_locally` latch: refuse host “alive” revive until full-health pool recycle or first-host-state stale-save case (`enemies.cpp` ~2023–2222).
- On genuine revive: `NotifyDownStateChanged(false)` + `SetActorPresent` + `RegisterEnemyTargetable` (~2233–2248).
- Periodic collision re-assert on live bodies (~500 ms).
- Knockdown **not** forced from wire (that alone caused the same “untouchable after stagger” class of bug).

### Fix (if still repro after confirming latch + revive path in both logs)
1. Keep `died_locally` until host `kEnemyDead` **or** full-health recycle; never clear on mere `!dead`.
2. On every revive edge, always run the stand-up triad (`SetDown(false)`, `NotifyDownStateChanged(false)`, presence + targetable). Log `revived and re-registered` once per body.
3. Never heal mid-fight from host (already gated by reset_gap); never `SetDown(false)` on stale sweeps (`EnemySweepIsFresh`).
4. After ownership handoff mid-death, re-assert collision once the body is live again.
5. Verify with `esync:` / `death:` on **both** machines: no `revived … from client-only death` while health is still racing; no flap of `died HERE` every frame.

**Files:** `enemies.cpp` (`ApplyRemoteEnemies`), `actors.cpp` (`SetDown`, `NotifyDownStateChanged`, `SetActorPresent`, `RegisterEnemyTargetable`).

---

## Bug 2 — Observer: host-killed enemies stand motionless (not on the floor)

### Symptom
Host kills an enemy; on the observer that body stays upright / idle instead of falling.

### Root cause
Observer brains are stopped (`KeepClientBrainStopped` / `suppress_client_ai`). Sifu plays death through Orders from a running brain. Writing health to 0 (or `Kill`) without a real impact **and** without `NotifyDownStateChanged(true)` leaves a standing “corpse.”

Measured history:
- Host restarted brain on lease expiry after peer kill → aborted fall (fixed: corpses never regain brain on lease expiry — `AI-HANDOFF` §0).
- Host peer-damage path applied lethal damage while brain already stopped → same standing corpse on host (`ApplyPeerDamage` ~1171–1231: brain restored **before** lethal hit; then `SetDown` + `NotifyDownStateChanged`).
- Observer death edge must still: damage → optional Kill → `SetDown` → `NotifyDownStateChanged` → optional death AnimSequence (`ApplyRemoteEnemies` ~2290–2365; `HealthKillHook` / `NoteEnemyDeathAnimation`).

Without `sync_enemy_death_animations=1`, observer may only get a generic down, not the owner’s fall clip. Without the notify, even generic fall may not play.

### Fix
1. On observer dead edge (host says dead / health ≤ 0): always `NotifyDownStateChanged(true)`; never claim `was_down` without presenting (`enemies.cpp` ~2264–2278 comment).
2. Prefer host death AnimSequence (`sync_enemy_death_animations=1`; `AnimationSemantic::Death` only).
3. Never `StartBrain` on a corpse after fall starts.
4. For peer kills on host: keep “brain back **before** lethal `ApplyDamage`” order.
5. Confirm config: `sync_enemy_death_animations` and death announce window after pool recycle (`PublishEnemies` death_announce).

**Verify:** `death:` lines with `kill` / `anim` / `down` on the observer; no standing body with `hp=0` and `down=0`.

**Files:** `enemies.cpp` (death branch, `NoteEnemyDeathAnimation`), `orders.cpp` (`HealthKillHook`), `actors.cpp`.

---

## Bug 3 — Observer: enemies chasing the host slide (no locomotion anim)

### Symptom
Enemy translates toward the host on the observer screen but plays idle / no run cycle — looks like sliding.

### Root cause
Locomotion AnimBP samples movement/root velocity. `DriveActorTo` wrote presentation velocity during `UGameEngine::Tick`; the character movement component then braked a body with **no input**, so by `USCAnimInstance::NativeUpdateAnimation` velocity was **0** → idle while transform teleports.

Documented at `puppet.cpp` ~66–73. Same class of bug the remote **player** already had.

### Current mitigation in tree (deployed, not play-tested per handoff)
- `NoteEnemyPresentation` stores velocity + speed band per enemy (24 slots).
- `SCAnimUpdateHook` re-publishes velocity **after** Sifu’s update, before graph eval (~391–417).
- Enemy path must **not** write `UPlayerAnim`-only fields (`m_vOwnerVelocity` etc.) — those are player-only and would corrupt enemy memory.

### Fix / verification
1. Confirm hook installed on both builds (`offsets::USCAnimInstance_NativeUpdateAnimation` non-zero; log shows ACTIVE).
2. Confirm `EnemyEntry.velocity_*` non-zero on chase in host publish (`PublishEnemies` samples `GetActorVelocity`).
3. If crowded rooms: raise `kEnemyPresentationSlots` (overflow silently keeps old path → slide).
4. Keep sampling real actor velocity, not transform delta alone.

**Files:** `puppet.cpp` (`NoteEnemyPresentation`, `SCAnimUpdateHook`, `WritePresentationVelocity`), `enemies.cpp` (`DriveActorTo` / publish velocity).

---

## Bug 4 — Enemy sync disagreeing between machines (general)

### Symptom
Positions, attacks, reactions, or “who is fighting whom” diverge; fights feel mirrored wrong.

### Root cause (composite, not one defect)
1. **Dual streams:** host `EnemyState` vs joiner `OwnedEnemy` for peer-owned fights — ownership thrash mid-order desyncs pose and attack.
2. **One brain rule:** `client_simulates_enemies=0` is required; two brains = two attack selectors (measured 2026-08-11).
3. **Observer is transform + cosmetic attack**, not a full combat sim — missing reaction/order channel (bugs 2, 6).
4. **Identity:** name hash + spawner `source_hash`; wrong match → drive the wrong body.
5. **Level gate:** sweeps refused if level paths differ.
6. **Health races:** joiner damage vs host catch-up (`host_caught_up`, phantom damage from recording wrong `last_local_health`).

### Fix direction
- Sticky ownership (claim debounce / min hold / lease — already present; harden against mid-montage flips).
- Exactly one simulating brain per enemy.
- Attacks: owner’s AnimSequence + semantic (`observer_cosmetic_enemy_attacks_only`).
- Add reaction/impact channel (bug 6).
- Keep health host-authoritative with `died_locally` + cumulative damage ledger.
- Always deploy **matching** dlls; new offsets must be regenerated on **both** Epic and Steam builds.

**Files:** `enemies.cpp` (`PublishEnemies`, `ApplyRemoteEnemies`, ownership), `orders.cpp`, `protocol.h`, `session.cpp`.

---

## Bug 5 — Enemies stop and don’t hit; often kick the dead / not-yet-revived partner

### Symptom
Enemies approach then freeze / only deflect; or they keep working a downed player while the survivor is ignored. Sometimes they only act when “prompted” (damage / aggro pulse).

### Root cause
**Primary — two-field bug (`AI-HANDOFF` §5):**
`ForceAttackTarget` sets aim. Permission to swing is a **director combat role** (`DirectOpponent`), allocated per target. The puppet/partner never gets roles because:

- `director_targets_partner` + `force_enemy_engage` **crash** in director weak-object bookkeeping (`FWeakObjectPtr::IsValid` @ 0x24c via `ReditributeCombatRolesForAllTarget`). Forced off in config load.
- Measured census: many `targets:` on partner, `roles:` direct=0.

So enemies **aim** at the partner and never become DirectOpponent → stand / deflect.

**Secondary — corpse targeting:**
Nothing cleared aim when a player went down, so they kept beating a corpse. `retarget_from_down_peer` steers attack-component target via peer-aggro lease **without** granting roles (`PublishEnemies` ~1282–1352). Aim moves; permission still missing for partner fights.

**Tertiary:** peer aggro after damage is temporary (lease). When it expires without a role, behaviour collapses again → “only hit when prompted.”

### Fix
**Do not** re-enable `force_enemy_engage` / `director_targets_partner`.

Viable directions:
1. Treat `peer_fights_locally` as the **only** reliable partner fight (joiner is player 0 on their machine → director allocates roles normally). Harden claim/release so host-owned enemies near the joiner transfer cleanly.
2. Keep `retarget_from_down_peer=1` so corpse aim clears immediately on down and does not re-lock until stand-up completes.
3. Longer / refreshed peer-aggro only helps **aim**, not swings — do not confuse census `targets:` with success.
4. Long-term: a partner body the director accounts for natively (not the crashed ticket path; `real_second_player` is a settled dead end).
5. If partner still takes no damage after roles exist: there is still **no wire message “puppet was hit”** — damage on the puppet never reaches the owning player (`AI-HANDOFF` §6.2). Model on `SendEnemyDamage`.

**Verify:** simultaneous `targets:` and `roles:` lines. Success for partner combat requires non-zero `direct` on partner **or** proof the fight is owned by the joiner’s local brain.

**Files:** `enemies.cpp` (`ForceAttackTarget`, retarget block, `DumpEnemyTargets`), `coop.h` (unsafe flags stay 0).

---

## Bug 6 — Hurt / push / fall sync on both machines

### Symptom
Observer (or host watching a peer-owned fight) sees health drop / position change without proper hit, push, or fall presentation. Player reactions often look fine; enemy reactions do not.

### Root cause
1. Enemy reactions go through Orders (`Hitted`, `Pushed`, `KnockedDown`, …). `OrderBase::GetAnimPlayed` is a stub; only Attack/Dodge/FallOnSlope/PlayAnim override — so there is no general “get anim from order” path.
2. `MultiCastPlayOrder` never fires (no net driver).
3. Captured reaction asset is often **`UPoseAsset`**, not `UAnimSequence` — montage Cinematic slot **cannot** play it (heap corruption if forced). Type-gated on send/receive.
4. Health mirroring via `ApplyDamage` changes numbers without an `FHitRequest`, so no local hit order.
5. Intended bridge: zero-damage `BPF_LaunchImpact` (`LaunchReplicatedImpact`) so the observer generates a **local** impact/reaction. **Default off** (`mirror_hit_reactions=false`); never confirmed live.
6. Forcing knockdown from wire via `SetDown` was deliberately disabled — it caused phase-through (bug 1).

Push/fall families live in `AIHittedDB` / archetype DBs (direction/height/strength). Copying HP + transform cannot select them.

### Fix
1. Enable `mirror_hit_reactions=1` on both machines; confirm `reaction: … LAUNCHED` on **OBSERVED** bodies (not only local-brain ones).
2. Sweep `_fDamage=0`, `_bLethal`, `_fStunTime` if reaction is weak/wrong; failure mode should be “nothing,” not crash.
3. Optional fallback: `UGuardDB::BPF_GetHittedGuardAnim` for guard-only sequences through the proven AnimSequence path.
4. Do **not** rebuild `FHitRequest` / `BPF_GenerateFakeImpact`.
5. Do **not** serialize knockdown as `SetDown` from wire; let impact/death presentation own down-state.
6. If LaunchImpact fails live: send portable AnimSequence paths with `AnimationSemantic::Reaction` / `Fall` when the owner has a sequence (not a pose asset).

**Files:** `enemies.cpp` (`LaunchReplicatedImpact`, health-decrease sites), `orders.cpp` (`OrderHittedOnStartHook`, `PumpReactionCaptures`, `IsReactionOrder`), `puppet.cpp` (montage drain), `coop.h`.

---

## Bug 7 — Enemy slowdown / hitstop sync

### Symptom
One machine sees freeze-frame / time stutter on hits; the other does not (or stutters on different frames).

### Root cause
`OrderFreezeFrame` is heavy locally and was never order-replicated. Hit timing between screens disagreed.

### Current mitigation in tree
- `EnemyEntry.time_dilation` carries `AActor::CustomTimeDilation` (protocol comment ~236–240).
- Host publish: `GetActorTimeDilation` (`PublishEnemies` ~1530–1534).
- Observer apply: `SetActorTimeDilation` only when `!entry.local_brain` (~2144–2149) so owner’s own freeze is not overwritten.
- Clamped to `(0.01, 4]` on wire (`session.cpp` ~903–905; `actors.cpp`).

### Remaining gaps / fix
1. Confirm both Epic and Steam dlls have non-zero offset for CustomTimeDilation accessor (offset-table trap).
2. Packet loss → missed stutter frames; acceptable unless you add reliable “freeze pulse” events.
3. Player hitstop is **not** on `SnapshotPacket` — puppet may not match player freeze. Add if needed.
4. Optional: put dilation on `OwnedEnemy` too if host must mirror joiner-owned fight freeze.
5. FreezeFrame **order** itself need not be replicated if dilation proxy is enough — verify in paired fight logs.

**Files:** `protocol.h`, `enemies.cpp`, `actors.cpp`, `session.cpp`; optionally `OwnedEnemy` path + player snapshot.

---

## Cross-cutting call graph

```
TickEnemies
  Host:  ApplyPeerDamage → PublishEnemies → SendEnemyStates
  Client: ApplyRemoteEnemies → SendOwnedEnemies / SendDamageReports

PlayOrder / OrderAttack::OnStart / OrderHitted::OnStart / Health::Kill
  → OrderEvent / MontagePacket (Attack | Reaction | Death)

TickPuppet / drive path
  DriveActorTo → NoteEnemyPresentation
  SCAnimUpdateHook → WritePresentationVelocity (locomotion)
  PopMontageState → PlayAnimationAsset / NoteEnemyDeathAnimation
```

---

## Suggested verification order (paired logs)

Read both logs from the last `=== SifuCoop attached` forward.

| Priority | Symptom | Log prefixes to prove cause |
|---|---|---|
| 1 | Won’t hit / kick corpse | `targets:` vs `roles:`; `retarget` lines |
| 2 | Phase-through after revive | `died HERE`, `revived`, `stays down`, `esync:` collision/hp |
| 3 | Standing corpses | `death:`, `NotifyDown`, brain stop/start |
| 4 | Sliding chase | presentation / velocity; hook active; slot overflow |
| 5 | No hurt anim | `reaction:` LAUNCHED on OBSERVED; `mirror_hit_reactions` |
| 6 | Hit timing | `time_dilation` on wire; freeze during hits |

**One risky config change per run.** Do not re-enable unsafe director switches to “fix” targeting.

---

## Status snapshot (as of handoff 2026-08-12)

| # | User report | Cause class | Code status |
|---|---|---|---|
| 1 | Phase-through after death/revive | Down-state / collision bookkeeping race | Latch + stand-up path deployed; **play-test unconfirmed** |
| 2 | Observer standing corpses | Stopped brain + incomplete death presentation | Death notify / death anim / no brain on corpse deployed; **confirm live** |
| 3 | Sliding chase | Presentation velocity timing | `SCAnimUpdateHook` republish deployed; **not play-tested** |
| 4 | General enemy desync | Ownership + cosmetic observer + missing reactions | Architectural; harden + channels above |
| 5 | Stop / kick dead partner | Aim without DirectOpponent + corpse retarget | Retarget safe; **roles for partner still blocked** |
| 6 | Hurt/push/fall | No safe order mirror; PoseAsset dead end | `BPF_LaunchImpact` implemented, **default off / unproven** |
| 7 | Slowdown sync | FreezeFrame never on wire | `CustomTimeDilation` on `EnemyEntry` deployed; **confirm both builds** |
