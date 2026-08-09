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

// UGameplayStatics is a class of static functions, so its calls are made on the
// class default object -- the same route runstate.cpp already uses for
// GetGameState. No new offsets, no ABI guesswork: everything here is a
// reflected UFunction invoked through ProcessEvent.
ue::UObject* GameplayStatics() {
    return ue::FindObjectByPath(L"/Script/Engine.Default__GameplayStatics");
}

// The controller, kept across frames. The PAWN is deliberately not cached: it is
// destroyed and rebuilt on death, on aging, and on every level change, which is
// exactly the dangling-pointer bug the puppet already had once.
ue::UObject* g_controller = nullptr;

// A level transition invalidates the old world and all of its player-controller
// bookkeeping for several frames. Never invoke a reflected method on the cached
// controller during that window; resolve controller 1 from the new world only
// after it has settled.
ue::UObject* g_controller_world = nullptr;
int g_travel_settle_frames = 0;
std::uintptr_t g_base = 0;
// A second local PlayerController shares the operating system's input stream
// with player one. Filtering it at APlayerController::InputKey keeps keyboard
// and gamepad events out of controller 1 without setting the controller-wide
// ignore flags that also reject the mod's AddMovementInput calls.
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
    // Controller 1 exists solely as the network peer's in-world representative.
    // Its input arrives through the same physical device as controller 0, so it
    // must be consumed here. Network steering calls AddMovementInput directly
    // and therefore never crosses this boundary.
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

// --- The second player's HUD -------------------------------------------------
//
// A second local player is a real player, so Sifu builds it a real HUD. With the
// viewport forced out of splitscreen, that HUD is drawn into the SAME
// full-screen viewport as player one's -- and because the mod mirrors the remote
// peer's actual health onto this body, what lands on the screen is the other
// player's life bar, sitting where yours does. That is the reported "life from
// the client appears as mine".
//
// Sifu's HUD widget registers itself by calling the controller's virtual
// BPF_SetHUD. Declining that one call for the second controller, and taking the
// widget back out of the viewport, is the narrowest place to stop it: no widget
// is left half-initialised, player one's HUD is untouched, and a build that does
// not export the symbol simply keeps the old behaviour.
using SetHudFn = void(__fastcall*)(void* self, void* widget);

void* g_set_hud_trampoline = nullptr;
bool g_hud_hook_installed = false;

// Player one's controller, remembered so the hook can identify "not the human's
// HUD" during the CreatePlayer call itself -- at that moment g_controller does
// not exist yet, because the call that would set it has not returned. This is
// exactly when Sifu's HUD registers, so matching on g_controller alone would
// miss the one registration that matters.
ue::UObject* g_primary_controller = nullptr;
bool g_creating_second_player = false;

void __fastcall SetHudHook(void* self, void* widget) {
    const bool is_primary = self && self == g_primary_controller;
    const bool is_second = self && !is_primary &&
                           (self == g_controller || g_creating_second_player);
    if (is_second && coop::Get().hide_second_player_hud) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            SC_LOG("player2: declined the second player's HUD registration");
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

// Worlds in which a second local player must never be registered.
//
// Asking the engine for a second player is, as far as Sifu is concerned,
// indistinguishable from somebody pressing Start on a second pad -- so doing it
// in a menu or the hideout level picker can advance the front end on its own.
// A second player only ever makes sense once there is a real character standing
// in a real level.
bool InPlayableWorld(ue::UObject* world) {
    if (!coop::Get().second_player_in_gameplay_only) return true;
    if (!world) return false;
    if (!ue::GetPlayerCharacter(world, 0)) return false;

    char level[192] = {};
    if (!ue::GetCurrentLevelPath(level, sizeof(level))) return false;
    static const char* const kNonGameplay[] = {"SelectHideoutLevel", "MainMenu", "Frontend",
                                               "Startup", "EntryLevel"};
    for (const char* fragment : kNonGameplay) {
        if (strstr(level, fragment) != nullptr) return false;
    }
    return true;
}

// Hidden and parked rather than destroyed -- see RemoveSecondPlayer.
bool g_retired = false;

// Frames remaining during which player one's camera is re-asserted every tick.
// One call at creation reported success and the view still ended up stuck on the
// level's start camera, which means something reclaims it on a later frame.
// Re-applying briefly costs nothing and beats a one-shot override.
int g_view_reassert_frames = 0;

// Adding a player normally splits the viewport in two, which is not what anyone
// wants here -- the second player is on the other machine, and their half of the
// screen would be wasted showing them a camera they cannot use. This is the
// engine's own opt-out, and it is Blueprint-exposed like everything else.
//
//   SetForceDisableSplitscreen(WorldContextObject @0x00, bDisable @0x08)
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

// Resolve a controller from the live world instead of trusting a pointer that
// may belong to the world Unreal is currently tearing down.
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

// First use in a new world is safe when no second controller exists. If one
// does exist, this is a level travel: leave it completely untouched until the
// new world has finished reconstructing its player-controller list.
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

// AController::K2_GetPawn.
ue::UObject* PawnOf(ue::UObject* controller) {
    if (!controller) return nullptr;
    struct Params {
        ue::UObject* ReturnValue;
    } params = {};
    if (!ue::CallFunction(controller, L"K2_GetPawn", &params)) return nullptr;
    return params.ReturnValue;
}

// AController::Possess / UnPossess.
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

// Give player one their character back, and build the second player a body.
//
// Sifu's game mode does not spawn a fresh character for a second player: asked
// to log one in, it handed over the character player ONE was already using. The
// log made it unambiguous -- the same pawn pointer moved from index 0 to index
// 1, and player one was left possessing nothing, unable to move or attack while
// watching their own body from the outside.
//
// So the theft is undone: the new controller releases the stolen pawn, player
// one takes it back, and the second player is given a freshly spawned clone of
// the same character class to possess. The clone is a real pawn possessed by a
// real PlayerController -- which is the entire point of this approach, and the
// thing a puppet could never be.
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

    // Spawn the second player its own body, just to one side of player one.
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


// Sifu preserves the local PlayerController across travel but does not respawn
// the synthetic second player's pawn. Reuse that live controller and give it a
// new body in the current world; creating a second LocalPlayer here would add a
// duplicate input slot and is precisely what caused the repeated clone churn.
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
// Every local player, controller and pawn the game currently has.
//
// Logged either side of creating the second player, because the first attempt
// left player ONE with no pawn -- the human could not move, attack, or see their
// own character. Whatever CreatePlayer disturbs, this shows it rather than
// leaving it to be inferred from how the game felt.
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

// Put the camera back on player one, and say plainly what the engine did.
//
// Adding a local player mid-game moved the view: the screen showed the second
// character in third person and the keyboard no longer drove player one. That is
// the engine choosing a view target for the newly added player, not anything the
// mod asked for -- so it is explicitly put back, and the resulting state is
// logged rather than assumed.
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
        ue::UObject* NewViewTarget;   // 0x00
        std::uint8_t bLockOutgoing[8];
        float BlendTime;              // 0x08 -- overlaps intentionally, see layout below
    } unused = {};
    (void)unused;

    // Layout from the reflection tables: NewViewTarget @0x00, bLockOutgoing @0x01
    // (inside the first slot), BlendTime @0x08, BlendFunc @0x0C, BlendExp @0x10.
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

// Sifu UI components that cannot survive being created for a second player.
//
// The pattern, established by two crashes: in UWorld::SpawnPlayActor the engine
// runs the new controller's BeginPlay from Login(), and only links it to its
// local player afterwards with SetPlayer(). So every UI component hanging off
// the controller runs BeginPlay while its owning player is still null, asks for
// a screen it has not got, and dereferences null. Player one never hits this --
// the world has not begun play when it is created, so the engine defers its
// BeginPlay until everything is wired.
//
// The first attempt skipped the whole controller BeginPlay and re-ran it later.
// That fixed the symptom and broke something worse: with the actor's begun-play
// bookkeeping inconsistent, teardown never properly unregistered the components,
// and the next frame's tick walked a destroyed tick function --
// FTickFunction::QueueTickFunction reading 0xffffffffffffffff. Corrupting actor
// lifetime is far worse than a missing widget.
//
// So the controller's BeginPlay runs untouched, and only these leaf components
// are skipped, for the duration of the one call that creates the player. They
// are HUD: a second player's reticle and widget pool belong on the machine that
// player is actually sitting at, not on this one. Adding another is one line
// here plus one in pdbdump's WANTED.
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

// Keep this keyboard from driving the second character without suppressing the
// remote peer's movement. Epic supplies the native InputKey symbol, which is
// the precise input boundary. The currently supported Steam PDB omits it, so
// that build retains the prior engine-supported ignore-input fallback instead
// of refusing to create the second player outright.
void SilenceLocalInput(ue::UObject* controller, ue::UObject* pawn) {
    (void)pawn;
    if (!controller) {
        SC_LOG("player2: INPUT ISOLATION FAILED -- second controller is null");
        return;
    }

    if (g_input_hook_installed) {
        SC_LOG("player2: input isolated at APlayerController::InputKey (network movement retained)");
    } else {
        // The fallback prevents a shared keyboard/gamepad from driving the
        // remote body. DriveTo still applies its final transform directly, so
        // remote position remains correct; this path simply cannot preserve
        // movement-component velocity animation until the Steam InputKey symbol
        // is available from a matching PDB.
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
// A second AFightingPlayerController would otherwise be able to open Sifu's own
// in-game menu, and a menu opened by a player who is not sitting at this keyboard
// cannot be closed from here.
void SuppressMenus(ue::UObject* controller) {
    if (!controller) return;
    struct Empty {
    } none = {};
    if (ue::CallFunction(controller, L"BPF_DisableInGameMenu", &none)) {
        SC_LOG("player2: in-game menu disabled for the second controller");
    }
}

// Applied to whichever pawn currently belongs to the second player. A pawn that
// the game mode respawns arrives with none of this, so it is re-applied rather
// than done once at creation.
void ConfigurePawn(ue::UObject* pawn) {
    if (!pawn) return;
    ue::UObject* world = ue::GetWorld();
    ue::UObject* primary = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    const int primary_faction = primary ? GetFaction(primary) : -1;
    if (primary_faction >= 0) SetFaction(pawn, primary_faction);
    // This pawn represents the player whose own machine decides their damage
    // and death. It shares player one's faction so level AI can choose either
    // player while the two players remain co-operative.
    //
    // The faction is checked again after a respawn or level travel rather than
    // assumed to be inherited from the previous pawn.    // The remote body is a replicated visual/AI target, not a local physical
    // obstacle. Leaving its capsule enabled makes K2_TeleportTo refuse a
    // snapshot whenever the two player capsules overlap, which is the source
    // of the visible standing-inside-each-other jitter. Enemy targeting remains
    // explicit through the attack component; damage remains authoritative on
    // the owning peer.
    struct CollisionParams {
        std::uint8_t bNewActorEnableCollision[8];
    } collision = {};
    collision.bNewActorEnableCollision[0] = 1;
    const bool collision_on = ue::CallFunction(pawn, L"SetActorEnableCollision", &collision);
    SC_LOG("player2: peer body collision %s", collision_on ? "enabled" : "NOT enabled");
    SetInvincible(pawn, true);
}

}  // namespace

void InitPlayer2(std::uintptr_t module_base) { g_base = module_base; }

bool SecondPlayerActive() { return g_controller != nullptr; }

ue::UObject* PrimaryPlayerPawn() {
    return PawnOf(PlayerControllerAt(ue::GetWorld(), 0));
}

ue::UObject* PrimaryPlayerController() {
    return PlayerControllerAt(ue::GetWorld(), 0);
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
                // Bring the retired one back rather than asking the engine for
                // another: creating is the risky operation, and we already have
                // a perfectly good player standing in the basement.
                struct HideParams {
                    std::uint8_t bNewHidden[8];
                } show = {};
                ue::CallFunction(existing, L"SetActorHiddenInGame", &show);
                struct CollisionParams {
                    std::uint8_t bNewActorEnableCollision[8];
                } collision = {};
                collision.bNewActorEnableCollision[0] = 1;
                // It remains non-blocking when revived; it is the peer's
                // visual body, so player-vs-player capsule collision is never
                // useful and would make snapshot teleports fail.
                ue::CallFunction(existing, L"SetActorEnableCollision", &collision);
                g_retired = false;
                SC_LOG("player2: retired second player brought back (collision enabled)");
            }
            return existing;
        }
        // Controller alive but no pawn yet: mid-respawn, not an error.
        return nullptr;
    }

    ue::UObject* statics = GameplayStatics();
    if (!statics) {
        SC_LOG("player2: GameplayStatics default object not found");
        return nullptr;
    }

    // Remembered so the theft below can be detected and undone.
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

    // Order matters: disable the split BEFORE the player exists, or the viewport
    // reshapes for a frame and the camera has to be put back.
    if (coop::Get().second_player_disable_splitscreen) ForceDisableSplitscreen(world, true);

    // Layout recovered from the reflection tables rather than assumed:
    //   WorldContextObject @0x00, ControllerId @0x08, bSpawnPlayerController @0x0C,
    //   ReturnValue @0x10.
    // The bool is written across 0x0C..0x0F because the generated table reports
    // bools by byte-and-mask, and those neighbouring bytes are padding.
    struct alignas(8) Params {
        ue::UObject* WorldContextObject;  // 0x00
        std::int32_t ControllerId;        // 0x08
        std::uint8_t bSpawnPlayerController[4];
        ue::UObject* ReturnValue;         // 0x10
    } params = {};
    params.WorldContextObject = world;
    params.ControllerId = 1;  // 0 is the local player already sitting at the keyboard
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
    // Installed BEFORE the player exists: the HUD registers itself during the
    // new controller's BeginPlay, which happens inside the CreatePlayer call.
    if (coop::Get().hide_second_player_hud) EnsureHudHook();
    // Narrowest possible window: armed for this one call and disarmed straight
    // after, including on every failure path below.
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
        // The call worked but the game refused. Most likely the game mode caps
        // the player count, which is the single most important thing to know
        // about whether this approach can work at all.
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
        // A controller with no pawn means the game mode made the controller but
        // did not spawn a character for it -- worth saying plainly, because the
        // controller alone is useless to us.
        SC_LOG("player2: controller has NO pawn yet (game mode may spawn it late)");
        return nullptr;
    }

    char path[256] = {};
    ue::GetObjectPathName(pawn, path, sizeof(path));
    ue::FVector where = {};
    ue::GetActorLocation(pawn, &where);
    SC_LOG("player2: REAL SECOND PLAYER pawn %p at (%.0f, %.0f, %.0f) -- %s",
           static_cast<void*>(pawn), where.X, where.Y, where.Z, path);

    // The game mode hands a second player the FIRST player's character rather
    // than making a new one. Undo that before touching anything else: until it
    // is undone, the human at this keyboard has no character.
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
        // Rate-limited: if the game mode refuses a second player it will refuse
        // every frame, and retrying at frame rate would bury the log and stall
        // the game thread for no gain.
        static DWORD last_attempt = 0;
        const DWORD now = GetTickCount();
        if (now - last_attempt < 2000) return nullptr;
        last_attempt = now;
        return CreateSecondPlayer();
    }

    static ue::UObject* known_primary_pawn = nullptr;
    ue::UObject* primary_now = PawnOf(PlayerControllerAt(world, 0));
    if (primary_now && primary_now != known_primary_pawn) {
        if (known_primary_pawn) {
            SC_LOG("player2: player one respawned (%p -> %p) -- re-asserting camera and "
                   "splitscreen override", static_cast<void*>(known_primary_pawn),
                   static_cast<void*>(primary_now));
            if (coop::Get().second_player_disable_splitscreen) {
                ForceDisableSplitscreen(world, true);
            }
            g_view_reassert_frames = 180;
        }
        known_primary_pawn = primary_now;
    }

    if (g_view_reassert_frames > 0) {
        --g_view_reassert_frames;
        RestorePrimaryView();
    }

    // Re-resolved every frame rather than cached. A pawn pointer that outlives
    // its pawn is the single most dangerous thing in this codebase.
    ue::UObject* pawn = PawnOf(g_controller);

    // The theft is not a one-off at creation. Sifu's game mode hands the second
    // player player ONE's character, and it can do it again after a death, an
    // aging respawn or a level travel -- at which point everything downstream
    // treats the human's own body as the remote one: it gets the peer's health
    // written into it (their life bar showing as yours), it is made invincible,
    // and the network drive tries to walk it to wherever the peer is standing.
    //
    // A pawn has exactly one controller, so the signature of the theft is not
    // "both controllers name the same pawn" -- it is player ONE holding nothing
    // while the second player holds a body. That is also what a perfectly normal
    // respawn looks like for a moment, so it has to persist before it is treated
    // as a theft; repairing a respawn in progress would hand the human the
    // remote player's character, which is the same bug facing the other way.
    ue::UObject* first_controller = PlayerControllerAt(world, 0);
    if (first_controller) g_primary_controller = first_controller;
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
                // Better no remote body this frame than driving the human's.
                return nullptr;
            }
        } else {
            // Undecided: do not configure or hand out a body that may be the
            // human's own.
            return nullptr;
        }
    } else {
        primary_pawnless_since = 0;
    }

    // Direct alias, whatever the cause: never present the human's own character
    // as the remote player's.
    if (pawn && pawn == ue::GetPlayerCharacter(world, 0)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SC_LOG("player2: refusing to use player one's own character as the remote body");
        }
        return nullptr;
    }

    // The controller legitimately survives a level load, but on this game its
    // synthetic pawn does not. Once the new world has had a moment to finish
    // normal spawning, rebuild and possess exactly one fresh body instead of
    // creating an unrelated puppet clone every retry.
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
        // The old controller was destroyed as part of travel. It must never be
        // used as a reflection target merely to retire it.
        g_controller = nullptr;
        g_retired = false;
        return;
    }
    g_controller = current;
    // Retiring is idempotent. Without this the per-frame maintenance re-retired
    // an already-retired player every frame, filling the log twelve times in an
    // eighth of a second and re-parking a pawn that was already parked.
    if (g_retired) return;

    // Deliberately does NOT call UGameplayStatics::RemovePlayer any more.
    //
    // That is the engine's own teardown, and on this game it corrupts the tick
    // manager: the frame after it ran, UWorld::Tick walked a destroyed tick
    // function and died reading 0xffffffffffffffff. It reproduced every single
    // time, and it is inside engine code the mod cannot correct.
    //
    // So the second player is retired rather than destroyed -- hidden, made
    // non-colliding, and parked far below the level, exactly the way enemies the
    // host has not activated are parked. Nothing ticks it into anything, nothing
    // can see or hit it, and the controller stays alive to be reused if the peer
    // reconnects. Costing one dormant actor is a straight trade against crashing
    // the player's game.
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

}  // namespace sifucoop::game
