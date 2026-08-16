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


bool MakeFName(const wchar_t* text, FName* out);


struct UObject;
struct UFunction;



bool InitReflection(std::uintptr_t module_base);




bool CallFunction(UObject* object, const wchar_t* function_name, void* params);


bool GetActorLocation(UObject* actor, FVector* out);
bool GetActorRotation(UObject* actor, FRotator* out);


UObject* GetPlayerCharacter(UObject* world_context, int index);


UObject* GetWorld();



struct AnimState {
    UObject* montage = nullptr;
    float position = 0.f;
};



bool ReadAnimState(UObject* actor, AnimState* out);


bool ApplyAnimState(UObject* actor, const AnimState& state);





bool PlayAnimationAsset(UObject* actor, UObject* animation_asset, float start_at = 0.f);





void* GetAnimInstanceClass(UObject* actor);





bool RestoreAnimationBlueprint(UObject* actor, void* anim_class);






bool IsValidObject(UObject* object);



float GetAnimationAssetLength(UObject* animation_asset);


UObject* GetSkeletalMeshComponent(UObject* actor);
UObject* GetAnimInstance(UObject* actor);









bool GetObjectPathName(UObject* object, char* out, int out_size);




bool GetObjectClassPathName(UObject* object, char* out, int out_size);
bool ObjectClassIs(UObject* object, const char* leaf_name);


UObject* FindObjectByPath(const wchar_t* path_name);







bool GetCurrentLevelPath(char* out, int out_size);




bool OpenLevel(const char* level_path);




bool ExecuteConsoleCommand(const char* command, UObject* specific_player = nullptr);

}
