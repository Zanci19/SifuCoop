# SifuCoop — complete handoff

Self-contained brief for an agent picking this project up cold. `HANDOFF.md` is the long-form
history (19 sections); this is everything you need to start without reading it. When the two
disagree, trust this file — and see "Do not trust unverified negatives" below, because several
confident claims in `HANDOFF.md` turned out to be wrong.

---

## 1. What this is

A two-player co-op mod for **Sifu** (Unreal Engine 4.26), shipped as a `dsound.dll` proxy that
the game loads on startup. It is a hand-written stand-in for UE4 replication: a UDP mirror
between two machines, each running its own copy of the game.

Repo: `C:\Users\zanci\SifuCoop` (git). Every change is committed with the reasoning in the
message; `git log` is a genuine record of why things are the way they are.

**Source map** (~15k lines):

```
src/game/enemies.cpp   2545   enemy tracking, damage ledger, targeting, death
src/game/puppet.cpp    2234   the second body, its animation, vitals, relationships
src/net/session.cpp    2020   UDP transport, packets, interpolation
src/game/orders.cpp    1630   Sifu's Order system: attacks, reactions, the PlayOrder hook
src/game/player2.cpp   1061   the real-second-player path (currently off)
src/game/actors.cpp     772   reflection helpers: health, guard, faction, relationships
src/ue/reflection.cpp   405   UObject/UFunction plumbing, ProcessEvent wrapper
src/game/runstate.cpp   394   age, outfit, room progress, weapon
src/core/offsets.g.h    241   GENERATED. Per-build symbol addresses. Never hand-edit.
```

---

## 2. Environment — read this before running anything

Two physical machines, both running Sifu, both needing the same dll:

| | machine | game folder |
|---|---|---|
| **host** | Epic, this PC | `C:\Program Files\Epic Games\Sifu\Sifu\Binaries\Win64` |
| **joiner** | Steam, over SMB as `Z:` = `\\192.168.0.37\FullPC` | `Z:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64` |

**Logs — read these before theorising. Both are readable from the host machine.**

- host: `%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log`
- joiner: `Z:\Users\Zanci19\AppData\Local\Sifu\Saved\Logs\SifuCoop.log`

Logs are appended across sessions; find the last `=== SifuCoop attached` line and read forward.
Crash reports: `...\Saved\Crashes\UE4CC-*\CrashContext.runtime-xml` — the `<CallStack>` element
names the faulting function with file and line, which has identified every crash in this project
in one read.

### Build and deploy — BOTH machines, every time

```powershell
cd C:\Users\zanci\SifuCoop
.\build.ps1 -NoGen -Deploy
.\build.ps1 -NoGen -GameDir "Z:\Program Files (x86)\Steam\steamapps\common\Sifu\Sifu\Binaries\Win64" -LocalBuildName steam -Deploy
```

Drop `-NoGen` to regenerate the offset table from that folder's PDB (~2 s local, ~60 s over SMB).

**The trap that has cost this project five rounds.** Adding any new symbol regenerates the table
only for the game folder `build.ps1` was pointed at. The other build keeps **zeros**, every call
through those offsets silently does nothing, and the feature appears broken on exactly one
machine. Sequence for a new symbol: regen on steam, regen on epic, then `-NoGen -Deploy` both.
Verify before shipping:

```bash
python - <<'PY'
import io,re
s=io.open('src/core/offsets.g.h',encoding='utf-8').read()
for m in re.finditer(r'\{0x[0-9A-F]+, 0x[0-9A-F]+, "(\w+)", \{([^}]*)\}\}', s):
    vals=[v.strip() for v in m.group(2).split(',')]
    print(m.group(1), "zeros:", sum(1 for v in vals if v=='0x00000000'))
PY
```

Both must read `zeros: 0`. The two deployed dlls should differ only in build-stamp bytes
(offsets 137-138, 217-218, and one debug-directory copy) — `cmp -l` them.

The startup log prints `guard: build 'epic' matched` / `'steam' matched`, and reports any missing
offsets.

---

## 3. Architecture

UDP mirror, **protocol v15** — both machines must match or they refuse to connect.

- **Host is authoritative** for enemy health, damage and death.
- The joiner **simulates every enemy locally** (`client_simulates_enemies`) so they animate;
  position is corrected on drift (400 units) rather than driven per frame.
- Enemies near the joiner are claimed by it (`peer_fights_locally`), and their transforms are
  published back to the host (`OwnedEnemy` packet) so both screens agree.
- Each machine spawns a **puppet**: a clone of the local player standing in for the remote one.
  It inherits everything local — age, outfit, max health — which is the root of a whole family
  of bugs (§6).
- Everything is done through **reflection** (`ProcessEvent` on Blueprint-exposed `BPF_*`
  functions) where possible, and native calls at PDB-resolved addresses where not. Struct
  offsets come from the PDB via `WANTED_MEMBERS` in `tools/pdbdump/pdbdump.py` — never hardcode.

### Diagnostics in the log

| prefix | meaning |
|---|---|
| `esync:` | per enemy per second: health local/host, damage totals, caught_up, down/dead, owned, target, faction |
| `targets:` | census — how many enemies aim at each player |
| `roles:` | census — combat roles per player. **Different field from `targets:` — see §5** |
| `death:` | per death: kill / anim / health_comp → down |
| `orders: census` | every `EOrderType` seen, with counts inside the window after you were hit |
| `reaction:` | hit-reaction capture attempts |
| `coop:` | heartbeat — rtt, enemies active/driven, damage in/out, packet rates |

---

## 4. Settled facts — do not relitigate

Each of these cost a round or more. All are verified.

1. **A native UE4 listen server is impossible in this build.** `UWorld::Listen` and
   `UNetDriver::ServerReplicateActors` are ICF-folded empty stubs — the two functions behind
   `#if WITH_SERVER_CODE`. The server half of UE4 networking is compiled out. That is why this
   mod exists in its current form.
2. **`MultiCastPlayOrder` never fires.** Zero calls in every log ever recorded. It is a multicast
   RPC and there is no net driver. The engine's `FBuffer` order-serialisation route is
   unreachable.
3. **Faction is not what Sifu's AI discriminates on.** Every enemy and both players read
   faction 0. The faction experiment is removed.
4. **`ABaseCharacter::UpdateRelationshipToOtherCharacters` is a folded stub.** Nothing recomputes
   relationships behind your back.
5. **`OrderBase::GetAnimPlayed` is a folded stub.** Only `OrderAttack`, `OrderDodge`,
   `OrderFallOnSlope` and `OrderPlayAnim` override it — which is exactly why attacks replicate
   and nothing else does.
6. **`OpenLevel`'s `Options` parameter is by value and the callee frees it.** A stack buffer
   crashes the allocator.
7. **`BPF_ForceEnemy` alone crashes** in `AAIDirectorActor::OnDeathDetected` → 
   `RemoveActorFromSystems` → `AddRemoveCandidate`, reading 0x108, when anything dies. The cause
   is now understood — see §5 — and the fix is deployed but untested.

---

## 5. THE CENTRAL FINDING — two target fields

This is the most important thing in this document.

**There are two separate "who is this enemy fighting" fields, and the two census lines read one
each:**

| census line | reads | written by |
|---|---|---|
| `targets:` | the **attack component's** target | `g_set_attack_target` — correct for weeks |
| `roles:` | the **AI's own enemy** (`ReadAIEnemy`) | `ForceEnemy` **only** — which was switched off |

Only a `DirectOpponent` may swing. Roles are allocated by `AAIDirectorActor` out of a ticket
manager kept **per target**.

Every targeting fix in this project moved `targets` to the partner while permission stayed with
the host. The measured result, every sample for three sessions:

```
targets: 0 enemies on YOU, 3 on the second player, 0 elsewhere, 0 idle
roles:   fighting YOU direct=1 indirect=1 | fighting your partner direct=0 indirect=0 non=0 none=0
```

Three enemies **aimed** at the partner; all four role counters at zero. Aim without permission.
They walk over and stand there. **One cause behind three separate bug reports:** the partner
takes no damage, enemies ignore the partner, and enemies keep hitting a dead host.

It also explains the `BPF_ForceEnemy` crash: it handed out a ticket for a target the director had
no manager for, so removal on death walked a null.

**The fix is two halves, and neither does anything alone:**

- `AAIDirectorActor::RegisterOrRemoveFromCombatRoleTicketManagerForTarget(partner, Alerted,
  partner)` creates the ticket manager. Implemented as `MaintainPartnerAsDirectorTarget` in
  `enemies.cpp`, gated by `director_targets_partner`. **Verified live: runs, logs, does not
  crash.** Did not change the roles line by itself.
- `ForceEnemy(ai_fighting, partner, Alerted)` puts the enemy into it. Gated by
  `force_enemy_engage`, disabled for a week after its crash, **re-enabled now that the
  prerequisite exists.**

**Both are ON and have never been tested together. Your first job is to read both logs and find
out whether that pair worked or crashed.** If it crashed, `force_enemy_engage=0` in both inis
stops it without a rebuild. The line that decides it:

```
roles: fighting your partner direct=? ...
```

Non-zero means it worked.

---

## 6. Open bugs

### 6.1 Enemy hurt / push / stagger animations do not appear on the observing machine

The player's own reactions **do** replicate, via `UPlayerAnim::m_LastActionAnim` — verified in
the log. Enemies have no equivalent: that field is declared on `UPlayerAnim` only, and enemies
animate through `USCAnimInstance`.

`USCAnimInstance::m_CachedCurrentPoseAsset` (offset `0x390`) holds a **`UPoseAsset`** during a
reaction. `PlaySlotAnimationAsDynamicMontage` — the Cinematic-slot path that works for attacks —
takes a `UAnimSequenceBase`. `UPoseAsset` is not one. Feeding it one corrupted the heap. **Closed
by type, not by tuning.**

> **Unverified caveat:** nobody has confirmed `m_CachedCurrentPoseAsset` is actually the
> reaction. It was sampled during a reaction window and was non-null; it may be non-null always.
> Check this before building on it.

Four routes, all fold-checked and real (epic addresses):

| route | address | verdict |
|---|---|---|
| `AFightingCharacter::BPF_LaunchImpact(float, bool, float)` | `019B5070` | **Best.** Three scalars, no structs or pointers. Would make the observing machine generate a *real* impact so the enemy picks its own reaction locally, correctly, with its own direction. Parameters unknown — sweep them on one enemy with logging. Failure mode is "nothing happens", not a crash. |
| `UGuardDB::BPF_GetHittedGuardAnim(EQuadrantTypes, ESCCardinalPoints, EHeight)` | `01950160` | Returns a real `UAnimSequence*`. Completely safe — plays through the existing proven path. But covers **guard/deflect only**, not clean hits. |
| `UHittedAnimHelper::BPF_MakeGenericHitAnim(FHittedAnimContainer&, ...)` | — | Covers unguarded hits, which is what is wanted, but struct-by-reference. Dump the layout from the PDB offline first. Do not probe live. |
| `BPF_GenerateFakeImpact` / `BPF_GenerateForeignImpact` | — | The 1104-byte `FHitRequest` wall, with `FWeakObjectPtr` serial numbers that cannot be rebuilt from outside the process. **Leave alone.** |

Recommended: `BPF_LaunchImpact` first, `UGuardDB` as a safe partial fallback.

### 6.2 Enemies do not damage the partner

Should follow from §5. If roles read non-zero and he still takes nothing, note that **there is no
wire message for "the puppet was hit"** — damage dealt to the puppet on one machine is never
reported to the machine that owns that player. `SendEnemyDamage` (cumulative totals, idempotent,
resent on a timer) is the model to copy.

### 6.3 Age and costume appearance

Both writes work and are verified: `run: partner's body aged to 45 and the model was rebuilt
(OnStatsUpdated)`, and `puppet: max health here 96, theirs 96 -- agreed` proves the age write
reaches everything derived from it.

- Age: `UStatsComponent::BPF_SetCharacterAge`, then
  `UPlayerFightingComponent::OnStatsUpdated` — private, `void()`, no args — to rebuild the model.
  Writing the number without calling that leaves the character aged on paper only.
- Outfit: `m_iOutfitIndex` (offset `0x33C`) read directly; `BPF_SwapOutfit(int32,
  UMaterialInterface*, bool)` — **three** parameters — to write.
- Both are aimed at the puppet only, guarded by `IsSafeToDress`.

Owner still reports it looking wrong. **Verify against the log before assuming it is broken.**
Open question: `BPF_GetCharacterAge` returned 45 on one machine and 1–3 on the other; whether
that is the displayed age or a counter has never been established.

### 6.4 Corpses and phase-through (fix deployed, unconfirmed)

The joiner simulates the enemies fighting it, so it reaches zero **first**. The host still says
"alive" for a few hundred ms, and the revive branch acted on that — health back to 1,
`SetDown(false)` — aborting the death animation already playing. Then the host's death arrived
and killed it again from outside. Three forced state transitions in half a second; a body walked
in and out of Sifu's down-state machine from outside comes back upright and **no longer a valid
hit target**, which is the "attacks phase through enemies that were already hit" report.

Fixed with a `died_locally` latch: a body that dies under a brain this machine runs is not
resurrected by a host "alive". Cleared when the host agrees, or when the pool lifts the body back
at full health.

---

## 7. Techniques that work

**Fold check before building on any function** — the single highest-value habit here:

```bash
awk -F'\t' '$1=="019B5070"' research/wf-attacksel/allsyms.tsv | wc -l
```

`1` means a real function. Tens of thousands means the linker's identical-code-folding collapsed
an empty body and the function does nothing. `research/wf-attacksel/allsyms.tsv` is a cached
906k-symbol dump — grep it instead of re-parsing the 970 MB PDB.

**Enum names come out of the exe** as contiguous `EName::Value\0` runs. This produced `EOrderType`
(72 entries — **hit reaction is type 3, `Hitted`**; also `12 Pushed`, `10 KnockedDown`,
`11 Dizzy`, `65 Deflected`, `33 StructureBroken`), `ERelationshipTypes`, `EGlobalBehaviors`,
`ESCAICombatRolesChangeReason`. Seconds of work, removes all guessing about magic numbers:

```python
import re
data = open(r"...\Sifu-Win64-Shipping.exe", 'rb').read()
hits = sorted((m.start(), m.group(1).decode())
              for m in re.finditer(rb"EOrderType::([A-Za-z0-9_]+)\x00", data))
for i, (off, name) in enumerate(hits): print(i, name)
```

**Class property tables** are also in the symbols:
`Z_Construct_UClass_<Class>_Statics::NewProp_<field>`. That is how
`m_CachedCurrentPoseAsset` and `m_iOutfitIndex` were found, and how you confirm which class
actually declares a field.

**Member offsets from the PDB**, via `WANTED_MEMBERS` in `tools/pdbdump/pdbdump.py`. Add an entry,
regenerate both tables, use `offsets::M_Class_field`.

---

## 8. Mistakes made. Do not repeat these.

- **Scanned an order object for `UObject` pointers and called `GetPathName` on the hits.** Crashed
  on the first punch of the session. `LooksLikeUObject` proves *shape*, not identity, and
  `GetPathName` walks the Outer chain of whatever it is handed. Worse: the log already printed
  `uobjects=0` for every order inspected, so the scan could never have worked. **The evidence was
  there before the code was written.**
- **Matched a field by name and shipped it without checking its type.**
  `m_CachedCurrentPoseAsset` → montage slot → heap corruption mid-fight. `ue::ObjectClassIs` now
  exists; use it on anything read from a raw field before handing it to the engine.
- **Wrote peer state onto the local player.** `ApplyPeerVitals` has carried a guard against
  exactly that since it was written, and two new writes were added ten lines away without copying
  it. `IsSafeToDress` now exists.
- **Trusted confidently-worded negatives in `HANDOFF.md` that had never been verified.** Three of
  them were wrong:
  - "`BPF_ServerChangeRelationship` is a no-op on this build" — the setter is real and unfolded.
    The *reader* was wrong: there are two different `BPF_GetRelationship` functions, one on the
    actor and one on the `USocialComponent` that owns the map, and the code wrote to one and read
    from the other.
  - "`USCAnimInstance` has no current-action asset, it was dumped and checked" — it has
    `m_CachedCurrentPoseAsset`, listed plainly in its property table.
  - "`MultiCastPlayOrder`'s `FBuffer` is the correct target for order serialisation" — it has
    never fired once.

  **If a document in this repo says something does not exist, re-check it.**

---

## 9. Current config (both machines, identical)

```
mode=host|client  port=7777  snapshot_hz=60  interp_delay_ms=60
sync_enemies=1  suppress_client_ai=1  sync_enemy_vitals=1  report_damage=1
mirror_peer_vitals=1  echo_enemy_attacks=1  echo_player_attacks=0
remote_player_attacks=1  sync_montages=1  mirror_hit_reactions=1
puppet_invincible=0        <- was an invincible decoy soaking most of the room
force_enemy_engage=1       <- NEW, untested, the other half of §5
director_targets_partner=1 <- NEW, verified not to crash
real_second_player=0       <- see below
verbose_enemies=1  verbose_orders=1
```

**`real_second_player=1` is the untried structural alternative.** The puppet is a spawned clone,
which is why the director cannot account for it. `UGameplayStatics::CreatePlayer` produces a
genuine `AFightingPlayerController` with a game-mode pawn — the kind of actor the director's
bookkeeping is built for. The machinery exists (`player2.cpp`, 1061 lines) and is off. If §5's
two-part fix fails, this is the next thing to try, and it should be tried **alone**.

---

## 10. How to work on this

1. **Read both logs first.** Every real fix in this project came from a log line; every guess cost
   a test round, and two of those were crashes in the owner's live session.
2. **Fold-check every function before building on it.** One `awk` line.
3. **Check the type of anything read from a raw field** before handing it to the engine.
4. **One risky change per run.** The owner tests manually across two machines and cannot tell
   which of four changes caused a result.
5. **When a measurement and the thing it measures disagree, suspect the measurement.** That is how
   both the relationship bug and the two-field bug were finally found.
6. **Put anything unverifiable behind a default-off config toggle** with an honest comment, and
   separate "verified live" from "implemented but untested" from "researched only" in every status
   you write. The owner is testing on real hardware and needs to know which is which.
