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

}  // namespace

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
    const bool ok = ue::ExecuteConsoleCommand(command);
    SC_LOG("native-net: host command '%s' -> %s", command, ok ? "dispatched" : "FAILED");
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
