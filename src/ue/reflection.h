#pragma once

#include <cstdint>

namespace sifucoop::ue {

struct FVector {
    float X = 0.f, Y = 0.f, Z = 0.f;
};

struct FRotator {
    float Pitch = 0.f, Yaw = 0.f, Roll = 0.f;
};

struct FName {
    std::int32_t comparison_index = 0;
    std::int32_t number = 0;
};

struct UObject;
struct UFunction;

// Resolves the engine entry points against the running module. Must be called
// once before anything else here. Returns false if any target is missing.
bool InitReflection(std::uintptr_t module_base);

// Calls a Blueprint-exposed UFunction by name. `params` must match the
// function's parameter layout exactly (inputs first, return value last).
// Returns false if the function could not be found on the object.
bool CallFunction(UObject* object, const wchar_t* function_name, void* params);

// Convenience wrappers over the above, for the calls the mod makes constantly.
bool GetActorLocation(UObject* actor, FVector* out);
bool GetActorRotation(UObject* actor, FRotator* out);

// UGameplayStatics::GetPlayerCharacter(world_context, index) -> ACharacter*
UObject* GetPlayerCharacter(UObject* world_context, int index);

// The current UWorld, read from the GWorld proxy.
UObject* GetWorld();

// --- Animation (M4) -------------------------------------------------------

struct AnimState {
    UObject* montage = nullptr;  // currently active UAnimMontage, may be null
    float position = 0.f;        // playback position in seconds
};

// Walks actor -> SkeletalMeshComponent -> AnimInstance -> active montage.
// Returns false if any link is missing (e.g. actor has no mesh yet).
bool ReadAnimState(UObject* actor, AnimState* out);

// Plays `montage` on `actor` and seeks it to `position`.
bool ApplyAnimState(UObject* actor, const AnimState& state);

// The UAnimInstance driving an actor's skeletal mesh, or null
UObject* GetAnimInstance(UObject* actor);

// Writes `object`'s full path name into `out` as UTF-8. False if unavailable.
bool GetObjectPathName(UObject* object, char* out, int out_size);

// Resolves a path name produced by GetObjectPathName back to a live object.
UObject* FindObjectByPath(const wchar_t* path_name);

// Package path of the level currently loaded
bool GetCurrentLevelPath(char* out, int out_size);

bool OpenLevel(const char* level_path);

// args to lvl
bool OpenLevelWithOptions(const char* level_path, const char* options);

bool ExecuteConsoleCommand(const char* command, UObject* specific_player = nullptr);

}  // namespace sifucoop::ue
