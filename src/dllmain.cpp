#include <windows.h>

#include "core/hooks.h"
#include "core/log.h"
#include "game/actors.h"
#include "game/coop.h"
#include "game/enemies.h"
#include "game/orders.h"
#include "game/player2.h"
#include "game/puppet.h"
#include "game/selftest.h"
#include "net/session.h"
#include "ui/overlay.h"
#include "core/offsets.g.h"
#include "ue/reflection.h"

namespace sifucoop::proxy {
bool Init();
}

namespace offsets = sifucoop::offsets;

namespace {

struct PeIdentity {
    DWORD time_date_stamp;
    DWORD size_of_image;
};

bool ReadPeIdentity(HMODULE module, PeIdentity* out) {
    auto* base = reinterpret_cast<const BYTE*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    out->time_date_stamp = nt->FileHeader.TimeDateStamp;
    out->size_of_image = nt->OptionalHeader.SizeOfImage;
    return true;
}

bool VerifyGameBuild(HMODULE game, uintptr_t base) {
    PeIdentity id = {};
    if (!ReadPeIdentity(game, &id)) {
        SC_LOG("guard: could not parse exe headers -- refusing to hook");
        return false;
    }

    SC_LOG("guard: exe TimeDateStamp=0x%08lX SizeOfImage=0x%08lX", id.time_date_stamp,
           id.size_of_image);

    // address table differs per-build (steam vs epic)
    const char* build = offsets::SelectBuild(id.time_date_stamp, id.size_of_image);
    if (!build) {
        SC_LOG("guard: UNKNOWN BUILD -- no offsets for this executable.");
        SC_LOG("guard: known builds (%d):", offsets::kBuildCount);
        for (const auto& entry : offsets::kBuilds) {
            SC_LOG("guard:   %-12s stamp=0x%08X size=0x%08X", entry.name,
                   entry.time_date_stamp, entry.size_of_image);
        }
        SC_LOG("guard: to add this one, run on THIS machine:");
        SC_LOG("guard:   python pdbdump.py <Sifu-Win64-Shipping.pdb> "
               "--exe <Sifu-Win64-Shipping.exe> --emit-build <name> build.json");
        return false;
    }

    SC_LOG("guard: build '%s' matched. image base = 0x%llX", build,
           static_cast<unsigned long long>(base));
    return true;
}

void LogResolved(uintptr_t base, const char* name, uint32_t rva) {
    const uintptr_t address = base + rva;
    const auto* bytes = reinterpret_cast<const BYTE*>(address);
    SC_LOG("  %-38s rva=0x%08X  addr=0x%llX  bytes=%02X %02X %02X %02X %02X", name, rva,
           static_cast<unsigned long long>(address), bytes[0], bytes[1], bytes[2],
           bytes[3], bytes[4]);
}

DWORD WINAPI Bootstrap(LPVOID) {
    HMODULE game = GetModuleHandleA(nullptr);
    const auto base = reinterpret_cast<uintptr_t>(game);

    if (!VerifyGameBuild(game, base)) {
        SC_LOG("bootstrap: aborted, mod is inert (the game will run normally)");
        return 0;
    }

    SC_LOG("bootstrap: resolved targets --");
    LogResolved(base, "UGameEngine::Tick", offsets::UGameEngine_Tick);
    LogResolved(base, "UWorld::SpawnActor", offsets::UWorld_SpawnActor);
    LogResolved(base, "AActor::SetActorLocationAndRotation",
                offsets::AActor_SetActorLocationAndRotation);
    LogResolved(base, "AActor::K2_DestroyActor", offsets::AActor_K2_DestroyActor);
    LogResolved(base, "UEngine::GetWorldFromContextObject",
                offsets::UEngine_GetWorldFromContextObject);
    LogResolved(base, "GEngine", offsets::GEngine);
    LogResolved(base, "GWorld", offsets::GWorld);

    auto** gengine = reinterpret_cast<void**>(base + offsets::GEngine);
    void* engine = nullptr;
    for (int i = 0; i < 600; ++i) {
        engine = *gengine;
        if (engine) {
            SC_LOG("bootstrap: GEngine populated after ~%d.%ds -> 0x%llX", i / 2, (i % 2) * 5,
                   reinterpret_cast<unsigned long long>(engine));
            break;
        }
        Sleep(500);
    }

    if (!engine) {
        SC_LOG("bootstrap: GEngine still null after 5 minutes -- offset is suspect");
        return 0;
    }

    Sleep(2000);

    if (!sifucoop::ue::InitReflection(base)) {
        SC_LOG("bootstrap: reflection unavailable -- stopping before hooking");
        return 0;
    }

    sifucoop::coop::Load();
    sifucoop::game::InitActors(base);
    sifucoop::game::InitPuppet(base);
    sifucoop::game::InitPlayer2(base);
    sifucoop::game::InitEnemies(base);
    sifucoop::game::InstallPlayOrderHook(base);
    sifucoop::game::InitSelfTest();
    sifucoop::net::StartSession();
    bool in_game = false;
    if (sifucoop::coop::Get().in_game_overlay) {
        in_game = sifucoop::ui::StartInGameOverlay();
    } else {
        SC_LOG("d3d: in-game overlay disabled by config -- using the window overlay");
    }
    if (!in_game) sifucoop::ui::StartOverlay();

    if (!sifucoop::hooks::InstallTickHook(base, engine)) {
        SC_LOG("bootstrap: tick hook not installed -- mod is inert");
        return 0;
    }

    SC_LOG("bootstrap: armed -- press F1 in game for the menu");
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        sifucoop::log::Open();

        // Forwarding must be wired up before the game's first dsound call
        if (!sifucoop::proxy::Init()) {
            SC_LOG("proxy: init incomplete -- audio may misbehave");
        }

        // Everything else runs off the loader lock
        HANDLE thread = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        sifucoop::log::Close();
    }
    return TRUE;
}



