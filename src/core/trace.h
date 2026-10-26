#pragma once

namespace sifucoop::trace {

// One fixed-shape line per interval describing everything a divergence can be
// read from: where both bodies are on this machine, what the wire says the
// partner is doing, what each is animating, and the counters that show whether
// the two machines are still agreeing. Emitted on shared-clock boundaries, so
// the host log and the client log carry lines with the SAME t= values and can
// be diffed directly (tools/tracediff/tracediff.py).
void Tick();

// Forces the next Tick to emit, whatever the interval. Used after an event
// worth having a full state line next to.
void RequestDigest();

}
