#include "player2.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../../third_party/minhook/include/MinHook.h"
#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../ue/reflection.h"
#include "actors.h"
#include "coop.h"
#include "puppet.h"

namespace sifucoop::game {
namespace {

namespace ue = sifucoop::ue;
namespace offsets = sifucoop::offsets;
namespace coop = sifucoop::coop;

ue::UObject* GameplayStatics() {
    return ue::FindObjectByPath(L"/Script/Engine.Default__GameplayStatics");
}

ue::UObject* g_controller = nullptr;

ue::UObject* g_controller_world = nullptr;
int g_travel_settle_frames = 0;
std::uintptr_t g_base = 0;

struct FKey {
    std::uint32_t comparison_index;
    std::uint32_t number;
};
static_assert(sizeof(FKey) == 8, "UE4 FKey ABI changed");

using PlayerControllerInputKeyFn =
    bool(__fastcall*)(void* self, FKey key, std::uint32_t input_event, float amount, bool gamepad);

PlayerControllerInputKeyFn g_input_key = nullptr;
bool g_input_hook_installed = false;

bool __fastcall PlayerControllerInputKeyHook(void* self, FKey key, std::uint32_t input_event,
                                             float amount, bool gamepad) {

    if (self && self == g_controller) return false;
    return g_input_key ? g_input_key(self, key, input_event, amount, gamepad) : false;
}

bool EnsureInputHook() {
    if (g_input_hook_installed) return true;
    if (g_base == 0 || offsets::APlayerController_InputKey == 0) {
        SC_LOG("player2: APlayerController::InputKey offset unavailable");
        return false;
    }

    void* target = reinterpret_cast<void*>(g_base + offsets::APlayerController_InputKey);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&PlayerControllerInputKeyHook),
                      reinterpret_cast<void**>(&g_input_key)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        SC_LOG("player2: could not hook APlayerController::InputKey at %p", target);
        return false;
    }

    g_input_hook_installed = true;
    SC_LOG("player2: APlayerController::InputKey hooked at %p", target);
    return true;
}

using SetHudFn = void(__fastcall*)(void* self, void* widget);

void* g_set_hud_trampoline = nullptr;
bool g_hud_hook_installed = false;

ue::UObject* g_primary_controller = nullptr;
bool g_creating_second_player = false;

void __fastcall SetHudHook(void* self, void* widget) {
    const bool is_second =
        self && (self == g_controller ||
                 (g_creating_second_player && self != g_primary_controller));
    if (is_second && coop::Get().hide_second_player_hud) {

        if (widget) {
            struct Empty {
            } none = {};
            ue::CallFunction(static_cast<ue::UObject*>(widget), L"RemoveFromParent", &none);
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            SC_LOG("player2: declined the second player's HUD (it would draw over yours)");
        }
        return;
    }
    if (g_set_hud_trampoline) reinterpret_cast<SetHudFn>(g_set_hud_trampoline)(self, widget);
}

bool EnsureHudHook() {
    if (g_hud_hook_installed) return true;
    if (g_base == 0 || offsets::AFightingPlayerController_BPF_SetHUD == 0) {
        SC_LOG("player2: BPF_SetHUD offset unavailable on this build -- the second player's "
               "HUD will draw over yours (set hide_second_player_hud=0 to stop asking)");
        return false;
    }
    void* target =
        reinterpret_cast<void*>(g_base + offsets::AFightingPlayerController_BPF_SetHUD);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&SetHudHook), &g_set_hud_trampoline) !=
            MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        SC_LOG("player2: could not hook BPF_SetHUD at %p", target);
        return false;
    }
    g_hud_hook_installed = true;
    SC_LOG("player2: BPF_SetHUD hooked at %p", target);
    return true;
}

bool InPlayableWorld(ue::UObject* world) {
    if (!coop::Get().second_player_in_gameplay_only) return true;
    if (!world) return false;
    if (!ue::GetPlayerCharacter(world, 0)) return false;

    char level[192] = {};
    if (!ue::GetCurrentLevelPath(level, sizeof(level))) return false;
    static const char* const kNonGameplay[] = {"SelectHideoutLevel", "MainMenu", "Frontend",
                                               "Startup", "EntryLevel", "Hideout_0_Main"};
    for (const char* fragment : kNonGameplay) {
        if (strstr(level, fragment) != nullptr) return false;
    }
    return true;
}

bool g_retired = false;

int g_view_reassert_frames = 0;

void ForceDisableSplitscreen(ue::UObject* world, bool disable) {
    ue::UObject* statics = GameplayStatics();
    if (!statics) return;

    struct Params {
        ue::UObject* WorldContextObject;
        std::uint8_t bDisable[8];
    } params = {};
    params.WorldContextObject = world;
    if (disable) {
        for (int i = 0; i < 4; ++i) params.bDisable[i] = 1;
    }

    if (ue::CallFunction(statics, L"SetForceDisableSplitscreen", &params)) {
        SC_LOG("player2: splitscreen force-disable %s", disable ? "ON" : "OFF");
    } else {
        SC_LOG("player2: SetForceDisableSplitscreen unavailable -- expect a split view");
    }
}

ue::UObject* PlayerControllerAt(ue::UObject* world, std::int32_t index) {
    if (!world) return nullptr;
    ue::UObject* statics = GameplayStatics();
    if (!statics) return nullptr;
    struct GetPcParams {
        ue::UObject* WorldContextObject;
        std::int32_t PlayerIndex;
        std::uint8_t pad[4];
        ue::UObject* ReturnValue;
    } params = {};
    params.WorldContextObject = world;
    params.PlayerIndex = index;
    if (!ue::CallFunction(statics, L"GetPlayerController", &params)) return nullptr;
    return params.ReturnValue;
}

bool SecondPlayerWorldReady(ue::UObject* world) {
    if (!world) return false;
    if (world != g_controller_world) {
        const bool had_controller = g_controller != nullptr;
        g_controller_world = world;
        g_travel_settle_frames = had_controller ? 120 : 0;
        if (had_controller) {
            SC_LOG("player2: new world detected -- deferring second-player access for %d frames",
                   g_travel_settle_frames);
            return false;
        }
    }
    if (g_travel_settle_frames > 0) {
        --g_travel_settle_frames;
        return false;
    }
    return true;
}

ue::UObject* PawnOf(ue::UObject* controller) {
    if (!controller) return nullptr;
    struct Params {
        ue::UObject* ReturnValue;
    } params = {};
    if (!ue::CallFunction(controller, L"K2_GetPawn", &params)) return nullptr;
    return params.ReturnValue;
}

bool Possess(ue::UObject* controller, ue::UObject* pawn) {
    if (!controller || !pawn) return false;
    struct Params {
        ue::UObject* InPawn;
    } params = {};
    params.InPawn = pawn;
    return ue::CallFunction(controller, L"Possess", &params);
}

bool UnPossess(ue::UObject* controller) {
    if (!controller) return false;
    struct Empty {
    } none = {};
    return ue::CallFunction(controller, L"UnPossess", &none);
}

bool ReturnStolenPawnAndRehouse(ue::UObject* second_controller,
                                ue::UObject* first_controller,
                                ue::UObject* stolen_pawn) {
    if (!UnPossess(second_controller)) {
        SC_LOG("player2: UnPossess failed -- cannot return player one's character");
        return false;
    }
    if (!Possess(first_controller, stolen_pawn)) {
        SC_LOG("player2: FAILED to give player one back their character (%p)",
               static_cast<void*>(stolen_pawn));
        return false;
    }
    SC_LOG("player2: returned player one's character (%p)", static_cast<void*>(stolen_pawn));

    ue::FVector location = {};
    ue::FRotator rotation = {};
    if (!ue::GetActorLocation(stolen_pawn, &location) ||
        !ue::GetActorRotation(stolen_pawn, &rotation)) {
        SC_LOG("player2: could not read player one's transform to place the new body");
        return false;
    }
    const float yaw_radians = rotation.Yaw * 3.14159265f / 180.f;
    location.X += 150.f * cosf(yaw_radians);
    location.Y += 150.f * sinf(yaw_radians);

    ue::UObject* fresh = SpawnPlayerClone(location, rotation);
    if (!fresh) {
        SC_LOG("player2: could not spawn a body for the second player");
        return false;
    }
    if (!Possess(second_controller, fresh)) {
        SC_LOG("player2: spawned a body (%p) but the second controller refused to possess it",
               static_cast<void*>(fresh));
        return false;
    }
    SC_LOG("player2: second player now possesses its OWN body %p at (%.0f, %.0f, %.0f)",
           static_cast<void*>(fresh), location.X, location.Y, location.Z);
    return true;
}

ue::UObject* RehouseSecondPlayerAfterTravel(ue::UObject* second_controller) {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* first_controller = PlayerControllerAt(world, 0);
    ue::UObject* first_pawn = PawnOf(first_controller);
    if (!second_controller || !first_pawn) {
        SC_LOG("player2: cannot rebuild second body -- primary pawn is not ready");
        return nullptr;
    }

    ue::FVector location = {};
    ue::FRotator rotation = {};
    if (!ue::GetActorLocation(first_pawn, &location) ||
        !ue::GetActorRotation(first_pawn, &rotation)) {
        SC_LOG("player2: cannot read primary transform for post-travel body");
        return nullptr;
    }
    const float yaw_radians = rotation.Yaw * 3.14159265f / 180.f;
    location.X += 150.f * cosf(yaw_radians);
    location.Y += 150.f * sinf(yaw_radians);

    ue::UObject* fresh = SpawnPlayerClone(location, rotation);
    if (!fresh) {
        SC_LOG("player2: failed to spawn a post-travel second body");
        return nullptr;
    }
    if (!Possess(second_controller, fresh)) {
        SC_LOG("player2: post-travel body %p could not be possessed", static_cast<void*>(fresh));
        return nullptr;
    }
    SC_LOG("player2: rebuilt second body %p after travel at (%.0f, %.0f, %.0f)",
           static_cast<void*>(fresh), location.X, location.Y, location.Z);
    return fresh;
}

void LogPlayerTable(const char* when) {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* statics = GameplayStatics();
    if (!world || !statics) return;

    for (int i = 0; i < 3; ++i) {
        struct GetPcParams {
            ue::UObject* WorldContextObject;
            std::int32_t PlayerIndex;
            std::uint8_t pad[4];
            ue::UObject* ReturnValue;
        } pc = {};
        pc.WorldContextObject = world;
        pc.PlayerIndex = i;
        if (!ue::CallFunction(statics, L"GetPlayerController", &pc) || !pc.ReturnValue) {
            if (i == 0) SC_LOG("players[%s]: index %d -> NO CONTROLLER", when, i);
            continue;
        }
        ue::UObject* pawn = PawnOf(pc.ReturnValue);
        char path[192] = {};
        if (pawn) ue::GetObjectPathName(pawn, path, sizeof(path));
        SC_LOG("players[%s]: index %d controller=%p pawn=%p %s", when, i,
               static_cast<void*>(pc.ReturnValue), static_cast<void*>(pawn),
               pawn ? path : "(none)");
    }
}

void RestorePrimaryView() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* statics = GameplayStatics();
    if (!world || !statics) return;

    struct GetPcParams {
        ue::UObject* WorldContextObject;
        std::int32_t PlayerIndex;
        std::uint8_t pad[4];
        ue::UObject* ReturnValue;
    } pc = {};
    pc.WorldContextObject = world;
    pc.PlayerIndex = 0;
    if (!ue::CallFunction(statics, L"GetPlayerController", &pc) || !pc.ReturnValue) {
        SC_LOG("player2: could not resolve player one's controller to restore the camera");
        return;
    }

    ue::UObject* first_pawn = PawnOf(pc.ReturnValue);
    if (!first_pawn) {
        SC_LOG("player2: player one has no pawn to look at");
        return;
    }

    struct ViewParams {
        ue::UObject* NewViewTarget;
        std::uint8_t bLockOutgoing[8];
        float BlendTime;
    } unused = {};
    (void)unused;

    struct alignas(8) Params {
        ue::UObject* NewViewTarget;
        float BlendTime;
        std::uint8_t BlendFunc;
        std::uint8_t pad0[3];
        float BlendExp;
        std::uint8_t pad1[4];
    } params = {};
    params.NewViewTarget = first_pawn;
    params.BlendTime = 0.f;

    const bool ok = ue::CallFunction(pc.ReturnValue, L"SetViewTargetWithBlend", &params);
    static ue::UObject* last_logged = nullptr;
    if (last_logged != first_pawn) {
        last_logged = first_pawn;
        SC_LOG("player2: camera restored to player one (%s), pawn %p", ok ? "ok" : "FAILED",
               static_cast<void*>(first_pawn));
    }
}

struct SuppressedComponent {
    std::uint32_t rva;
    const char* name;
    void* trampoline;
};

SuppressedComponent g_suppressed[] = {
    {0, "UTargetableWidgetUpdaterComponent::BeginPlay", nullptr},
    {0, "UWidgetPoolComponent::BeginPlay", nullptr},
};

bool g_suppress_ui = false;
bool g_ui_hooks_installed = false;

using ComponentBeginPlayFn = void(__fastcall*)(void* self);

template <int Index>
void __fastcall SuppressedBeginPlayHook(void* self) {
    if (g_suppress_ui) {
        SC_LOG("player2: skipped %s for the second player", g_suppressed[Index].name);
        return;
    }
    reinterpret_cast<ComponentBeginPlayFn>(g_suppressed[Index].trampoline)(self);
}

bool EnsureUiHooks() {
    if (g_ui_hooks_installed) return true;
    if (g_base == 0) {
        SC_LOG("player2: module base unknown -- InitPlayer2 was never called");
        return false;
    }

    g_suppressed[0].rva = offsets::UTargetableWidgetUpdaterComponent_BeginPlay;
    g_suppressed[1].rva = offsets::UWidgetPoolComponent_BeginPlay;

    void* hooks[] = {reinterpret_cast<void*>(&SuppressedBeginPlayHook<0>),
                     reinterpret_cast<void*>(&SuppressedBeginPlayHook<1>)};

    for (int i = 0; i < 2; ++i) {
        if (g_suppressed[i].rva == 0) {
            SC_LOG("player2: no offset for %s on this build", g_suppressed[i].name);
            return false;
        }
        auto* target = reinterpret_cast<void*>(g_base + g_suppressed[i].rva);
        if (MH_CreateHook(target, hooks[i], &g_suppressed[i].trampoline) != MH_OK ||
            MH_EnableHook(target) != MH_OK) {
            SC_LOG("player2: could not hook %s", g_suppressed[i].name);
            return false;
        }
        SC_LOG("player2: %s hooked at %p", g_suppressed[i].name, target);
    }
    g_ui_hooks_installed = true;
    return true;
}

void SilenceLocalInput(ue::UObject* controller, ue::UObject* pawn) {
    (void)pawn;
    if (!controller) {
        SC_LOG("player2: INPUT ISOLATION FAILED -- second controller is null");
        return;
    }

    if (g_input_hook_installed) {
        SC_LOG("player2: input isolated at APlayerController::InputKey (network movement retained)");
    } else {

        struct BoolParam {
            std::uint8_t value[8];
        } enabled = {};
        enabled.value[0] = 1;
        const bool move = ue::CallFunction(controller, L"SetIgnoreMoveInput", &enabled);
        const bool look = ue::CallFunction(controller, L"SetIgnoreLookInput", &enabled);
        SC_LOG("player2: input isolated with Steam fallback (move=%s look=%s; remote locomotion "
               "animation may be limited)",
               move ? "ok" : "FAILED", look ? "ok" : "FAILED");
    }

    ue::UObject* statics = GameplayStatics();
    if (statics) {
        struct IdParams {
            ue::UObject* Player;
            std::int32_t ReturnValue;
            std::uint8_t pad[4];
        } id = {};
        id.Player = controller;
        if (ue::CallFunction(statics, L"GetPlayerControllerID", &id)) {
            SC_LOG("player2: second player's controller id = %d", id.ReturnValue);
        }
    }
}

void SuppressMenus(ue::UObject* controller) {
    if (!controller) return;
    struct Empty {
    } none = {};
    if (ue::CallFunction(controller, L"BPF_DisableInGameMenu", &none)) {
        SC_LOG("player2: in-game menu disabled for the second controller");
    }
}

bool IgnorePawnCollision(ue::UObject* pawn) {
    if (!pawn) return false;
    struct ComponentParams {
        void* ComponentClass;
        ue::UObject* ReturnValue;
    } capsule = {};
    capsule.ComponentClass = ue::FindObjectByPath(L"/Script/Engine.CapsuleComponent");
    if (!capsule.ComponentClass ||
        !ue::CallFunction(pawn, L"GetComponentByClass", &capsule) || !capsule.ReturnValue) {
        return false;
    }
    struct CollisionResponseParams {
        std::uint8_t Channel;
        std::uint8_t NewResponse;
    } response = {};
    response.Channel = 2;
    response.NewResponse = 0;
    return ue::CallFunction(capsule.ReturnValue, L"SetCollisionResponseToChannel", &response);
}

void ConfigurePawn(ue::UObject* pawn) {
    if (!pawn) return;
    ue::UObject* world = ue::GetWorld();
    ue::UObject* primary = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    const int primary_faction = primary ? GetFaction(primary) : -1;
    if (primary_faction >= 0) SetFaction(pawn, primary_faction);

    struct CollisionParams {
        std::uint8_t bNewActorEnableCollision[8];
    } collision = {};
    collision.bNewActorEnableCollision[0] = 1;
    const bool collision_on = ue::CallFunction(pawn, L"SetActorEnableCollision", &collision);
    const bool pawn_ignored = IgnorePawnCollision(pawn);
    SC_LOG("player2: peer body collision world=%s pawn=%s", collision_on ? "enabled" : "FAILED",
           pawn_ignored ? "ignored" : "FAILED");
    SetInvincible(pawn, true);
}

}

void InitPlayer2(std::uintptr_t module_base) { g_base = module_base; }

bool SecondPlayerActive() { return g_controller != nullptr; }

ue::UObject* PrimaryPlayerPawn() {
    return PawnOf(PlayerControllerAt(ue::GetWorld(), 0));
}

ue::UObject* GetSecondPlayerPawn() {
    ue::UObject* world = ue::GetWorld();
    if (!SecondPlayerWorldReady(world) || !g_controller) return nullptr;
    ue::UObject* current = PlayerControllerAt(world, 1);
    if (!current) {
        SC_LOG("player2: controller 1 is absent from the settled world");
        g_controller = nullptr;
        g_retired = false;
        return nullptr;
    }
    g_controller = current;
    return PawnOf(current);
}

ue::UObject* CreateSecondPlayer() {
    ue::UObject* world = ue::GetWorld();
    if (!world) {
        SC_LOG("player2: no world -- cannot create a second player");
        return nullptr;
    }
    if (!SecondPlayerWorldReady(world)) return nullptr;
    if (!g_controller && !InPlayableWorld(world)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            SC_LOG("player2: not creating a second player outside a playable level -- "
                   "the engine treats that like a second pad pressing Start");
        }
        return nullptr;
    }

    if (g_controller) {
        ue::UObject* current = PlayerControllerAt(world, 1);
        if (!current) {
            SC_LOG("player2: old controller is not in the settled world; waiting to recreate it");
            g_controller = nullptr;
            g_retired = false;
            return nullptr;
        }
        g_controller = current;
        ue::UObject* existing = PawnOf(current);        if (existing) {
            if (g_retired) {

                struct HideParams {
                    std::uint8_t bNewHidden[8];
                } show = {};
                ue::CallFunction(existing, L"SetActorHiddenInGame", &show);
                struct CollisionParams {
                    std::uint8_t bNewActorEnableCollision[8];
                } collision = {};
                collision.bNewActorEnableCollision[0] = 1;

                ue::CallFunction(existing, L"SetActorEnableCollision", &collision);
                g_retired = false;
                SC_LOG("player2: retired second player brought back (collision enabled)");
            }
            return existing;
        }

        return nullptr;
    }

    ue::UObject* statics = GameplayStatics();
    if (!statics) {
        SC_LOG("player2: GameplayStatics default object not found");
        return nullptr;
    }

    ue::UObject* first_controller = nullptr;
    ue::UObject* first_pawn_before = nullptr;
    {
        struct GetPcParams {
            ue::UObject* WorldContextObject;
            std::int32_t PlayerIndex;
            std::uint8_t pad[4];
            ue::UObject* ReturnValue;
        } pc = {};
        pc.WorldContextObject = world;
        pc.PlayerIndex = 0;
        if (ue::CallFunction(statics, L"GetPlayerController", &pc) && pc.ReturnValue) {
            first_controller = pc.ReturnValue;
            first_pawn_before = PawnOf(first_controller);
        }
    }

    LogPlayerTable("before");

    if (coop::Get().second_player_disable_splitscreen) ForceDisableSplitscreen(world, true);

    struct alignas(8) Params {
        ue::UObject* WorldContextObject;
        std::int32_t ControllerId;
        std::uint8_t bSpawnPlayerController[4];
        ue::UObject* ReturnValue;
    } params = {};
    params.WorldContextObject = world;
    params.ControllerId = 1;
    for (int i = 0; i < 4; ++i) params.bSpawnPlayerController[i] = 1;

    if (!EnsureUiHooks()) {
        SC_LOG("player2: refusing to create a second player without the crash guard in place");
        coop::ReportProblem("second player unavailable (crash guard missing)");
        ForceDisableSplitscreen(world, false);
        return nullptr;
    }
    if (!EnsureInputHook()) {
        SC_LOG("player2: InputKey hook unavailable; using controlled input-ignore fallback");
    }

    if (coop::Get().hide_second_player_hud) EnsureHudHook();

    g_primary_controller = first_controller;
    g_suppress_ui = true;
    g_creating_second_player = true;
    const bool called = ue::CallFunction(statics, L"CreatePlayer", &params);
    g_creating_second_player = false;
    g_suppress_ui = false;

    if (!called) {
        SC_LOG("player2: CreatePlayer UFunction not found -- this build does not expose it");
        coop::ReportProblem("second player unsupported on this build");
        ForceDisableSplitscreen(world, false);
        return nullptr;
    }

    if (!params.ReturnValue) {

        SC_LOG("player2: CreatePlayer returned NULL -- the game mode refused a second player");
        coop::ReportProblem("the game refused to create a second player");
        ForceDisableSplitscreen(world, false);
        return nullptr;
    }

    if (first_controller && params.ReturnValue == first_controller) {
        SC_LOG("player2: CreatePlayer returned controller 0 -- refusing to suppress local input");
        coop::ReportProblem("second player unavailable (controller alias)");
        ForceDisableSplitscreen(world, false);
        return nullptr;
    }
    g_controller = params.ReturnValue;
    g_controller_world = world;
    g_travel_settle_frames = 0;
    SC_LOG("player2: PlayerController created %p (controller id 1)",
           static_cast<void*>(g_controller));

    SuppressMenus(g_controller);

    ue::UObject* pawn = PawnOf(g_controller);
    if (!pawn) {

        SC_LOG("player2: controller has NO pawn yet (game mode may spawn it late)");
        return nullptr;
    }

    char path[256] = {};
    ue::GetObjectPathName(pawn, path, sizeof(path));
    ue::FVector where = {};
    ue::GetActorLocation(pawn, &where);
    SC_LOG("player2: REAL SECOND PLAYER pawn %p at (%.0f, %.0f, %.0f) -- %s",
           static_cast<void*>(pawn), where.X, where.Y, where.Z, path);

    if (first_pawn_before && pawn == first_pawn_before && first_controller &&
        first_controller != g_controller) {
        SC_LOG("player2: the game mode gave the second player YOUR character -- repairing");
        if (ReturnStolenPawnAndRehouse(g_controller, first_controller, first_pawn_before)) {
            pawn = PawnOf(g_controller);
        }
    }

    LogPlayerTable("after");
    if (!pawn) {
        SC_LOG("player2: no body for the second player after repair");
        return nullptr;
    }
    SilenceLocalInput(g_controller, pawn);
    ConfigurePawn(pawn);
    RestorePrimaryView();
    g_view_reassert_frames = 120;
    return pawn;
}

ue::UObject* MaintainSecondPlayer() {
    if (!coop::Get().real_second_player) return nullptr;

    ue::UObject* world = ue::GetWorld();
    if (!SecondPlayerWorldReady(world)) return nullptr;

    if (g_controller) {
        ue::UObject* current = PlayerControllerAt(world, 1);
        if (!current) {
            SC_LOG("player2: controller 1 did not survive travel; it will be recreated safely");
            g_controller = nullptr;
            g_retired = false;
            return nullptr;
        }
        g_controller = current;
    }

    if (!g_controller) {

        static DWORD last_attempt = 0;
        const DWORD now = GetTickCount();
        if (now - last_attempt < 2000) return nullptr;
        last_attempt = now;
        return CreateSecondPlayer();
    }

    if (g_view_reassert_frames > 0) {
        --g_view_reassert_frames;
        RestorePrimaryView();
    }

    ue::UObject* pawn = PawnOf(g_controller);

    ue::UObject* first_controller = PlayerControllerAt(world, 0);
    ue::UObject* first_pawn = PawnOf(first_controller);
    const bool foreign_controller = first_controller && first_controller != g_controller;

    static DWORD primary_pawnless_since = 0;
    const DWORD theft_now = GetTickCount();
    if (pawn && foreign_controller && !first_pawn) {
        if (primary_pawnless_since == 0) primary_pawnless_since = theft_now;
        constexpr DWORD kTheftConfirmMs = 1000;
        if (theft_now - primary_pawnless_since >= kTheftConfirmMs) {
            primary_pawnless_since = 0;
            SC_LOG("player2: player one has had no character for a second while the second "
                   "player has one -- treating it as a re-theft and repairing");
            if (ReturnStolenPawnAndRehouse(g_controller, first_controller, pawn)) {
                pawn = PawnOf(g_controller);
            } else {

                return nullptr;
            }
        } else {

            return nullptr;
        }
    } else {
        primary_pawnless_since = 0;
    }

    if (pawn && pawn == ue::GetPlayerCharacter(world, 0)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SC_LOG("player2: refusing to use player one's own character as the remote body");
        }
        return nullptr;
    }

    static DWORD pawn_missing_since = 0;
    static DWORD last_rehouse_attempt = 0;
    const DWORD now = GetTickCount();
    if (!pawn) {
        if (pawn_missing_since == 0) {
            pawn_missing_since = now;
            SC_LOG("player2: second controller has no pawn after travel; waiting before rehouse");
        }
        if (now - pawn_missing_since >= 750 && now - last_rehouse_attempt >= 2000) {
            last_rehouse_attempt = now;
            SC_LOG("player2: rebuilding the missing second body in the settled world");
            pawn = RehouseSecondPlayerAfterTravel(g_controller);
            if (pawn) pawn_missing_since = 0;
        }
    } else {
        pawn_missing_since = 0;
    }

    static ue::UObject* known_pawn = nullptr;
    if (pawn != known_pawn) {
        known_pawn = pawn;
        if (pawn) {
            ue::FVector where = {};
            ue::GetActorLocation(pawn, &where);
            SC_LOG("player2: pawn is now %p at (%.0f, %.0f, %.0f) -- (re)configuring",
                   static_cast<void*>(pawn), where.X, where.Y, where.Z);
            SilenceLocalInput(g_controller, pawn);
            ConfigurePawn(pawn);
        } else {
            SC_LOG("player2: second player has no pawn right now (respawning?)");
        }
    }
    return pawn;
}

void RemoveSecondPlayer() {
    if (!g_controller) return;

    ue::UObject* world = ue::GetWorld();
    if (!SecondPlayerWorldReady(world)) {
        SC_LOG("player2: removal deferred while the level transition settles");
        return;
    }
    ue::UObject* current = PlayerControllerAt(world, 1);
    if (!current) {

        g_controller = nullptr;
        g_retired = false;
        return;
    }
    g_controller = current;

    if (g_retired) return;

    ue::UObject* pawn = PawnOf(g_controller);
    if (pawn) {
        struct HideParams {
            std::uint8_t bNewHidden[8];
        } hide = {};
        hide.bNewHidden[0] = 1;
        const bool hidden = ue::CallFunction(pawn, L"SetActorHiddenInGame", &hide);

        struct CollisionParams {
            std::uint8_t bNewActorEnableCollision[8];
        } collision = {};
        const bool uncollided = ue::CallFunction(pawn, L"SetActorEnableCollision", &collision);

        SetInvincible(pawn, true);

        ue::FVector parked = {0.f, 0.f, -1000000.f};
        ue::FRotator keep = {};
        ue::GetActorRotation(pawn, &keep);
        TeleportActor(pawn, parked, keep);

        SC_LOG("player2: second player retired -- hidden=%s collision-off=%s, parked below "
               "the level (not destroyed: RemovePlayer crashes the tick manager)",
               hidden ? "ok" : "FAILED", uncollided ? "ok" : "FAILED");
    } else {
        SC_LOG("player2: second player had no pawn to retire");
    }

    g_retired = true;
}

}
