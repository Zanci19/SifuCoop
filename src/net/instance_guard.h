#pragma once

namespace sifucoop::net {

// Acquired once at DLL bootstrap and intentionally held until Windows tears
// down the process. Network restarts must never open a gap in this guard.
inline constexpr char kGameProcessMutexNameA[] =
    "Local\\SifuCoop.Game.Process";
inline constexpr wchar_t kGameProcessMutexNameW[] =
    L"Local\\SifuCoop.Game.Process";

// Separate host-port ownership improves the bind error and follows runtime
// host/client reconfiguration. The process guard above remains authoritative.
inline constexpr char kHostPortMutexFormatA[] =
    "Local\\SifuCoop.Host.Port.%d";

}  // namespace sifucoop::net
