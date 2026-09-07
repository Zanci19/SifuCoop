#pragma once

namespace sifucoop::net {

inline constexpr char kGameProcessMutexNameA[] =
    "Local\\SifuCoop.Game.Process";
inline constexpr wchar_t kGameProcessMutexNameW[] =
    L"Local\\SifuCoop.Game.Process";

inline constexpr char kHostPortMutexFormatA[] =
    "Local\\SifuCoop.Host.Port.%d";

}
