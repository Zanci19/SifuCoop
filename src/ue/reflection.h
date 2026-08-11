#pragma once

#include <cstdint>

// Minimal mirrors of UE 4.26 types. Only layouts that are ABI-stable and
// verified against the shipped build appear here -- everything else goes
// through the reflection layer instead of a hand-written struct.

namespace sifucoop::ue {

struct FVector {
    float X = 0.f, Y = 0.f, Z = 0.f;
};

struct FRotator {
    float Pitch = 0.f, Yaw = 0.f, Roll = 0.f;
};

// FName in 4.26 without FNAME_OUTLINE_NUMBER: two int32s.
struct FName {
    std::int32_t comparison_index = 0;
    std::int32_t number = 0;
};

// Opaque -- we only ever hold pointers and pass them back to the game.
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

// Plays a raw UAnimationAsset as a dynamic montage through the player
// AnimBlueprint's Cinematic slot, without entering Sifu's UAttackComponent.
// This produces a visible strike but no hitbox, target selection or damage,
// and it never tears down the locomotion graph.
bool PlayAnimationAsset(UObject* actor, UObject* animation_asset, float start_at = 0.f);

// Restores the mesh to its AnimBlueprint after PlayAnimationAsset. UE switches
// the component to single-node mode while a raw sequence is playing.
// The animation blueprint class an actor is currently running, so it can be
// named again after single-node playback has cleared the instance.
void* GetAnimInstanceClass(UObject* actor);

// Put the animation blueprint back after PlayAnimationAsset. Pass the class
// captured before playback; it is the fallback for when asking for
// AnimationBlueprint mode alone does not produce an instance, which is the
// difference between the character animating again and standing in a T-pose.
bool RestoreAnimationBlueprint(UObject* actor, void* anim_class);

// UKismetSystemLibrary::IsValid -- false for null and for anything already
// marked pending kill. Worth a ProcessEvent only where handing a dead actor to
// the game is fatal rather than merely wrong: Sifu's relationship multicast
// walks the actor it is given, so a pointer from a level that is being torn
// down takes the process with it.
bool IsValidObject(UObject* object);

// UAnimationAsset::GetPlayLength, reflected so raw sequence replay knows when
// to return control to Sifu's normal locomotion AnimBlueprint.
float GetAnimationAssetLength(UObject* animation_asset);

// The UAnimInstance driving an actor's skeletal mesh, or null.
UObject* GetAnimInstance(UObject* actor);

// --- Cross-machine object identity ----------------------------------------
//
// Object pointers are process-local, so they cannot be sent to a peer. Both
// peers run the identical build and load identical assets, so an object's path
// name ("/Game/.../DA_Attack_Light_01.DA_Attack_Light_01") is a stable
// identifier on both sides. These two calls are the bridge.

// Writes `object`'s full path name into `out` as UTF-8. False if unavailable.
bool GetObjectPathName(UObject* object, char* out, int out_size);

// The object's CLASS as a path name, and a leaf-name comparison built on it.
// Only ever call these on pointers already known to be UObjects -- they are for
// asking "what IS this", not "is this an object at all".
bool GetObjectClassPathName(UObject* object, char* out, int out_size);
bool ObjectClassIs(UObject* object, const char* leaf_name);

// Resolves a path name produced by GetObjectPathName back to a live object.
UObject* FindObjectByPath(const wchar_t* path_name);

// --- Level travel ----------------------------------------------------------

// Package path of the level currently loaded, e.g.
// "/Game/Maps/Hideout3/Hideout_3_Main". False when there is no world yet.
// Read at runtime rather than from a hardcoded list -- map names live inside
// the encrypted pak, and this works for levels we have never seen.
bool GetCurrentLevelPath(char* out, int out_size);

// Travels to `level_path`. This is how the joining player is pulled into the
// host's level; the host reaches theirs through Sifu's own menus so their game
// state is set up normally.
bool OpenLevel(const char* level_path);

// Runs a UE console command through the engine's reflected Kismet bridge. This
// is deliberately limited to the mod's own fixed commands; callers must never
// feed it untrusted network text.
bool ExecuteConsoleCommand(const char* command, UObject* specific_player = nullptr);

}  // namespace sifucoop::ue
