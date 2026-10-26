#pragma once

namespace sifucoop::net::tailscale {

// A tailnet is small -- a handful of a person's own machines plus whoever
// they share with -- and the picker only has to hold the ones a partner could
// be sitting at. Anything past this is dropped with a line in the log.
constexpr int kMaxDevices = 16;

struct Device {
    char name[48] = {};
    char ip[48] = {};
    bool online = false;
    bool self = false;
};

// Both of these run `tailscale` as a child process, which takes tens of
// milliseconds on a good day and seconds on a bad one, so neither is ever
// called on the frame. Everything below reads a cache that a worker thread
// fills; the only cost on the caller's thread is a critical section.

// Refreshes the cache in the background when it is older than a few seconds.
// Returns immediately, every time. Call it from the game tick.
void Poll();

// Refreshes now whatever the cache's age, for the Refresh button. Also
// returns immediately; the answer appears in a later frame.
void RequestRefresh();

// This PC's tailnet address, or "" when Tailscale is not running here.
void GetSelfIp(char* out, int size);

// Copies up to `max` devices into `out` and returns how many were written.
// Online devices come first; past that the order is Tailscale's own.
int GetDevices(Device* out, int max);

// False until the first refresh has finished, whatever it found. The UI needs
// the difference: "looking for Tailscale" and "no Tailscale here" are not the
// same message, and showing the second one for the first two seconds after
// the menu opens would be a lie.
bool HaveAnswer();

}
