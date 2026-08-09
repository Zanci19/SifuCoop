#pragma once

namespace sifucoop::game::native_net {

// Starts Sifu's own UE4 listen-server path on the current map. This performs
// a full level reload, so it is intentionally called only from an explicit F1
// menu action, never automatically from a UDP handshake.
bool HostCurrentLevel(int port);

// Leaves the current local world and connects to an existing UE4 listen server
// at a ZeroTier IPv4 endpoint. The address is parsed before it enters a console
// command, preventing URL/console-option injection from the editable field.
bool JoinHost(const char* address, int port);
// Server-only native co-op setup. Once Unreal has completed its own login and
// spawn sequence this groups the real pawns on the host's faction, letting the
// engine replicate movement, combat and relationships instead of the legacy
// UDP puppet layer doing so.
void TickNativeCoop();

}  // namespace sifucoop::game::native_net
