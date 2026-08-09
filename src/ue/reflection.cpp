#include "reflection.h"

#include <windows.h>

#include <cstring>

#include "../core/log.h"
#include "../core/offsets.g.h"

namespace sifucoop::ue {
namespace {

namespace offsets = sifucoop::offsets;

constexpr int kFNameAdd = 1;

using FNameCtorFn = FName*(__fastcall*)(FName*, const wchar_t*, int);
using FindFunctionFn = UFunction*(__fastcall*)(const UObject*, FName);
using ProcessEventFn = void(__fastcall*)(UObject*, UFunction*, void*);
using GetPlayerCharacterFn = UObject*(__fastcall*)(const UObject*, int);
using StaticClassFn = void*(__fastcall*)();
using MontageGetPositionFn = float(__fastcall*)(const UObject*, const UObject*);
using MontagePlayFn = float(__fastcall*)(UObject*, UObject*, float, int, float, bool);
using GetPathNameFn = void(__fastcall*)(const UObject* self, const UObject* stop_outer,
                                        void* out_string);
using StaticFindObjectSafeFn = UObject*(__fastcall*)(void* uclass, UObject* outer,
                                                     const wchar_t* name, bool exact_class);
// void OpenLevel(const UObject*, FName, bool, FString); FName is 8 bytes and it is in a register; FString is 16 and is passed by address
using OpenLevelFn = void(__fastcall*)(const UObject* world_context, FName level,
                                      bool absolute, void* options);

FNameCtorFn g_fname_ctor = nullptr;
FindFunctionFn g_find_function = nullptr;
ProcessEventFn g_process_event = nullptr;
GetPlayerCharacterFn g_get_player_character = nullptr;
StaticClassFn g_skeletal_mesh_class = nullptr;
MontageGetPositionFn g_montage_get_position = nullptr;
MontagePlayFn g_montage_play = nullptr;
GetPathNameFn g_get_path_name = nullptr;
StaticFindObjectSafeFn g_static_find_object_safe = nullptr;
OpenLevelFn g_open_level = nullptr;
UObject** g_gworld = nullptr;

bool g_ready = false;

FName MakeName(const wchar_t* text) {
    FName name = {};
    g_fname_ctor(&name, text, kFNameAdd);
    return name;
}

}  // namespace

bool InitReflection(std::uintptr_t base) {
    g_fname_ctor = reinterpret_cast<FNameCtorFn>(base + offsets::FName_FromWide);
    g_find_function = reinterpret_cast<FindFunctionFn>(base + offsets::UObject_FindFunction);
    g_process_event = reinterpret_cast<ProcessEventFn>(base + offsets::UObject_ProcessEvent);
    g_get_player_character = reinterpret_cast<GetPlayerCharacterFn>(
        base + offsets::UGameplayStatics_GetPlayerCharacter);
    g_skeletal_mesh_class =
        reinterpret_cast<StaticClassFn>(base + offsets::USkeletalMeshComponent_StaticClass);
    g_montage_get_position = reinterpret_cast<MontageGetPositionFn>(
        base + offsets::UAnimInstance_Montage_GetPosition);
    g_montage_play =
        reinterpret_cast<MontagePlayFn>(base + offsets::UAnimInstance_Montage_Play);
    g_get_path_name =
        reinterpret_cast<GetPathNameFn>(base + offsets::UObjectBaseUtility_GetPathName);
    g_static_find_object_safe =
        reinterpret_cast<StaticFindObjectSafeFn>(base + offsets::StaticFindObjectSafe);
    g_open_level =
        reinterpret_cast<OpenLevelFn>(base + offsets::UGameplayStatics_OpenLevel);
    g_gworld = reinterpret_cast<UObject**>(base + offsets::GWorld);

    g_ready = g_fname_ctor && g_find_function && g_process_event && g_get_player_character &&
              g_gworld;
    SC_LOG("reflection: %s", g_ready ? "ready" : "FAILED to resolve entry points");
    return g_ready;
}

bool CallFunction(UObject* object, const wchar_t* function_name, void* params) {
    if (!g_ready || !object) return false;

    const FName name = MakeName(function_name);
    UFunction* function = g_find_function(object, name);
    if (!function) return false;

    g_process_event(object, function, params);
    return true;
}

bool GetActorLocation(UObject* actor, FVector* out) {
    struct Params {
        FVector ReturnValue;
    } params = {};
    if (!CallFunction(actor, L"K2_GetActorLocation", &params)) return false;
    *out = params.ReturnValue;
    return true;
}

bool GetActorRotation(UObject* actor, FRotator* out) {
    struct Params {
        FRotator ReturnValue;
    } params = {};
    if (!CallFunction(actor, L"K2_GetActorRotation", &params)) return false;
    *out = params.ReturnValue;
    return true;
}

UObject* GetPlayerCharacter(UObject* world_context, int index) {
    if (!g_ready || !world_context) return nullptr;
    return g_get_player_character(world_context, index);
}

UObject* GetWorld() {
    // GWorld is a UWorldProxy whose first member is the UWorld*.
    if (!g_gworld) return nullptr;
    return *g_gworld;
}

bool GetObjectPathName(UObject* object, char* out, int out_size) {
    if (!g_get_path_name || !object || out_size <= 0) return false;

    struct FString {
        wchar_t* data;
        std::int32_t num;
        std::int32_t max;
    } result = {};

    // out-parameter overload: nothing is returned by value
    g_get_path_name(object, nullptr, &result);

    if (!result.data || result.num <= 0) return false;

    const int written = WideCharToMultiByte(CP_UTF8, 0, result.data, -1, out, out_size, nullptr,
                                            nullptr);
    out[out_size - 1] = '\0';
    return written > 0;
}

UObject* FindObjectByPath(const wchar_t* path_name) {
    if (!g_static_find_object_safe || !path_name) return nullptr;
    // (UClass* Class = null -> any type, UObject* Outer = null, name, bExactClass = false)
    return g_static_find_object_safe(nullptr, nullptr, path_name, false);
}

bool GetCurrentLevelPath(char* out, int out_size) {
    UObject* world = GetWorld();
    if (!world) return false;

    char full[512] = {};
    if (!GetObjectPathName(world, full, sizeof(full))) return false;

    // A world's path is "/Game/Maps/X/Y.Y"
    char* dot = strrchr(full, '.');
    if (dot) *dot = '\0';

    lstrcpynA(out, full, out_size);
    return out[0] != '\0';
}

bool OpenLevelWithOptions(const char* level_path, const char* option_text) {
    if (!g_ready || !g_open_level || !level_path || !level_path[0]) return false;

    UObject* world = GetWorld();
    if (!world) return false;

    wchar_t wide_level[512] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, level_path, -1, wide_level, 512) <= 0) return false;
    const FName name = MakeName(wide_level);

    wchar_t wide_options[128] = {};
    struct FString {
        wchar_t* data;
        std::int32_t num;
        std::int32_t max;
    } options = {};
    if (option_text && option_text[0]) {
        const int chars = MultiByteToWideChar(CP_UTF8, 0, option_text, -1, wide_options,
                                              static_cast<int>(sizeof(wide_options) / sizeof(wide_options[0])));
        if (chars <= 0) return false;
        options = {wide_options, chars, chars};
    }

    SC_LOG("level: opening '%s' options='%s'", level_path,
           option_text && option_text[0] ? option_text : "");
    g_open_level(world, name, false, &options);
    return true;
}

bool OpenLevel(const char* level_path) {
    return OpenLevelWithOptions(level_path, nullptr);
}
bool ExecuteConsoleCommand(const char* command, UObject* specific_player) {
    if (!command || !command[0]) return false;
    UObject* world = GetWorld();
    UObject* kismet = FindObjectByPath(L"/Script/Engine.Default__KismetSystemLibrary");
    if (!world || !kismet) return false;

    wchar_t wide[512] = {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, command, -1, wide, 512);
    if (chars <= 0) return false;
    struct FString {
        wchar_t* data;
        std::int32_t num;
        std::int32_t max;
    } text = {wide, chars, chars};
    struct Params {
        UObject* WorldContextObject;
        FString Command;
        UObject* SpecificPlayer;
    } params = {world, text, specific_player};
    return CallFunction(kismet, L"ExecuteConsoleCommand", &params);
}

UObject* GetAnimInstance(UObject* actor) {
    if (!g_ready || !actor || !g_skeletal_mesh_class) return nullptr;

    // AActor::GetComponentByClass is BlueprintCallable (sloclap w)
    struct ComponentParams {
        void* ComponentClass;
        UObject* ReturnValue;
    } component_params = {};
    component_params.ComponentClass = g_skeletal_mesh_class();
    if (!CallFunction(actor, L"GetComponentByClass", &component_params)) return nullptr;
    if (!component_params.ReturnValue) return nullptr;

    struct AnimParams {
        UObject* ReturnValue;
    } anim_params = {};
    if (!CallFunction(component_params.ReturnValue, L"GetAnimInstance", &anim_params)) {
        return nullptr;
    }
    return anim_params.ReturnValue;
}

bool ReadAnimState(UObject* actor, AnimState* out) {
    UObject* anim_instance = GetAnimInstance(actor);
    if (!anim_instance) return false;

    struct MontageParams {
        UObject* ReturnValue;
    } montage_params = {};
    if (!CallFunction(anim_instance, L"GetCurrentActiveMontage", &montage_params)) return false;

    out->montage = montage_params.ReturnValue;
    out->position = 0.f;

    if (out->montage && g_montage_get_position) {
        out->position = g_montage_get_position(anim_instance, out->montage);
    }
    return true;
}

bool ApplyAnimState(UObject* actor, const AnimState& state) {
    if (!state.montage || !g_montage_play) return false;
    UObject* anim_instance = GetAnimInstance(actor);
    if (!anim_instance) return false;

    // EMontagePlayReturnType::MontageLength = 0.
    g_montage_play(anim_instance, state.montage, 1.f, 0, state.position, true);
    return true;
}

}  // namespace sifucoop::ue


