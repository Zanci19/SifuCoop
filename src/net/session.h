#pragma once

#include <cstdint>

#include "protocol.h"

#include "../ue/reflection.h"

namespace sifucoop::net {

enum class Role { Offline, Host, Client };
enum class AnimationSemantic : std::uint8_t;
enum class AnimationAssetKind : std::uint8_t;

bool StartSession();

const char* GetStartFailure();

bool Reconfigure(bool host_mode, const char* address, int port, const char* passphrase);

bool RestartSession();

void DisconnectSession();
void StopSession();

Role GetRole();
bool IsConnected();

struct LocalState {
    ue::FVector location;
    ue::FRotator rotation;
    ue::FVector velocity;
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    bool state_valid = false;
    bool is_down = false;
    bool in_level = false;
};

void TickSession(const LocalState& local);

bool GetPeerTransform(ue::FVector* location, ue::FRotator* rotation, ue::FVector* velocity);

struct PeerVitals {
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    bool is_down = false;
    bool in_level = false;
};

bool GetPeerVitals(PeerVitals* out);

bool ConsumeConnectedEvent();
bool ConsumeDisconnectedEvent();

void SendOrderEvent(std::uint32_t actor_hash, std::uint32_t order_type,
                    std::int32_t attack_index, std::int32_t attack_depth);

bool PopOrderEvent(std::uint32_t* actor_hash, std::uint32_t* order_type,
                   std::int32_t* attack_index, std::int32_t* attack_depth);

void SendLevelSync(const char* level_path);

void BumpLevelRequest();

void SendLevelPresence(const char* level_path);

bool PopLevelSync(char* out_level_path, int out_size);

void SendInviteReply(std::uint32_t request_id, bool accepted);

bool PopInviteReply(bool* out_accepted);

std::uint32_t GetPendingInviteId();

void ClearPendingInvite();

const char* GetPeerLevel();

struct EnemyStateOut {
    std::uint32_t name_hash = 0;
    std::uint32_t source_hash = 0;
    float x = 0.f, y = 0.f, z = 0.f, yaw = 0.f;
    float velocity_x = 0.f, velocity_y = 0.f, velocity_z = 0.f;
    float health = 0.f;
    float max_health = 0.f;
    float guard = 0.f;
    float damage_applied = 0.f;
    float guard_damage_applied = 0.f;
    float time_dilation = 1.f;
    std::uint8_t flags = 0;
};

void SendEnemyStates(const EnemyStateOut* entries, int count);

int GetEnemyStates(EnemyStateOut* out, int max_out);

bool HasEnemySweep();

bool EnemySweepIsFresh();

void ResetEnemyReplication();

struct DamageReport {
    std::uint32_t name_hash = 0;
    float total = 0.f;
    float guard_total = 0.f;
};

void SendEnemyDamage(const DamageReport* entries, int count);

struct OwnedEnemy {
    std::uint32_t name_hash = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f;
    float velocity_x = 0.f, velocity_y = 0.f, velocity_z = 0.f;
};

void SendOwnedEnemies(const OwnedEnemy* entries, int count);

bool GetOwnedEnemy(std::uint32_t name_hash, OwnedEnemy* out);

int GetEnemyDamage(DamageReport* out, int max_out);

struct RunSnapshot {
    bool age_valid = false;
    int age = 0;
    bool has_weapon = false;
    char weapon_path[192] = {};
    bool outfit_valid = false;
    int outfit_index = -1;
};

void SendRunState(const RunSnapshot& state);

bool GetPeerRunState(RunSnapshot* out);

struct CheatSnapshot {
    std::uint8_t active[kCheatStateBytes] = {};
};
void SendCheatState(const CheatSnapshot& state);
bool GetHostCheatState(CheatSnapshot* out);

void DiscoverPublicAddress();

const char* GetPublicAddress();

void SendMontageState(const char* montage_path, float position);
void SendAnimationSequence(const char* asset_path, std::uint32_t actor_hash,
                           AnimationSemantic semantic, float position = 0.f);
void SendPoseAsset(const char* asset_path, std::uint32_t actor_hash,
                   AnimationSemantic semantic);

bool PopMontageState(char* out_path, int out_size, float* out_position,
                     AnimationAssetKind* out_kind, std::uint32_t* out_actor_hash,
                     AnimationSemantic* out_semantic);

int GetRoundTripMs();

int GetInterpolationDelayMs();

}
