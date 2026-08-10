#include "native_net.h"

#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>

#include "../core/log.h"
#include "../ue/reflection.h"
#include "coop.h"

namespace sifucoop::game::native_net {
namespace {

bool ValidPort(int port) { return port >= 1 && port <= 65535; }

bool ValidIpv4(const char* address) {
    if (!address || !address[0]) return false;
    IN_ADDR parsed = {};
    return InetPtonA(AF_INET, address, &parsed) == 1;
}

// UKismetSystemLibrary::IsServer / IsStandalone, so the answer to "did the
// engine actually go into network mode" comes from the engine rather than from
// whether a console command was dispatched.
//
// Without this the experiment is unreadable: `open ?listen` always reports
// success because ExecuteConsoleCommand only says the command was delivered,
// and a listen server that refused to start looks exactly like one that worked
// until a peer fails to appear.
bool QueryNetFlag(const wchar_t* function_name) {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* kismet = ue::FindObjectByPath(L"/Script/Engine.Default__KismetSystemLibrary");
    if (!world || !kismet) return false;
    struct Params {
        ue::UObject* WorldContextObject;
        bool ReturnValue;
    } params = {world, false};
    if (!ue::CallFunction(kismet, function_name, &params)) return false;
    return params.ReturnValue;
}

}  // namespace

void LogNetMode(const char* when) {
    const bool standalone = QueryNetFlag(L"IsStandalone");
    const bool server = QueryNetFlag(L"IsServer");
    // IsServer is true in standalone as well -- a standalone game IS the
    // authority -- so it answers nothing on its own. IsStandalone is the one
    // that discriminates, and reading only the pair together is meaningful.
    SC_LOG("native-net: %s -- standalone=%d server=%d (%s)", when, standalone, server,
           standalone ? "NOT networked" : (server ? "listen server" : "connected client"));
}

DWORD g_report_at = 0;
const char* g_report_what = nullptr;

// The outcome, a few seconds later.
//
// Only "before" was ever logged, which made the experiment unreadable in the
// one way that mattered: `open` returns immediately, the map reload takes
// seconds, and the interesting state exists only afterwards. Both attempts now
// schedule a reading.
void TickNativeNet() {
    if (!g_report_at || GetTickCount() < g_report_at) return;
    g_report_at = 0;
    LogNetMode(g_report_what ? g_report_what : "after");
}

void ReportShortly(const char* what) {
    g_report_what = what;
    g_report_at = GetTickCount() + 8000;
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

    // UE4 URL options are separated by '?'. `listen` makes the reloaded map a
    // listen server; `port` is consumed by UIpNetDriver::InitListen.
    char command[256] = {};
    _snprintf(command, sizeof(command) - 1, "open %s?listen?port=%d", level, port);
    LogNetMode("before hosting");
    const bool ok = ue::ExecuteConsoleCommand(command);
    SC_LOG("native-net: host command '%s' -> %s", command, ok ? "dispatched" : "FAILED");
    if (ok) ReportShortly("after hosting");
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
    LogNetMode("before joining");
    const bool ok = ue::ExecuteConsoleCommand(command);
    SC_LOG("native-net: join command '%s' -> %s", command, ok ? "dispatched" : "FAILED");
    if (ok) ReportShortly("after joining");
    if (!ok) coop::ReportProblem("could not start the engine connection");
    return ok;
}

}  // namespace sifucoop::game::native_net
