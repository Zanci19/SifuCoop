#!/usr/bin/env python3
"""Lay the host log and the client log of one SifuCoop session side by side.

Both machines stamp every line with the host's clock (`t=`) and emit a `dgst`
state line on the same shared-clock boundaries, so the two files can be joined
on `t=` without guessing. What one log can never show -- the two machines
disagreeing about the same body at the same instant -- is exactly what this
prints.

    python tools/tracediff/tracediff.py host.log client.log

The order of the arguments does not matter; the role letter in each line says
which file is which.
"""

import argparse
import bisect
import re
import sys
from collections import Counter

LINE = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d\d\d) ([HC\-]) t=(\d+|-)\]\s*(.*)$")

# Distances are Unreal units. Sifu's player capsule is roughly 90 tall and the
# body about 40 wide, so a disagreement past a body width is visible on screen
# and past a few metres is a different part of the room.
BODY_WIDTH = 40.0
ROOM_SCALE = 300.0


def parse_vec(text):
    text = text.lstrip("?")
    parts = text.split(",")
    if len(parts) != 3:
        return None
    try:
        return tuple(float(p) for p in parts)
    except ValueError:
        return None


def parse_pair(text):
    """'90.0/100.0' -> (90.0, 100.0). None for anything unparsable."""
    parts = text.split("/")
    try:
        return tuple(float(p) for p in parts)
    except ValueError:
        return tuple(None for _ in parts)


def parse_digest(body):
    fields = {}
    for token in body.split():
        key, sep, value = token.partition("=")
        if sep:
            fields[key] = value
    return fields


def distance(a, b):
    if a is None or b is None:
        return None
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


class Log:
    def __init__(self, path):
        self.path = path
        self.role = None
        self.lines = []      # (t or None, wall, body)
        self.digests = {}    # t -> fields
        self.unstamped = 0
        self.commit = None
        self.config = []
        self._read()

    def _read(self):
        roles = Counter()
        with open(self.path, "r", encoding="utf-8", errors="replace") as handle:
            for raw in handle:
                match = LINE.match(raw.rstrip("\n"))
                if not match:
                    # A line from a build older than the shared-clock logging.
                    self.lines.append((None, "", raw.rstrip("\n")))
                    self.unstamped += 1
                    continue
                wall, role, stamp, body = match.groups()
                if role != "-":
                    roles[role] += 1

                # The build banner and the configuration dump are both written
                # during startup, before any packet has arrived, so they carry
                # t=- . They still identify the build and settings, which is
                # the first thing worth comparing -- read them either way.
                if body.startswith("build:") and self.commit is None:
                    self.commit = body
                elif body.startswith("coop: "):
                    self.config.append(body)

                if stamp == "-":
                    self.unstamped += 1
                    self.lines.append((None, wall, body))
                    continue
                t = int(stamp)
                self.lines.append((t, wall, body))
                if body.startswith("dgst "):
                    self.digests[t] = parse_digest(body[5:])
        self.role = roles.most_common(1)[0][0] if roles else None

    def events_near(self, t, window_ms):
        """Non-digest lines within +/- window of t, for context."""
        out = []
        for stamp, _wall, body in self.lines:
            if stamp is None or body.startswith("dgst "):
                continue
            if abs(stamp - t) <= window_ms:
                out.append((stamp, body))
        return out


def align(a, b, tolerance):
    """Pair digests from two logs on the shared clock."""
    b_keys = sorted(b.digests)
    pairs = []
    if not b_keys:
        return pairs
    for t in sorted(a.digests):
        i = bisect.bisect_left(b_keys, t)
        best = None
        for j in (i - 1, i, i + 1):
            if 0 <= j < len(b_keys):
                delta = abs(b_keys[j] - t)
                if best is None or delta < best[0]:
                    best = (delta, b_keys[j])
        if best and best[0] <= tolerance:
            pairs.append((t, best[1], a.digests[t], b.digests[best[1]]))
    return pairs


def summarize(name, values, unit="units"):
    values = [v for v in values if v is not None]
    if not values:
        return "  %s: no samples" % name
    values_sorted = sorted(values)
    mean = sum(values) / len(values)
    p95 = values_sorted[max(0, int(len(values_sorted) * 0.95) - 1)]
    return ("  %s: mean %.1f %s, p95 %.1f, max %.1f  (n=%d)"
            % (name, mean, unit, p95, values_sorted[-1], len(values)))


def divergence_series(pairs, observer_key, subject_key):
    """One machine's copy of the partner's body vs what the partner reported."""
    series = []
    for t_a, _t_b, fa, fb in pairs:
        raw = fa.get(observer_key, "")
        seen = None if raw.startswith("?") else parse_vec(raw)
        truth = parse_vec(fb.get(subject_key, ""))
        d = distance(seen, truth)
        if d is not None:
            series.append((t_a, d))
    return series


def report_positions(out, label, series):
    out.append(label)
    if not series:
        out.append("  no overlapping samples -- one side never had a body for the other")
        return []
    out.append(summarize("disagreement", [d for _, d in series]))
    visible = [s for s in series if s[1] > BODY_WIDTH]
    bad = [s for s in series if s[1] > ROOM_SCALE]
    out.append("  over one body width (%.0f): %d/%d samples (%.1f%%)"
               % (BODY_WIDTH, len(visible), len(series),
                  100.0 * len(visible) / len(series)))
    out.append("  over %.0f units, a different part of the room: %d"
               % (ROOM_SCALE, len(bad)))
    return sorted(series, key=lambda s: -s[1])[:5]


def report_scalar(out, label, pairs, observer_key, subject_key, tolerance):
    diffs = []
    for t_a, _t_b, fa, fb in pairs:
        seen = parse_pair(fa.get(observer_key, ""))[0]
        truth = parse_pair(fb.get(subject_key, ""))[0]
        if seen is None or truth is None or seen < 0 or truth < 0:
            continue
        diffs.append((t_a, abs(seen - truth)))
    if not diffs:
        out.append("%s: no samples" % label)
        return []
    over = [d for d in diffs if d[1] > tolerance]
    out.append("%s: mean %.1f, max %.1f, %d/%d samples off by more than %.1f"
               % (label, sum(d[1] for d in diffs) / len(diffs),
                  max(d[1] for d in diffs), len(over), len(diffs), tolerance))
    return sorted(diffs, key=lambda d: -d[1])[:3]


def report_anim(out, label, pairs, observer_key, subject_key):
    mismatches = Counter()
    total = 0
    for _t_a, _t_b, fa, fb in pairs:
        seen = fa.get(observer_key, "-").split("@")[0]
        truth = fb.get(subject_key, "-").split("@")[0]
        if seen == "-" and truth == "-":
            continue
        total += 1
        if seen != truth:
            mismatches[(truth, seen)] += 1
    if not total:
        out.append("%s: no samples" % label)
        return
    count = sum(mismatches.values())
    out.append("%s: %d/%d samples showing a different clip (%.1f%%)"
               % (label, count, total, 100.0 * count / total))
    for (truth, seen), n in mismatches.most_common(6):
        out.append("    they played %-28r shown here as %r x%d" % (truth, seen, n))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", nargs=2, help="the two SifuCoop.log files, in any order")
    parser.add_argument("--tolerance-ms", type=int, default=200,
                        help="how far apart two digests may be and still be one instant")
    parser.add_argument("--context-ms", type=int, default=1000,
                        help="event lines to print either side of a worst moment")
    parser.add_argument("--worst", type=int, default=3,
                        help="how many worst moments to expand with context")
    args = parser.parse_args()

    logs = [Log(p) for p in args.logs]
    host = next((l for l in logs if l.role == "H"), None)
    client = next((l for l in logs if l.role == "C"), None)

    out = []
    out.append("=" * 78)
    for log in logs:
        out.append("%s: role=%s lines=%d digests=%d unstamped=%d"
                   % (log.path, log.role or "UNKNOWN", len(log.lines),
                      len(log.digests), log.unstamped))
        if log.commit:
            out.append("    " + log.commit)
    out.append("=" * 78)

    if host is None or client is None or host is client:
        out.append("")
        out.append("Both a host log and a client log are needed, and these are not that.")
        out.append("A file with no role letter predates the shared-clock logging, or its")
        out.append("session never connected -- the role stays '-' while offline.")
        print("\n".join(out))
        return 1

    # Configuration first: a difference here has explained behaviour before,
    # with no further analysis needed.
    only_host = sorted(set(host.config) - set(client.config))
    only_client = sorted(set(client.config) - set(host.config))
    if only_host or only_client:
        out.append("")
        out.append("CONFIGURATION DIFFERS BETWEEN THE TWO MACHINES")
        for line in only_host:
            out.append("  host only:   " + line)
        for line in only_client:
            out.append("  client only: " + line)

    pairs = align(host, client, args.tolerance_ms)
    out.append("")
    out.append("aligned %d instants on the shared clock (host %d digests, client %d)"
               % (len(pairs), len(host.digests), len(client.digests)))
    if not pairs:
        out.append("")
        out.append("Nothing aligned. Either the two logs are from different sessions, or")
        out.append("the client never estimated the clock offset -- it prints t=- until the")
        out.append("first snapshot arrives, so the two never exchanged traffic at all.")
        print("\n".join(out))
        return 1

    swapped = [(t_b, t_a, fb, fa) for t_a, t_b, fa, fb in pairs]

    out.append("")
    out.append("-- WHERE EACH PLAYER THINKS THE OTHER IS " + "-" * 36)
    worst_a = report_positions(
        out, "host's copy of the client's body vs the client's own position:",
        divergence_series(pairs, "pup", "me"))
    worst_b = report_positions(
        out, "client's copy of the host's body vs the host's own position:",
        divergence_series(swapped, "pup", "me"))

    out.append("")
    out.append("-- LOCAL SMOOTHING (each machine chasing the wire) " + "-" * 27)
    for label, log in (("host", host), ("client", client)):
        errors = []
        for fields in log.digests.values():
            try:
                value = float(fields.get("err", "-1"))
            except ValueError:
                continue
            if value >= 0:
                errors.append(value)
        out.append(summarize("%s puppet chase error" % label, errors))

    out.append("")
    out.append("-- VITALS " + "-" * 67)
    worst_hp_a = report_scalar(out, "host's view of the client's health", pairs,
                               "php", "hp", 1.0)
    worst_hp_b = report_scalar(out, "client's view of the host's health", swapped,
                               "php", "hp", 1.0)

    down_mismatch = [t for t, _t_b, fa, fb in pairs
                     if fa.get("pdn") not in ("-1", None)
                     and fb.get("dn") not in ("-1", None)
                     and fa.get("pdn") != fb.get("dn")]
    out.append("down-state disagreement (host's view of the client): %d/%d samples"
               % (len(down_mismatch), len(pairs)))

    out.append("")
    out.append("-- ANIMATION " + "-" * 64)
    report_anim(out, "host showing the client's body", pairs, "panim", "anim")
    report_anim(out, "client showing the host's body", swapped, "panim", "anim")

    out.append("")
    out.append("-- LINK AND LEDGERS " + "-" * 57)
    for label, log in (("host", host), ("client", client)):
        last = log.digests[max(log.digests)] if log.digests else {}
        out.append("  %s: rtt=%s drop=%s rej=%s enemies(known/active/driven/unmatched)=%s "
                   "dmg(reports/reported/applied)=%s"
                   % (label, last.get("rtt", "?"), last.get("drop", "?"),
                      last.get("rej", "?"), last.get("en", "?"), last.get("dmg", "?")))
    out.append("  A rising 'unmatched' is an enemy one side knows and the other cannot")
    out.append("  find; reported far above applied is damage that never landed.")

    levels = set()
    for _t_a, _t_b, fa, fb in pairs:
        levels.add((fa.get("lvl"), fb.get("lvl")))
    disagreeing = [pair for pair in levels if pair[0] != pair[1]]
    if disagreeing:
        out.append("")
        out.append("  LEVELS DISAGREED during the session (host, client): "
                   + ", ".join(str(p) for p in sorted(disagreeing)[:6]))

    # The payoff: name the instants, then print what both machines logged there.
    # Only moments big enough to have been visible are worth the context dump;
    # padding the list with the largest of a set of small numbers would read
    # like a finding when nothing went wrong.
    moments = []
    for series, what in ((worst_a, "host's copy of the client drifted"),
                         (worst_b, "client's copy of the host drifted")):
        for t, magnitude in series[:args.worst]:
            if magnitude > BODY_WIDTH:
                moments.append((magnitude, t, what))
    for series, what in ((worst_hp_a, "host's view of the client's health was wrong"),
                         (worst_hp_b, "client's view of the host's health was wrong")):
        for t, magnitude in series:
            if magnitude > 1.0:
                moments.append((magnitude, t, what))
    moments.sort(key=lambda m: -m[0])

    out.append("")
    out.append("=" * 78)
    if moments:
        out.append("WORST MOMENTS, WITH WHAT BOTH MACHINES LOGGED AROUND THEM")
    else:
        out.append("No moment where the two machines disagreed enough to be seen.")
    out.append("=" * 78)

    seen_t = []
    shown = 0
    for magnitude, t, what in moments:
        if shown >= args.worst:
            break
        if any(abs(t - s) < 2000 for s in seen_t):
            continue
        seen_t.append(t)
        shown += 1
        out.append("")
        out.append("t=%d  %s by %.1f" % (t, what, magnitude))
        for label, log in (("H", host), ("C", client)):
            events = log.events_near(t, args.context_ms)
            if not events:
                out.append("  [%s] (nothing logged within %d ms)" % (label, args.context_ms))
                continue
            for stamp, body in events[:12]:
                out.append("  [%s] %+6dms  %s" % (label, stamp - t, body[:150]))

    out.append("")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
