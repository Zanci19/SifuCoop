#pragma once

namespace sifucoop::ui {

// A small always-on-top status window drawn beside the game.
//
// The obvious route -- UEngine::AddOnScreenDebugMessage -- is unavailable: in
// this Shipping build it is an empty function folded by ICF onto the same
// address as several unrelated stubs, so calling it does nothing at all.
//
// Note: a layered window sits above borderless/windowed games. Exclusive
// fullscreen will hide it -- switch Sifu to borderless if you cannot see it.
void StartOverlay();
void StopOverlay();

// Thread-safe; the text is picked up by the overlay's own thread.
void SetOverlayText(const char* text);

// Draws inside the game's frame by hooking the swap chain, so the status is
// visible in exclusive fullscreen where no window can appear. Returns false if
// the hook could not be installed, in which case the window overlay remains the
// only display.
bool StartInGameOverlay();
void SetInGameOverlayText(const char* text);

// --- Menu (F1) -------------------------------------------------------------
//
// The menu is drawn on the RENDER thread, so it must never call into the game.
// It only records what the user asked for; the game thread picks these up in
// its own tick and acts there. Calling OpenLevel or spawning from inside
// Present would be a race against the engine's own use of those systems.
//
// Config toggles are the exception and are edited in place: they are plain
// scalars that the game thread only ever reads, so a checkbox needs no round
// trip to take effect.

struct MenuRequests {
    bool travel = false;          // travel to `level`, and invite the peer
    char level[192] = {};
    bool apply_network = false;   // reconnect using `host_mode` / `address`
    bool host_mode = true;
    char address[64] = {};
    char passphrase[64] = {};
    int port = 7777;
    bool discover_address = false;  // ask a STUN server what our public address is
    bool spawn_puppet = false;
    bool despawn_puppet = false;
    bool invite_peer = false;     // pull the peer into the level we are in now
    bool save_config = false;     // persist the toggles to SifuCoop.ini
    bool log_roster = false;      // dump every tracked character to the log
};

// Game thread: takes and clears whatever the menu asked for. Returns false when
// there is nothing pending.
bool TakeMenuRequests(MenuRequests* out);

// Live state, published by the game thread for the menu to show.
struct MenuStatus {
    bool connected = false;
    bool hosting = true;
    bool offline = false;
    bool have_level = false;
    bool together = false;        // both players report the same level
    char my_level[128] = {};
    char peer_level[128] = {};
    char detail[128] = {};

    float my_health = 0.f;
    float my_max_health = 0.f;
    float my_guard = 0.f;

    bool peer_known = false;      // the peer has reported vitals at least once
    bool peer_down = false;
    float peer_health = 0.f;
    float peer_max_health = 0.f;
    float peer_guard = 0.f;

    bool puppet_alive = false;
    int faction_mine = -1;
    int faction_puppet = -1;

    // What this machine looks like from outside, once discovery has answered.
    char public_address[64] = {};
};

void SetMenuStatus(const MenuStatus& status);

// One row of the live enemy table. Mirrored rather than read straight out of
// the game's own list: the render thread must never walk a structure the game
// thread is rebuilding underneath it.
struct SyncRow {
    unsigned int hash = 0;
    float distance = 0.f;
    float health = 0.f;
    float max_health = 0.f;
    bool active = false;
    bool down = false;
    bool driven = false;
    bool ai_stopped = false;
    char name[40] = {};
};

void SetSyncRows(const SyncRow* rows, int count);

bool IsMenuOpen();

}  // namespace sifucoop::ui
