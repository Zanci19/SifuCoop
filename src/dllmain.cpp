#include <windows.h>

#include <cstdio>

#include "core/hooks.h"
#include "core/log.h"
#include "game/actors.h"
#include "game/coop.h"
#include "game/enemies.h"
#include "game/orders.h"
#include "game/player2.h"
#include "game/puppet.h"
#include "game/selftest.h"
#include "net/instance_guard.h"
#include "net/session.h"
#include "ui/overlay.h"
#include "core/offsets.g.h"
#include "ue/reflection.h"

namespace sifucoop::proxy {
bool Init();
}

namespace offsets = sifucoop::offsets;

namespace {

// Deliberately never closed by StopSession/RestartSession. The kernel releases
// it when Sifu exits, leaving no reconnect window in which a second DLL can arm
// its hooks in another process.
HANDLE g_game_process_mutex = nullptr;

bool AcquireGameProcessGuard() {
    SetLastError(ERROR_SUCCESS);
    HANDLE mutex = CreateMutexA(nullptr, FALSE, sifucoop::net::kGameProcessMutexNameA);
    const DWORD error = GetLastError();
    if (!mutex) {
        char message[192] = {};
        _snprintf(message, sizeof(message),
                  "SifuCoop could not create its process guard (Windows error %lu). "
                  "The mod will stay inactive to avoid unsafe duplicate hooks.",
                  error);
        SC_LOG("guard: process mutex creation failed (%lu) -- refusing to hook", error);
        MessageBoxA(nullptr, message, "SifuCoop not started",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return false;
    }
    if (error == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        SC_LOG("guard: another SifuCoop game process is active -- refusing to hook");
        MessageBoxA(nullptr,
                    "Another SifuCoop-enabled Sifu process is already running. Close it "
                    "before starting another copy. This game will continue without the mod.",
                    "SifuCoop already running",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return false;
    }

    g_game_process_mutex = mutex;
    SC_LOG("guard: acquired process-lifetime instance guard");
    return true;
}

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

// Offsets were generated against one exact build. Applied to a different one
// they point into unrelated code, so a mismatch must stop us before we hook
// anything -- a loud refusal is far better than corrupting the process.
bool VerifyGameBuild(HMODULE game, uintptr_t base) {
    PeIdentity id = {};
    if (!ReadPeIdentity(game, &id)) {
        SC_LOG("guard: could not parse exe headers -- refusing to hook");
        return false;
    }

    SC_LOG("guard: exe TimeDateStamp=0x%08lX SizeOfImage=0x%08lX", id.time_date_stamp,
           id.size_of_image);

    // Offsets differ per executable, but the protocol does not: Epic and Steam
    // ship the same game version, so asset paths and combo indices match and
    // the two stores can play together. Only the address table is per-build.
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

    // How many entries in THIS build's table are zero.
    //
    // Adding a symbol regenerates the table for whichever game folder build.ps1
    // was pointed at, and only that one. The other machine keeps an older table
    // in which the new entries are zero, every call through them silently does
    // nothing, and the feature looks broken on exactly one side. That is not
    // hypothetical: the two callbacks that make a body fall over were resolved
    // on Epic and zero on Steam, so corpses stood up for the joining player and
    // nowhere else, and it cost a full test round to find. One number at startup
    // makes it obvious.
    for (const auto& entry : offsets::kBuilds) {
        if (_stricmp(entry.name, build) != 0) continue;
        int missing = 0;
        for (std::uint32_t value : entry.values) {
            if (value == 0) ++missing;
        }
        if (missing > 0) {
            SC_LOG("guard: %d of %d offsets are MISSING from the '%s' table -- regenerate it "
                   "on this machine (build.ps1 -GameDir <this game folder> -LocalBuildName %s) "
                   "or those features will silently do nothing here",
                   missing, static_cast<int>(sizeof(entry.values) / sizeof(entry.values[0])),
                   build, build);
        }
        break;
    }
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
    if (!AcquireGameProcessGuard()) return 0;

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

    // GEngine is populated during engine init, well after we load. Watching it
    // flip from null is the cheapest proof that we are reading the right
    // address and that our view of the process is correct.
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

    // GEngine exists but the engine is still bringing itself up; give the
    // vtable a moment to settle before we touch it.
    Sleep(2000);

    if (!sifucoop::ue::InitReflection(base)) {
        SC_LOG("bootstrap: reflection unavailable -- stopping before hooking");
        return 0;
    }

    sifucoop::coop::Load();
    const bool network_started = sifucoop::net::StartSession();
    if (!network_started && sifucoop::net::GetRole() == sifucoop::net::Role::Host) {
        const char* failure = sifucoop::net::GetStartFailure();
        if (!failure || !failure[0]) failure = "The SifuCoop host socket could not start.";
        SC_LOG("bootstrap: host startup failed before hooks -- mod is inert: %s", failure);
        MessageBoxA(nullptr, failure, "SifuCoop host not started",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 0;
    }

    // Network ownership is decided before anything below installs a gameplay
    // or UI hook. A duplicate host therefore cannot leave a second Sifu process
    // half-active after its UDP bind fails.
    sifucoop::game::InitActors(base);
    sifucoop::game::InitPuppet(base);
    sifucoop::game::InitPlayer2(base);
    sifucoop::game::InitEnemies(base);
    sifucoop::game::InstallPlayOrderHook(base);
    sifucoop::game::InitSelfTest();
    // Prefer drawing inside the game: a window cannot appear over exclusive
    // fullscreen. The window overlay is only started if the hook fails -- or if
    // the swap-chain hook has been switched off, which is the escape hatch for
    // anyone whose driver or overlay stack does not get on with it. Hooking
    // somebody else's graphics pipeline is the riskiest thing here and the
    // least essential, so it has to be optional.
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

        // Forwarding must be wired up before the game's first dsound call.
        if (!sifucoop::proxy::Init()) {
            SC_LOG("proxy: init incomplete -- audio may misbehave");
        }

        // Everything else runs off the loader lock.
        HANDLE thread = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        sifucoop::log::Close();
    }
    return TRUE;
}



