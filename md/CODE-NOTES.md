# Code notes

Reasoning that used to live in comments. Each entry says why the code is shaped
the way it is, usually because the obvious shape was tried first and broke
something. Keyed by file and function; the code carries a one-line marker where
an edit made without reading this would reintroduce the fault.

---

## `src/net/session.cpp` — `g_level_activity_ms`

Records when either machine last did something level-shaped: sent an invite,
received one, or reported a different level.

A machine loading a map does not send for several seconds and cannot announce
that it is loading, which is exactly the window in which the ordinary peer
timeout must not fire.

## `src/net/session.cpp` — `TickSession`, the timeout grace

The grace used to apply only when the **host** had an invite outstanding, so a
joiner loading for any other reason — joining, restarting, travelling on its own
— got none.

Measured 2026-08-16: the joiner dumped its enemy roster at 16:53:39 mid-load and
the host cut it at 16:53:51 with `peer timed out after 12003ms`, killing the
session at the moment both sides were trying to meet.

Loading is silent by nature and cannot be announced, so grace is granted to
**either** role whenever anything level-shaped happened recently.

---

## `src/ue/reflection.cpp` — `RangeReadable`

Answers whether memory is committed and readable, without faulting to find out.

## `src/ue/reflection.cpp` — `IsValidObject`

Whether a pointer can be **touched**. Deliberately not "is this UObject alive".

This used to ask Kismet's `IsValid` through `ProcessEvent`, which is a
contradiction: dispatching a UFunction on the object dereferences the very
pointer the call is meant to vet. Every caller was a stale-pointer guard, so the
guard itself was the fault. It crashed the host on 2026-08-16 at 16:05:

```
EXCEPTION_ACCESS_VIOLATION reading 0x8
UKismetSystemLibrary::execIsValidClass -> UFunction::Invoke
  -> UObject::ProcessEvent -> dsound
```

reached from `WriteRelationship`, which validates a social component and a target
on every relationship assert.

The test is now structural, with no call at all: committed readable memory, a
vtable inside the game module, and a `ClassPrivate` that is itself a readable
object with a module vtable. That is the same bar `orders.cpp` applies before
touching an order, and the strongest test available without walking
`GUObjectArray`.

It proves the memory is safe to read. It does **not** prove the object is live,
so it must never decide gameplay — only avoid faulting. Callers that need
liveness track it themselves (world identity, sweep freshness).

The `0x10` is `UObjectBase::ClassPrivate`, the one fixed offset this codebase
takes on faith everywhere else too.

---

## `src/game/puppet.cpp` — speed band re-commanding

The band is commanded on a change **and** when the graph simply did not take it.

Latching on our own wanted value alone is why the band stayed wrong: we issue V0
once, the movement component does not adopt it, and the latch then says "already
V0" forever. The 07:09 log is full of that — `ours 0/V0, Sifu's 0/V3`, a standing
body playing a sprint, and `ours 850/V3, Sifu's 850/V1`, a sprinting body playing
a walk. 25 disagreements against 7 agreements.

Re-asserting cannot go back to being per-frame: `BaseMovementDB` gives these
transitions 0.3–1.0 s and a command restarts the blend it interrupts. So a
disagreement is re-commanded at most every 400 ms — slower than any blend can be
restarted by it, and still fast enough that a wrong band cannot persist.

---

## `src/game/orders.cpp` — `PendingReaction::reason`

Records why the last capture attempt gave up, so the expiry message can name it.

Every failure in the sweep below used to be a bare `continue`, so the expiry
message could not say which step failed — call, array, type or duplicate. Four
different faults shared one line.

## `src/game/orders.cpp` — which expiries are worth reporting

Only orders that are supposed to *have* a hit animation are worth complaining
about. `StructureBroken` and `Dizzy` are consequences of a hit rather than hit
reactions, and Sifu appends nothing to the history for them — counting those as
failures buried the real ones in noise.

---

## `src/game/puppet.cpp` — `SCAnimUpdateHook` write order

Enemy presentation velocity and speed band are written **before** the original
`USCAnimInstance::NativeUpdateAnimation` runs, not after.

Both values are graph *inputs*: `WritePresentationVelocity` writes the movement
and root components, and `SetMovementSpeedState` writes the band's real owner
(`UFightingMovementComponent`). `NativeUpdateAnimation` samples those sources to
build the pose for this frame, so writing after it means the graph sampled the
body's real velocity — zero, for a remotely driven enemy — and the injected value
does not land until the next frame. That is the "remote enemies slide with no
locomotion animation" symptom.

The cinematic weight write stays *after* the original, because that one writes a
field the original computes rather than a source it reads.

---

## `src/net/session.cpp` — `HandleOwnedEnemies` round staging

The joiner may own more enemies than fit in one datagram. `OwnedEnemyEntry` is 32
bytes and the packet budget is 1024, so a chunk carries at most 24 while tracking
allows 96.

A publish round is therefore split across up to four packets sharing one `round`
id, and the receiver stages them instead of applying each on arrival. The live
table is swapped in only once every chunk of that round has been seen. A round
that loses a packet is discarded whole rather than half-applied — cheap, because
rounds are published every 33 ms and ownership only goes stale after 1500 ms.

Applying chunks as they arrived would leave entries from the previous round
mixed with the current one, and an enemy that appears owned when it is not gets
its brain stopped on both machines at once.

## `src/game/puppet.cpp` — `ReadLocomotionThresholds`

Enemies and the player share `UPlayerAnim`; there is no separate enemy anim
class (`USCAnimInstance`, the base, carries only the cinematic fields). So an
enemy's own `m_fOwnerVelocityMaxForV0Anim`/`V1`/`V2` sit at the same offsets the
puppet path already reads, and enemies no longer need the hardcoded 20/240/475.

The bounds check is deliberately wide but finite. Three ascending positive floats
can occur in unrelated memory, and a false positive picks wrong animations
silently, so each threshold must also fall in a plausible range before the read
is trusted. Anything else falls back to the old constants.
