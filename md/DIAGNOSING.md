# Diagnosing a session

Almost every bug left in SifuCoop is a *disagreement*: the two machines stop
describing the same fight. One log cannot show that. It records what one PC
believed, and what one PC believed is exactly what is wrong. Two logs can show
it, but only if they can be lined up — and wall-clock time on two PCs cannot be
lined up, because the clocks differ by an unknown amount.

So both machines log on **one** clock: the host's.

## What is in a log now

Every line looks like this:

```
[21:14:07.412 C t=93840612] puppet: released local down state for the incoming resurrection sequence
```

* `C` — this machine's role. `H` host, `C` client, `-` not in a session.
* `t=93840612` — milliseconds on the **host's** clock. The client converts using
  the offset the snapshot stream already estimates. Both logs of one session
  carry the same numbers, so a line in one can be placed against a line in the
  other. `t=-` means the offset was not known yet (before the first packet
  arrived), and that line cannot be aligned.

Four times a second each machine also writes a `dgst` line: a fixed set of
fields covering both bodies, both animations, vitals, enemy counts and link
counters. Because the interval is measured on the shared clock, the host and the
client emit lines at the *same instants* and the two sets join on `t=`.

The fields that matter most:

| field | meaning |
| --- | --- |
| `me=` | where this machine's own player is — the authoritative answer |
| `pup=` | where this machine is drawing the *partner's* body |
| `wire=` | where the wire says the partner is (`?` prefix: no fresh data) |
| `err=` | gap between `pup` and `wire` — this machine's own smoothing quality |
| `anim=` / `panim=` | clip playing on this player / on the partner's body here |
| `hp=` / `php=` | this player's health / the partner's health as believed here |
| `en=` | enemies known / active / driven / unmatched |
| `dmg=` | damage reports / reported total / applied total |

The real test is not `err`. It is **this machine's `pup=` against the other
machine's `me=` at the same `t=`** — what one player saw versus where the other
player actually was. That number is the bug.

## Capturing a session

1. Both PCs on the same build. The commit is in the first lines of each log.
2. Play until the problem happens. Note roughly when and what it looked like —
   "slid across the room", "took damage from nothing", "wrong animation" are
   three different bugs with the same shape in a log.
3. Collect both files, from each PC:
   `%LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log`
   (or beside `dsound.dll` if that folder was not writable — the log says which).
4. Rename them so they are distinguishable, e.g. `host.log` and `client.log`.
   The tool reads the role from inside the file, so the order does not matter.

The log rolls over at 64 MB to `SifuCoop.prev.log`, so it will not grow without
limit; but it is still appended to across runs, and a single long file holds
several evenings. Note the time the session started.

## Reading them

```bash
python tools/tracediff/tracediff.py host.log client.log
```

It prints, in order:

* whether the two builds and the two configurations match — a difference here
  has explained the behaviour outright before now;
* how many instants aligned;
* how far each machine's copy of the partner was from where the partner really
  was, as a distribution, and how often it was past a body width;
* each machine's own chase error, for comparison — a small `err` with a large
  divergence means the smoothing is fine and the *data* is wrong;
* health, down-state and animation disagreements;
* the link counters and enemy ledgers at the end of the session;
* and finally the worst moments by name, each with every non-digest line both
  machines logged within a second either side.

That last section is the point. It turns "it went wrong at some point" into a
timestamp with both machines' accounts of it printed next to each other.

## Turning it off

`trace=0` in `SifuCoop.ini` stops the digest (the `t=` stamp and role letter
stay). `trace_hz` sets the rate, 1–30, default 4. There is no good reason to
turn it off for a session you intend to report.
