#pragma once

#include <cstdint>

// An in-game harness for the one question a protocol bot cannot answer.
//
// The joining player's screen shows enemies whose AI is switched off, swinging
// because the host told us they swung. Whether those replayed swings actually
// *hit* -- whether the attack produces a live hitbox that damages the local
// player -- is the hinge the whole co-op design turns on. If it does, both
// players fight a real shared encounter. If it does not, damage to players has
// to become host-authoritative instead, which is a different mechanism.
//
// It cannot be checked from outside the game: a bot can drive the protocol but
// has no body to be hit. It cannot easily be checked by hand either, because it
// needs an enemy in reach, its AI in a specific state, and a health reading
// either side of one swing.
//
// So the game answers it itself: stage an enemy next to the player, run the
// same code path the joining side runs, and watch the player's health. Results
// go to the log, so this works with nobody watching the screen.
//
// Off unless selftest=1 in SifuCoop.ini. It teleports an enemy and forces
// attacks, which is not something to have happening during a real session.

namespace sifucoop::game {

void InitSelfTest();

// Called each frame. Does nothing at all unless the self-test is enabled.
void TickSelfTest();

// True while a test is staging or measuring, so the co-op layer can leave the
// enemy it is manipulating alone.
bool SelfTestActive();

}  // namespace sifucoop::game
