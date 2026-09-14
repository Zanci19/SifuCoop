
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
