// Simulated second player.
//
// Stands in for a real peer so the mod can be exercised without a second copy
// of Sifu. It is a real protocol peer -- it speaks the same packets a game
// client does, so anything it drives is genuinely going over UDP.
//
// Modes:
//   bot     (default) walks toward you, faces you, attacks in range
//   damage            everything bot does, plus it reports damage on the enemy
//                     nearest to you -- so you can watch that enemy lose health
//                     and die in your own game. This is the one test that
//                     proves the joining player can actually fight, and it is
//                     the half of co-op that cannot be checked any other way
//                     with a single machine.
//   circle            orbits you, for eyeballing interpolation smoothness
//   idle              connects and reports vitals, nothing else
//
// Usage: testclient.exe [host] [port] [mode] [passphrase]
//
// The passphrase must match the game's. Leave it off if the game has none set.

#define _CRT_RAND_S

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/net/crypto.h"
#include "../src/net/protocol.h"

using namespace sifucoop::net;

namespace {

// Sifu's units are centimetres.
constexpr float kEngageDistance = 170.f;     // stop closing at roughly striking range
constexpr float kDisengageDistance = 320.f;  // beyond this, walk in
constexpr float kMoveSpeed = 260.f;          // cm/s, close to a brisk walk
constexpr DWORD kAttackIntervalMs = 1800;

// Slow on purpose. The point is to watch a health bar drain in the game and
// confirm the enemy dies at the end of it, not to delete it in one packet.
constexpr float kDamagePerTick = 6.f;
constexpr DWORD kDamageIntervalMs = 400;

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

float Distance2D(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

// Degrees, matching FRotator::Yaw.
float YawTowards(const Vec3& from, const Vec3& to) {
    return atan2f(to.y - from.y, to.x - from.x) * 57.29578f;
}

// The host's enemy sweep, reassembled from however many chunks it arrived in.
struct EnemyView {
    EnemyEntry entries[kMaxTrackedEnemies];
    int count = 0;
    std::uint32_t generation = 0;
    std::uint32_t chunks_seen = 0;
    int staging_count = 0;
    EnemyEntry staging[kMaxTrackedEnemies];
    bool complete = false;
};

void AbsorbEnemyChunk(EnemyView& view, const EnemyStatePacket& packet) {
    if (packet.generation != view.generation) {
        view.generation = packet.generation;
        view.staging_count = 0;
        view.chunks_seen = 0;
    }
    int count = packet.count;
    if (count > kMaxEnemiesPerPacket) count = kMaxEnemiesPerPacket;
    for (int i = 0; i < count && view.staging_count < kMaxTrackedEnemies; ++i) {
        view.staging[view.staging_count++] = packet.entries[i];
    }
    if (packet.chunk < 32) view.chunks_seen |= (1u << packet.chunk);

    const std::uint32_t wanted =
        packet.chunk_count >= 32 ? 0xFFFFFFFFu : (1u << packet.chunk_count) - 1u;
    if ((view.chunks_seen & wanted) != wanted) return;

    for (int i = 0; i < view.staging_count; ++i) view.entries[i] = view.staging[i];
    view.count = view.staging_count;
    view.staging_count = 0;
    view.chunks_seen = 0;
    view.complete = true;
}

// --- Authentication, mirroring the game exactly -----------------------------

std::uint8_t g_base_key[kSha256Size] = {};
std::uint8_t g_session_key[kSha256Size] = {};
bool g_have_session_key = false;
std::uint8_t g_local_nonce[kSessionNonceSize] = {};

void DeriveBaseKey(const char* passphrase) {
    char material[192] = {};
    _snprintf(material, sizeof(material) - 1, "SifuCoop-v%u-key:%s", kProtocolVersion,
              passphrase ? passphrase : "");
    Sha256(material, strlen(material), g_base_key);
}

void DeriveSessionKey(const std::uint8_t* host_nonce, const std::uint8_t* joiner_nonce) {
    std::uint8_t material[kSha256Size + kSessionNonceSize * 2];
    memcpy(material, g_base_key, kSha256Size);
    memcpy(material + kSha256Size, host_nonce, kSessionNonceSize);
    memcpy(material + kSha256Size + kSessionNonceSize, joiner_nonce, kSessionNonceSize);
    Sha256(material, sizeof(material), g_session_key);
    g_have_session_key = true;
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
    const std::uint8_t* key = (handshake || !g_have_session_key) ? g_base_key : g_session_key;
    std::uint8_t digest[kSha256Size];
    HmacSha256(key, kSha256Size, buffer, static_cast<std::size_t>(size), digest);
    memcpy(header->tag, received, kAuthTagSize);
    return SecureEqual(received, digest, kAuthTagSize);
}

}  // namespace

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? atoi(argv[2]) : kDefaultPort;
    const char* mode = argc > 3 ? argv[3] : "bot";
    const char* passphrase = argc > 4 ? argv[4] : "";

    DeriveBaseKey(passphrase);
    for (int i = 0; i < kSessionNonceSize; i += 4) {
        unsigned int value = 0;
        if (rand_s(&value) != 0) value = static_cast<unsigned int>(GetTickCount()) + i;
        memcpy(g_local_nonce + i, &value, 4);
    }

    const bool damage_mode = (_stricmp(mode, "damage") == 0);
    const bool bot_mode = damage_mode || (_stricmp(mode, "bot") == 0);
    const bool circle_mode = (_stricmp(mode, "circle") == 0);

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        printf("socket() failed: %d\n", WSAGetLastError());
        return 1;
    }
    u_long non_blocking = 1;
    ioctlsocket(sock, FIONBIO, &non_blocking);

    sockaddr_in peer = {};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, host, &peer.sin_addr) != 1) {
        printf("bad address: %s\n", host);
        return 1;
    }

    auto send_packet = [&](void* data, int size) {
        SignPacket(data, size);
        sendto(sock, static_cast<const char*>(data), size, 0,
               reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    };

    printf("testclient -> %s:%d  mode=%s  passphrase=%s  (protocol v%u)\n", host, port, mode,
           passphrase[0] ? "set" : "none", kProtocolVersion);
    if (damage_mode) {
        printf("DAMAGE MODE: the enemy nearest you will lose %.0f health every %lums.\n"
               "Watch it in game -- if it drains and dies, the joining player can fight.\n",
               kDamagePerTick, kDamageIntervalMs);
    }
    printf("waiting for the game...\n");

    bool connected = false;
    std::uint32_t sequence = 0;
    DWORD last_hello = 0;
    DWORD last_snapshot = 0;
    DWORD last_attack = 0;
    DWORD last_damage = 0;
    DWORD last_enemy_print = 0;
    DWORD received = 0;
    const DWORD start = GetTickCount();

    Vec3 player;  // real player's position, from their snapshots
    bool have_player = false;
    Vec3 self;    // our simulated position
    bool placed = false;
    float yaw = 0.f;
    bool is_down = false;
    char my_level[192] = {};
    bool have_level = false;
    DWORD last_presence = 0;
    DWORD last_runstate = 0;

    EnemyView enemies;
    std::uint32_t damage_target = 0;
    float damage_total = 0.f;

    static const std::int32_t kAttackIndices[] = {0x2C, 0x0E, 0x07, 0x20, 0x30};
    int attack_cursor = 0;

    auto fill_header = [&](PacketHeader* header, PacketType type) {
        header->magic = kMagic;
        header->version = kProtocolVersion;
        header->type = static_cast<std::uint16_t>(type);
        header->sequence = ++sequence;
        header->send_time_ms = GetTickCount();
    };

    for (;;) {
        const DWORD now = GetTickCount();

        if (!connected && now - last_hello > 500) {
            last_hello = now;
            HelloPacket hello = {};
            fill_header(&hello.header, PacketType::Hello);
            lstrcpynA(hello.name, "testclient", sizeof(hello.name));
            memcpy(hello.nonce, g_local_nonce, kSessionNonceSize);
            g_have_session_key = false;  // no session yet; sign with the base key
            send_packet(&hello, sizeof(hello));
        }

        // Drain everything waiting.
        char buffer[kMaxPacketSize];
        sockaddr_in from = {};
        for (;;) {
            int from_size = sizeof(from);
            const int bytes = recvfrom(sock, buffer, sizeof(buffer), 0,
                                       reinterpret_cast<sockaddr*>(&from), &from_size);
            if (bytes < static_cast<int>(sizeof(PacketHeader))) break;

            PacketHeader header = {};
            memcpy(&header, buffer, sizeof(header));
            if (header.magic != kMagic) continue;
            if (header.version != kProtocolVersion) {
                printf("!! peer speaks protocol v%u, we speak v%u\n", header.version,
                       kProtocolVersion);
                continue;
            }

            const auto type = static_cast<PacketType>(header.type);
            const bool handshake =
                (type == PacketType::Hello || type == PacketType::Welcome);
            if (!VerifyPacket(buffer, bytes, handshake)) {
                static int rejected = 0;
                if (++rejected <= 3) {
                    printf("!! packet failed authentication -- passphrase mismatch?\n");
                }
                continue;
            }

            if (type == PacketType::Welcome && bytes >= static_cast<int>(sizeof(WelcomePacket))) {
                WelcomePacket welcome = {};
                memcpy(&welcome, buffer, sizeof(welcome));
                if (!SecureEqual(welcome.echo_nonce, g_local_nonce, kSessionNonceSize)) {
                    printf("!! host echoed the wrong nonce -- ignoring\n");
                    continue;
                }
                DeriveSessionKey(welcome.nonce, g_local_nonce);
                if (!connected) {
                    connected = true;
                    printf("connected (authenticated)\n");
                }
            } else if (type == PacketType::Snapshot &&
                       bytes >= static_cast<int>(sizeof(SnapshotPacket))) {
                SnapshotPacket snapshot = {};
                memcpy(&snapshot, buffer, sizeof(snapshot));
                ++received;

                player = {snapshot.x, snapshot.y, snapshot.z};
                if (!have_player) {
                    have_player = true;
                    printf("found the player at (%.0f, %.0f, %.0f) hp=%.0f/%.0f\n", player.x,
                           player.y, player.z, snapshot.health, snapshot.max_health);
                }
                // Start a little away from them so there is something to close.
                if (!placed) {
                    placed = true;
                    self = {player.x + 400.f, player.y, player.z};
                }
                if (received % 120 == 0) {
                    printf("  player (%.0f, %.0f) hp=%.0f  me (%.0f, %.0f)  dist %.0f\n",
                           player.x, player.y, snapshot.health, self.x, self.y,
                           Distance2D(self, player));
                }
            } else if (type == PacketType::Ping &&
                       bytes >= static_cast<int>(sizeof(PingPacket))) {
                // Answering is what makes the game's ping readout work; without
                // it the overlay sits on "measuring..." forever and the
                // adaptive interpolation never gets a number to work from.
                PingPacket ping = {};
                memcpy(&ping, buffer, sizeof(ping));
                PingPacket pong = {};
                fill_header(&pong.header, PacketType::Pong);
                pong.probe_time_ms = ping.probe_time_ms;  // echoed verbatim
                send_packet(&pong, sizeof(pong));
            } else if (type == PacketType::EnemyState &&
                       bytes >= static_cast<int>(sizeof(EnemyStatePacket))) {
                EnemyStatePacket packet = {};
                memcpy(&packet, buffer, sizeof(packet));
                AbsorbEnemyChunk(enemies, packet);
            } else if (type == PacketType::OrderEvent &&
                       bytes >= static_cast<int>(sizeof(OrderEventPacket))) {
                OrderEventPacket order = {};
                memcpy(&order, buffer, sizeof(order));
                if (order.actor_hash == 0) {
                    printf("  <- player attacked (index 0x%X)\n", order.attack_index);
                } else {
                    printf("  <- enemy %08X attacked (index 0x%X)\n", order.actor_hash,
                           order.attack_index);
                }
            } else if (type == PacketType::LevelSync &&
                       bytes >= static_cast<int>(sizeof(LevelSyncPacket))) {
                // Accepting invites makes the lobby testable solo: without this
                // the peer shows as "?" forever and F2 looks like it does
                // nothing, when in fact the invite was sent and ignored.
                LevelSyncPacket level = {};
                memcpy(&level, buffer, sizeof(level));
                level.level_path[sizeof(level.level_path) - 1] = '\0';
                if (level.request_id != 0) {
                    printf("  <- invited to '%s' - accepting\n", level.level_path);
                } else if (strcmp(my_level, level.level_path) != 0) {
                    printf("  <- host is in '%s'\n", level.level_path);
                }
                lstrcpynA(my_level, level.level_path, sizeof(my_level));
                have_level = true;
            } else if (type == PacketType::Goodbye) {
                printf("peer said goodbye\n");
                connected = false;
            }
        }

        // Report what the host says its enemies are doing. This is the whole of
        // Phase A visible from outside the game: if this list is empty while
        // you are mid-fight, the host is not publishing and nothing downstream
        // can possibly work.
        if (connected && enemies.complete && now - last_enemy_print > 3000) {
            last_enemy_print = now;
            printf("  enemies: %d active\n", enemies.count);
            for (int i = 0; i < enemies.count && i < 8; ++i) {
                const EnemyEntry& e = enemies.entries[i];
                printf("    %08X hp=%.0f/%.0f guard=%.0f applied=%.0f%s at (%.0f, %.0f)\n",
                       e.name_hash, e.health, e.max_health, e.guard, e.damage_applied,
                       (e.flags & kEnemyDown) ? " DOWN" : "", e.x, e.y);
            }
        }

        // Run-state: age / room-clear / weapon, a couple of times a second. The
        // game logs the peer's ("run: peer age=.. room=.. weapon=.."), so this
        // is what makes that display testable on ONE machine -- without a second
        // Sifu there is otherwise nothing sending run-state. The values are
        // synthetic and move slowly on purpose so the change is visible: age
        // climbs one per 20 s from a base, room-clear ramps 0..100% over a
        // minute, and a placeholder weapon path exercises the string path.
        if (connected && now - last_runstate >= 500) {
            last_runstate = now;
            const DWORD elapsed = now - start;

            RunStatePacket run = {};
            fill_header(&run.header, PacketType::RunState);
            run.age = 20 + static_cast<int>(elapsed / 20000);
            run.room_clear_percent =
                static_cast<float>((elapsed / 1000) % 60) / 60.f;  // 0..~1 loop
            lstrcpynA(run.weapon_path,
                      "/Game/Weapons/Bat/BaseWeaponData_Bat.BaseWeaponData_Bat",
                      sizeof(run.weapon_path));
            run.flags = kRunAgeValid | kRunRoomClearValid | kRunHasWeapon;
            send_packet(&run, sizeof(run));
        }

        if (connected && have_player && placed && now - last_snapshot >= 1000 / kSnapshotHz) {
            // last_snapshot starts at 0, so the first delta would be the whole
            // system uptime -- which sent the bot roughly 15 km in one step and
            // made the puppet vanish off the map. Seed it, and clamp: a hitch or
            // a breakpoint must never translate into a teleport either.
            constexpr float kMaxStepSeconds = 0.1f;
            float dt = (last_snapshot == 0) ? (1.f / kSnapshotHz)
                                            : (now - last_snapshot) / 1000.f;
            if (dt > kMaxStepSeconds) dt = kMaxStepSeconds;
            last_snapshot = now;

            const float distance = Distance2D(self, player);
            yaw = YawTowards(self, player);  // always face them

            if (bot_mode) {
                // Close the gap, then hold at striking range. Backing off when
                // too close keeps it from standing inside the player.
                float step = 0.f;
                if (distance > kEngageDistance) {
                    step = kMoveSpeed * (distance > kDisengageDistance ? 1.f : 0.6f);
                } else if (distance < kEngageDistance * 0.6f) {
                    step = -kMoveSpeed * 0.5f;
                }
                if (step != 0.f) {
                    const float radians = yaw * 0.0174533f;
                    self.x += cosf(radians) * step * dt;
                    self.y += sinf(radians) * step * dt;
                }
                self.z = player.z;  // no pathfinding: stay on their plane
            } else if (circle_mode) {
                const float t = (now - start) / 1000.f;
                const float angle = t * 0.8f;
                self.x = player.x + 300.f * cosf(angle);
                self.y = player.y + 300.f * sinf(angle);
                self.z = player.z;
            }

            // Go down briefly every 30s to exercise the remote knockdown path.
            const int cycle = static_cast<int>((now - start) / 1000) % 30;
            is_down = (cycle >= 25 && cycle < 28);

            SnapshotPacket snapshot = {};
            fill_header(&snapshot.header, PacketType::Snapshot);
            snapshot.x = self.x;
            snapshot.y = self.y;
            snapshot.z = self.z;
            snapshot.yaw = yaw;
            // Vitals that visibly move, so the puppet's health bar in the game
            // can be seen tracking a peer rather than sitting at a constant.
            snapshot.max_health = 100.f;
            snapshot.health = 100.f - static_cast<float>(cycle) * 2.f;
            snapshot.guard = 100.f - static_cast<float>(cycle % 10) * 8.f;
            snapshot.flags = kFlagStateValid | kFlagInLevel;
            if (is_down) snapshot.flags |= kFlagIsDown;
            send_packet(&snapshot, sizeof(snapshot));

            // Echo our level back so the game's lobby can show where we are.
            if (have_level && now - last_presence > 1000) {
                last_presence = now;
                LevelSyncPacket presence = {};
                fill_header(&presence.header, PacketType::LevelSync);
                presence.request_id = 0;  // presence, not an invite
                lstrcpynA(presence.level_path, my_level, sizeof(presence.level_path));
                send_packet(&presence, sizeof(presence));
            }

            // Attack only when actually in range and upright, so the timing
            // looks like a real opponent rather than a metronome.
            if (bot_mode && !is_down && distance <= kEngageDistance * 1.25f &&
                now - last_attack > kAttackIntervalMs) {
                last_attack = now;
                OrderEventPacket order = {};
                fill_header(&order.header, PacketType::OrderEvent);
                order.actor_hash = 0;  // our own character
                order.attack_index = kAttackIndices[attack_cursor % 5];
                order.attack_depth = attack_cursor % 3;
                ++attack_cursor;
                send_packet(&order, sizeof(order));
                printf("  -> attack index=0x%02X (dist %.0f)\n", order.attack_index, distance);
            }
        }

        // Damage reporting: pick the enemy closest to the player and chip away
        // at it. Totals are cumulative and idempotent, so this is also a live
        // test that a resent report is not applied twice.
        if (damage_mode && connected && enemies.complete && enemies.count > 0 &&
            now - last_damage >= kDamageIntervalMs) {
            last_damage = now;

            if (damage_target == 0) {
                float best = 1e9f;
                for (int i = 0; i < enemies.count; ++i) {
                    const EnemyEntry& e = enemies.entries[i];
                    if (e.flags & kEnemyDown) continue;
                    const Vec3 at = {e.x, e.y, e.z};
                    const float d = Distance2D(at, player);
                    if (d >= best) continue;
                    best = d;
                    damage_target = e.name_hash;
                }
                if (damage_target != 0) {
                    damage_total = 0.f;
                    printf("  targeting enemy %08X (%.0f units from you)\n", damage_target,
                           best);
                }
            }

            if (damage_target != 0) {
                // Drop the target once the host reports it down, and pick a new
                // one next tick -- which also proves the down state came back.
                bool still_up = false;
                for (int i = 0; i < enemies.count; ++i) {
                    if (enemies.entries[i].name_hash != damage_target) continue;
                    still_up = (enemies.entries[i].flags & kEnemyDown) == 0;
                    if (!still_up) {
                        printf("  enemy %08X is DOWN after %.0f reported damage\n",
                               damage_target, damage_total);
                    }
                    break;
                }
                if (!still_up) {
                    damage_target = 0;
                } else {
                    damage_total += kDamagePerTick;
                    EnemyDamagePacket packet = {};
                    fill_header(&packet.header, PacketType::EnemyDamage);
                    packet.count = 1;
                    packet.entries[0].name_hash = damage_target;
                    packet.entries[0].total = damage_total;
                    send_packet(&packet, sizeof(packet));
                }
            }
        }

        Sleep(5);
    }
}
