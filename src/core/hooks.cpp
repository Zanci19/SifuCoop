#include "hooks.h"

#include <windows.h>

#include "../game/enemies.h"
#include "../game/orders.h"
#include "../game/puppet.h"
#include "../game/runstate.h"
#include "../game/selftest.h"
#include "../ue/reflection.h"
#include "log.h"
#include "offsets.g.h"

namespace sifucoop::hooks {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;

using TickFn = void(__fastcall*)(void*, float, bool);

TickFn g_original_tick = nullptr;
void** g_vtable = nullptr;
int g_tick_slot = -1;

std::uintptr_t g_base = 0;
unsigned long long g_frames = 0;

void OnFrame() {
    ++g_frames;
    // this refreshes their position each time
    // TODO: fix the animations of walking, running, hitting attacks, etc. they might not appear because I keep refreshing ts. consider adding predictions or only read client's buttoms, start and stop walking/running...
    // u stoopid
    sifucoop::game::TickEnemies();
    sifucoop::game::TickPuppet();
    sifucoop::game::TickSelfTest();

    if (g_frames % 30 != 0) return;

    ue::UObject* world = ue::GetWorld();
    if (!world) return;

    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player) return;

    sifucoop::game::TickRunState(player);

    // a memorial of how i thought per-frame logging was a good idea
    // "yeah per frame, sure why not!" 2 minutes later "why the fuck is my pc hotter than arizona"
    //                                                                         ~ Zanci19, 6.8.2026
    if (!sifucoop::game::IsOrderHookInstalled()) {
        sifucoop::game::InstallOrderHook(g_base, player);

        // the bridge between the two
        char path[512] = {};
        if (ue::GetObjectPathName(player, path, sizeof(path))) {
            wchar_t wide[512] = {};
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 512);
            ue::UObject* resolved = ue::FindObjectByPath(wide);
            SC_LOG("objmap: player path='%s' -> %s", path,
                   resolved == player ? "resolves back, MATCH" : "MISMATCH");
        } else {
            SC_LOG("objmap: GetPathName failed");
        }
    }
}

float g_frame_delta = 1.f / 60.f;

void __fastcall TickHook(void* self, float delta_seconds, bool idle_mode) {
    if (delta_seconds > 0.f && delta_seconds < 0.25f) {
        g_frame_delta = delta_seconds;
    } else {
        g_frame_delta = 1.f / 60.f;
    }

    g_original_tick(self, delta_seconds, idle_mode);

    // No SEH, GCC does not implement this __try/__except bullcrap, so we use OnFrame() (every pointer it touches is null-checked)
    OnFrame();
}

}  // namespace

bool InstallTickHook(std::uintptr_t base, void* gengine) {
    g_base = base;

    const auto target = reinterpret_cast<void*>(base + offsets::UGameEngine_Tick);
    auto** vtable = *reinterpret_cast<void***>(gengine);

    // Locate Tick
    for (int i = 0; i < 512; ++i) {
        if (IsBadReadPtr(&vtable[i], sizeof(void*))) break;
        if (vtable[i] == target) {
            g_tick_slot = i;
            break;
        }
    }

    if (g_tick_slot < 0) {
        SC_LOG("hook: UGameEngine::Tick not found in GEngine vtable -- not hooking");
        return false;
    }

    SC_LOG("hook: Tick is vtable slot %d", g_tick_slot);

    DWORD old_protect = 0;
    if (!VirtualProtect(&vtable[g_tick_slot], sizeof(void*), PAGE_READWRITE, &old_protect)) {
        SC_LOG("hook: VirtualProtect failed (err %lu)", GetLastError());
        return false;
    }

    g_original_tick = reinterpret_cast<TickFn>(vtable[g_tick_slot]);
    vtable[g_tick_slot] = reinterpret_cast<void*>(&TickHook);
    VirtualProtect(&vtable[g_tick_slot], sizeof(void*), old_protect, &old_protect);

    g_vtable = vtable;
    SC_LOG("hook: installed (original at %p)", reinterpret_cast<void*>(g_original_tick));
    return true;
}

void RemoveTickHook() {
    if (!g_vtable || g_tick_slot < 0 || !g_original_tick) return;
    DWORD old_protect = 0;
    if (VirtualProtect(&g_vtable[g_tick_slot], sizeof(void*), PAGE_READWRITE, &old_protect)) {
        g_vtable[g_tick_slot] = reinterpret_cast<void*>(g_original_tick);
        VirtualProtect(&g_vtable[g_tick_slot], sizeof(void*), old_protect, &old_protect);
    }
    g_vtable = nullptr;
}

float FrameDeltaSeconds() { return g_frame_delta; }

}  // namespace sifucoop::hooks

