#pragma once

namespace sifucoop::ui {

void StartOverlay();
void StopOverlay();

void SetOverlayText(const char* text);

bool StartInGameOverlay();
void SetInGameOverlayText(const char* text);

struct MenuRequests {
    bool travel = false;
    char level[192] = {};
    bool apply_network = false;
    bool host_mode = true;
    char address[64] = {};
    char passphrase[64] = {};
    int port = 7777;
    bool discover_address = false;
    bool spawn_puppet = false;
    bool despawn_puppet = false;
    bool invite_peer = false;
    bool accept_invite = false;
    bool decline_invite = false;
    bool teleport_to_peer = false;
    bool save_config = false;
    bool restart_network = false;
    bool disconnect_network = false;
    bool log_roster = false;
    bool refresh_tailnet = false;};

bool TakeMenuRequests(MenuRequests* out);

struct MenuStatus {
    bool connected = false;
    bool hosting = true;
    bool offline = false;
    bool have_level = false;
    bool together = false;
    char my_level[128] = {};
    char peer_level[128] = {};
    char detail[128] = {};

    float my_health = 0.f;
    float my_max_health = 0.f;
    float my_guard = 0.f;

    bool peer_known = false;
    bool peer_down = false;
    float peer_health = 0.f;
    float peer_max_health = 0.f;
    float peer_guard = 0.f;

    bool puppet_alive = false;
    int faction_mine = -1;
    int faction_puppet = -1;

    bool invite_pending = false;
    char invite_level[128] = {};

    bool invite_answer_valid = false;
    bool invite_answer_accepted = false;

    bool friendly_confirmed = false;

    char public_address[64] = {};

    // This PC's own tailnet address, "" when Tailscale is not running here.
    char tailnet_ip[64] = {};
    // False until the first lookup has finished, so the Setup tab can say
    // "looking" instead of announcing there is no Tailscale before it knows.
    bool tailnet_answered = false;
};

void SetMenuStatus(const MenuStatus& status);

// One machine on the tailnet, for the picker in Setup. Kept separate from
// net::tailscale::Device so the overlay keeps knowing nothing about the
// network, the same way SyncRow keeps it from knowing about enemies.
struct TailnetRow {
    char name[48] = {};
    char ip[48] = {};
    bool online = false;
    bool self = false;
};

void SetTailnetRows(const TailnetRow* rows, int count);

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

}
