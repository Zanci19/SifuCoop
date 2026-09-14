
---

## `src/net/protocol.h` / `session.cpp` — order events are acknowledged

Attacks, dodges and guards are one-shot events. A dropped snapshot costs nothing
because the next one carries the whole state, but a dropped `OrderEvent` is a
swing that simply never happens on the other screen. Until protocol 22 these
were fire-and-forget.

Each event now carries an `event_id` that stays fixed across retransmits, and
the sender keeps it in a small pending ring, resending every 40 ms for up to 6
attempts until the peer acknowledges it. The receiver rejects duplicates by
`event_id` and acknowledges **every** copy it sees, including duplicates,
because the acknowledgement itself can be lost and a peer still retransmitting
needs to hear it again.

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
