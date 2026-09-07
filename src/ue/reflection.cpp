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

bool RangeReadable(const void* address, std::size_t size) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) return false;
    if (info.State != MEM_COMMIT) return false;
    constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
    if (info.Protect & kNoRead) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = start + info.RegionSize;
    return reinterpret_cast<std::uintptr_t>(address) + size <= end;
}
std::uintptr_t g_module_base = 0;

FName MakeName(const wchar_t* text) {
    FName name = {};
    g_fname_ctor(&name, text, kFNameAdd);
    return name;
}

}

bool InitReflection(std::uintptr_t base) {
    g_module_base = base;
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

bool MakeFName(const wchar_t* text, FName* out) {
    if (!g_ready || !text || !out) return false;
    *out = MakeName(text);
    return true;
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

    g_get_path_name(object, nullptr, &result);

    if (!result.data || result.num <= 0) return false;

    const int written = WideCharToMultiByte(CP_UTF8, 0, result.data, -1, out, out_size, nullptr,
                                            nullptr);
    out[out_size - 1] = '\0';
    return written > 0;
}

UObject* FindObjectByPath(const wchar_t* path_name) {
    if (!g_static_find_object_safe || !path_name) return nullptr;

    return g_static_find_object_safe(nullptr, nullptr, path_name, false);
}

bool GetCurrentLevelPath(char* out, int out_size) {
    UObject* world = GetWorld();
    if (!world) return false;

    char full[512] = {};
    if (!GetObjectPathName(world, full, sizeof(full))) return false;

    char* dot = strrchr(full, '.');
    if (dot) *dot = '\0';

    lstrcpynA(out, full, out_size);
    return out[0] != '\0';
}

bool OpenLevel(const char* level_path) {
    if (!g_ready || !g_open_level || !level_path || !level_path[0]) return false;

    UObject* world = GetWorld();
    if (!world) return false;

    wchar_t wide[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, level_path, -1, wide, 512);

    const FName name = MakeName(wide);

    struct FString {
        wchar_t* data;
        std::int32_t num;
        std::int32_t max;
    } options = {};

    SC_LOG("level: opening '%s'", level_path);
    g_open_level(world, name, false, &options);
    return true;
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

UObject* GetSkeletalMeshComponent(UObject* actor) {
    if (!g_ready || !actor || !g_skeletal_mesh_class) return nullptr;

    struct ComponentParams {
        void* ComponentClass;
        UObject* ReturnValue;
    } params = {};
    params.ComponentClass = g_skeletal_mesh_class();
    if (!CallFunction(actor, L"GetComponentByClass", &params)) return nullptr;
    return params.ReturnValue;
}

UObject* GetAnimInstance(UObject* actor) {
    UObject* mesh = GetSkeletalMeshComponent(actor);
    if (!mesh) return nullptr;
    struct AnimParams {
        UObject* ReturnValue;
    } anim_params = {};
    if (!CallFunction(mesh, L"GetAnimInstance", &anim_params)) return nullptr;
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

    g_montage_play(anim_instance, state.montage, 1.f, 0, state.position, true);
    return true;
}

bool PlayAnimationAsset(UObject* actor, UObject* animation_asset, float start_at) {
    if (!animation_asset) return false;
    UObject* anim_instance = GetAnimInstance(actor);
    if (!anim_instance) return false;

    struct Params {
        UObject* Asset;
        FName SlotNodeName;
        float BlendInTime;
        float BlendOutTime;
        float InPlayRate;
        std::int32_t LoopCount;
        float BlendOutTriggerTime;
        float InTimeToStartMontageAt;
        UObject* ReturnValue;
    } params = {};
    params.Asset = animation_asset;
    params.SlotNodeName = MakeName(L"Cinematic");
    params.BlendInTime = 0.03f;
    params.BlendOutTime = 0.08f;
    params.InPlayRate = 1.f;
    params.LoopCount = 1;
    params.BlendOutTriggerTime = -1.f;

    params.InTimeToStartMontageAt = start_at > 0.f ? start_at : 0.f;
    if (!CallFunction(anim_instance, L"PlaySlotAnimationAsDynamicMontage", &params)) {
        return false;
    }
    return params.ReturnValue != nullptr;
}

bool GetObjectClassPathName(UObject* object, char* out, int out_size) {
    if (!object || !out || out_size <= 0) return false;
    out[0] = 0;
    auto* klass = *reinterpret_cast<UObject**>(reinterpret_cast<std::uintptr_t>(object) + 0x10);
    if (!klass) return false;
    return GetObjectPathName(klass, out, out_size);
}

bool ObjectClassIs(UObject* object, const char* leaf_name) {
    char path[160] = {};
    if (!GetObjectClassPathName(object, path, sizeof(path))) return false;
    const char* dot = strrchr(path, '.');
    const char* leaf = dot ? dot + 1 : path;
    return lstrcmpA(leaf, leaf_name) == 0;
}

void* GetAnimInstanceClass(UObject* actor) {
    UObject* instance = GetAnimInstance(actor);
    if (!instance) return nullptr;

    return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(instance) + 0x10);
}

bool RestoreAnimationBlueprint(UObject* actor, void* anim_class) {
    UObject* mesh = GetSkeletalMeshComponent(actor);
    if (!mesh) return false;

    struct ModeParams {
        std::uint8_t InAnimationMode;
        bool bForceInitAnimScript;
    } mode = {0, true};
    CallFunction(mesh, L"SetAnimationMode", &mode);

    if (GetAnimInstance(actor)) return true;

    if (!anim_class) return false;
    struct ClassParams {
        void* NewClass;
    } klass = {anim_class};
    if (!CallFunction(mesh, L"SetAnimInstanceClass", &klass)) return false;
    return GetAnimInstance(actor) != nullptr;
}

// Safe to READ; not proof of liveness. See CODE-NOTES.md.
bool IsValidObject(UObject* object) {
    if (!object || g_module_base == 0) return false;

    const auto address = reinterpret_cast<std::uintptr_t>(object);
    if (address < 0x10000 || (address & 7) != 0) return false;
    if (!RangeReadable(object, 0x20)) return false;

    constexpr std::uintptr_t kModuleSpan = 0x10000000u;
    const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
    if (vtable < g_module_base || vtable - g_module_base > kModuleSpan) return false;

    // UObjectBase::ClassPrivate.
    const auto class_private = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<const std::uint8_t*>(object) + 0x10);
    if (class_private < 0x10000 || (class_private & 7) != 0) return false;
    if (!RangeReadable(reinterpret_cast<const void*>(class_private), 8)) return false;

    const auto class_vtable = *reinterpret_cast<const std::uintptr_t*>(class_private);
    return class_vtable >= g_module_base && class_vtable - g_module_base <= kModuleSpan;
}

float GetAnimationAssetLength(UObject* animation_asset) {
    if (!animation_asset) return 0.f;
    struct Params {
        float ReturnValue;
    } params = {};
    if (!CallFunction(animation_asset, L"GetPlayLength", &params)) return 0.f;
    return params.ReturnValue;
}

}
