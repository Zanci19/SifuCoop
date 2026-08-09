#include "native_net.h"

#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>

#include "../core/log.h"
#include "../ue/reflection.h"
#include "coop.h"
#include "actors.h"

namespace sifucoop::game::native_net {

bool ValidPort(int port) { return port >= 1 && port <= 65535; }

bool ValidIpv4(const char* address) {
    if (!address || !address[0]) return false;
    IN_ADDR parsed = {};
    return InetPtonA(AF_INET, address, &parsed) == 1;
}

ue::UObject* GetAuthoritativeGameMode(ue::UObject* world) {
    ue::UObject* statics = ue::FindObjectByPath(L"/Script/Engine.Default__GameplayStatics");
    if (!statics || !world) return nullptr;
    struct Params {
        ue::UObject* WorldContextObject;
        ue::UObject* ReturnValue;
    } params = {world, nullptr};
    if (!ue::CallFunction(statics, L"GetGameMode", &params)) return nullptr;
    return params.ReturnValue;
}

struct ControllerArray {
    ue::UObject** data = nullptr;
    std::int32_t num = 0;
    std::int32_t max = 0;
};

int ReadHostControllers(ue::UObject* game_mode, ue::UObject** out, int cap) {
    if (!game_mode || !out || cap <= 0) return 0;
    struct Params {
        ControllerArray ReturnValue;
    } params = {};
    if (!ue::CallFunction(game_mode, L"BPF_GetPlayers", &params)) return 0;
    if (!params.ReturnValue.data || params.ReturnValue.num <= 0 ||
        params.ReturnValue.num > 16 || params.ReturnValue.max < params.ReturnValue.num) {
        return 0;
    }
    const int count = params.ReturnValue.num < cap ? params.ReturnValue.num : cap;
    for (int i = 0; i < count; ++i) out[i] = params.ReturnValue.data[i];
    return count;
}
ue::UObject* PawnOf(ue::UObject* controller);
ue::UObject* g_last_world = nullptr;
DWORD g_next_roster_check = 0;
bool g_factions_initialized = false;
int g_roster_attempts = 0;

void TickNativeCoop() {
    if (!coop::NativeNetworkActive()) return;

    ue::UObject* world = ue::GetWorld();
    if (!world) return;
    if (world != g_last_world) {
        g_last_world = world;
        g_next_roster_check = 0;
        g_factions_initialized = false;
        g_roster_attempts = 0;
        SC_LOG("native-net: world changed; waiting for authoritative player roster");
    }
    if (g_factions_initialized || GetTickCount() < g_next_roster_check) return;
    g_next_roster_check = GetTickCount() + 1000;

    ue::UObject* game_mode = GetAuthoritativeGameMode(world);
    if (!game_mode) return;  // Expected on the joining client.

    ue::UObject* primary = ue::GetPlayerCharacter(world, 0);
    const int faction = primary ? GetFaction(primary) : -1;
    if (faction < 0) return;

    ue::UObject* controllers[4] = {};
    const int count = ReadHostControllers(game_mode, controllers, 4);
    if (count < 2) {
        if (++g_roster_attempts <= 5) {
            SC_LOG("native-net: authoritative roster has %d player(s); waiting for joiner", count);
        }
        return;
    }

    int changed = 0;
    for (int i = 0; i < count; ++i) {
        ue::UObject* pawn = PawnOf(controllers[i]);
        if (!pawn || pawn == primary) continue;
        if (GetFaction(pawn) != faction) {
            // BPF_SetFaction is Sifu's own server/multicast route, relationship map and attack filters update on every client
            SetFaction(pawn, faction);
            ++changed;
        }
    }
    if (changed == 0) {
        g_factions_initialized = true;
        SC_LOG("native-net: %d real players share faction %d; engine owns combat/AI/animation",
               count, faction);
    } else {
        SC_LOG("native-net: assigned host faction %d to %d joining pawn(s)", faction, changed);
    }
}

ue::UObject* PawnOf(ue::UObject* controller) {
    if (!controller) return nullptr;
    struct Params {
        ue::UObject* ReturnValue;
    } params = {};
    if (!ue::CallFunction(controller, L"K2_GetPawn", &params)) return nullptr;
    return params.ReturnValue;
}



bool HostCurrentLevel(int port) {
    if (!ValidPort(port)) {
        coop::ReportProblem("native host port must be 1-65535");
        return false;
    }
    char level[192] = {};
    if (!ue::GetCurrentLevelPath(level, sizeof(level))) {
        coop::ReportProblem("native host needs a loaded level");
        return false;
    }

    // Console `open` accepted the text but only performed local travel in this
    // shipping build. Give the URL options directly to UGameplayStatics so UE4
    // receives the listen request when it rebuilds the world.
    char options[48] = {};
    _snprintf(options, sizeof(options) - 1, "?listen?port=%d", port);
    const bool ok = ue::OpenLevelWithOptions(level, options);
    SC_LOG("native-net: direct host travel '%s%s' -> %s", level, options,
           ok ? "dispatched" : "FAILED");
    if (!ok) coop::ReportProblem("could not start the engine listen server");
    return ok;
}

bool JoinHost(const char* address, int port) {
    if (!ValidPort(port) || !ValidIpv4(address)) {
        coop::ReportProblem("native join needs a valid ZeroTier IPv4 and port");
        return false;
    }

    char command[96] = {};
    _snprintf(command, sizeof(command) - 1, "open %s:%d", address, port);
    const bool ok = ue::ExecuteConsoleCommand(command);
    SC_LOG("native-net: join command '%s' -> %s", command, ok ? "dispatched" : "FAILED");


    if (!ok) coop::ReportProblem("could not start the engine connection");
    return ok;
}

}  // namespace sifucoop::game::native_net
