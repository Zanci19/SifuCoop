#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {

// A REAL second player, created by the engine's own UGameplayStatics::CreatePlayer,
// instead of the clone-and-puppet approach.
//
// Why this matters more than it sounds. The puppet is an ordinary actor we spawn
// and shove around: the game does not consider it a player, so Sifu's AI never
// picks it as a target (every enemy fights the host), its hitboxes behave like
// scenery, and its attacks have to be reconstructed from the peer's input rather
// than performed. Those are not three bugs, they are one consequence of faking a
// player instead of asking for one.
//
// CreatePlayer returns a genuine APlayerController with a pawn spawned by the
// game mode -- the same way player one exists. Sifu already supports this: a
// community split-screen mod uses exactly this call, and the binary ships the
// whole path (CreateLocalPlayer, SpawnPlayActor, GetNumPlayers, and even the
// full UE4 net driver stack from Sloclap's Absolver lineage).
//
// UNVERIFIED until it runs. It is behind `real_second_player`, default off.

// Records the module base, matching every other subsystem here.
void InitPlayer2(std::uintptr_t module_base);

// Creates the second player and returns its pawn, or null on failure (every
// failure path logs why). Safe to call twice: returns the existing pawn.
ue::UObject* CreateSecondPlayer();

// Tears it down again. Safe when nothing was created.
void RemoveSecondPlayer();

// The second player's pawn, re-resolved from the controller each time because
// the pawn is destroyed and respawned on death and level change.
ue::UObject* GetSecondPlayerPawn();

// True once a controller exists, whether or not it currently has a pawn.
bool SecondPlayerActive();

// Called every frame while the feature is on. Creates the player if it does not
// exist yet (rate-limited, because a game mode that refuses once will refuse
// every frame), re-resolves the pawn after a death or a level travel, and
// re-applies the settings a freshly spawned pawn would not have. Returns the
// current pawn, or null while there is not one.
//
// The pawn is deliberately never cached across frames anywhere: the game mode
// destroys and respawns it on death, on aging and on every level change, and a
// stale pawn pointer is the exact bug that used to crash this mod.
ue::UObject* MaintainSecondPlayer();

// Whatever body PlayerController 0 -- the human at this keyboard -- is currently
// possessing, resolved live. Anything that writes remote state has to be able to
// ask this: Sifu's game mode has been observed handing the second player player
// one's own character, and the mod must refuse to treat that body as remote.
ue::UObject* PrimaryPlayerPawn();

}  // namespace sifucoop::game
