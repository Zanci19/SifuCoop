#pragma once

#include <cstddef>
#include <cstdint>

// SHA-256 and HMAC-SHA256, self-contained.
//
// Used to authenticate every packet against a passphrase both players share.
// Nothing here is a novel construction: it is RFC 6234 and RFC 2104 written out
// so the mod carries no dependency, and so the one security-relevant thing it
// does can be read end to end in one file.
//
// What this defends against: anyone who can reach the UDP port injecting
// packets. That is not hypothetical once a port is forwarded to the internet --
// the protocol moves a player's position, tells the game which level to load,
// and applies damage, so an unauthenticated packet is a stranger reaching into
// your session. What it does not defend against is the person you are playing
// with, who legitimately holds the key.

namespace sifucoop::net {

constexpr int kSha256Size = 32;

void Sha256(const void* data, std::size_t size, std::uint8_t out[kSha256Size]);

// Two-part message, so a packet's header and body can be signed without being
// copied into one contiguous buffer first.
void HmacSha256(const std::uint8_t* key, std::size_t key_size, const void* data,
                std::size_t size, std::uint8_t out[kSha256Size]);

// Constant-time compare. A byte-at-a-time memcmp leaks, through timing, how
// much of a forged tag was correct -- which is enough to find the rest one byte
// at a time.
bool SecureEqual(const std::uint8_t* a, const std::uint8_t* b, std::size_t size);

}  // namespace sifucoop::net
