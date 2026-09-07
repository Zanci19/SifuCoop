#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {

void InitActors(std::uintptr_t module_base);

bool ActorsReady();

struct Fighter {
    ue::UObject* actor = nullptr;
    ue::UObject* health = nullptr;
    ue::UObject* attack = nullptr;
    ue::UObject* defense = nullptr;

    bool valid() const { return actor != nullptr; }
};

Fighter ResolveFighter(ue::UObject* actor);

float GetHealth(const Fighter& fighter);
float GetMaxHealth(const Fighter& fighter);

void SetHealth(const Fighter& fighter, float health);

float GetGuard(const Fighter& fighter);
void SetGuard(const Fighter& fighter, float guard);

void ApplyDamage(const Fighter& fighter, float amount);

bool KillWithAnimation(const Fighter& fighter, ue::UObject* instigator,
                       ue::UObject* death_animation);

bool IsDown(const Fighter& fighter);
bool IsDead(const Fighter& fighter);

void SetDown(const Fighter& fighter, bool down);

void SetDeathState(const Fighter& fighter);

bool SetCurrentPoseAsset(ue::UObject* actor, ue::UObject* pose_asset);

void NotifyDownStateChanged(const Fighter& fighter, bool down);

int GetFaction(ue::UObject* actor);
void SetFaction(ue::UObject* actor, int faction);
void SetInvincible(ue::UObject* actor, bool invincible);

ue::UObject* GetDefaultCombo(const Fighter& fighter);

bool StopBrain(ue::UObject* actor);

bool StartBrain(ue::UObject* actor);

void SetActorPresent(ue::UObject* actor, bool present);

void SetActorCollisionEnabled(ue::UObject* actor, bool enabled);

bool SetPresentationVelocity(ue::UObject* actor, const ue::FVector& velocity);

struct PresentationTargets {
    ue::UObject* movement = nullptr;
    ue::UObject* root = nullptr;

    bool valid() const { return movement != nullptr && root != nullptr; }
};

PresentationTargets ResolvePresentationTargets(ue::UObject* actor);

bool GetActorVelocity(ue::UObject* actor, ue::FVector* out);

void WritePresentationVelocity(const PresentationTargets& targets,
                               const ue::FVector& velocity);

bool SetMovementSpeedState(ue::UObject* movement_component, int state);

bool SetActorSpeedState(ue::UObject* actor, int state);
bool RequestDirectMove(ue::UObject* actor, const ue::FVector& desired_velocity,
                       bool force_max_speed = false);

bool TeleportActor(ue::UObject* actor, const ue::FVector& location,
                   const ue::FRotator& rotation);

void GetTeleportFallbackCounts(std::uint32_t* fallbacks, std::uint32_t* hard_failures);

namespace relationship {
constexpr int kUnknown = -1;
constexpr int kEnemy = 0;
constexpr int kFight = 1;
constexpr int kObject = 2;
constexpr int kNeutral = 3;
constexpr int kCoop = 4;
constexpr int kAlly = 5;
constexpr int kCount = 6;
const char* Name(int value);
}

ue::UObject* GetSocialComponent(ue::UObject* character);

int ReadRelationship(ue::UObject* from_actor, ue::UObject* to_actor);

int ReadRelationshipViaComponent(ue::UObject* from_actor, ue::UObject* to_actor);

bool WriteRelationship(ue::UObject* social, ue::UObject* toward, int value);

int RelationshipMapSize(ue::UObject* social);
bool RelationshipMapProbeTrusted();

float GetActorTimeDilation(ue::UObject* actor);
bool SetActorTimeDilation(ue::UObject* actor, float dilation);

constexpr float kPooledZ = -900000.f;
bool IsPooled(const ue::FVector& location);

std::uint32_t HashName(const char* text);
bool LeafName(ue::UObject* object, char* out, int out_size);

bool SplitRuntimeSuffix(char* name, std::uint32_t* number_out);

std::uint32_t HashNameWithOrdinal(const char* text, int ordinal);
std::uint32_t ActorHash(ue::UObject* actor);

}
