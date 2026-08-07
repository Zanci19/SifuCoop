#include "session.h"

// rand_s is the CRT's OS-backed generator; it is only declared when this is
// defined, and it must be defined before <stdlib.h> is first reached.
#define _CRT_RAND_S

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdlib>

#include <cstdio>
#include <cstring>

#include "../core/log.h"
#include "../game/coop.h"
#include "crypto.h"
#include "protocol.h"

namespace sifucoop::net {
namespace {

namespace coop = sifucoop::coop;

// Peer-authoritative over its own character: there is no server code in this
// build to arbitrate, so each side owns its own transform and simply reports
// it. Host vs Client differs in who owns the *enemies*, not in who owns you.
SOCKET g_socket = INVALID_SOCKET;
Role g_role = Role::Offline;
bool g_connected = false;
bool g_winsock_started = false;

sockaddr_in g_peer_addr = {};
bool g_have_peer_addr = false;

std::uint32_t g_send_sequence = 0;
std::uint32_t g_last_snapshot_sequence = 0;
DWORD g_last_recv_ms = 0;
DWORD g_last_send_ms = 0;
DWORD g_last_ping_ms = 0;

// Interpolation buffer. Holding peer state slightly in the past and playing it
// back smoothly is what hides jitter; rendering the newest packet immediately
// would snap on every late or reordered datagram.
constexpr int kBufferSize = 64;
constexpr DWORD kTimeoutMs = 5000;
constexpr DWORD kPingIntervalMs = 500;

// GetTickCount only advances every ~15.6 ms. With snapshots arriving every
// 16 ms that quantised the interpolation factor into a few discrete steps,
// which is seen as the puppet moving in visible jerks. A performance-counter
// clock gives a genuinely continuous blend.
DWORD NowMs() {
    static LARGE_INTEGER frequency = {};
    static LARGE_INTEGER start = {};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return static_cast<DWORD>(((now.QuadPart - start.QuadPart) * 1000) / frequency.QuadPart);
}

struct PeerState {
    DWORD received_ms = 0;
    ue::FVector location;
    ue::FRotator rotation;
    bool valid = false;
};

PeerState g_peer_buffer[kBufferSize];
int g_peer_head = 0;

bool g_peer_state_valid = false;
PeerVitals g_peer_vitals;

RunSnapshot g_peer_run;
bool g_peer_run_valid = false;

bool g_connected_event = false;
bool g_disconnected_event = false;

int g_rtt_ms = -1;
int g_rtt_jitter_ms = 0;

// Bandwidth accounting, sampled once a second so the overlay has a rate rather
// than an ever-growing total.
std::uint32_t g_bytes_in = 0;
std::uint32_t g_bytes_out = 0;
DWORD g_rate_window_start = 0;

// Small ring of inbound order events. Attacks are bursty and must not be lost
// behind a single-slot latch, but an unbounded queue would let a stall turn
// into a flood of stale moves replayed at once.
constexpr int kOrderQueueSize = 64;

struct QueuedOrder {
    std::uint32_t actor_hash;
    std::uint32_t order_type;
    std::int32_t attack_index;
    std::int32_t attack_depth;
};

QueuedOrder g_order_queue[kOrderQueueSize];
int g_order_write = 0;
int g_order_read = 0;

// Enemy state is a snapshot, not a queue: only the newest matters, and
// replaying stale enemy positions would drag them backwards exactly as it
// would for the player puppet.
//
// Chunks of one sweep are gathered into `staging` and only promoted to `live`
// once the whole generation has arrived, so a half-updated set is never shown.
EnemyStateOut g_enemies_live[kMaxTrackedEnemies];
int g_enemy_live_count = 0;
bool g_enemy_sweep_seen = false;

EnemyStateOut g_enemies_staging[kMaxTrackedEnemies];
int g_enemy_staging_count = 0;
std::uint32_t g_enemy_staging_generation = 0;
std::uint32_t g_enemy_chunks_seen = 0;
std::uint8_t g_enemy_chunk_total = 0;

DamageReport g_damage[kMaxDamagePerPacket];
int g_damage_count = 0;
std::uint32_t g_damage_sequence = 0;

// --- STUN and hole punching -------------------------------------------------
//
// Two machines behind home routers cannot reach each other by default: neither
// has an address the other can send to, and neither router will forward an
// unsolicited packet inward. There are three ways out, and this supports all
// three because which one works depends on routers nobody here controls.
//
//   1. A private VPN (ZeroTier, Tailscale). Always works, needs no router
//      configuration, and is what the documentation still recommends first.
//   2. The host forwards a UDP port. Always works, needs router access.
//   3. Hole punching. Both sides learn their public address from a STUN server
//      and send to each other at the same time; most home routers then let the
//      replies back in because they look like answers to something outgoing.
//      Free, but not universal -- symmetric NATs defeat it.
//
// The STUN client below is RFC 5389's binding request, which is 20 bytes and
// has no dependencies. Crucially it goes out on the *game's own socket*: a NAT
// mapping belongs to a specific local port, so asking from a different socket
// would return an address that no longer means anything by the time a peer
// tried to use it.

constexpr std::uint32_t kStunCookie = 0x2112A442;
constexpr std::uint16_t kStunBindingRequest = 0x0001;
constexpr std::uint16_t kStunBindingResponse = 0x0101;
constexpr std::uint16_t kStunXorMappedAddress = 0x0020;
constexpr std::uint16_t kStunMappedAddress = 0x0001;

std::uint8_t g_stun_transaction[12] = {};
bool g_stun_pending = false;
DWORD g_stun_sent_ms = 0;
char g_public_address[64] = {};

// Optional: an address the host also pokes while waiting, so the router opens a
// path inward before the joiner's first real packet arrives.
sockaddr_in g_punch_addr = {};
bool g_have_punch_addr = false;
DWORD g_last_punch_ms = 0;

bool ParseEndpoint(const char* text, sockaddr_in* out) {
    if (!text || !text[0]) return false;
    char host[64] = {};
    lstrcpynA(host, text, sizeof(host));
    char* colon = strrchr(host, ':');
    if (!colon) return false;
    *colon = '\0';
    const int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return false;

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(static_cast<u_short>(port));
    return inet_pton(AF_INET, host, &out->sin_addr) == 1;
}

// A STUN response carries the cookie at a fixed place and our own transaction
// id after it, so it is distinguishable from a game packet without ambiguity.
bool LooksLikeStunResponse(const std::uint8_t* data, int size) {
    if (size < 20) return false;
    const std::uint16_t type = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    if (type != kStunBindingResponse) return false;
    const std::uint32_t cookie = (static_cast<std::uint32_t>(data[4]) << 24) |
                                 (static_cast<std::uint32_t>(data[5]) << 16) |
                                 (static_cast<std::uint32_t>(data[6]) << 8) | data[7];
    return cookie == kStunCookie;
}

void HandleStunResponse(const std::uint8_t* data, int size) {
    if (!g_stun_pending) return;
    if (memcmp(data + 8, g_stun_transaction, sizeof(g_stun_transaction)) != 0) return;

    int offset = 20;
    while (offset + 4 <= size) {
        const std::uint16_t attr =
            static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
        const std::uint16_t length =
            static_cast<std::uint16_t>((data[offset + 2] << 8) | data[offset + 3]);
        const int value = offset + 4;
        if (value + length > size) break;

        if ((attr == kStunXorMappedAddress || attr == kStunMappedAddress) && length >= 8 &&
            data[value + 1] == 0x01) {  // family 0x01 = IPv4
            std::uint16_t port =
                static_cast<std::uint16_t>((data[value + 2] << 8) | data[value + 3]);
            std::uint32_t address = (static_cast<std::uint32_t>(data[value + 4]) << 24) |
                                    (static_cast<std::uint32_t>(data[value + 5]) << 16) |
                                    (static_cast<std::uint32_t>(data[value + 6]) << 8) |
                                    data[value + 7];
            if (attr == kStunXorMappedAddress) {
                // XOR'd with the cookie precisely so middleboxes that rewrite
                // addresses in payloads do not silently mangle it.
                port ^= static_cast<std::uint16_t>(kStunCookie >> 16);
                address ^= kStunCookie;
            }
            in_addr in = {};
            in.s_addr = htonl(address);
            char ip[64] = {};
            inet_ntop(AF_INET, &in, ip, sizeof(ip));
            _snprintf(g_public_address, sizeof(g_public_address) - 1, "%s:%u", ip, port);
            g_stun_pending = false;
            SC_LOG("net: this machine looks like %s from outside", g_public_address);
            return;
        }
        // Attributes are padded to a 4-byte boundary.
        offset = value + ((length + 3) & ~3);
    }
}

// --- Authentication ---------------------------------------------------------

// Derived from the passphrase alone. Used for the handshake, before the two
// sides have agreed on a session.
std::uint8_t g_base_key[kSha256Size] = {};
// Derived from the passphrase plus both nonces. Used for everything after.
std::uint8_t g_session_key[kSha256Size] = {};
bool g_have_session_key = false;

std::uint8_t g_local_nonce[kSessionNonceSize] = {};
std::uint8_t g_remote_nonce[kSessionNonceSize] = {};

std::uint32_t g_rejected_packets = 0;
DWORD g_last_reject_log = 0;

void DeriveBaseKey(const char* passphrase) {
    // A passphrase is not a key: it is short, low entropy and typed by a human.
    // Hashing it with a fixed label is the minimum that stops the raw string
    // from being the HMAC key, and keeps a short passphrase from producing a
    // short key.
    char material[192] = {};
    _snprintf(material, sizeof(material) - 1, "SifuCoop-v%u-key:%s", kProtocolVersion,
              passphrase ? passphrase : "");
    Sha256(material, strlen(material), g_base_key);
    g_have_session_key = false;
}

// Neither side alone decides the session key: it comes from the passphrase and
// both nonces, in a fixed order so the two machines agree on which is which.
void DeriveSessionKey(const std::uint8_t* host_nonce, const std::uint8_t* joiner_nonce) {
    std::uint8_t material[kSha256Size + kSessionNonceSize * 2];
    memcpy(material, g_base_key, kSha256Size);
    memcpy(material + kSha256Size, host_nonce, kSessionNonceSize);
    memcpy(material + kSha256Size + kSessionNonceSize, joiner_nonce, kSessionNonceSize);
    Sha256(material, sizeof(material), g_session_key);
    g_have_session_key = true;
}

// Not cryptographic randomness -- Windows' own is, and this is the one place
// the mod needs unpredictability. A guessable nonce would let a recorded
// session's packets be replayed into a new one.
void MakeNonce(std::uint8_t out[kSessionNonceSize]) {
    // rand_s is the CRT's cryptographically secure generator; it is seeded by
    // the OS rather than by us and needs no initialisation.
    for (int i = 0; i < kSessionNonceSize; i += 4) {
        unsigned int value = 0;
        if (rand_s(&value) != 0) {
            // Falling back to a timer would be worse than useless -- it is
            // exactly what an attacker can predict. Refusing is the honest
            // outcome; the caller reports it and the session does not start.
            value = 0;
        }
        memcpy(out + i, &value, 4);
    }
}

// Signs `packet` in place: zero the tag, HMAC the whole thing, keep the first
// kAuthTagSize bytes.
void SignPacket(void* packet, int size) {
    auto* header = static_cast<PacketHeader*>(packet);
    memset(header->tag, 0, kAuthTagSize);

    const std::uint8_t* key = g_have_session_key ? g_session_key : g_base_key;
    std::uint8_t digest[kSha256Size];
    HmacSha256(key, kSha256Size, packet, static_cast<std::size_t>(size), digest);
    memcpy(header->tag, digest, kAuthTagSize);
}

// Verifies a received packet. `buffer` is modified (the tag is zeroed) and must
// be a private copy, which it is -- every caller memcpy's out of it afterwards.
bool VerifyPacket(void* buffer, int size, bool handshake) {
    if (size < static_cast<int>(sizeof(PacketHeader))) return false;

    auto* header = static_cast<PacketHeader*>(buffer);
    std::uint8_t received[kAuthTagSize];
    memcpy(received, header->tag, kAuthTagSize);
    memset(header->tag, 0, kAuthTagSize);

    // Handshake packets predate the session key, so they are checked against
    // the passphrase alone.
    const std::uint8_t* key =
        (handshake || !g_have_session_key) ? g_base_key : g_session_key;
    std::uint8_t digest[kSha256Size];
    HmacSha256(key, kSha256Size, buffer, static_cast<std::size_t>(size), digest);

    memcpy(header->tag, received, kAuthTagSize);
    return SecureEqual(received, digest, kAuthTagSize);
}

void NoteRejected(const sockaddr_in& from) {
    ++g_rejected_packets;
    coop::GetStats().packets_rejected = g_rejected_packets;
    const DWORD now = GetTickCount();
    // Rate limited hard: a flood must not turn into a flood of log writes,
    // which would be a denial of service we inflicted on ourselves.
    if (now - g_last_reject_log < 5000 && g_last_reject_log != 0) return;
    g_last_reject_log = now;

    char ip[64] = {};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    SC_LOG("net: rejected an unauthenticated packet from %s:%d (%u so far) -- wrong "
           "passphrase, or someone else is probing the port",
           ip, ntohs(from.sin_port), g_rejected_packets);
    coop::ReportProblem("rejected %u packets that failed authentication", g_rejected_packets);
}

// Everything the peer sends that reaches the game gets checked here first.
//
// A level path arrives as bytes and ends up in UGameplayStatics::OpenLevel. Even
// with authentication in place that deserves a shape check: a peer running a
// corrupted build, or a future version, should fail visibly rather than ask the
// engine to open something arbitrary.
bool LooksLikeLevelPath(const char* path) {
    if (!path || !path[0]) return false;
    if (strncmp(path, "/Game/", 6) != 0) return false;
    for (const char* p = path; *p; ++p) {
        const char c = *p;
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '.' ||
                             c == '-';
        if (!allowed) return false;
    }
    // ".." cannot mean anything to a package path and is the shape of a
    // traversal attempt, so it is refused rather than reasoned about.
    return strstr(path, "..") == nullptr;
}

bool IsFinite(float value) {
    // NaN fails every comparison with itself; infinities exceed any bound. Both
    // reach the engine as a position or a health value if not stopped here, and
    // a NaN position propagates into the movement component and stays there.
    return value == value && value > -1e9f && value < 1e9f;
}

bool IsFiniteVector(float x, float y, float z) {
    return IsFinite(x) && IsFinite(y) && IsFinite(z);
}

void ReadConfig(char* host, int host_size, int* port, bool* is_host) {
    char ini_path[MAX_PATH] = {};
    coop::IniPath(ini_path, sizeof(ini_path));

    char mode[32] = {};
    GetPrivateProfileStringA("net", "mode", "off", mode, sizeof(mode), ini_path);
    GetPrivateProfileStringA("net", "host", "127.0.0.1", host, host_size, ini_path);
    *port = GetPrivateProfileIntA("net", "port", kDefaultPort, ini_path);

    char passphrase[128] = {};
    GetPrivateProfileStringA("net", "passphrase", "", passphrase, sizeof(passphrase), ini_path);
    DeriveBaseKey(passphrase);

    *is_host = (_stricmp(mode, "host") == 0);
    const bool off = (_stricmp(mode, "off") == 0);

    SC_LOG("net: config %s -- mode=%s host=%s port=%d passphrase=%s", ini_path, mode, host,
           *port, passphrase[0] ? "set" : "EMPTY");
    if (!passphrase[0] && !off) {
        // Not fatal -- an empty passphrase still produces a key, and two peers
        // that both leave it empty will still talk to each other. It is simply
        // a key everyone else also has, which is fine on a private VPN and not
        // fine on a forwarded port. Say so rather than deciding for them.
        SC_LOG("net: no passphrase set -- anyone who can reach this port can join. "
               "Fine over a VPN; set one before forwarding a port.");
        coop::ReportProblem("no passphrase set -- only safe on a private network");
    }

    if (off) g_role = Role::Offline;
    else g_role = *is_host ? Role::Host : Role::Client;
}

// A machine on a VPN has several addresses, and the host has to tell their
// peer the right one. ZeroTier hands out 10.x / 172.2x addresses on its own
// adapter, so the candidates are listed and the likely VPN one is flagged
// rather than leaving the user to guess from ipconfig.
void LogLocalAddresses(int port) {
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) return;

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* results = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &results) != 0) return;

    SC_LOG("net: give your peer ONE of these for 'host=' in their SifuCoop.ini --");
    for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
        auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
        char ip[64] = {};
        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

        const unsigned long host_order = ntohl(addr->sin_addr.s_addr);
        const unsigned int first = (host_order >> 24) & 0xFF;
        const unsigned int second = (host_order >> 16) & 0xFF;

        // ZeroTier's managed ranges: 10.x and 172.16-31.x. A 192.168.x address
        // is an ordinary LAN, which only works if you are on the same network.
        const bool likely_vpn = (first == 10) || (first == 172 && second >= 16 && second <= 31);
        SC_LOG("net:   %s:%d%s", ip, port, likely_vpn ? "   <-- likely ZeroTier/VPN" : "");
    }
    freeaddrinfo(results);
}

// The single egress point, so signing happens exactly once and cannot be
// forgotten by a new packet type.
void SendPacket(void* data, int size) {
    if (!g_have_peer_addr || g_socket == INVALID_SOCKET) return;
    SignPacket(data, size);
    sendto(g_socket, static_cast<const char*>(data), size, 0,
           reinterpret_cast<sockaddr*>(&g_peer_addr), sizeof(g_peer_addr));
    g_bytes_out += static_cast<std::uint32_t>(size);
    ++coop::GetStats().packets_sent;
}

// Replies to a specific address rather than the pinned peer. Only the
// handshake needs this: until a peer is accepted there is nobody to reply to.
void SendPacketTo(void* data, int size, const sockaddr_in& to) {
    if (g_socket == INVALID_SOCKET) return;
    SignPacket(data, size);
    sendto(g_socket, static_cast<const char*>(data), size, 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    g_bytes_out += static_cast<std::uint32_t>(size);
    ++coop::GetStats().packets_sent;
}

void FillHeader(PacketHeader* header, PacketType type) {
    header->magic = kMagic;
    header->version = kProtocolVersion;
    header->type = static_cast<std::uint16_t>(type);
    header->sequence = ++g_send_sequence;
    header->send_time_ms = NowMs();
}

// Samples, queued orders and sequence numbers from a previous connection are
// meaningless to a new one: without this, a reconnect interpolates the puppet
// from wherever the last peer was standing, and a stale sequence number can
// silently discard every packet of the new session.
// Edge-triggered: only the newest montage matters, and a queue would replay a
// backlog of stale animations after any stall.
char g_montage_path[192] = {};
float g_montage_position = 0.f;
bool g_have_montage = false;

void ResetPeerState() {
    g_have_montage = false;
    for (PeerState& state : g_peer_buffer) state.valid = false;
    g_peer_head = 0;
    g_last_snapshot_sequence = 0;
    g_order_read = g_order_write;
    g_peer_state_valid = false;
    g_peer_vitals = PeerVitals();
    g_peer_run_valid = false;
    g_peer_run = RunSnapshot();
    g_enemy_live_count = 0;
    g_enemy_staging_count = 0;
    g_enemy_chunks_seen = 0;
    g_enemy_chunk_total = 0;
    g_enemy_sweep_seen = false;
    g_damage_count = 0;
    g_damage_sequence = 0;
    g_rtt_ms = -1;
    g_rtt_jitter_ms = 0;
}

void HandleSnapshot(const SnapshotPacket& packet) {
    // A NaN position does not stay in the packet: it goes into the puppet's
    // movement component and lives there, because every later comparison
    // against it is false and no correction ever fires.
    if (!IsFiniteVector(packet.x, packet.y, packet.z)) return;
    if (!IsFiniteVector(packet.pitch, packet.yaw, packet.roll)) return;
    if (!IsFinite(packet.health) || !IsFinite(packet.max_health) || !IsFinite(packet.guard)) {
        return;
    }

    // Drop packets that arrive out of order; UDP makes no ordering promise and
    // an older transform would drag the puppet backwards. Snapshots carry their
    // own sequence line because they are the only stream where order matters.
    if (g_last_snapshot_sequence != 0) {
        if (packet.header.sequence <= g_last_snapshot_sequence) return;
        const std::uint32_t gap = packet.header.sequence - g_last_snapshot_sequence;
        if (gap > 1) coop::GetStats().packets_dropped += gap - 1;
    }
    g_last_snapshot_sequence = packet.header.sequence;

    g_peer_head = (g_peer_head + 1) % kBufferSize;
    PeerState& state = g_peer_buffer[g_peer_head];
    state.received_ms = NowMs();
    state.location = {packet.x, packet.y, packet.z};
    state.rotation = {packet.pitch, packet.yaw, packet.roll};
    state.valid = true;

    if (packet.flags & kFlagStateValid) {
        g_peer_state_valid = true;
        g_peer_vitals.health = packet.health;
        g_peer_vitals.max_health = packet.max_health;
        g_peer_vitals.guard = packet.guard;
        g_peer_vitals.is_down = (packet.flags & kFlagIsDown) != 0;
        g_peer_vitals.in_level = (packet.flags & kFlagInLevel) != 0;
    }
}

// Level invites are idempotent: the host repeats them, and acting on the same
// request twice would restart a load that is already running.
std::uint32_t g_level_request_seen = 0;
std::uint32_t g_level_request_next = 1;
char g_pending_level[192] = {};
bool g_have_pending_level = false;
char g_peer_level[192] = {};

void HandleLevelSync(const LevelSyncPacket& packet) {
    // This string ends up in UGameplayStatics::OpenLevel. Authentication means
    // it came from the person we are playing with; the shape check means a
    // corrupted or mismatched build fails visibly instead of asking the engine
    // to open something arbitrary.
    if (!LooksLikeLevelPath(packet.level_path)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SC_LOG("net: refusing a level path that is not a /Game/ package");
            coop::ReportProblem("peer sent an unusable level path");
        }
        return;
    }

    // Remember where they are regardless, so the lobby can show it.
    lstrcpynA(g_peer_level, packet.level_path, sizeof(g_peer_level));

    if (packet.request_id == 0 || packet.request_id == g_level_request_seen) return;
    g_level_request_seen = packet.request_id;

    lstrcpynA(g_pending_level, packet.level_path, sizeof(g_pending_level));
    g_have_pending_level = true;
    SC_LOG("net: host invited us to '%s'", g_pending_level);
}

void HandleEnemyState(const EnemyStatePacket& packet) {
    int count = packet.count;
    if (count < 0) count = 0;
    if (count > kMaxEnemiesPerPacket) count = kMaxEnemiesPerPacket;

    // A chunk from a newer sweep abandons whatever was half-assembled: the old
    // generation can no longer complete, and keeping it would strand the buffer
    // forever if a single chunk were lost.
    if (packet.generation != g_enemy_staging_generation) {
        g_enemy_staging_generation = packet.generation;
        g_enemy_staging_count = 0;
        g_enemy_chunks_seen = 0;
        g_enemy_chunk_total = packet.chunk_count;
    }

    for (int i = 0; i < count && g_enemy_staging_count < kMaxTrackedEnemies; ++i) {
        const EnemyEntry& in = packet.entries[i];
        // Same reasoning as snapshots: these become teleport destinations and
        // health writes. A single bad value here is a character parked at
        // infinity that nothing can bring back.
        if (!IsFiniteVector(in.x, in.y, in.z) || !IsFinite(in.yaw)) continue;
        if (!IsFinite(in.health) || !IsFinite(in.max_health) || !IsFinite(in.guard) ||
            !IsFinite(in.damage_applied)) {
            continue;
        }
        EnemyStateOut& out = g_enemies_staging[g_enemy_staging_count++];
        out.name_hash = in.name_hash;
        out.x = in.x;
        out.y = in.y;
        out.z = in.z;
        out.yaw = in.yaw;
        out.health = in.health;
        out.max_health = in.max_health;
        out.guard = in.guard;
        out.damage_applied = in.damage_applied;
        out.flags = in.flags;
    }

    if (packet.chunk < 32) g_enemy_chunks_seen |= (1u << packet.chunk);

    const std::uint32_t wanted =
        packet.chunk_count >= 32 ? 0xFFFFFFFFu : (1u << packet.chunk_count) - 1u;
    if ((g_enemy_chunks_seen & wanted) != wanted) return;

    // Whole sweep present: promote it in one step so the game thread never
    // reads a set that is half this frame and half the last.
    for (int i = 0; i < g_enemy_staging_count; ++i) g_enemies_live[i] = g_enemies_staging[i];
    g_enemy_live_count = g_enemy_staging_count;
    g_enemy_staging_count = 0;
    g_enemy_chunks_seen = 0;
    g_enemy_sweep_seen = true;
}

// Each damage packet is a complete statement of the client's running totals for
// the enemies it is currently fighting, so the whole set is replaced rather
// than merged. Merging with a max() would have been loss-tolerant but wrong at
// the one moment that matters: when an enemy is recycled from the pool the
// client's total restarts at zero, and a max() would pin it at the old value
// forever, so the host would never again register a hit on that body.
//
// Ordering is settled by sequence number; an out-of-order packet is ignored
// rather than allowed to undo a newer one.
void HandleEnemyDamage(const EnemyDamagePacket& packet) {
    if (g_damage_sequence != 0 && packet.header.sequence <= g_damage_sequence) return;
    g_damage_sequence = packet.header.sequence;

    int count = static_cast<int>(packet.count);
    if (count < 0) count = 0;
    if (count > kMaxDamagePerPacket) count = kMaxDamagePerPacket;

    int kept = 0;
    for (int i = 0; i < count; ++i) {
        // A total that is not a finite, non-negative number would be applied as
        // damage. Bounded above as well: the honest ceiling for one enemy's
        // accumulated damage is far below this, and anything past it is a bug
        // or a lie either way.
        const float total = packet.entries[i].total;
        if (!IsFinite(total) || total < 0.f || total > 1e6f) continue;
        g_damage[kept].name_hash = packet.entries[i].name_hash;
        g_damage[kept].total = total;
        ++kept;
    }
    g_damage_count = kept;
}

void HandlePing(const PingPacket& packet) {
    PingPacket pong = {};
    FillHeader(&pong.header, PacketType::Pong);
    pong.probe_time_ms = packet.probe_time_ms;  // echoed verbatim
    SendPacket(&pong, sizeof(pong));
}

void HandlePong(const PingPacket& packet) {
    const DWORD now = NowMs();
    // Our own clock both ways, so the two machines never have to be in step.
    const int sample = static_cast<int>(now - packet.probe_time_ms);
    if (sample < 0 || sample > 5000) return;

    if (g_rtt_ms < 0) {
        g_rtt_ms = sample;
    } else {
        const int deviation = sample > g_rtt_ms ? sample - g_rtt_ms : g_rtt_ms - sample;
        // Jitter tracked separately from the mean: the interpolation delay has
        // to cover the spread, not the average, or every outlier is a stutter.
        g_rtt_jitter_ms = (g_rtt_jitter_ms * 3 + deviation) / 4;
        g_rtt_ms = (g_rtt_ms * 7 + sample) / 8;
    }

    coop::Stats& stats = coop::GetStats();
    stats.rtt_ms = g_rtt_ms;
    stats.rtt_jitter_ms = g_rtt_jitter_ms;
}

void HandleMontage(const MontagePacket& packet) {
    lstrcpynA(g_montage_path, packet.montage_path, sizeof(g_montage_path));
    g_montage_position = packet.position;
    g_have_montage = true;
}

void HandleRunState(const RunStatePacket& packet) {
    // Vitals this side only ever displays, or uses as an optional nudge of the
    // joiner's own game state -- so the same NaN discipline applies: one bogus
    // value would otherwise show as an impossible age or a door that opened
    // early.
    g_peer_run = RunSnapshot();
    g_peer_run.age = packet.age;
    g_peer_run.room_clear_percent =
        IsFinite(packet.room_clear_percent) ? packet.room_clear_percent : -1.f;
    g_peer_run.age_valid = (packet.flags & kRunAgeValid) != 0;
    g_peer_run.room_clear_valid =
        (packet.flags & kRunRoomClearValid) != 0 && g_peer_run.room_clear_percent >= 0.f;
    g_peer_run.has_weapon = (packet.flags & kRunHasWeapon) != 0;
    lstrcpynA(g_peer_run.weapon_path, packet.weapon_path, sizeof(g_peer_run.weapon_path));
    g_peer_run_valid = true;
}

void QueueOrder(const OrderEventPacket& packet) {
    const int next = (g_order_write + 1) % kOrderQueueSize;
    if (next == g_order_read) {
        // Full: drop the oldest rather than the newest. A stale move is worth
        // less than the one that just happened.
        g_order_read = (g_order_read + 1) % kOrderQueueSize;
    }
    g_order_queue[g_order_write] = {packet.actor_hash, packet.order_type, packet.attack_index,
                                    packet.attack_depth};
    g_order_write = next;
}

void PumpReceive() {
    char buffer[kMaxPacketSize];
    sockaddr_in from = {};

    // Bounded so a flood cannot stall a frame; anything beyond this is read on
    // the next one, and UDP is free to have dropped it anyway.
    for (int i = 0; i < 64; ++i) {
        // recvfrom writes the actual address length back, so it must be reset
        // for every call -- not once before the loop.
        int from_size = sizeof(from);
        const int received = recvfrom(g_socket, buffer, sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_size);
        if (received <= 0) break;
        if (received < static_cast<int>(sizeof(PacketHeader))) continue;

        // STUN replies share this socket on purpose (the NAT mapping belongs to
        // this port, not to some other one), so they are recognised before the
        // game protocol gets a look.
        if (LooksLikeStunResponse(reinterpret_cast<const std::uint8_t*>(buffer), received)) {
            HandleStunResponse(reinterpret_cast<const std::uint8_t*>(buffer), received);
            continue;
        }

        PacketHeader header = {};
        memcpy(&header, buffer, sizeof(header));
        if (header.magic != kMagic) continue;
        if (header.version != kProtocolVersion) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                SC_LOG("net: peer speaks protocol v%u, we speak v%u -- ignoring",
                       header.version, kProtocolVersion);
                coop::ReportProblem("peer runs a different SifuCoop version (v%u vs v%u)",
                                    header.version, kProtocolVersion);
            }
            continue;
        }

        const auto type = static_cast<PacketType>(header.type);
        const bool handshake = (type == PacketType::Hello || type == PacketType::Welcome);

        // Authenticate before a single field is interpreted. Everything below
        // this line moves a character, loads a level or applies damage, and
        // none of it should ever run on bytes from an unknown sender.
        if (!VerifyPacket(buffer, received, handshake)) {
            NoteRejected(from);
            continue;
        }

        // Once a peer is accepted, only that address is listened to. Without
        // this an authenticated session could still be hijacked by anyone who
        // learned the passphrase later, and more practically it stops a stray
        // packet from an old session from disturbing a live one.
        if (g_connected && !handshake) {
            if (from.sin_addr.s_addr != g_peer_addr.sin_addr.s_addr ||
                from.sin_port != g_peer_addr.sin_port) {
                continue;
            }
        }

        g_last_recv_ms = NowMs();
        g_bytes_in += static_cast<std::uint32_t>(received);
        ++coop::GetStats().packets_received;

        // Anything smaller than the struct it claims to be would read past the
        // end of what actually arrived, so every case checks its own size.
        auto fits = [&](std::size_t size) { return received >= static_cast<int>(size); };

        switch (type) {
            case PacketType::Hello: {
                if (!fits(sizeof(HelloPacket))) break;
                HelloPacket hello = {};
                memcpy(&hello, buffer, sizeof(hello));

                const bool same_peer =
                    g_connected && from.sin_addr.s_addr == g_peer_addr.sin_addr.s_addr &&
                    from.sin_port == g_peer_addr.sin_port;

                // A Hello from somewhere else while a session is live is not a
                // reconnection, it is a second person knocking. Answering it
                // would hand our session to whoever knocked last.
                if (g_connected && !same_peer) {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        char ip[64] = {};
                        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                        SC_LOG("net: ignoring a join from %s -- already playing with someone",
                               ip);
                        coop::ReportProblem("someone else tried to join; already in a session");
                    }
                    break;
                }

                g_peer_addr = from;
                g_have_peer_addr = true;
                memcpy(g_remote_nonce, hello.nonce, kSessionNonceSize);
                // Host contributes the first nonce, joiner the second, in that
                // fixed order so both machines derive the same key.
                DeriveSessionKey(g_local_nonce, g_remote_nonce);

                if (!g_connected) {
                    g_connected = true;
                    g_connected_event = true;
                    // Deliberately after DeriveSessionKey: ResetPeerState must
                    // not clear the key it just installed.
                    ResetPeerState();
                    char ip[64] = {};
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    SC_LOG("net: peer connected from %s:%d (authenticated)", ip,
                           ntohs(from.sin_port));
                }

                // Signed with the base key, because the joiner cannot derive
                // the session key until it reads the nonce this carries.
                WelcomePacket welcome = {};
                FillHeader(&welcome.header, PacketType::Welcome);
                welcome.peer_id = 1;
                memcpy(welcome.nonce, g_local_nonce, kSessionNonceSize);
                memcpy(welcome.echo_nonce, g_remote_nonce, kSessionNonceSize);
                const bool had_session = g_have_session_key;
                g_have_session_key = false;  // sign the reply with the base key
                SendPacketTo(&welcome, sizeof(welcome), from);
                g_have_session_key = had_session;
                break;
            }
            case PacketType::Welcome: {
                if (!fits(sizeof(WelcomePacket))) break;
                WelcomePacket welcome = {};
                memcpy(&welcome, buffer, sizeof(welcome));

                // The host echoes our nonce back. Checking it is what makes
                // this a live exchange rather than a recording: a replayed
                // Welcome carries a nonce we are no longer using.
                if (!SecureEqual(welcome.echo_nonce, g_local_nonce, kSessionNonceSize)) {
                    NoteRejected(from);
                    break;
                }

                memcpy(g_remote_nonce, welcome.nonce, kSessionNonceSize);
                DeriveSessionKey(g_remote_nonce, g_local_nonce);
                g_peer_addr = from;
                g_have_peer_addr = true;

                if (!g_connected) {
                    g_connected = true;
                    g_connected_event = true;
                    ResetPeerState();
                    SC_LOG("net: host accepted us (authenticated)");
                }
                break;
            }
            case PacketType::Snapshot:
                if (fits(sizeof(SnapshotPacket))) {
                    SnapshotPacket snapshot = {};
                    memcpy(&snapshot, buffer, sizeof(snapshot));
                    HandleSnapshot(snapshot);
                }
                break;
            case PacketType::OrderEvent:
                if (fits(sizeof(OrderEventPacket))) {
                    OrderEventPacket order = {};
                    memcpy(&order, buffer, sizeof(order));
                    QueueOrder(order);
                }
                break;
            case PacketType::LevelSync:
                if (fits(sizeof(LevelSyncPacket))) {
                    LevelSyncPacket level = {};
                    memcpy(&level, buffer, sizeof(level));
                    level.level_path[sizeof(level.level_path) - 1] = '\0';
                    HandleLevelSync(level);
                }
                break;
            case PacketType::EnemyState:
                if (fits(sizeof(EnemyStatePacket))) {
                    EnemyStatePacket enemies = {};
                    memcpy(&enemies, buffer, sizeof(enemies));
                    HandleEnemyState(enemies);
                }
                break;
            case PacketType::EnemyDamage:
                if (fits(sizeof(EnemyDamagePacket))) {
                    EnemyDamagePacket damage = {};
                    memcpy(&damage, buffer, sizeof(damage));
                    HandleEnemyDamage(damage);
                }
                break;
            case PacketType::Ping:
                if (fits(sizeof(PingPacket))) {
                    PingPacket ping = {};
                    memcpy(&ping, buffer, sizeof(ping));
                    HandlePing(ping);
                }
                break;
            case PacketType::Pong:
                if (fits(sizeof(PingPacket))) {
                    PingPacket pong = {};
                    memcpy(&pong, buffer, sizeof(pong));
                    HandlePong(pong);
                }
                break;
            case PacketType::RunState:
                if (fits(sizeof(RunStatePacket))) {
                    RunStatePacket run = {};
                    memcpy(&run, buffer, sizeof(run));
                    run.weapon_path[sizeof(run.weapon_path) - 1] = '\0';
                    HandleRunState(run);
                }
                break;
            case PacketType::MontageState:
                if (fits(sizeof(MontagePacket))) {
                    MontagePacket montage = {};
                    memcpy(&montage, buffer, sizeof(montage));
                    montage.montage_path[sizeof(montage.montage_path) - 1] = '\0';
                    HandleMontage(montage);
                }
                break;
            case PacketType::Goodbye:
                SC_LOG("net: peer disconnected");
                if (g_connected) g_disconnected_event = true;
                g_connected = false;
                break;
        }
    }
}

void UpdateRates(DWORD now) {
    if (g_rate_window_start == 0) g_rate_window_start = now;
    const DWORD elapsed = now - g_rate_window_start;
    if (elapsed < 1000) return;

    coop::Stats& stats = coop::GetStats();
    stats.bytes_per_second_in = g_bytes_in * 1000 / elapsed;
    stats.bytes_per_second_out = g_bytes_out * 1000 / elapsed;
    g_bytes_in = 0;
    g_bytes_out = 0;
    g_rate_window_start = now;
}

}  // namespace

bool StartSession() {
    char host[128] = {};
    int port = kDefaultPort;
    bool is_host = false;
    ReadConfig(host, sizeof(host), &port, &is_host);

    if (g_role == Role::Offline) {
        SC_LOG("net: disabled (set mode=host or mode=client in SifuCoop.ini)");
        return false;
    }

    // A fresh nonce per session start. Everything derived from it -- and so
    // every packet of this session -- is unrelated to the last one, which is
    // what stops a recording of an earlier session being replayed into this
    // one under the same passphrase.
    MakeNonce(g_local_nonce);
    g_have_session_key = false;
    g_rejected_packets = 0;

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        SC_LOG("net: WSAStartup failed");
        return false;
    }
    g_winsock_started = true;

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET) {
        SC_LOG("net: socket() failed (%d)", WSAGetLastError());
        return false;
    }

    // Non-blocking: this is pumped from the game thread and must never stall a
    // frame waiting on the network.
    u_long non_blocking = 1;
    ioctlsocket(g_socket, FIONBIO, &non_blocking);

    // A burst of enemy chunks can arrive between two frames; the default
    // receive buffer is generous but saying so costs nothing and a silent
    // overflow would look exactly like packet loss.
    int recv_buffer = 256 * 1024;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&recv_buffer),
               sizeof(recv_buffer));

    char ini_path[MAX_PATH] = {};
    coop::IniPath(ini_path, sizeof(ini_path));

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    // The host binds the agreed port; the client normally takes any free one
    // and is discovered by the host from its Hello.
    //
    // `local_port` overrides that, and exists for hole punching: a joiner on an
    // ephemeral port has an address the host cannot predict, so both sides have
    // to pin one before they can tell each other where to aim.
    const int local_port = GetPrivateProfileIntA("net", "local_port", 0, ini_path);
    const int bind_port = is_host ? port : local_port;
    local.sin_port = htons(static_cast<u_short>(bind_port));

    if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        SC_LOG("net: bind failed (%d) -- is another instance already hosting?",
               WSAGetLastError());
        coop::ReportProblem("could not bind UDP %d -- already hosting elsewhere?", port);
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
        return false;
    }

    // Optional punch target, useful in either role: the public address the
    // other player read off their own STUN result and sent you.
    char punch[80] = {};
    GetPrivateProfileStringA("net", "punch", "", punch, sizeof(punch), ini_path);
    g_have_punch_addr = ParseEndpoint(punch, &g_punch_addr);
    if (punch[0] && !g_have_punch_addr) {
        SC_LOG("net: punch='%s' is not a valid address:port -- ignoring", punch);
    } else if (g_have_punch_addr) {
        SC_LOG("net: will open a path toward %s while waiting", punch);
    }

    if (is_host) {
        SC_LOG("net: HOSTING on port %d -- waiting for a peer", port);
        LogLocalAddresses(port);
    } else {
        g_peer_addr.sin_family = AF_INET;
        g_peer_addr.sin_port = htons(static_cast<u_short>(port));
        if (inet_pton(AF_INET, host, &g_peer_addr.sin_addr) != 1) {
            SC_LOG("net: '%s' is not a valid IPv4 address", host);
            coop::ReportProblem("'%s' is not a valid IPv4 address", host);
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
            return false;
        }
        g_have_peer_addr = true;
        SC_LOG("net: JOINING %s:%d", host, port);
    }
    return true;
}

bool Reconfigure(bool host_mode, const char* address, int port, const char* passphrase) {
    char ini_path[MAX_PATH] = {};
    coop::IniPath(ini_path, sizeof(ini_path));

    char port_text[16] = {};
    _snprintf(port_text, sizeof(port_text), "%d", port);

    // Persist first, so the choice survives a restart even if the socket fails.
    WritePrivateProfileStringA("net", "mode", host_mode ? "host" : "client", ini_path);
    if (address && address[0]) WritePrivateProfileStringA("net", "host", address, ini_path);
    WritePrivateProfileStringA("net", "port", port_text, ini_path);
    // Written even when empty: clearing the field has to be able to clear the
    // setting, or a passphrase could never be removed from inside the game.
    WritePrivateProfileStringA("net", "passphrase", passphrase ? passphrase : "", ini_path);

    SC_LOG("net: reconfiguring as %s %s:%d", host_mode ? "HOST" : "CLIENT",
           address ? address : "?", port);

    StopSession();
    ResetPeerState();
    g_connected = false;
    g_have_peer_addr = false;
    g_send_sequence = 0;
    g_last_recv_ms = 0;
    g_last_send_ms = 0;
    return StartSession();
}

void StopSession() {
    // Winsock is reference counted, so an early return here without the
    // matching cleanup leaks a reference on every Reconfigure -- which is the
    // one path that calls Stop then Start repeatedly.
    if (g_socket == INVALID_SOCKET) {
        if (g_winsock_started) {
            g_winsock_started = false;
            WSACleanup();
        }
        return;
    }
    if (g_connected) {
        PacketHeader goodbye = {};
        goodbye.magic = kMagic;
        goodbye.version = kProtocolVersion;
        goodbye.type = static_cast<std::uint16_t>(PacketType::Goodbye);
        SendPacket(&goodbye, sizeof(goodbye));
    }
    closesocket(g_socket);
    g_socket = INVALID_SOCKET;
    g_connected = false;
    if (g_winsock_started) {
        g_winsock_started = false;
        WSACleanup();
    }
}

Role GetRole() { return g_role; }
bool IsConnected() { return g_connected; }

bool GetPeerVitals(PeerVitals* out) {
    if (!g_peer_state_valid || !out) return false;
    *out = g_peer_vitals;
    return true;
}

bool ConsumeConnectedEvent() {
    const bool fired = g_connected_event;
    g_connected_event = false;
    return fired;
}

bool ConsumeDisconnectedEvent() {
    const bool fired = g_disconnected_event;
    g_disconnected_event = false;
    return fired;
}

void SendOrderEvent(std::uint32_t actor_hash, std::uint32_t order_type,
                    std::int32_t attack_index, std::int32_t attack_depth) {
    if (!g_connected) return;
    OrderEventPacket packet = {};
    FillHeader(&packet.header, PacketType::OrderEvent);
    packet.actor_hash = actor_hash;
    packet.order_type = order_type;
    packet.attack_index = attack_index;
    packet.attack_depth = attack_depth;
    // Sent immediately, not at snapshot rate: a missed attack is far more
    // visible than a missed position update.
    SendPacket(&packet, sizeof(packet));
}

bool PopOrderEvent(std::uint32_t* actor_hash, std::uint32_t* order_type,
                   std::int32_t* attack_index, std::int32_t* attack_depth) {
    if (g_order_read == g_order_write) return false;
    const QueuedOrder& order = g_order_queue[g_order_read];
    g_order_read = (g_order_read + 1) % kOrderQueueSize;
    *actor_hash = order.actor_hash;
    *order_type = order.order_type;
    *attack_index = order.attack_index;
    *attack_depth = order.attack_depth;
    return true;
}

void SendLevelSync(const char* level_path) {
    if (!g_connected || !level_path || !level_path[0]) return;
    LevelSyncPacket packet = {};
    FillHeader(&packet.header, PacketType::LevelSync);
    packet.request_id = g_level_request_next;
    lstrcpynA(packet.level_path, level_path, sizeof(packet.level_path));
    SendPacket(&packet, sizeof(packet));
}

void BumpLevelRequest() { ++g_level_request_next; }

void SendLevelPresence(const char* level_path) {
    if (!g_connected || !level_path || !level_path[0]) return;
    LevelSyncPacket packet = {};
    FillHeader(&packet.header, PacketType::LevelSync);
    packet.request_id = 0;  // 0 = presence, never acted upon
    lstrcpynA(packet.level_path, level_path, sizeof(packet.level_path));
    SendPacket(&packet, sizeof(packet));
}

bool PopLevelSync(char* out_level_path, int out_size) {
    if (!g_have_pending_level) return false;
    g_have_pending_level = false;
    lstrcpynA(out_level_path, g_pending_level, out_size);
    return true;
}

const char* GetPeerLevel() { return g_peer_level; }

void SendEnemyStates(const EnemyStateOut* entries, int count) {
    if (!g_connected) return;
    if (count < 0) count = 0;
    if (count > kMaxTrackedEnemies) count = kMaxTrackedEnemies;
    if (count > 0 && !entries) return;

    static std::uint32_t generation = 0;
    ++generation;

    // An empty sweep is still a sweep, and it has to be sent: it is how the
    // client learns that a fight is over. Without it the client would keep
    // driving the last set it heard about long after those enemies were
    // returned to the pool.
    const int chunks =
        count == 0 ? 1 : (count + kMaxEnemiesPerPacket - 1) / kMaxEnemiesPerPacket;
    for (int chunk = 0; chunk < chunks; ++chunk) {
        EnemyStatePacket packet = {};
        FillHeader(&packet.header, PacketType::EnemyState);
        packet.generation = generation;
        packet.chunk = static_cast<std::uint8_t>(chunk);
        packet.chunk_count = static_cast<std::uint8_t>(chunks);

        const int first = chunk * kMaxEnemiesPerPacket;
        const int remaining = count - first;
        const int here = remaining < kMaxEnemiesPerPacket ? remaining : kMaxEnemiesPerPacket;
        packet.count = static_cast<std::uint8_t>(here);

        for (int i = 0; i < here; ++i) {
            const EnemyStateOut& in = entries[first + i];
            EnemyEntry& out = packet.entries[i];
            out.name_hash = in.name_hash;
            out.x = in.x;
            out.y = in.y;
            out.z = in.z;
            out.yaw = in.yaw;
            out.health = in.health;
            out.max_health = in.max_health;
            out.guard = in.guard;
            out.damage_applied = in.damage_applied;
            out.flags = in.flags;
        }
        SendPacket(&packet, sizeof(packet));
    }
}

int GetEnemyStates(EnemyStateOut* out, int max_out) {
    if (!out || max_out <= 0) return 0;
    const int count = g_enemy_live_count < max_out ? g_enemy_live_count : max_out;
    for (int i = 0; i < count; ++i) out[i] = g_enemies_live[i];
    return count;
}

bool HasEnemySweep() { return g_enemy_sweep_seen; }

void SendEnemyDamage(const DamageReport* entries, int count) {
    if (!g_connected || !entries || count <= 0) return;

    // Deliberately not chunked. The receiver replaces its whole table from one
    // packet, so a split report would have each half erase the other. The cap
    // is far above the number of enemies one player can be mid-fight with, and
    // the caller reports the most recently damaged first, but a silent
    // truncation would still be a hit that never lands -- so say so.
    if (count > kMaxDamagePerPacket) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SC_LOG("net: %d damaged enemies exceeds the %d that fit one report -- "
                   "the oldest are being dropped",
                   count, kMaxDamagePerPacket);
        }
        count = kMaxDamagePerPacket;
    }

    EnemyDamagePacket packet = {};
    FillHeader(&packet.header, PacketType::EnemyDamage);
    packet.count = static_cast<std::uint32_t>(count);
    for (int i = 0; i < count; ++i) {
        packet.entries[i].name_hash = entries[i].name_hash;
        packet.entries[i].total = entries[i].total;
    }
    SendPacket(&packet, sizeof(packet));
}

int GetEnemyDamage(DamageReport* out, int max_out) {
    if (!out || max_out <= 0) return 0;
    const int count = g_damage_count < max_out ? g_damage_count : max_out;
    for (int i = 0; i < count; ++i) out[i] = g_damage[i];
    return count;
}

void SendMontageState(const char* montage_path, float position) {
    if (!g_connected || !montage_path || !montage_path[0]) return;
    MontagePacket packet = {};
    FillHeader(&packet.header, PacketType::MontageState);
    packet.position = position;
    lstrcpynA(packet.montage_path, montage_path, sizeof(packet.montage_path));
    SendPacket(&packet, sizeof(packet));
}

bool PopMontageState(char* out_path, int out_size, float* out_position) {
    if (!g_have_montage || !out_path || out_size <= 0) return false;
    g_have_montage = false;
    lstrcpynA(out_path, g_montage_path, out_size);
    if (out_position) *out_position = g_montage_position;
    return true;
}

void SendRunState(const RunSnapshot& state) {
    if (!g_connected) return;
    RunStatePacket packet = {};
    FillHeader(&packet.header, PacketType::RunState);
    packet.age = state.age;
    packet.room_clear_percent = state.room_clear_percent;
    packet.flags = 0;
    if (state.age_valid) packet.flags |= kRunAgeValid;
    if (state.room_clear_valid && IsFinite(state.room_clear_percent) &&
        state.room_clear_percent >= 0.f) {
        packet.flags |= kRunRoomClearValid;
    }
    if (state.has_weapon && state.weapon_path[0]) packet.flags |= kRunHasWeapon;
    lstrcpynA(packet.weapon_path, state.weapon_path, sizeof(packet.weapon_path));
    SendPacket(&packet, sizeof(packet));
}

bool GetPeerRunState(RunSnapshot* out) {
    if (!out || !g_peer_run_valid) return false;
    *out = g_peer_run;
    return true;
}

void DiscoverPublicAddress() {
    if (g_socket == INVALID_SOCKET) {
        coop::ReportProblem("connect or host first -- discovery uses the game's own socket");
        return;
    }

    char ini_path[MAX_PATH] = {};
    coop::IniPath(ini_path, sizeof(ini_path));
    char server[128] = {};
    GetPrivateProfileStringA("net", "stun_server", "stun.l.google.com", server, sizeof(server),
                             ini_path);
    const int stun_port = GetPrivateProfileIntA("net", "stun_port", 19302, ini_path);

    // One blocking resolve, on a button press, once per session. Doing it
    // asynchronously would mean a thread and a lifetime problem for something
    // that costs a few milliseconds when the user has explicitly asked.
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    char port_text[16] = {};
    _snprintf(port_text, sizeof(port_text), "%d", stun_port);

    if (getaddrinfo(server, port_text, &hints, &results) != 0 || !results) {
        SC_LOG("net: could not resolve STUN server '%s'", server);
        coop::ReportProblem("could not reach the STUN server '%s'", server);
        return;
    }

    std::uint8_t request[20] = {};
    request[0] = 0x00;
    request[1] = 0x01;  // Binding Request
    request[2] = 0x00;
    request[3] = 0x00;  // no attributes
    request[4] = 0x21;
    request[5] = 0x12;
    request[6] = 0xA4;
    request[7] = 0x42;  // magic cookie

    for (int i = 0; i < 12; i += 4) {
        unsigned int value = 0;
        if (rand_s(&value) != 0) value = static_cast<unsigned int>(NowMs()) + i;
        memcpy(g_stun_transaction + i, &value, 4);
    }
    memcpy(request + 8, g_stun_transaction, sizeof(g_stun_transaction));

    sendto(g_socket, reinterpret_cast<const char*>(request), sizeof(request), 0,
           results->ai_addr, static_cast<int>(results->ai_addrlen));
    freeaddrinfo(results);

    g_stun_pending = true;
    g_stun_sent_ms = NowMs();
    g_public_address[0] = '\0';
    SC_LOG("net: asked %s:%d what we look like from outside", server, stun_port);
}

const char* GetPublicAddress() { return g_public_address; }

int GetRoundTripMs() { return g_rtt_ms; }

int GetInterpolationDelayMs() {
    const coop::Config& config = coop::Get();
    if (!config.adaptive_interp || g_rtt_ms < 0) return config.interp_delay_ms;

    // Half the round trip is the one-way latency; the jitter margin is what
    // stops an occasional late packet from becoming a visible stutter. Two
    // snapshot intervals on top covers the sampling grid itself.
    int delay = g_rtt_ms / 2 + g_rtt_jitter_ms * 2 + 2000 / kSnapshotHz;
    // Floor raised from 30 to 50: on localhost RTT is ~0, so the formula lands
    // near one snapshot interval, leaving only a single sample of buffer. One
    // late packet then empties it and the puppet stutters. 50 ms holds roughly
    // three snapshots at 60 Hz -- enough slack to ride out ordinary jitter while
    // staying well under the perceptible-lag threshold.
    if (delay < 50) delay = 50;
    if (delay > 250) delay = 250;
    return delay;
}

void TickSession(const LocalState& local) {
    if (g_socket == INVALID_SOCKET) return;

    PumpReceive();

    const DWORD now = NowMs();
    UpdateRates(now);

    if (g_connected && g_last_recv_ms != 0 && now - g_last_recv_ms > kTimeoutMs) {
        SC_LOG("net: peer timed out after %lums", now - g_last_recv_ms);
        coop::ReportProblem("peer timed out (no packets for %lums)", now - g_last_recv_ms);
        g_disconnected_event = true;
        g_connected = false;
    }

    if (g_stun_pending && now - g_stun_sent_ms > 4000) {
        g_stun_pending = false;
        SC_LOG("net: no STUN reply -- the server may be unreachable from here");
        coop::ReportProblem("STUN server did not answer");
    }

    // Hole punching. While waiting for a peer, the host keeps poking the
    // address it was given so its router has an outgoing flow on record; the
    // joiner's first packet then looks like a reply and is let through. Both
    // sides must be doing this at roughly the same time for it to work, which
    // is why it runs on a timer rather than once.
    if (!g_connected && g_have_punch_addr && now - g_last_punch_ms > 500) {
        g_last_punch_ms = now;
        PingPacket punch = {};
        FillHeader(&punch.header, PacketType::Ping);
        punch.probe_time_ms = now;
        const bool had_session = g_have_session_key;
        g_have_session_key = false;  // no session yet; sign with the base key
        SendPacketTo(&punch, sizeof(punch), g_punch_addr);
        g_have_session_key = had_session;
    }

    // Clients keep saying hello until accepted. Signed with the base key: the
    // session key does not exist until the host has seen this nonce.
    if (!g_connected && g_role == Role::Client && now - g_last_send_ms > 500) {
        g_last_send_ms = now;
        HelloPacket hello = {};
        FillHeader(&hello.header, PacketType::Hello);
        lstrcpynA(hello.name, "sifu-peer", sizeof(hello.name));
        memcpy(hello.nonce, g_local_nonce, kSessionNonceSize);
        g_have_session_key = false;
        SendPacket(&hello, sizeof(hello));
        return;
    }

    if (!g_connected) return;

    if (now - g_last_ping_ms >= kPingIntervalMs) {
        g_last_ping_ms = now;
        PingPacket ping = {};
        FillHeader(&ping.header, PacketType::Ping);
        ping.probe_time_ms = now;
        SendPacket(&ping, sizeof(ping));
    }

    int hz = coop::Get().snapshot_hz;
    if (hz < 10) hz = 10;
    if (now - g_last_send_ms < static_cast<DWORD>(1000 / hz)) return;
    g_last_send_ms = now;

    SnapshotPacket snapshot = {};
    FillHeader(&snapshot.header, PacketType::Snapshot);
    snapshot.x = local.location.X;
    snapshot.y = local.location.Y;
    snapshot.z = local.location.Z;
    snapshot.pitch = local.rotation.Pitch;
    snapshot.yaw = local.rotation.Yaw;
    snapshot.roll = local.rotation.Roll;
    snapshot.health = local.health;
    snapshot.max_health = local.max_health;
    snapshot.guard = local.guard;
    snapshot.flags = 0;
    if (local.state_valid) {
        snapshot.flags |= kFlagStateValid;
        if (local.is_down) snapshot.flags |= kFlagIsDown;
    }
    if (local.in_level) snapshot.flags |= kFlagInLevel;
    SendPacket(&snapshot, sizeof(snapshot));
}

bool GetPeerTransform(ue::FVector* location, ue::FRotator* rotation) {
    const DWORD target = NowMs() - static_cast<DWORD>(GetInterpolationDelayMs());

    // Find the two samples bracketing the render time and blend between them.
    const PeerState* older = nullptr;
    const PeerState* newer = nullptr;
    for (int i = 0; i < kBufferSize; ++i) {
        const PeerState& state = g_peer_buffer[i];
        if (!state.valid) continue;
        if (state.received_ms <= target && (!older || state.received_ms > older->received_ms)) {
            older = &state;
        }
        if (state.received_ms > target && (!newer || state.received_ms < newer->received_ms)) {
            newer = &state;
        }
    }

    if (!older && !newer) return false;
    if (!older) {
        *location = newer->location;
        *rotation = newer->rotation;
        return true;
    }
    if (!newer) {
        *location = older->location;
        *rotation = older->rotation;
        return true;
    }

    const DWORD span = newer->received_ms - older->received_ms;
    const float alpha = span == 0 ? 0.f : static_cast<float>(target - older->received_ms) / span;

    location->X = older->location.X + (newer->location.X - older->location.X) * alpha;
    location->Y = older->location.Y + (newer->location.Y - older->location.Y) * alpha;
    location->Z = older->location.Z + (newer->location.Z - older->location.Z) * alpha;

    // Yaw is interpolated the short way round so a 359->1 degree step does not
    // spin the puppet almost all the way back the other direction.
    float delta_yaw = newer->rotation.Yaw - older->rotation.Yaw;
    while (delta_yaw > 180.f) delta_yaw -= 360.f;
    while (delta_yaw < -180.f) delta_yaw += 360.f;

    rotation->Pitch = older->rotation.Pitch;
    rotation->Yaw = older->rotation.Yaw + delta_yaw * alpha;
    rotation->Roll = older->rotation.Roll;
    return true;
}

}  // namespace sifucoop::net


