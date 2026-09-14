#include "replay.h"

#include <windows.h>

#include <cstdio>

#include "../core/log.h"
#include "../ue/reflection.h"
#include "coop.h"

namespace sifucoop::game {
namespace {

ue::UObject* g_replay_cdo = nullptr;
bool g_probe_running = false;
DWORD g_last_report_ms = 0;
bool g_resolve_attempted = false;

// UReplaySystem's control surface is static and Blueprint-exposed, so it is
// dispatched on the class default object. The owning script package is not
// recorded anywhere readable, so the candidates are tried in turn and the one
// that resolves is logged; see CODE-NOTES.md.
const wchar_t* const kReplaySystemPaths[] = {
    L"/Script/Sifu.Default__UReplaySystem",
    L"/Script/SCCore.Default__UReplaySystem",
    L"/Script/SCCore.Default__ReplaySystem",
    L"/Script/Sifu.Default__ReplaySystem",
};

ue::UObject* ResolveReplaySystem() {
    if (g_replay_cdo) return g_replay_cdo;
    if (g_resolve_attempted) return nullptr;
    g_resolve_attempted = true;

    for (const wchar_t* path : kReplaySystemPaths) {
        ue::UObject* found = ue::FindObjectByPath(path);
        if (!found || !ue::IsValidObject(found)) continue;
        g_replay_cdo = found;
        char name[256] = {};
        if (ue::GetObjectPathName(found, name, sizeof(name))) {
            SC_LOG("replay: UReplaySystem resolved at '%s'", name);
        } else {
            SC_LOG("replay: UReplaySystem resolved (path unreadable)");
        }
        return g_replay_cdo;
    }

    SC_LOG("replay: could not resolve UReplaySystem -- tried %d candidate paths. "
           "If the class exists under another package, add it to kReplaySystemPaths.",
           static_cast<int>(sizeof(kReplaySystemPaths) / sizeof(kReplaySystemPaths[0])));
    return nullptr;
}

struct WorldContextBool {
    ue::UObject* WorldContextObject = nullptr;
    bool ReturnValue = false;
};

struct WorldContextFloat {
    ue::UObject* WorldContextObject = nullptr;
    float ReturnValue = 0.f;
};

struct WorldContextOnly {
    ue::UObject* WorldContextObject = nullptr;
};

struct WorldContextObjectOut {
    ue::UObject* WorldContextObject = nullptr;
    ue::UObject* ReturnValue = nullptr;
};

struct StopRecordingParams {
    ue::UObject* WorldContextObject = nullptr;
    std::uint8_t Reason = 0;
};

struct ActorFloat {
    ue::UObject* Actor = nullptr;
    float ReturnValue = 0.f;
};

bool CallBool(const wchar_t* name, ue::UObject* world, bool* out) {
    ue::UObject* system = ResolveReplaySystem();
    if (!system) return false;
    WorldContextBool params = {};
    params.WorldContextObject = world;
    if (!ue::CallFunction(system, name, &params)) return false;
    *out = params.ReturnValue;
    return true;
}

}  // namespace

void InitReplayProbe() {
    if (!coop::Get().replay_probe) return;
    SC_LOG("replay: probe enabled -- press F10 to start and stop recording");
}

bool StartReplayProbe() {
    ue::UObject* world = ue::GetWorld();
    if (!world) {
        SC_LOG("replay: no world yet");
        return false;
    }
    ue::UObject* system = ResolveReplaySystem();
    if (!system) return false;

    bool can_start = false;
    if (CallBool(L"BPF_CanStartRecording", world, &can_start)) {
        SC_LOG("replay: BPF_CanStartRecording = %s", can_start ? "true" : "false");
    } else {
        SC_LOG("replay: BPF_CanStartRecording did not dispatch");
    }

    bool disabled = false;
    if (CallBool(L"BPF_IsRecordingDisabled", world, &disabled)) {
        SC_LOG("replay: BPF_IsRecordingDisabled = %s", disabled ? "true" : "false");
    }

    WorldContextOnly start = {};
    start.WorldContextObject = world;
    if (!ue::CallFunction(system, L"BPF_ReplayStartRecording", &start)) {
        SC_LOG("replay: BPF_ReplayStartRecording did not dispatch");
        return false;
    }

    bool recording = false;
    CallBool(L"BPF_IsRecording", world, &recording);
    SC_LOG("replay: start requested -- IsRecording = %s", recording ? "TRUE" : "false");

    WorldContextObjectOut driver = {};
    driver.WorldContextObject = world;
    if (ue::CallFunction(system, L"BPF_GetDemoNetDriver", &driver) && driver.ReturnValue) {
        char name[256] = {};
        if (ue::GetObjectClassPathName(driver.ReturnValue, name, sizeof(name))) {
            SC_LOG("replay: demo net driver is live (%s)", name);
        } else {
            SC_LOG("replay: demo net driver is live");
        }
    } else {
        SC_LOG("replay: no demo net driver -- recording did not actually begin");
    }

    g_probe_running = recording;
    g_last_report_ms = GetTickCount();
    return recording;
}

void StopReplayProbe() {
    ue::UObject* system = ResolveReplaySystem();
    ue::UObject* world = ue::GetWorld();
    if (!system || !world) return;

    StopRecordingParams stop = {};
    stop.WorldContextObject = world;
    stop.Reason = 0;
    if (!ue::CallFunction(system, L"BPF_ReplayStopRecording", &stop)) {
        SC_LOG("replay: BPF_ReplayStopRecording did not dispatch");
    } else {
        SC_LOG("replay: stop requested");
    }
    g_probe_running = false;
}

void TickReplayProbe() {
    if (!coop::Get().replay_probe) return;

    // F10 toggles. Edge-detected so holding the key does not thrash it.
    static bool f10_was_down = false;
    const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10_down && !f10_was_down) {
        if (g_probe_running) {
            StopReplayProbe();
        } else {
            StartReplayProbe();
        }
    }
    f10_was_down = f10_down;

    if (!g_probe_running) return;

    const DWORD now = GetTickCount();
    if (now - g_last_report_ms < 2000) return;
    g_last_report_ms = now;

    ue::UObject* system = ResolveReplaySystem();
    ue::UObject* world = ue::GetWorld();
    if (!system || !world) return;

    bool recording = false;
    CallBool(L"BPF_IsRecording", world, &recording);

    WorldContextFloat demo_time = {};
    demo_time.WorldContextObject = world;
    float seconds = -1.f;
    if (ue::CallFunction(system, L"BPF_GetCurrentDemoTimeS", &demo_time)) {
        seconds = demo_time.ReturnValue;
    }

    // The whole point of the probe: how often does Sifu itself decide a
    // fighting character is worth replicating?
    float player_age = -1.f;
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (player) {
        ActorFloat last = {};
        last.Actor = player;
        if (ue::CallFunction(system, L"BPF_GetActorLastReplicationTime", &last)) {
            player_age = last.ReturnValue;
        }
    }

    SC_LOG("replay: recording=%s demo_time=%.2fs player_last_replicated=%.3f",
           recording ? "yes" : "NO", seconds, player_age);

    if (!recording) {
        SC_LOG("replay: recording stopped on its own -- probe going idle");
        g_probe_running = false;
    }
}

}  // namespace sifucoop::game
