#include "runstate.h"

#include <windows.h>

#include <climits>
#include <cstdint>
#include <cstring>

#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../net/session.h"
#include "../ue/reflection.h"
#include "coop.h"
#include "puppet.h"

namespace sifucoop::game {
namespace {

namespace ue = sifucoop::ue;
namespace net = sifucoop::net;
namespace coop = sifucoop::coop;
namespace offsets = sifucoop::offsets;

// player -> UStatsComponent -> character age. Both are reflection getters, so
// this reads real data without dereferencing any unknown field. Returns -1 when
// the chain is unavailable (e.g. before a character exists).
int ReadLocalAge(ue::UObject* player) {
    struct StatsRet {
        ue::UObject* ReturnValue;
    } stats = {};
    if (!ue::CallFunction(player, L"BPF_GetStatsComponent", &stats) || !stats.ReturnValue) {
        return -1;
    }
    struct AgeRet {
        int ReturnValue;
    } age = {};
    if (!ue::CallFunction(stats.ReturnValue, L"BPF_GetCharacterAge", &age)) return -1;
    return age.ReturnValue;
}

// The same chain, written instead of read, and only ever aimed at the PUPPET.
//
// Sifu's characters are aged by their stats component and their appearance
// follows from it, so a clone of the local player carries the LOCAL player's
// age -- which is why the host at 48 saw two old men and the joiner at 20 saw
// two young ones, each machine showing its partner at its own age. The peer's
// real age has been on the wire and in the log all along
// (`run: peer age=...`); nothing consumed it.
//
// BPF_SetCharacterAge is Blueprint-exposed (it has an exec thunk), so this is
// reflection like everything around it -- no offset, no unknown field. Aimed at
// the puppet and nowhere else: the local player's age is their own run and must
// never be written from the network.
bool WritePuppetAge(ue::UObject* puppet, int years) {
    if (!puppet || years < 0) return false;
    struct StatsRet {
        ue::UObject* ReturnValue;
    } stats = {};
    if (!ue::CallFunction(puppet, L"BPF_GetStatsComponent", &stats) || !stats.ReturnValue) {
        return false;
    }
    struct AgeArg {
        int Age;
    } arg = {years};
    return ue::CallFunction(stats.ReturnValue, L"BPF_SetCharacterAge", &arg);
}

// player -> currently held weapon actor -> its UBaseWeaponData asset -> the
// asset's object path, which is portable across machines exactly like a combo
// tree or a level package. Empty string when the player is unarmed.
bool ReadLocalWeaponPath(ue::UObject* player, char* out, int out_size) {
    out[0] = '\0';
    struct WeaponRet {
        ue::UObject* ReturnValue;
    } weapon = {};
    if (!ue::CallFunction(player, L"BPF_GetPickedUpWeapon", &weapon) || !weapon.ReturnValue) {
        return false;  // unarmed -- not an error
    }
    struct DataRet {
        ue::UObject* ReturnValue;
    } data = {};
    if (!ue::CallFunction(weapon.ReturnValue, L"BPF_GetWeaponData", &data) || !data.ReturnValue) {
        return false;
    }
    return ue::GetObjectPathName(data.ReturnValue, out, out_size) && out[0] != '\0';
}

// UGameplayStatics::GetGameState is a static Blueprint function; it is reached
// by calling it on the class's default object, which the reflection layer can
// resolve by path. No engine-layout guesswork.
ue::UObject* GetGameState() {
    ue::UObject* world = ue::GetWorld();
    if (!world) return nullptr;
    ue::UObject* statics = ue::FindObjectByPath(L"/Script/Engine.Default__GameplayStatics");
    if (!statics) return nullptr;
    struct Params {
        ue::UObject* WorldContextObject;
        ue::UObject* ReturnValue;
    } params = {};
    params.WorldContextObject = world;
    if (!ue::CallFunction(statics, L"GetGameState", &params)) return nullptr;
    return params.ReturnValue;
}

// Reads AThePlainesGameState::m_fRoomClearedLifePercent (offset from the PDB).
// Guarded twice: the game state's own name must identify it as a ThePlaines
// state (subclasses inherit the layout, so the offset stays valid), and the
// value must land in 0..1. Either guard failing means "no reading", never a
// blind dereference -- on a menu the active game state is a different class.
bool ReadLocalRoomClear(float* out) {
    if (offsets::M_AThePlainesGameState_fRoomClearedLifePercent == 0) return false;
    ue::UObject* game_state = GetGameState();
    if (!game_state) return false;

    char path[256] = {};
    if (!ue::GetObjectPathName(game_state, path, sizeof(path))) return false;
    if (!std::strstr(path, "ThePlaines")) return false;

    const float value = *reinterpret_cast<const float*>(
        reinterpret_cast<std::uintptr_t>(game_state) +
        offsets::M_AThePlainesGameState_fRoomClearedLifePercent);
    if (!(value >= 0.f && value <= 1.f)) return false;
    *out = value;
    return true;
}

// A menu is not the place to talk about age or rooms, so a one-time proof that
// the local getters resolve is enough when offline.
bool g_logged_local_probe = false;

// Change-triggered peer logging: the numbers move slowly, so a line per change
// is informative rather than noise.
int g_last_peer_age = INT_MIN;
int g_last_peer_room_pct = INT_MIN;  // rounded to whole percent for the compare

DWORD g_last_send = 0;

}  // namespace

void TickRunState(ue::UObject* player) {
    if (!coop::Get().sync_run_state) return;
    if (!player) return;

    if (!net::IsConnected()) {
        // Offline: prove once that the read path works (verifiable on one
        // machine), then stay quiet until a peer is actually present.
        if (!g_logged_local_probe) {
            const int age = ReadLocalAge(player);
            float room = -1.f;
            const bool have_room = ReadLocalRoomClear(&room);
            if (age >= 0) {
                if (have_room) {
                    SC_LOG("run: local read ok -- age=%d room-clear=%.0f%%", age,
                           room * 100.f);
                } else {
                    SC_LOG("run: local read ok -- age=%d room-clear=n/a", age);
                }
                g_logged_local_probe = true;
            }
        }
        return;
    }

    const DWORD now = GetTickCount();
    if (now - g_last_send >= 500) {
        g_last_send = now;

        net::RunSnapshot local;
        const int age = ReadLocalAge(player);
        if (age >= 0) {
            local.age = age;
            local.age_valid = true;
        }
        float room = -1.f;
        if (ReadLocalRoomClear(&room)) {
            local.room_clear_percent = room;
            local.room_clear_valid = true;
        }
        if (ReadLocalWeaponPath(player, local.weapon_path, sizeof(local.weapon_path))) {
            local.has_weapon = true;
        }
        net::SendRunState(local);
    }

    // Peer side: surface it in the log when it changes. Applying it back into
    // the game is intentionally not done -- see below.
    net::RunSnapshot peer;
    if (net::GetPeerRunState(&peer)) {
        const int peer_room_pct =
            peer.room_clear_valid ? static_cast<int>(peer.room_clear_percent * 100.f + 0.5f)
                                  : INT_MIN;
        const int peer_age = peer.age_valid ? peer.age : INT_MIN;
        if (peer_age != g_last_peer_age || peer_room_pct != g_last_peer_room_pct) {
            g_last_peer_age = peer_age;
            g_last_peer_room_pct = peer_room_pct;
            char age_buf[16] = "n/a";
            char room_buf[16] = "n/a";
            if (peer_age != INT_MIN) wsprintfA(age_buf, "%d", peer_age);
            if (peer_room_pct != INT_MIN) wsprintfA(room_buf, "%d%%", peer_room_pct);
            SC_LOG("run: peer age=%s room=%s weapon=%s", age_buf, room_buf,
                   peer.has_weapon ? peer.weapon_path : "none");
        }
    }

    // Age IS applied, to the puppet, because a partner shown at your own age is
    // a visible defect rather than a missing feature. Whether Sifu refreshes the
    // model from this on its own is exactly what the next run measures, so the
    // outcome is logged either way rather than assumed.
    if (coop::Get().sync_peer_age && coop::Get().mode == coop::Mode::Coop) {
        net::RunSnapshot ages;
        ue::UObject* puppet = GetPuppet();
        if (puppet && net::GetPeerRunState(&ages) && ages.age_valid && ages.age >= 0) {
            static ue::UObject* aged_puppet = nullptr;
            static int aged_to = INT_MIN;
            if (puppet != aged_puppet || ages.age != aged_to) {
                const bool ok = WritePuppetAge(puppet, ages.age);
                aged_puppet = puppet;
                aged_to = ages.age;
                SC_LOG("run: partner's body aged to %d %s", ages.age,
                       ok ? "-- if they still look your age, the model does not follow the "
                            "stats component and needs a mesh refresh"
                          : "FAILED -- BPF_SetCharacterAge did not dispatch");
            }
        }
    }

    // NOT applied on purpose:
    //  - Room-clear: COOP-PLAN.md 9 argues the joiner's room should clear on its
    //    own once its local enemies die (which the mod already syncs). Whether
    //    that holds is a two-machine test; until it is run, writing the host's
    //    percentage into the local game state -- with unverified semantics and
    //    direction -- would be a guess. `fix_room_clear` stays dormant.
    //  - Weapon: making the puppet hold the peer's weapon means spawning and
    //    attaching a weapon actor, the highest-risk unverifiable change on the
    //    board. The path is captured and sent so the data is ready the day that
    //    is built; nothing consumes it yet.
    if (coop::Get().fix_room_clear) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SC_LOG("run: fix_room_clear is on, but applying the peer's room-clear "
                   "is intentionally not implemented yet -- see COOP-PLAN.md 9");
        }
    }
}

}  // namespace sifucoop::game
