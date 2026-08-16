#pragma once

#include <cstddef>
#include <cstdint>



















namespace sifucoop::net {

constexpr std::uint32_t kMagic = 0x53434F50;





constexpr std::uint16_t kProtocolVersion = 20;

















constexpr int kAuthTagSize = 8;




constexpr int kSessionNonceSize = 8;

constexpr int kDefaultPort = 7777;
constexpr int kSnapshotHz = 60;




constexpr int kMaxEnemiesPerPacket = 12;
constexpr int kMaxTrackedEnemies = 96;


constexpr int kMaxPacketSize = 1024;


constexpr std::uint8_t kFlagStateValid = 1 << 0;
constexpr std::uint8_t kFlagIsDown = 1 << 1;
constexpr std::uint8_t kFlagInLevel = 1 << 2;


constexpr std::uint8_t kEnemyActive = 1 << 0;






constexpr std::uint8_t kEnemyDown = 1 << 1;


constexpr std::uint8_t kEnemyTargetsHost = 1 << 2;
constexpr std::uint8_t kEnemyTargetsPeer = 1 << 3;



constexpr std::uint8_t kEnemyDead = 1 << 4;




constexpr std::uint8_t kEnemyReactionMotion = 1 << 5;

enum class PacketType : std::uint16_t {
    Hello = 1,
    Welcome = 2,
    Snapshot = 3,
    Goodbye = 4,
    OrderEvent = 5,
    LevelSync = 6,
    EnemyState = 7,
    EnemyDamage = 8,
    Ping = 9,
    Pong = 10,
    RunState = 11,
    MontageState = 12,
    OwnedEnemy = 13,
    InviteReply = 14,
    CheatState = 15,
};





enum class AnimationSemantic : std::uint8_t {
    Generic = 0,
    Attack = 1,
    Reaction = 2,
    Fall = 3,
    Death = 4,
};

enum class AnimationAssetKind : std::uint8_t {
    Montage = 0,
    Sequence = 1,
    PoseAsset = 2,
};

#pragma pack(push, 1)




struct PacketHeader {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kProtocolVersion;
    std::uint16_t type = 0;
    std::uint32_t sequence = 0;
    std::uint32_t send_time_ms = 0;
    std::uint8_t tag[kAuthTagSize] = {};
};







struct HelloPacket {
    PacketHeader header;
    char name[32] = {};
    std::uint8_t nonce[kSessionNonceSize] = {};
};

struct WelcomePacket {
    PacketHeader header;
    std::uint32_t peer_id = 0;
    std::uint8_t nonce[kSessionNonceSize] = {};
    std::uint8_t echo_nonce[kSessionNonceSize] = {};
};






struct SnapshotPacket {
    PacketHeader header;
    float x = 0.f, y = 0.f, z = 0.f;
    float pitch = 0.f, yaw = 0.f, roll = 0.f;
    float velocity_x = 0.f, velocity_y = 0.f, velocity_z = 0.f;





    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    std::uint8_t flags = 0;
    std::uint8_t reserved[3] = {};
};




struct OrderEventPacket {
    PacketHeader header;


    std::uint32_t actor_hash = 0;
    std::uint32_t order_type = 0;
    std::int32_t attack_index = 0;
    std::int32_t attack_depth = 0;
};







struct LevelSyncPacket {
    PacketHeader header;
    std::uint32_t request_id = 0;
    char level_path[192] = {};
};








struct InviteReplyPacket {
    PacketHeader header;
    std::uint32_t request_id = 0;
    std::uint8_t accepted = 0;
    std::uint8_t reserved[3] = {};
};

struct EnemyEntry {
    std::uint32_t name_hash = 0;



    std::uint32_t source_hash = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f;




    float velocity_x = 0.f, velocity_y = 0.f, velocity_z = 0.f;
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;




    float damage_applied = 0.f;




    float guard_damage_applied = 0.f;




    float time_dilation = 1.f;
    std::uint8_t flags = 0;
    std::uint8_t reserved[3] = {};
};




struct EnemyStatePacket {
    PacketHeader header;
    std::uint32_t generation = 0;
    std::uint8_t chunk = 0;
    std::uint8_t chunk_count = 1;
    std::uint8_t count = 0;
    std::uint8_t reserved = 0;
    EnemyEntry entries[kMaxEnemiesPerPacket];
};

constexpr std::size_t EnemyStatePacketSize(std::uint8_t count) {
    return offsetof(EnemyStatePacket, entries) +
           static_cast<std::size_t>(count) * sizeof(EnemyEntry);
}

struct DamageEntry {
    std::uint32_t name_hash = 0;



    float total = 0.f;


    float guard_total = 0.f;
};


constexpr int kMaxDamagePerPacket = 24;

struct EnemyDamagePacket {
    PacketHeader header;
    std::uint32_t count = 0;
    DamageEntry entries[kMaxDamagePerPacket];
};













struct OwnedEnemyEntry {
    std::uint32_t name_hash = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f;
    float velocity_x = 0.f, velocity_y = 0.f, velocity_z = 0.f;
};

constexpr int kMaxOwnedEnemiesPerPacket = 24;

struct OwnedEnemyPacket {
    PacketHeader header;
    std::uint32_t count = 0;
    OwnedEnemyEntry entries[kMaxOwnedEnemiesPerPacket];
};

constexpr std::size_t OwnedEnemyPacketSize(std::uint32_t count) {
    return offsetof(OwnedEnemyPacket, entries) +
           static_cast<std::size_t>(count) * sizeof(OwnedEnemyEntry);
}

constexpr std::size_t EnemyDamagePacketSize(std::uint32_t count) {
    return offsetof(EnemyDamagePacket, entries) +
           static_cast<std::size_t>(count) * sizeof(DamageEntry);
}




struct PingPacket {
    PacketHeader header;
    std::uint32_t probe_time_ms = 0;
};


constexpr std::uint8_t kRunAgeValid = 1 << 0;

constexpr std::uint8_t kRunHasWeapon = 1 << 2;
constexpr std::uint8_t kRunOutfitValid = 1 << 3;










struct RunStatePacket {
    PacketHeader header;
    std::int32_t age = 0;

    float reserved_value = 0.f;
    std::uint8_t flags = 0;




    std::int8_t outfit_index = -1;
    std::uint8_t reserved[2] = {};
    char weapon_path[192] = {};
};



constexpr int kCheatTagCount = 111;
constexpr int kCheatStateBytes = (kCheatTagCount + 7) / 8;
struct CheatStatePacket {
    PacketHeader header;
    std::uint8_t active[kCheatStateBytes] = {};
};











struct MontagePacket {
    PacketHeader header;
    float position = 0.f;
    std::uint8_t kind = 0;
    std::uint32_t actor_hash = 0;
    std::uint8_t semantic = 0;
    std::uint8_t reserved[2] = {};
    char montage_path[192] = {};
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 24, "header layout changed");
static_assert(sizeof(SnapshotPacket) == 24 + 36 + 12 + 4, "snapshot layout changed");
static_assert(sizeof(OrderEventPacket) == 24 + 16, "order event layout changed");
static_assert(sizeof(EnemyEntry) == 64, "enemy entry layout changed");
static_assert(sizeof(RunStatePacket) == 24 + 4 + 4 + 4 + 192, "run state layout changed");
static_assert(sizeof(CheatStatePacket) <= kMaxPacketSize, "cheat state packet exceeds buffer");
static_assert(sizeof(MontagePacket) == 228, "montage packet layout changed");
static_assert(sizeof(EnemyStatePacket) <= kMaxPacketSize, "enemy packet exceeds the buffer");
static_assert(sizeof(EnemyDamagePacket) <= kMaxPacketSize, "damage packet exceeds the buffer");

}
