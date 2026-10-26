
---

## `src/net/protocol.h` / `session.cpp` — order events are acknowledged

Attacks are one-shot events (dodges, guards and parries are not sent as events;
they reach the other screen as the animation the body plays). A dropped
snapshot costs nothing because the next one carries the whole state, but a
dropped `OrderEvent` is a swing that simply never happens on the other screen.
Until protocol 22 these were fire-and-forget.

Each event now carries an `event_id` that stays fixed across retransmits, and
the sender keeps it in a small pending ring, resending until the peer
acknowledges it. The retry interval follows the measured round trip (half the
RTT plus twice the jitter, clamped to 40-250 ms) and a swing is given up on
after 2 s: a fixed 40 ms x 6 gave up after ~200 ms, less than one round trip
on a loaded link, so a perfectly healthy peer could miss every copy. The
receiver rejects duplicates by `event_id` and acknowledges **every** copy it
sees, including duplicates, because the acknowledgement itself can be lost and
a peer still retransmitting needs to hear it again.

`header.sequence` could not be used for this: it increments on every packet of
every type, so a retransmit carries a different value each time. The old
`sequence <= last_seen` check also silently dropped any order that arrived out
of order; `event_id` dedup replaces it and reordering is no longer fatal.

## `src/game/replay.cpp` — why the CDO path is guessed

`UReplaySystem`'s control functions are static and Blueprint-exposed, so they
dispatch on the class default object rather than an instance. The owning script
package is not recoverable from the executable — `/Script/Sifu` and several
`/Script/SC*` modules exist, and CDO names are built at runtime rather than
stored as strings, so nothing in the binary says which package owns the class.

The probe therefore tries a short list of candidate paths and logs the one that
resolves, or logs that none did. That is the whole point of it being a probe.

## `src/core/log.cpp` / `trace.cpp` — one clock across two machines

Every remaining class of bug in this mod is a disagreement between the two
PCs, and a single log cannot contain one: it records what one machine
believed, and what it believed is the defect. Two logs can, but only if they
share an axis, and two Windows wall clocks do not.

The snapshot handler already estimates `g_clock_offset` (min-filtered
`local_recv - peer_send`) to place peer samples on the local timeline.
`SharedClockMs()` reuses it in the other direction: the host returns its own
clock, the client returns `local - offset`. Both machines are then stamping on
the **host's** clock, and `log::Write` prints that as `t=` on every line, with
a role letter. The client's stamps sit about one one-way trip early, which is
far below the resolution at which a divergence is read.

`trace::Tick` emits a `dgst` line whose interval is bucketed on that shared
clock rather than the local one. That detail is what makes the two files
joinable: both machines cross the same boundary at the same instant and print
a line keyed to the same number, so `tools/tracediff/tracediff.py` can put
one machine's `pup=` (where it is drawing the partner) against the other
machine's `me=` (where the partner actually was) with no interpolation and no
guessing. That difference is the bug; `err=`, which each machine can compute
alone, only measures its own smoothing and stays small while the data it is
smoothing is wrong.

`log.cpp` takes the clock as a function pointer installed by `dllmain`, so the
logger keeps knowing nothing about the network.

## `src/game/enemies.cpp` — what actually puts a replicated corpse down

Two theories have now been tested against paired logs. Both were wrong in an
instructive way, so both are recorded.

**The symptom.** On the observing machine a dead enemy stood upright at zero
health for 750 ms and then snapped into a death pose with no fall. Every
replicated death printed `down=0` immediately and `repaired attempt=1` three
quarters of a second later -- five out of five on the joiner, three out of
three on the host. Knockdowns were unaffected, because they never touch this
path at all.

**Theory 1: zero health is not death, so call Kill.** `BPF_ApplyDamage` only
empties the health bar; the fall, the ragdoll and the down state were assumed
to live in `UCharacterHealthComponent::Kill`, which the code only reached as a
fallback when the damage had *failed* to kill -- that is, never. Supporting
evidence at the time: the instigator-repair line inside `HealthKillHook` had
never once fired, and the host recorded `dead=1 down=0`.

The next session answered flatly. With `Kill` always called, the host logged
`transition=0`, the joiner logged `kill=0`, and the repair still fired on every
single death. **Calling `Kill` does not put a replicated body down.** It was
reverted; an extra engine call that achieves nothing is not worth the risk of
whatever else it touches.

**Why the exact death sequence never helped either.** The same session got a
diagnostic in `HealthKillHook`, which answered immediately and repeatedly:
`the game killed this body with no death animation`. Sifu passes `Kill` a null
animation for these deaths and selects the death clip somewhere else entirely.
So the whole "capture the exact death sequence and replay it on the observer"
feature cannot work through that hook, and `anim=0` in the death lines is not a
bug to be fixed there -- it is the design assumption being wrong.

**What does work.** The repair itself, every time, on its first attempt:
`PresentClientReplicaDeath` (Sifu's own `CLast Hitted` with
`ClientReplicaFastDeath`) followed by `SetDeathState`, reporting
`replica=1 down=1` without exception. The action was never wrong. Only the
750 ms wait in front of it was, and that wait was measured doing nothing --
the body was still upright every time it expired.

So the delay is now `death_repair_ms`, defaulting to 100 ms instead of 750.
It is configuration rather than a constant because the old comment warns that
forcing the terminal state too eagerly is itself what left corpses standing
once, and that trade-off is an animation judgement a log cannot settle.
