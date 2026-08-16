#include "session.h"



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
#include "instance_guard.h"
#include "protocol.h"

namespace sifucoop::net {
namespace {

namespace coop = sifucoop::coop;




SOCKET g_socket = INVALID_SOCKET;
Role g_role = Role::Offline;
bool g_connected = false;
bool g_winsock_started = false;
HANDLE g_host_instance_mutex = nullptr;
int g_host_instance_port = 0;
char g_start_failure[192] = {};

sockaddr_in g_peer_addr = {};
bool g_have_peer_addr = false;

std::uint32_t g_send_sequence = 0;
std::uint32_t g_last_snapshot_sequence = 0;
DWORD g_last_recv_ms = 0;
// When either machine last did something level-shaped: sent an invite, received
// one, or reported a different level. A machine loading a map does not send for
// several seconds and cannot say so, which is exactly when the ordinary timeout
// must not fire.
DWORD g_level_activity_ms = 0;
DWORD g_last_send_ms = 0;
DWORD g_last_ping_ms = 0;
DWORD g_last_hello_ms = 0;




constexpr int kBufferSize = 64;









constexpr DWORD kTimeoutMs = 12000;



constexpr DWORD kStallMs = 500;
constexpr DWORD kPingIntervalMs = 500;





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


    DWORD sample_ms = 0;
    ue::FVector location;
    ue::FRotator rotation;
    ue::FVector velocity;
    bool valid = false;
};























ue::FVector g_last_output_location;
ue::FRotator g_last_output_rotation;
ue::FVector g_last_output_velocity;
bool g_have_last_output = false;

std::int64_t g_clock_offset = 0;
bool g_clock_offset_valid = false;




















constexpr DWORD kClockWindowMs = 2000;
std::int64_t g_clock_window_min = 0;
bool g_clock_window_valid = false;
DWORD g_clock_window_start = 0;

PeerState g_peer_buffer[kBufferSize];
int g_peer_head = 0;

bool g_peer_state_valid = false;
PeerVitals g_peer_vitals;

RunSnapshot g_peer_run;
bool g_peer_run_valid = false;
CheatSnapshot g_host_cheats;
bool g_host_cheats_valid = false;

bool g_connected_event = false;
bool g_disconnected_event = false;

int g_rtt_ms = -1;
int g_rtt_jitter_ms = 0;



std::uint32_t g_bytes_in = 0;
std::uint32_t g_bytes_out = 0;
DWORD g_rate_window_start = 0;




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
std::uint32_t g_last_order_sequence = 0;







EnemyStateOut g_enemies_live[kMaxTrackedEnemies];
int g_enemy_live_count = 0;
bool g_enemy_sweep_seen = false;




DWORD g_enemy_sweep_at = 0;

EnemyStateOut g_enemies_staging[kMaxTrackedEnemies];
int g_enemy_staging_count = 0;
std::uint32_t g_enemy_staging_generation = 0;
std::uint32_t g_enemy_chunks_seen = 0;
std::uint8_t g_enemy_chunk_total = 0;
void MergeEnemyLive(const EnemyStateOut& incoming) {
    for (int i = 0; i < g_enemy_live_count; ++i) {
        if (g_enemies_live[i].name_hash != incoming.name_hash) continue;
        g_enemies_live[i] = incoming;
        return;
    }
    if (g_enemy_live_count < kMaxTrackedEnemies) {
        g_enemies_live[g_enemy_live_count++] = incoming;
    }
}

DamageReport g_damage[kMaxDamagePerPacket];
int g_damage_count = 0;
std::uint32_t g_damage_sequence = 0;






















constexpr std::uint32_t kStunCookie = 0x2112A442;
constexpr std::uint16_t kStunBindingRequest = 0x0001;
constexpr std::uint16_t kStunBindingResponse = 0x0101;
constexpr std::uint16_t kStunXorMappedAddress = 0x0020;
constexpr std::uint16_t kStunMappedAddress = 0x0001;

std::uint8_t g_stun_transaction[12] = {};
bool g_stun_pending = false;
DWORD g_stun_sent_ms = 0;
char g_public_address[64] = {};



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
            data[value + 1] == 0x01) {
            std::uint16_t port =
                static_cast<std::uint16_t>((data[value + 2] << 8) | data[value + 3]);
            std::uint32_t address = (static_cast<std::uint32_t>(data[value + 4]) << 24) |
                                    (static_cast<std::uint32_t>(data[value + 5]) << 16) |
                                    (static_cast<std::uint32_t>(data[value + 6]) << 8) |
                                    data[value + 7];
            if (attr == kStunXorMappedAddress) {


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

        offset = value + ((length + 3) & ~3);
    }
}





std::uint8_t g_base_key[kSha256Size] = {};

std::uint8_t g_session_key[kSha256Size] = {};
bool g_have_session_key = false;

std::uint8_t g_local_nonce[kSessionNonceSize] = {};
std::uint8_t g_remote_nonce[kSessionNonceSize] = {};

std::uint32_t g_rejected_packets = 0;
DWORD g_last_reject_log = 0;

void DeriveBaseKey(const char* passphrase) {




    char material[192] = {};
    _snprintf(material, sizeof(material) - 1, "SifuCoop-v%u-key:%s", kProtocolVersion,
              passphrase ? passphrase : "");
    Sha256(material, strlen(material), g_base_key);
    g_have_session_key = false;
}



void DeriveSessionKey(const std::uint8_t* host_nonce, const std::uint8_t* joiner_nonce) {
    std::uint8_t material[kSha256Size + kSessionNonceSize * 2];
    memcpy(material, g_base_key, kSha256Size);
    memcpy(material + kSha256Size, host_nonce, kSessionNonceSize);
    memcpy(material + kSha256Size + kSessionNonceSize, joiner_nonce, kSessionNonceSize);
    Sha256(material, sizeof(material), g_session_key);
    g_have_session_key = true;
}




void MakeNonce(std::uint8_t out[kSessionNonceSize]) {


    for (int i = 0; i < kSessionNonceSize; i += 4) {
        unsigned int value = 0;
        if (rand_s(&value) != 0) {



            value = 0;
        }
        memcpy(out + i, &value, 4);
    }
}



void SignPacket(void* packet, int size) {
    auto* header = static_cast<PacketHeader*>(packet);
    memset(header->tag, 0, kAuthTagSize);

    const std::uint8_t* key = g_have_session_key ? g_session_key : g_base_key;
    std::uint8_t digest[kSha256Size];
    HmacSha256(key, kSha256Size, packet, static_cast<std::size_t>(size), digest);
    memcpy(header->tag, digest, kAuthTagSize);
}



bool VerifyPacket(void* buffer, int size, bool handshake) {
    if (size < static_cast<int>(sizeof(PacketHeader))) return false;

    auto* header = static_cast<PacketHeader*>(buffer);
    std::uint8_t received[kAuthTagSize];
    memcpy(received, header->tag, kAuthTagSize);
    memset(header->tag, 0, kAuthTagSize);



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


    if (now - g_last_reject_log < 5000 && g_last_reject_log != 0) return;
    g_last_reject_log = now;

    char ip[64] = {};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    SC_LOG("net: rejected an unauthenticated packet from %s:%d (%u so far) -- wrong "
           "passphrase, or someone else is probing the port",
           ip, ntohs(from.sin_port), g_rejected_packets);
    coop::ReportProblem("rejected %u packets that failed authentication", g_rejected_packets);
}







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


    return strstr(path, "..") == nullptr;
}

bool IsFinite(float value) {



    return value == value && value > -1e9f && value < 1e9f;
}

bool IsFiniteVector(float x, float y, float z) {
    return IsFinite(x) && IsFinite(y) && IsFinite(z);
}

void ReleaseHostInstanceGuard() {
    if (!g_host_instance_mutex) return;
    SC_LOG("net: released host instance guard for UDP port %d", g_host_instance_port);
    CloseHandle(g_host_instance_mutex);
    g_host_instance_mutex = nullptr;
    g_host_instance_port = 0;
}

bool AcquireHostInstanceGuard(int port) {
    if (g_host_instance_mutex && g_host_instance_port == port) return true;
    ReleaseHostInstanceGuard();

    char name[96] = {};
    _snprintf(name, sizeof(name), kHostPortMutexFormatA, port);
    SetLastError(ERROR_SUCCESS);
    HANDLE mutex = CreateMutexA(nullptr, FALSE, name);
    const DWORD error = GetLastError();
    if (!mutex) {
        _snprintf(g_start_failure, sizeof(g_start_failure),
                  "Could not create the SifuCoop host guard (Windows error %lu).", error);
        SC_LOG("net: host instance guard creation failed (%lu)", error);
        coop::ReportProblem("could not create host instance guard (%lu)", error);
        return false;
    }
    if (error == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        _snprintf(g_start_failure, sizeof(g_start_failure),
                  "Another SifuCoop host is already using UDP port %d. Close the other "
                  "Sifu process, or choose a different port.",
                  port);
        SC_LOG("net: refusing duplicate host -- instance guard already exists for port %d",
               port);
        coop::ReportProblem("another SifuCoop host already owns UDP %d", port);
        return false;
    }

    g_host_instance_mutex = mutex;
    g_host_instance_port = port;
    SC_LOG("net: acquired host instance guard for UDP port %d", port);
    return true;
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




        SC_LOG("net: no passphrase set -- anyone who can reach this port can join. "
               "Fine over a VPN; set one before forwarding a port.");
        coop::ReportProblem("no passphrase set -- only safe on a private network");
    }

    if (off) g_role = Role::Offline;
    else g_role = *is_host ? Role::Host : Role::Client;
}





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



        const bool likely_vpn = (first == 10) || (first == 172 && second >= 16 && second <= 31);
        SC_LOG("net:   %s:%d%s", ip, port, likely_vpn ? "   <-- likely ZeroTier/VPN" : "");
    }
    freeaddrinfo(results);
}



void SendPacket(void* data, int size) {
    if (!g_have_peer_addr || g_socket == INVALID_SOCKET) return;
    SignPacket(data, size);
    sendto(g_socket, static_cast<const char*>(data), size, 0,
           reinterpret_cast<sockaddr*>(&g_peer_addr), sizeof(g_peer_addr));
    g_bytes_out += static_cast<std::uint32_t>(size);
    ++coop::GetStats().packets_sent;
}



void SendPacketTo(void* data, int size, const sockaddr_in& to) {
    if (g_socket == INVALID_SOCKET) return;
    SignPacket(data, size);
    sendto(g_socket, static_cast<const char*>(data), size, 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    g_bytes_out += static_cast<std::uint32_t>(size);
    ++coop::GetStats().packets_sent;
}










constexpr int kPacketTypeCount = 16;
std::uint32_t g_send_sequence_by_type[kPacketTypeCount] = {};

void FillHeader(PacketHeader* header, PacketType type) {
    header->magic = kMagic;
    header->version = kProtocolVersion;
    header->type = static_cast<std::uint16_t>(type);
    const int slot = static_cast<int>(type);
    header->sequence = (slot >= 0 && slot < kPacketTypeCount) ? ++g_send_sequence_by_type[slot]
                                                              : ++g_send_sequence;
    header->send_time_ms = NowMs();
}





struct PendingAnimation {
    char path[192] = {};
    float position = 0.f;
    std::uint32_t actor_hash = 0;
    AnimationAssetKind kind = AnimationAssetKind::Montage;
    AnimationSemantic semantic = AnimationSemantic::Generic;
};
constexpr int kAnimationQueueSize = 32;
PendingAnimation g_animation_queue[kAnimationQueueSize] = {};
int g_animation_read = 0;
int g_animation_write = 0;
std::uint32_t g_last_animation_sequence = 0;







OwnedEnemyEntry g_owned_enemies[kMaxOwnedEnemiesPerPacket];
int g_owned_enemy_count = 0;
DWORD g_owned_enemies_at = 0;

void HandleOwnedEnemies(const OwnedEnemyPacket& packet) {
    const int count = static_cast<int>(packet.count);
    if (count < 0 || count > kMaxOwnedEnemiesPerPacket) return;
    for (int i = 0; i < count; ++i) {
        const OwnedEnemyEntry& in = packet.entries[i];
        if (!IsFiniteVector(in.x, in.y, in.z) || !IsFinite(in.yaw)) return;
        if (!IsFiniteVector(in.velocity_x, in.velocity_y, in.velocity_z)) return;
    }
    for (int i = 0; i < count; ++i) g_owned_enemies[i] = packet.entries[i];
    g_owned_enemy_count = count;
    g_owned_enemies_at = NowMs();
}

void ResetLevelSyncState();
void ResetPeerState() {
    g_animation_read = g_animation_write = 0;
    g_last_animation_sequence = 0;
    ResetLevelSyncState();
    for (PeerState& state : g_peer_buffer) state.valid = false;
    g_peer_head = 0;
    g_last_snapshot_sequence = 0;
    g_order_read = g_order_write;
    g_last_order_sequence = 0;
    g_peer_state_valid = false;
    g_peer_vitals = PeerVitals();
    g_peer_run_valid = false;
    g_peer_run = RunSnapshot();
    g_host_cheats_valid = false;
    g_host_cheats = CheatSnapshot();

    g_clock_offset_valid = false;
    g_clock_offset = 0;
    g_clock_window_valid = false;
    g_clock_window_min = 0;
    g_clock_window_start = 0;
    g_have_last_output = false;
    g_enemy_live_count = 0;
    g_enemy_staging_count = 0;
    g_enemy_staging_generation = 0;
    g_enemy_chunks_seen = 0;
    g_enemy_chunk_total = 0;
    g_enemy_sweep_seen = false;
    g_enemy_sweep_at = 0;
    g_damage_count = 0;
    g_damage_sequence = 0;
    g_owned_enemy_count = 0;
    g_rtt_ms = -1;
    g_rtt_jitter_ms = 0;
}

void HandleSnapshot(const SnapshotPacket& packet) {



    if (!IsFiniteVector(packet.x, packet.y, packet.z)) return;
    if (!IsFiniteVector(packet.pitch, packet.yaw, packet.roll)) return;
    if (!IsFiniteVector(packet.velocity_x, packet.velocity_y, packet.velocity_z)) return;
    if (!IsFinite(packet.health) || !IsFinite(packet.max_health) || !IsFinite(packet.guard)) {
        return;
    }




    if (g_last_snapshot_sequence != 0) {
        if (packet.header.sequence <= g_last_snapshot_sequence) return;
        const std::uint32_t gap = packet.header.sequence - g_last_snapshot_sequence;
        if (gap > 1) coop::GetStats().packets_dropped += gap - 1;
    }
    g_last_snapshot_sequence = packet.header.sequence;




    ++coop::GetStats().snapshots_received;

    g_peer_head = (g_peer_head + 1) % kBufferSize;
    PeerState& state = g_peer_buffer[g_peer_head];
    state.received_ms = NowMs();



    const std::int64_t observed =
        static_cast<std::int64_t>(state.received_ms) -
        static_cast<std::int64_t>(packet.header.send_time_ms);
    if (!g_clock_offset_valid) {
        g_clock_offset = observed;
        g_clock_offset_valid = true;
        g_clock_window_min = observed;
        g_clock_window_valid = true;
        g_clock_window_start = state.received_ms;
    } else {
        if (!g_clock_window_valid || observed < g_clock_window_min) {
            g_clock_window_min = observed;
            g_clock_window_valid = true;
        }

        if (observed < g_clock_offset) g_clock_offset = observed;




        if (state.received_ms - g_clock_window_start >= kClockWindowMs) {
            g_clock_window_start = state.received_ms;
            if (g_clock_window_min > g_clock_offset) {
                const std::int64_t step = g_clock_window_min - g_clock_offset;



                g_clock_offset += step;
            }
            g_clock_window_valid = false;
        }
    }
    state.sample_ms =
        static_cast<DWORD>(static_cast<std::int64_t>(packet.header.send_time_ms) +
                           g_clock_offset);
    state.location = {packet.x, packet.y, packet.z};
    state.rotation = {packet.pitch, packet.yaw, packet.roll};
    state.velocity = {packet.velocity_x, packet.velocity_y, packet.velocity_z};
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



std::uint32_t g_level_request_seen = 0;
std::uint32_t g_level_request_next = 1;
char g_pending_level[192] = {};
bool g_have_pending_level = false;
char g_peer_level[192] = {};
constexpr DWORD kLevelRetryIntervalMs = 250;
constexpr DWORD kLevelRetryTimeoutMs = 20000;

bool g_invite_reply_pending = false;
bool g_invite_reply_accepted = false;

char g_active_level_request[192] = {};
std::uint32_t g_active_level_request_id = 0;
DWORD g_active_level_first_sent = 0;
DWORD g_active_level_last_sent = 0;

void ResetLevelSyncState() {
    g_invite_reply_pending = false;
    g_invite_reply_accepted = false;
    g_level_request_seen = 0;
    g_level_request_next = 1;
    g_pending_level[0] = '\0';
    g_have_pending_level = false;
    g_peer_level[0] = '\0';
    g_active_level_request[0] = '\0';
    g_active_level_request_id = 0;
    g_active_level_first_sent = 0;
    g_active_level_last_sent = 0;
}

void HandleInviteReply(const InviteReplyPacket& packet) {



    if (packet.request_id == 0 || packet.request_id != g_active_level_request_id) return;
    g_invite_reply_pending = true;
    g_invite_reply_accepted = packet.accepted != 0;
    SC_LOG("net: your partner %s the invitation",
           g_invite_reply_accepted ? "ACCEPTED" : "DECLINED");
}

void HandleLevelSync(const LevelSyncPacket& packet) {




    if (!LooksLikeLevelPath(packet.level_path)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SC_LOG("net: refusing a level path that is not a /Game/ package");
            coop::ReportProblem("peer sent an unusable level path");
        }
        return;
    }


    const bool level_changed = _stricmp(g_peer_level, packet.level_path) != 0;
    lstrcpynA(g_peer_level, packet.level_path, sizeof(g_peer_level));
    // Level-shaped traffic: see the timeout grace in TickSession.
    if (level_changed || packet.request_id != 0) g_level_activity_ms = NowMs();

    if (packet.request_id == 0 || packet.request_id == g_level_request_seen) return;
    g_level_request_seen = packet.request_id;

    lstrcpynA(g_pending_level, packet.level_path, sizeof(g_pending_level));
    g_have_pending_level = true;
    SC_LOG("net: host invited us to '%s'", g_pending_level);
}

void HandleEnemyState(const EnemyStatePacket& packet) {
    if (packet.chunk_count == 0 || packet.chunk_count > 32 ||
        packet.chunk >= packet.chunk_count) return;

    if (g_enemy_staging_generation != 0) {
        const std::int32_t generation_delta =
            static_cast<std::int32_t>(packet.generation - g_enemy_staging_generation);
        if (generation_delta < 0) return;
    }

    const std::uint32_t chunk_bit = 1u << packet.chunk;
    if (packet.generation == g_enemy_staging_generation &&
        g_enemy_chunk_total != 0 && packet.chunk_count != g_enemy_chunk_total) return;

    if (packet.generation == g_enemy_staging_generation &&
        (g_enemy_chunks_seen & chunk_bit) != 0) return;

    int count = packet.count;
    if (count < 0) count = 0;
    if (count > kMaxEnemiesPerPacket) count = kMaxEnemiesPerPacket;




    if (packet.generation != g_enemy_staging_generation) {
        g_enemy_staging_generation = packet.generation;
        g_enemy_staging_count = 0;
        g_enemy_chunks_seen = 0;
        g_enemy_chunk_total = packet.chunk_count;
    }

    for (int i = 0; i < count && g_enemy_staging_count < kMaxTrackedEnemies; ++i) {
        const EnemyEntry& in = packet.entries[i];



        if (!IsFiniteVector(in.x, in.y, in.z) || !IsFinite(in.yaw) ||
            !IsFiniteVector(in.velocity_x, in.velocity_y, in.velocity_z)) continue;
        if (!IsFinite(in.health) || !IsFinite(in.max_health) || !IsFinite(in.guard) ||
            !IsFinite(in.damage_applied) || !IsFinite(in.guard_damage_applied) ||
            !IsFinite(in.time_dilation)) {
            continue;
        }
        EnemyStateOut& out = g_enemies_staging[g_enemy_staging_count++];
        out.name_hash = in.name_hash;
        out.source_hash = in.source_hash;
        out.x = in.x;
        out.y = in.y;
        out.z = in.z;
        out.yaw = in.yaw;
        out.velocity_x = in.velocity_x;
        out.velocity_y = in.velocity_y;
        out.velocity_z = in.velocity_z;
        out.health = in.health;
        out.max_health = in.max_health;
        out.guard = in.guard;
        out.damage_applied = in.damage_applied;
        out.guard_damage_applied = in.guard_damage_applied;



        out.time_dilation = in.time_dilation;
        if (!(out.time_dilation > 0.01f) || out.time_dilation > 4.f) {
            out.time_dilation = 1.f;
        }
        out.flags = in.flags;
        if (g_enemy_sweep_seen) MergeEnemyLive(out);
    }

    g_enemy_chunks_seen |= chunk_bit;

    const std::uint32_t wanted =
        packet.chunk_count >= 32 ? 0xFFFFFFFFu : (1u << packet.chunk_count) - 1u;
    if ((g_enemy_chunks_seen & wanted) != wanted) return;



    for (int i = 0; i < g_enemy_staging_count; ++i) g_enemies_live[i] = g_enemies_staging[i];
    g_enemy_live_count = g_enemy_staging_count;
    g_enemy_staging_count = 0;
    g_enemy_sweep_seen = true;
    g_enemy_sweep_at = NowMs();
}










void HandleEnemyDamage(const EnemyDamagePacket& packet) {
    if (g_damage_sequence != 0 && packet.header.sequence <= g_damage_sequence) return;
    g_damage_sequence = packet.header.sequence;

    int count = static_cast<int>(packet.count);
    if (count < 0) count = 0;
    if (count > kMaxDamagePerPacket) count = kMaxDamagePerPacket;

    int kept = 0;
    for (int i = 0; i < count; ++i) {




        const float total = packet.entries[i].total;
        const float guard_total = packet.entries[i].guard_total;
        if (!IsFinite(total) || total < 0.f || total > 1e6f ||
            !IsFinite(guard_total) || guard_total < 0.f || guard_total > 1e6f) continue;
        g_damage[kept].name_hash = packet.entries[i].name_hash;
        g_damage[kept].total = total;
        g_damage[kept].guard_total = guard_total;
        ++kept;
    }
    g_damage_count = kept;
}

void HandlePing(const PingPacket& packet) {
    PingPacket pong = {};
    FillHeader(&pong.header, PacketType::Pong);
    pong.probe_time_ms = packet.probe_time_ms;
    SendPacket(&pong, sizeof(pong));
}

void HandlePong(const PingPacket& packet) {
    const DWORD now = NowMs();

    const int sample = static_cast<int>(now - packet.probe_time_ms);
    if (sample < 0 || sample > 5000) return;

    if (g_rtt_ms < 0) {
        g_rtt_ms = sample;
    } else {
        const int deviation = sample > g_rtt_ms ? sample - g_rtt_ms : g_rtt_ms - sample;


        g_rtt_jitter_ms = (g_rtt_jitter_ms * 3 + deviation) / 4;
        g_rtt_ms = (g_rtt_ms * 7 + sample) / 8;
    }

    coop::Stats& stats = coop::GetStats();
    stats.rtt_ms = g_rtt_ms;
    stats.rtt_jitter_ms = g_rtt_jitter_ms;
}

void HandleMontage(const MontagePacket& packet) {
    if (packet.kind > 1 ||
        packet.semantic > static_cast<std::uint8_t>(AnimationSemantic::Death)) {
        return;
    }






    if (g_last_animation_sequence != 0 &&
        packet.header.sequence <= g_last_animation_sequence) {
        return;
    }
    g_last_animation_sequence = packet.header.sequence;

    const int next = (g_animation_write + 1) % kAnimationQueueSize;
    if (next == g_animation_read) {
        g_animation_read = (g_animation_read + 1) % kAnimationQueueSize;
    }
    PendingAnimation& pending = g_animation_queue[g_animation_write];
    lstrcpynA(pending.path, packet.montage_path, sizeof(pending.path));
    pending.position = packet.position;
    pending.actor_hash = packet.actor_hash;
    pending.kind = static_cast<AnimationAssetKind>(packet.kind);
    pending.semantic = static_cast<AnimationSemantic>(packet.semantic);
    g_animation_write = next;
}

void HandleRunState(const RunStatePacket& packet) {

    g_peer_run = RunSnapshot();
    g_peer_run.age = packet.age;
    g_peer_run.age_valid = (packet.flags & kRunAgeValid) != 0;
    g_peer_run.has_weapon = (packet.flags & kRunHasWeapon) != 0;
    g_peer_run.outfit_valid = (packet.flags & kRunOutfitValid) != 0 && packet.outfit_index >= 0;
    g_peer_run.outfit_index = packet.outfit_index;
    lstrcpynA(g_peer_run.weapon_path, packet.weapon_path, sizeof(g_peer_run.weapon_path));
    g_peer_run_valid = true;
}

void HandleCheatState(const CheatStatePacket& packet) {


    if (g_role != Role::Client) return;
    std::memcpy(g_host_cheats.active, packet.active, sizeof(g_host_cheats.active));
    g_host_cheats_valid = true;
}
void QueueOrder(const OrderEventPacket& packet) {



    if (g_last_order_sequence != 0 &&
        packet.header.sequence <= g_last_order_sequence) {
        return;
    }
    g_last_order_sequence = packet.header.sequence;

    const int next = (g_order_write + 1) % kOrderQueueSize;
    if (next == g_order_read) {


        g_order_read = (g_order_read + 1) % kOrderQueueSize;
    }
    g_order_queue[g_order_write] = {packet.actor_hash, packet.order_type, packet.attack_index,
                                    packet.attack_depth};
    g_order_write = next;
}

void PumpReceive() {
    char buffer[kMaxPacketSize];
    sockaddr_in from = {};



    for (int i = 0; i < 64; ++i) {


        int from_size = sizeof(from);
        const int received = recvfrom(g_socket, buffer, sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_size);
        if (received <= 0) break;
        if (received < static_cast<int>(sizeof(PacketHeader))) continue;




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




        if (!VerifyPacket(buffer, received, handshake)) {
            NoteRejected(from);
            continue;
        }





        if (g_connected && !handshake) {
            if (from.sin_addr.s_addr != g_peer_addr.sin_addr.s_addr ||
                from.sin_port != g_peer_addr.sin_port) {
                continue;
            }
        }

        g_last_recv_ms = NowMs();
        g_bytes_in += static_cast<std::uint32_t>(received);
        ++coop::GetStats().packets_received;



        auto fits = [&](std::size_t size) { return received >= static_cast<int>(size); };

        switch (type) {
            case PacketType::Hello: {
                if (!fits(sizeof(HelloPacket))) break;
                HelloPacket hello = {};
                memcpy(&hello, buffer, sizeof(hello));

                const bool same_peer =
                    g_connected && from.sin_addr.s_addr == g_peer_addr.sin_addr.s_addr &&
                    from.sin_port == g_peer_addr.sin_port;




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


                DeriveSessionKey(g_local_nonce, g_remote_nonce);

                if (!g_connected) {
                    g_connected = true;
                    g_connected_event = true;


                    ResetPeerState();
                    char ip[64] = {};
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    SC_LOG("net: peer connected from %s:%d (authenticated)", ip,
                           ntohs(from.sin_port));
                }



                WelcomePacket welcome = {};
                FillHeader(&welcome.header, PacketType::Welcome);
                welcome.peer_id = 1;
                memcpy(welcome.nonce, g_local_nonce, kSessionNonceSize);
                memcpy(welcome.echo_nonce, g_remote_nonce, kSessionNonceSize);
                const bool had_session = g_have_session_key;
                g_have_session_key = false;
                SendPacketTo(&welcome, sizeof(welcome), from);
                g_have_session_key = had_session;
                break;
            }
            case PacketType::Welcome: {
                if (!fits(sizeof(WelcomePacket))) break;
                WelcomePacket welcome = {};
                memcpy(&welcome, buffer, sizeof(welcome));




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
                if (fits(offsetof(EnemyStatePacket, entries))) {
                    EnemyStatePacket enemies = {};
                    memcpy(&enemies, buffer, received);
                    if (enemies.count <= kMaxEnemiesPerPacket &&
                        received == static_cast<int>(EnemyStatePacketSize(enemies.count))) {
                        HandleEnemyState(enemies);
                    }
                }
                break;
            case PacketType::EnemyDamage:
                if (fits(offsetof(EnemyDamagePacket, entries))) {
                    EnemyDamagePacket damage = {};
                    memcpy(&damage, buffer, received);
                    if (damage.count <= kMaxDamagePerPacket &&
                        received == static_cast<int>(EnemyDamagePacketSize(damage.count))) {
                        HandleEnemyDamage(damage);
                    }
                }
                break;
            case PacketType::OwnedEnemy:
                if (fits(offsetof(OwnedEnemyPacket, entries))) {
                    OwnedEnemyPacket owned = {};
                    memcpy(&owned, buffer, received);
                    if (owned.count <= kMaxOwnedEnemiesPerPacket &&
                        received == static_cast<int>(OwnedEnemyPacketSize(owned.count))) {
                        HandleOwnedEnemies(owned);
                    }
                }
                break;
            case PacketType::InviteReply:
                if (fits(sizeof(InviteReplyPacket))) {
                    InviteReplyPacket reply = {};
                    memcpy(&reply, buffer, sizeof(reply));
                    HandleInviteReply(reply);
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
            case PacketType::CheatState:
                if (fits(sizeof(CheatStatePacket))) {
                    CheatStatePacket cheats = {};
                    memcpy(&cheats, buffer, sizeof(cheats));
                    HandleCheatState(cheats);
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

}

bool StartSession() {
    g_start_failure[0] = '\0';
    char host[128] = {};
    int port = kDefaultPort;
    bool is_host = false;
    ReadConfig(host, sizeof(host), &port, &is_host);

    if (g_role == Role::Offline) {
        ReleaseHostInstanceGuard();
        SC_LOG("net: disabled (set mode=host or mode=client in SifuCoop.ini)");
        return false;
    }

    if (port < 1 || port > 65535) {
        _snprintf(g_start_failure, sizeof(g_start_failure),
                  "UDP port %d is invalid; choose a value from 1 to 65535.", port);
        SC_LOG("net: refusing invalid UDP port %d", port);
        coop::ReportProblem("UDP port must be between 1 and 65535");
        ReleaseHostInstanceGuard();
        return false;
    }

    if (is_host) {
        if (!AcquireHostInstanceGuard(port)) return false;
    } else {
        ReleaseHostInstanceGuard();
    }





    MakeNonce(g_local_nonce);
    g_have_session_key = false;
    g_rejected_packets = 0;

    WSADATA wsa = {};
    const int startup_error = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (startup_error != 0) {
        _snprintf(g_start_failure, sizeof(g_start_failure),
                  "Windows networking could not start (error %d).", startup_error);
        SC_LOG("net: WSAStartup failed (%d)", startup_error);
        StopSession();
        return false;
    }
    g_winsock_started = true;

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET) {
        const int error = WSAGetLastError();
        _snprintf(g_start_failure, sizeof(g_start_failure),
                  "SifuCoop could not create its UDP socket (error %d).", error);
        SC_LOG("net: socket() failed (%d)", error);
        StopSession();
        return false;
    }



    u_long non_blocking = 1;
    ioctlsocket(g_socket, FIONBIO, &non_blocking);




    int recv_buffer = 256 * 1024;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&recv_buffer),
               sizeof(recv_buffer));

    char ini_path[MAX_PATH] = {};
    coop::IniPath(ini_path, sizeof(ini_path));

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;






    const int local_port = GetPrivateProfileIntA("net", "local_port", 0, ini_path);
    const int bind_port = is_host ? port : local_port;
    local.sin_port = htons(static_cast<u_short>(bind_port));

    if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        _snprintf(g_start_failure, sizeof(g_start_failure),
                  "SifuCoop could not claim UDP port %d (error %d). Close another host or "
                  "choose a different port.",
                  bind_port, error);
        SC_LOG("net: bind failed (%d) -- is another instance already hosting?", error);
        coop::ReportProblem("could not bind UDP %d -- already hosting elsewhere?", port);
        StopSession();
        return false;
    }



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
            _snprintf(g_start_failure, sizeof(g_start_failure),
                      "'%s' is not a valid IPv4 host address.", host);
            SC_LOG("net: '%s' is not a valid IPv4 address", host);
            coop::ReportProblem("'%s' is not a valid IPv4 address", host);
            StopSession();
            return false;
        }
        g_have_peer_addr = true;
        SC_LOG("net: JOINING %s:%d", host, port);
    }
    return true;
}

const char* GetStartFailure() { return g_start_failure; }

bool RestartSession() {
    const bool was_connected = g_connected;
    StopSession();
    ResetPeerState();
    g_connected = false;
    g_have_peer_addr = false;
    g_send_sequence = 0;
    for (std::uint32_t& value : g_send_sequence_by_type) value = 0;
    g_last_recv_ms = 0;
    g_level_activity_ms = 0;
    g_last_send_ms = 0;
    g_last_ping_ms = 0;
    g_last_hello_ms = 0;
    if (was_connected) g_disconnected_event = true;
    SC_LOG("net: restarting configured session");
    return StartSession();
}

void DisconnectSession() {
    const bool was_connected = g_connected;
    StopSession();
    ResetPeerState();
    g_connected = false;
    g_have_peer_addr = false;
    g_role = Role::Offline;
    g_last_recv_ms = 0;
    g_level_activity_ms = 0;
    g_last_send_ms = 0;
    g_last_ping_ms = 0;
    g_last_hello_ms = 0;
    if (was_connected) g_disconnected_event = true;
    SC_LOG("net: session ended by user");
}
bool Reconfigure(bool host_mode, const char* address, int port, const char* passphrase) {
    if (port < 1 || port > 65535) {
        coop::ReportProblem("port must be between 1 and 65535");
        return false;
    }
    char ini_path[MAX_PATH] = {};
    coop::IniPath(ini_path, sizeof(ini_path));

    char port_text[16] = {};
    _snprintf(port_text, sizeof(port_text), "%d", port);


    WritePrivateProfileStringA("net", "mode", host_mode ? "host" : "client", ini_path);
    if (!host_mode && address && address[0]) WritePrivateProfileStringA("net", "host", address, ini_path);
    WritePrivateProfileStringA("net", "port", port_text, ini_path);


    WritePrivateProfileStringA("net", "passphrase", passphrase ? passphrase : "", ini_path);

    SC_LOG("net: reconfiguring as %s %s:%d", host_mode ? "HOST" : "CLIENT",
           address ? address : "?", port);

    return RestartSession();
}

void StopSession() {



    ReleaseHostInstanceGuard();
    if (g_socket == INVALID_SOCKET) {
        if (g_winsock_started) {
            g_winsock_started = false;
            WSACleanup();
        }
        return;
    }
    if (g_connected) {
        PacketHeader goodbye = {};
        FillHeader(&goodbye, PacketType::Goodbye);
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

void SendLevelSyncPacket(const char* level_path, std::uint32_t request_id) {
    LevelSyncPacket packet = {};
    FillHeader(&packet.header, PacketType::LevelSync);
    packet.request_id = request_id;
    lstrcpynA(packet.level_path, level_path, sizeof(packet.level_path));
    SendPacket(&packet, sizeof(packet));
}

void SendLevelSync(const char* level_path) {
    g_level_activity_ms = NowMs();
    if (!g_connected || !level_path || !level_path[0]) return;
    lstrcpynA(g_active_level_request, level_path, sizeof(g_active_level_request));
    g_active_level_request_id = g_level_request_next;
    g_active_level_first_sent = NowMs();
    g_active_level_last_sent = g_active_level_first_sent;
    SendLevelSyncPacket(g_active_level_request, g_active_level_request_id);
}

void BumpLevelRequest() {
    if (++g_level_request_next == 0) g_level_request_next = 1;
}

void SendLevelPresence(const char* level_path) {
    g_level_activity_ms = NowMs();
    if (!g_connected || !level_path || !level_path[0]) return;
    LevelSyncPacket packet = {};
    FillHeader(&packet.header, PacketType::LevelSync);
    packet.request_id = 0;
    lstrcpynA(packet.level_path, level_path, sizeof(packet.level_path));
    SendPacket(&packet, sizeof(packet));
}

bool PopLevelSync(char* out_level_path, int out_size) {
    if (!g_have_pending_level) return false;
    g_have_pending_level = false;
    lstrcpynA(out_level_path, g_pending_level, out_size);
    return true;
}

void SendInviteReply(std::uint32_t request_id, bool accepted) {
    if (!g_connected || request_id == 0) return;
    InviteReplyPacket packet = {};
    FillHeader(&packet.header, PacketType::InviteReply);
    packet.request_id = request_id;
    packet.accepted = accepted ? 1 : 0;
    SendPacket(&packet, sizeof(packet));
}

bool PopInviteReply(bool* out_accepted) {
    if (!g_invite_reply_pending) return false;
    g_invite_reply_pending = false;
    if (out_accepted) *out_accepted = g_invite_reply_accepted;
    return true;
}

std::uint32_t GetPendingInviteId() {
    return g_have_pending_level ? g_level_request_seen : 0;
}

void ClearPendingInvite() {
    g_have_pending_level = false;
    g_pending_level[0] = '\0';
}

const char* GetPeerLevel() { return g_peer_level; }

void SendEnemyStates(const EnemyStateOut* entries, int count) {
    if (!g_connected) return;
    if (count < 0) count = 0;
    if (count > kMaxTrackedEnemies) count = kMaxTrackedEnemies;
    if (count > 0 && !entries) return;

    static std::uint32_t generation = 0;
    ++generation;





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
            out.source_hash = in.source_hash;
            out.x = in.x;
            out.y = in.y;
            out.z = in.z;
            out.yaw = in.yaw;
            out.health = in.health;
            out.velocity_x = in.velocity_x;
            out.velocity_y = in.velocity_y;
            out.velocity_z = in.velocity_z;
            out.max_health = in.max_health;
            out.guard = in.guard;
            out.damage_applied = in.damage_applied;
            out.guard_damage_applied = in.guard_damage_applied;
            out.time_dilation = in.time_dilation;
            out.flags = in.flags;
        }
        SendPacket(&packet, EnemyStatePacketSize(packet.count));
    }
}

int GetEnemyStates(EnemyStateOut* out, int max_out) {
    if (!out || max_out <= 0) return 0;
    const int count = g_enemy_live_count < max_out ? g_enemy_live_count : max_out;
    for (int i = 0; i < count; ++i) out[i] = g_enemies_live[i];
    return count;
}

bool HasEnemySweep() { return g_enemy_sweep_seen; }

bool EnemySweepIsFresh() {



    constexpr DWORD kSweepStaleMs = 1000;
    if (!g_enemy_sweep_seen) return false;
    return NowMs() - g_enemy_sweep_at <= kSweepStaleMs;
}

void ResetEnemyReplication() {
    g_enemy_live_count = 0;
    g_enemy_staging_count = 0;
    g_enemy_staging_generation = 0;
    g_enemy_chunks_seen = 0;
    g_enemy_chunk_total = 0;
    g_enemy_sweep_seen = false;
    g_enemy_sweep_at = 0;
    g_damage_count = 0;
    g_damage_sequence = 0;
    g_owned_enemy_count = 0;
}

void SendOwnedEnemies(const OwnedEnemy* entries, int count) {
    if (!g_connected || !entries || count <= 0) return;
    if (count > kMaxOwnedEnemiesPerPacket) count = kMaxOwnedEnemiesPerPacket;

    OwnedEnemyPacket packet = {};
    FillHeader(&packet.header, PacketType::OwnedEnemy);
    packet.count = static_cast<std::uint32_t>(count);
    for (int i = 0; i < count; ++i) {
        OwnedEnemyEntry& out = packet.entries[i];
        out.name_hash = entries[i].name_hash;
        out.x = entries[i].x;
        out.y = entries[i].y;
        out.z = entries[i].z;
        out.yaw = entries[i].yaw;
        out.velocity_x = entries[i].velocity_x;
        out.velocity_y = entries[i].velocity_y;
        out.velocity_z = entries[i].velocity_z;
    }
    SendPacket(&packet, static_cast<int>(OwnedEnemyPacketSize(packet.count)));
}




bool GetOwnedEnemy(std::uint32_t name_hash, OwnedEnemy* out) {
    if (!out || g_owned_enemy_count <= 0) return false;








    constexpr DWORD kOwnershipStaleMs = 1500;
    if (NowMs() - g_owned_enemies_at > kOwnershipStaleMs) return false;
    for (int i = 0; i < g_owned_enemy_count; ++i) {
        if (g_owned_enemies[i].name_hash != name_hash) continue;
        const OwnedEnemyEntry& in = g_owned_enemies[i];
        out->name_hash = in.name_hash;
        out->x = in.x;
        out->y = in.y;
        out->z = in.z;
        out->yaw = in.yaw;
        out->velocity_x = in.velocity_x;
        out->velocity_y = in.velocity_y;
        out->velocity_z = in.velocity_z;
        return true;
    }
    return false;
}

void SendEnemyDamage(const DamageReport* entries, int count) {
    if (!g_connected || !entries || count <= 0) return;






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
        packet.entries[i].guard_total = entries[i].guard_total;
    }
    SendPacket(&packet, EnemyDamagePacketSize(packet.count));
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

void SendAnimationSequence(const char* asset_path, std::uint32_t actor_hash,
                           AnimationSemantic semantic, float position) {
    if (!g_connected || !asset_path || !asset_path[0]) return;
    MontagePacket packet = {};
    FillHeader(&packet.header, PacketType::MontageState);
    packet.kind = static_cast<std::uint8_t>(AnimationAssetKind::Sequence);
    packet.semantic = static_cast<std::uint8_t>(semantic);



    packet.position = position;
    lstrcpynA(packet.montage_path, asset_path, sizeof(packet.montage_path));
    packet.actor_hash = actor_hash;
    SendPacket(&packet, sizeof(packet));
}

void SendPoseAsset(const char* asset_path, std::uint32_t actor_hash,
                   AnimationSemantic semantic) {
    if (!g_connected || !asset_path || !asset_path[0] || actor_hash == 0) return;
    MontagePacket packet = {};
    FillHeader(&packet.header, PacketType::MontageState);
    packet.kind = static_cast<std::uint8_t>(AnimationAssetKind::PoseAsset);
    packet.semantic = static_cast<std::uint8_t>(semantic);
    packet.actor_hash = actor_hash;
    lstrcpynA(packet.montage_path, asset_path, sizeof(packet.montage_path));
    SendPacket(&packet, sizeof(packet));
}

bool PopMontageState(char* out_path, int out_size, float* out_position,
                     AnimationAssetKind* out_kind, std::uint32_t* out_actor_hash,
                     AnimationSemantic* out_semantic) {
    if (g_animation_read == g_animation_write || !out_path || out_size <= 0) return false;
    const PendingAnimation& pending = g_animation_queue[g_animation_read];
    lstrcpynA(out_path, pending.path, out_size);
    if (out_position) *out_position = pending.position;
    if (out_kind) *out_kind = pending.kind;
    if (out_actor_hash) *out_actor_hash = pending.actor_hash;
    if (out_semantic) *out_semantic = pending.semantic;
    g_animation_read = (g_animation_read + 1) % kAnimationQueueSize;
    return true;
}

void SendRunState(const RunSnapshot& state) {
    if (!g_connected) return;
    RunStatePacket packet = {};
    FillHeader(&packet.header, PacketType::RunState);
    packet.age = state.age;
    packet.reserved_value = 0.f;
    packet.flags = 0;
    if (state.age_valid) packet.flags |= kRunAgeValid;
    if (state.has_weapon && state.weapon_path[0]) packet.flags |= kRunHasWeapon;
    if (state.outfit_valid && state.outfit_index >= 0 && state.outfit_index < 127) {
        packet.outfit_index = static_cast<std::int8_t>(state.outfit_index);
        packet.flags |= kRunOutfitValid;
    }
    lstrcpynA(packet.weapon_path, state.weapon_path, sizeof(packet.weapon_path));
    SendPacket(&packet, sizeof(packet));
}

bool GetPeerRunState(RunSnapshot* out) {
    if (!out || !g_peer_run_valid) return false;
    *out = g_peer_run;
    return true;
}

void SendCheatState(const CheatSnapshot& state) {
    if (!g_connected || g_role != Role::Host) return;
    CheatStatePacket packet = {};
    FillHeader(&packet.header, PacketType::CheatState);
    std::memcpy(packet.active, state.active, sizeof(packet.active));
    SendPacket(&packet, sizeof(packet));
}

bool GetHostCheatState(CheatSnapshot* out) {
    if (!out || !g_host_cheats_valid || g_role != Role::Client) return false;
    *out = g_host_cheats;
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
    request[1] = 0x01;
    request[2] = 0x00;
    request[3] = 0x00;
    request[4] = 0x21;
    request[5] = 0x12;
    request[6] = 0xA4;
    request[7] = 0x42;

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




    int delay = g_rtt_ms / 2 + g_rtt_jitter_ms * 2 + 2000 / kSnapshotHz;





    if (delay < 80) delay = 80;
    if (delay > 250) delay = 250;
    return delay;
}

void TickSession(const LocalState& local) {
    if (g_socket == INVALID_SOCKET) return;

    PumpReceive();

    const DWORD now = NowMs();








    static DWORD last_tick_ms = 0;
    if (last_tick_ms != 0) {
        const DWORD gap = now - last_tick_ms;
        if (gap > kStallMs) {
            SC_LOG("net: %lums stall (level load or hitch) -- not counted against the peer",
                   gap);
            if (g_last_recv_ms != 0) {
                g_last_recv_ms += gap;


                if (g_last_recv_ms > now) g_last_recv_ms = now;
            }
        }
    }
    last_tick_ms = now;

    UpdateRates(now);






    // The grace used to apply only when the HOST had an invite outstanding, so a
    // joiner loading for any other reason -- joining, restarting, travelling on
    // its own -- got none. Measured 2026-08-16: the joiner dumped its enemy
    // roster at 16:53:39 mid-load and the host cut it at 16:53:51 with "peer
    // timed out after 12003ms", killing the session at the moment both sides
    // were trying to meet.
    //
    // Loading is silent by nature and cannot be announced. Grace is granted to
    // either role whenever anything level-shaped happened recently.
    const bool level_traffic_recent =
        g_level_activity_ms != 0 && now - g_level_activity_ms <= kLevelRetryTimeoutMs;
    const bool peer_loading = g_active_level_request[0] != 0 || level_traffic_recent;
    const DWORD timeout_ms = peer_loading ? kLevelRetryTimeoutMs : kTimeoutMs;
    if (g_connected && g_last_recv_ms != 0 && now - g_last_recv_ms > timeout_ms) {
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






    if (!g_connected && g_have_punch_addr && now - g_last_punch_ms > 500) {
        g_last_punch_ms = now;
        PingPacket punch = {};
        FillHeader(&punch.header, PacketType::Ping);
        punch.probe_time_ms = now;
        const bool had_session = g_have_session_key;
        g_have_session_key = false;
        SendPacketTo(&punch, sizeof(punch), g_punch_addr);
        g_have_session_key = had_session;
    }





    constexpr DWORD kHelloRenewMs = 2000;
    if (g_role == Role::Client &&
        ((!g_connected && now - g_last_hello_ms > 500) ||
         (g_connected && now - g_last_hello_ms >= kHelloRenewMs))) {
        g_last_hello_ms = now;
        HelloPacket hello = {};
        FillHeader(&hello.header, PacketType::Hello);
        lstrcpynA(hello.name, "sifu-peer", sizeof(hello.name));
        memcpy(hello.nonce, g_local_nonce, kSessionNonceSize);
        const bool had_session = g_have_session_key;
        g_have_session_key = false;
        SendPacket(&hello, sizeof(hello));
        g_have_session_key = had_session;
        if (!g_connected) return;
    }

    if (!g_connected) return;
    if (g_role == Role::Host && g_active_level_request[0]) {
        if (_stricmp(g_peer_level, g_active_level_request) == 0) {
            SC_LOG("net: level invite acknowledged by peer");
            g_active_level_request[0] = '\0';
            g_active_level_request_id = 0;
            g_active_level_first_sent = 0;
            g_active_level_last_sent = 0;
        } else if (now - g_active_level_first_sent >= kLevelRetryTimeoutMs) {
            SC_LOG("net: level invite timed out without peer presence");
            g_active_level_request[0] = '\0';
            g_active_level_request_id = 0;
            g_active_level_first_sent = 0;
            g_active_level_last_sent = 0;
        } else if (now - g_active_level_last_sent >= kLevelRetryIntervalMs) {
            SendLevelSyncPacket(g_active_level_request, g_active_level_request_id);
            g_active_level_last_sent = now;
        }
    }

    if (now - g_last_ping_ms >= kPingIntervalMs) {
        g_last_ping_ms = now;
        PingPacket ping = {};
        FillHeader(&ping.header, PacketType::Ping);
        ping.probe_time_ms = now;
        SendPacket(&ping, sizeof(ping));
    }

    int hz = coop::Get().snapshot_hz;
    if (hz < 30) hz = 30;
    if (hz > 60) hz = 60;
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
    snapshot.velocity_x = local.velocity.X;
    snapshot.velocity_y = local.velocity.Y;
    snapshot.velocity_z = local.velocity.Z;
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

bool GetPeerTransform(ue::FVector* location, ue::FRotator* rotation, ue::FVector* velocity) {
    if (!location || !rotation || !velocity) return false;
    const DWORD target = NowMs() - static_cast<DWORD>(GetInterpolationDelayMs());


    const PeerState* older = nullptr;
    const PeerState* newer = nullptr;
    for (int i = 0; i < kBufferSize; ++i) {
        const PeerState& state = g_peer_buffer[i];
        if (!state.valid) continue;
        if (state.sample_ms <= target && (!older || state.sample_ms > older->sample_ms)) {
            older = &state;
        }
        if (state.sample_ms > target && (!newer || state.sample_ms < newer->sample_ms)) {
            newer = &state;
        }
    }

    if (!older && !newer) return false;













    if (!newer) {
        constexpr DWORD kMaxExtrapolationMs = 200;
        DWORD ahead = target - older->sample_ms;
        if (ahead > kMaxExtrapolationMs) ahead = kMaxExtrapolationMs;
        const float seconds = static_cast<float>(ahead) / 1000.f;

        location->X = older->location.X + older->velocity.X * seconds;
        location->Y = older->location.Y + older->velocity.Y * seconds;
        location->Z = older->location.Z;
        *rotation = older->rotation;
        *velocity = older->velocity;

        g_last_output_location = *location;
        g_last_output_rotation = *rotation;
        g_last_output_velocity = *velocity;
        g_have_last_output = true;
        return true;
    }





    if (!older) {
        *location = newer->location;
        *rotation = newer->rotation;
        *velocity = newer->velocity;
        g_last_output_location = *location;
        g_last_output_rotation = *rotation;
        g_last_output_velocity = *velocity;
        g_have_last_output = true;
        return true;
    }

    const DWORD span = newer->sample_ms - older->sample_ms;
    const float alpha = span == 0 ? 0.f : static_cast<float>(target - older->sample_ms) / span;

    location->X = older->location.X + (newer->location.X - older->location.X) * alpha;
    location->Y = older->location.Y + (newer->location.Y - older->location.Y) * alpha;
    location->Z = older->location.Z + (newer->location.Z - older->location.Z) * alpha;



    float delta_yaw = newer->rotation.Yaw - older->rotation.Yaw;
    while (delta_yaw > 180.f) delta_yaw -= 360.f;
    while (delta_yaw < -180.f) delta_yaw += 360.f;

    rotation->Pitch = older->rotation.Pitch;
    rotation->Yaw = older->rotation.Yaw + delta_yaw * alpha;
    rotation->Roll = older->rotation.Roll;
    velocity->X = older->velocity.X + (newer->velocity.X - older->velocity.X) * alpha;
    velocity->Y = older->velocity.Y + (newer->velocity.Y - older->velocity.Y) * alpha;
    velocity->Z = older->velocity.Z + (newer->velocity.Z - older->velocity.Z) * alpha;

    g_last_output_location = *location;
    g_last_output_rotation = *rotation;
    g_last_output_velocity = *velocity;
    g_have_last_output = true;
    return true;
}

}


