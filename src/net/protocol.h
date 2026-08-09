#pragma once

#include <cstddef>
#include <cstdint>

// Wire format, shared verbatim with testclient/. Everything is little-endian
// and fixed-size: both ends are x86-64, so no serialisation layer is needed,
// but every struct is explicitly packed so an accidental padding change cannot
// silently desync the two builds.
//
// AUTHORITY MODEL
//
//   Your own character   -- you decide. Nobody else can tell you that you were
//                           hit, so your parries are never punished by latency.
//   Enemies              -- the host decides. Position, health and death all
//                           come from the host; the client's copies have their
//                           AI stopped and are driven.
//   Damage to enemies    -- the host applies it, but the client reports what it
//                           dealt locally, as a running total per enemy. Totals
//                           rather than deltas make the report idempotent, so a
//                           lost packet costs nothing: the next one carries the
//                           whole story and the host applies the difference.

namespace sifucoop::net {

constexpr std::uint32_t kMagic = 0x53434F50;  // 'SCOP'

// Bumped whenever any struct below changes -- or, as in v12, whenever a field's
// MEANING changes: `sequence` is now per packet type rather than one counter for
// the whole socket, and a v11 peer's numbering would read as constant loss.
// Peers refuse to talk across a mismatch rather than misinterpreting each other.
constexpr std::uint16_t kProtocolVersion = 12;

// AUTHENTICATION
//
// Every packet carries a truncated HMAC-SHA256 over its own bytes, keyed by a
// passphrase both players share. Anything that does not verify is dropped
// before a single field is read.
//
// This is not optional decoration. The protocol moves a player's position,
// tells the receiving game which level to load, and applies damage to
// characters -- so an unauthenticated packet is a stranger with a hand inside
// your session. That was tolerable while the only supported transport was a
// private VPN; it is not once a port is forwarded to the open internet.
//
// 8 bytes of tag is short by hashing standards and ample here: forging one
// costs 2^63 guesses against a target that also has to guess a per-session
// nonce, and every byte of tag is a byte not spent on game state in a 60 Hz
// datagram.
constexpr int kAuthTagSize = 8;

// Mixed into the key for the lifetime of one session, so a packet recorded from
// an earlier session cannot be replayed into a later one even though the
// passphrase has not changed.
constexpr int kSessionNonceSize = 8;

constexpr int kDefaultPort = 7777;
constexpr int kSnapshotHz = 60;

// A Sifu fight is a handful of enemies, but a level pre-spawns dozens and a
// crowd scene can lift many at once, so the host sends them in chunks rather
// than truncating at whatever fits one datagram.
constexpr int kMaxEnemiesPerPacket = 12;
constexpr int kMaxTrackedEnemies = 96;

// Comfortably above the largest packet below, and well under any path MTU.
constexpr int kMaxPacketSize = 1024;

// SnapshotPacket::flags
constexpr std::uint8_t kFlagStateValid = 1 << 0;  // sender could read its state
constexpr std::uint8_t kFlagIsDown = 1 << 1;      // sender is downed/dead
constexpr std::uint8_t kFlagInLevel = 1 << 2;     // sender has a live pawn

// EnemyEntry::flags
constexpr std::uint8_t kEnemyActive = 1 << 0;  // in the level, not pooled away
// KNOCKDOWN, not death. Sifu's IsDown() is the stagger/floored state that an
// enemy recovers from several times in an ordinary fight. Conflating it with
// death was a real bug: the joining side forced its copy through
// InternalSetDownState on every knockdown and back again on every recovery,
// and a body driven through that state machine from outside came back standing
// but no longer hittable. Only kEnemyDead may drive the local death path.
constexpr std::uint8_t kEnemyDown = 1 << 1;
// Which player this enemy is fighting, as seen by the host. The client flips
// the sense: the host's target is the client's puppet and vice versa.
constexpr std::uint8_t kEnemyTargetsHost = 1 << 2;
constexpr std::uint8_t kEnemyTargetsPeer = 1 << 3;
// The host says this body is actually dead (health at or below zero, or it left
// the fight while dying). This is the ONLY flag that makes the joining side put
// its copy down.
constexpr std::uint8_t kEnemyDead = 1 << 4;

enum class PacketType : std::uint16_t {
    Hello = 1,        // join request
    Welcome = 2,      // host accepts
    Snapshot = 3,     // per-tick character state
    Goodbye = 4,      // clean disconnect
    OrderEvent = 5,   // "this character performed this move" -- intent, never an object
    LevelSync = 6,    // host: "come to this level" / either: "I am here"
    EnemyState = 7,   // host: authoritative enemy transforms, vitals and liveness
    EnemyDamage = 8,  // client: "I have dealt this much to these enemies, in total"
    Ping = 9,         // either: round-trip probe
    Pong = 10,        // reply, echoing the probe's timestamp
    RunState = 11,    // either: age, room-clear progress, held weapon (Phase D)
    MontageState = 12,  // either: "my character is now playing this animation" (cosmetic)
};

#pragma pack(push, 1)

// `tag` is computed over the whole packet with the tag field itself zeroed, so
// verification is: copy, zero, recompute, compare. It sits in the header rather
// than trailing the packet so every struct below keeps a fixed size.
struct PacketHeader {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kProtocolVersion;
    std::uint16_t type = 0;
    std::uint32_t sequence = 0;
    std::uint32_t send_time_ms = 0;
    std::uint8_t tag[kAuthTagSize] = {};
};

// The handshake exchanges session nonces. Each side contributes one, and the
// key both sides use afterwards is derived from the passphrase and *both* --
// so neither peer alone decides what the session key will be, and a recorded
// handshake cannot be replayed to pin a known key.
//
// Hello and Welcome are themselves authenticated with the passphrase alone.
struct HelloPacket {
    PacketHeader header;
    char name[32] = {};
    std::uint8_t nonce[kSessionNonceSize] = {};
};

struct WelcomePacket {
    PacketHeader header;
    std::uint32_t peer_id = 0;
    std::uint8_t nonce[kSessionNonceSize] = {};
    std::uint8_t echo_nonce[kSessionNonceSize] = {};  // the joiner's, proving liveness
};

// The unit of state exchange for a player's own character.
//
// Each peer is authoritative over its own state: with no server there is
// nothing to arbitrate a disagreement, so a player decides their own outcome
// and the peer displays what was reported.
struct SnapshotPacket {
    PacketHeader header;
    float x = 0.f, y = 0.f, z = 0.f;
    float pitch = 0.f, yaw = 0.f, roll = 0.f;
    float velocity_x = 0.f, velocity_y = 0.f, velocity_z = 0.f;

    // Vitals are display-and-aggro only on the receiving side. They must never
    // be read as a liveness signal -- 0 is indistinguishable from "not reported
    // yet", which previously made every freshly connected peer appear dead.
    // kFlagStateValid is the signal; these are the numbers.
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    std::uint8_t flags = 0;
    std::uint8_t reserved[3] = {};
};

// Intent, not an object. Sifu's order structs contain heap-allocating
// containers and cannot be transported; the receiver rebuilds an equivalent
// move locally from these values.
struct OrderEventPacket {
    PacketHeader header;
    // 0 means the sender's own player; anything else is the name hash of an
    // enemy, so one packet type carries both without a second code path.
    std::uint32_t actor_hash = 0;
    std::uint32_t order_type = 0;
    std::int32_t attack_index = 0;
    std::int32_t attack_depth = 0;
};

// Sent by the host to pull the joiner into its level. Carries the package path
// read from the live world, so it works for any level without a hardcoded map
// list -- map names live inside the encrypted pak and are not in the binary.
//
// request_id == 0 means "I am here" (presence, never acted on); anything else
// is an invite, and repeats of an id already handled are swallowed.
struct LevelSyncPacket {
    PacketHeader header;
    std::uint32_t request_id = 0;
    char level_path[192] = {};
};

struct EnemyEntry {
    std::uint32_t name_hash = 0;
    // Stable placed-spawner identity. Runtime actor names are process-local,
    // especially after one player previously cleared the room, but the level
    // AISpawner path survives on both machines and still owns its pooled body.
    std::uint32_t source_hash = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f;
    // Position alone makes the receiving movement component stop whenever it
    // catches up to a snapshot, then start again on the next one. Keep the
    // host's actual horizontal motion continuous so its AnimBP sees a real
    // locomotion velocity between snapshots.
    float velocity_x = 0.f, velocity_y = 0.f, velocity_z = 0.f;
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    // Echoes back the running damage total the host has applied for this enemy.
    // The client only overwrites its local health once this matches what it has
    // sent -- otherwise the host's older, higher value would visibly heal an
    // enemy the client just hit.
    float damage_applied = 0.f;
    std::uint8_t flags = 0;
    std::uint8_t reserved[3] = {};
};

// Host -> client only. `chunk` and `chunk_count` let a crowd span several
// datagrams; `generation` ties the chunks of one sweep together so a client
// never mixes half of this frame's enemies with half of the last one's.
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
    // Cumulative since this enemy was last seen alive, not a delta. The host
    // applies (total - already_applied), so duplicates are harmless and losses
    // self-heal on the next send.
    float total = 0.f;
};

// Client -> host only.
constexpr int kMaxDamagePerPacket = 24;

struct EnemyDamagePacket {
    PacketHeader header;
    std::uint32_t count = 0;
    DamageEntry entries[kMaxDamagePerPacket];
};

constexpr std::size_t EnemyDamagePacketSize(std::uint32_t count) {
    return offsetof(EnemyDamagePacket, entries) +
           static_cast<std::size_t>(count) * sizeof(DamageEntry);
}

// Round-trip probe. The reply copies `probe_time_ms` back verbatim, so the
// sender needs no table of outstanding pings and the two clocks never have to
// agree on anything.
struct PingPacket {
    PacketHeader header;
    std::uint32_t probe_time_ms = 0;
};

// RunStatePacket::flags
constexpr std::uint8_t kRunAgeValid = 1 << 0;          // `age` means something
constexpr std::uint8_t kRunRoomClearValid = 1 << 1;    // `room_clear_percent` is real
constexpr std::uint8_t kRunHasWeapon = 1 << 2;         // `weapon_path` names a held weapon

// Periodic session status, independent of position. Carries the things a full
// playthrough makes players curious about on the other machine: alive at what
// age, how far the current room is cleared, and what weapon they are holding.
//
// Deliberately informational. Age, deaths and the room-clear percentage stay
// per-player in their own game as in singleplayer; this only lets each player
// see where the other is in the run. Nothing here is authoritative for the
// receiving side's own state.
//
// Sent on the same slow cadence as level presence (~1 Hz), so it is also how a
// peer that joined late learns where the session stands.
struct RunStatePacket {
    PacketHeader header;
    std::int32_t age = 0;              // sender's character age (BPF_GetCharacterAge)
    float room_clear_percent = -1.f;   // sender's live room-clear progress, 0..1
    std::uint8_t flags = 0;
    std::uint8_t reserved[3] = {};
    char weapon_path[192] = {};        // portable path of the held UBaseWeaponData
};

// "My character just started playing this animation." Sent only when the
// active montage changes, not per frame -- attacks and dodges are brief and
// bursty, exactly like order events. The receiver plays the same montage on the
// sender's puppet as a PURE VISUAL: it animates the swing without running the
// attack, so it produces no hitbox and cannot damage anyone. That is the
// correct model for a remote player -- their damage already resolved on their
// own machine, so on yours their attack should only be seen, never re-fought.
//
// The montage is addressed by object path, stable across machines exactly like
// a combo tree or a level package. `kind` makes UAnimMontage and raw
// UAnimSequence payloads unambiguous; Sifu attacks use the latter.
struct MontagePacket {
    PacketHeader header;
    float position = 0.f;              // playback position when captured, for seeking
    std::uint8_t kind = 0;             // 0 = UAnimMontage, 1 = raw UAnimationAsset
    std::uint8_t reserved[3] = {};
    char montage_path[192] = {};       // portable path of the animation asset
};

#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 24, "header layout changed");
static_assert(sizeof(SnapshotPacket) == 24 + 36 + 12 + 4, "snapshot layout changed");
static_assert(sizeof(OrderEventPacket) == 24 + 16, "order event layout changed");
static_assert(sizeof(EnemyEntry) == 56, "enemy entry layout changed");
static_assert(sizeof(RunStatePacket) == 24 + 4 + 4 + 4 + 192, "run state layout changed");
static_assert(sizeof(EnemyStatePacket) <= kMaxPacketSize, "enemy packet exceeds the buffer");
static_assert(sizeof(EnemyDamagePacket) <= kMaxPacketSize, "damage packet exceeds the buffer");

}  // namespace sifucoop::net
