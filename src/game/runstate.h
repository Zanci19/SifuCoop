#pragma once

#include "../ue/reflection.h"

namespace sifucoop::game {

// Publishes this player's run state (age, room-clear progress, held weapon) to
// the peer on a slow timer and logs the peer's in return. Informational: each
// machine's own numbers stay authoritative. Applying the peer's values back
// into the local game (room-clear nudge, puppet weapon) is deliberately NOT
// done here yet -- see runstate.cpp and COOP-PLAN.md for why.
void TickRunState(ue::UObject* player);

}  // namespace sifucoop::game
