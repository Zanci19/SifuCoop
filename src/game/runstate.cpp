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
#include "player2.h"
#include "puppet.h"

namespace sifucoop::game {
namespace {

namespace ue = sifucoop::ue;
namespace net = sifucoop::net;
namespace coop = sifucoop::coop;
namespace offsets = sifucoop::offsets;

ue::UObject* PlayerFightingComponent(ue::UObject* character);

// Never write cosmetics onto the local player.
//
// Reported after the first version of this shipped: on the joiner, whose player
// is 20, BOTH bodies started showing the host's age. The write was landing on
// the local character. ApplyPeerVitals has carried the guard against exactly
// this since it was written -- "Sifu's game mode has been observed handing the
// second player the FIRST player's character, and it does that on respawn and
// travel as well as at creation" -- and these two writes were added without it.
//
// Both directions of the same test, for the same reason it is done twice there:
// GetPlayerCharacter(0) can lag the possession by a frame.
bool IsSafeToDress(ue::UObject* puppet) {
    if (!puppet) return false;
    ue::UObject* world = ue::GetWorld();
    ue::UObject* local_player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (local_player && puppet == local_player) return false;
    if (puppet == PrimaryPlayerPawn()) return false;
    return true;
}

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
    if (!puppet || years < 0 || !IsSafeToDress(puppet)) return false;
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

// player -> UPlayerFightingComponent -> m_iOutfitIndex.
//
// Sifu has no BPF_GetOutfitIndex, so the index is read from the property
// directly at an offset the build tool resolves from the PDB -- the same way
// health, guard and faction are read. Writing it DOES have a Blueprint entry
// point, BPF_SwapOutfit, so the round trip is a raw read and a reflected write.
// AFightingCharacter has no BPF getter for it -- the whole BPF_ surface was
// listed and there is none -- so it is reached the way the capsule already is:
// resolve the class and ask AActor::GetComponentByClass.
ue::UObject* PlayerFightingComponent(ue::UObject* character) {
    if (!character) return nullptr;
    void* klass = ue::FindObjectByPath(L"/Script/Sifu.PlayerFightingComponent");
    if (!klass) return nullptr;
    struct ComponentParams {
        void* ComponentClass;
        ue::UObject* ReturnValue;
    } params = {};
    params.ComponentClass = klass;
    if (!ue::CallFunction(character, L"GetComponentByClass", &params)) return nullptr;
    return params.ReturnValue;
}

int ReadLocalOutfit(ue::UObject* player) {
    if (offsets::M_UPlayerFightingComponent_iOutfitIndex == 0) return -1;
    ue::UObject* comp_object = PlayerFightingComponent(player);
    if (!comp_object) return -1;
    struct CompRet {
        ue::UObject* ReturnValue;
    } comp = {comp_object};
    std::int32_t index = 0;
    std::memcpy(&index,
                reinterpret_cast<const std::uint8_t*>(comp.ReturnValue) +
                    offsets::M_UPlayerFightingComponent_iOutfitIndex,
                sizeof(index));
    return (index < 0 || index > 64) ? -1 : index;
}

// Rebuild the body after writing stats to it.
//
// The age write works and has been measured working: "aged to 45" followed by
// "max health here 96, theirs 96 -- agreed", so BPF_SetCharacterAge reaches
// everything computed from it. The FACE still did not change, and the reason is
// that Sifu rebuilds the model from a callback rather than polling the stats:
// UPlayerFightingComponent::OnStatsUpdated, private, void(), no arguments, one
// symbol at its RVA on both builds. Writing the number without ringing the bell
// left the character aged on paper and unchanged on screen.
//
// Direct native call rather than reflection because it is not Blueprint-exposed.
// Trivial ABI -- a this pointer and nothing else.
using OnStatsUpdatedFn = void(__fastcall*)(ue::UObject* component);
OnStatsUpdatedFn g_on_stats_updated = nullptr;

bool RefreshAppearance(ue::UObject* puppet) {
    // Bound on first use from the game module, so this file needs no init hook.
    static bool bound = false;
    if (!bound) {
        bound = true;
        if (offsets::UPlayerFightingComponent_OnStatsUpdated != 0) {
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
            g_on_stats_updated = reinterpret_cast<OnStatsUpdatedFn>(
                base + offsets::UPlayerFightingComponent_OnStatsUpdated);
        }
    }
    if (!g_on_stats_updated || !puppet) return false;
    ue::UObject* comp = PlayerFightingComponent(puppet);
    if (!comp) return false;
    g_on_stats_updated(comp);
    return true;
}

// Aimed at the PUPPET only, exactly like the age write. The second argument is
// an optional material override; passing null means "just the outfit".
bool WritePuppetOutfit(ue::UObject* puppet, int index) {
    if (!puppet || index < 0 || !IsSafeToDress(puppet)) return false;
    ue::UObject* comp = PlayerFightingComponent(puppet);
    if (!comp) return false;
    // Three parameters, from the decorated name:
    //   BPF_SwapOutfit(int32, UMaterialInterface*, bool)
    // The second is an optional material override -- null means "just the
    // outfit" -- and the third is a flag whose meaning is unknown, so it is left
    // false. Getting the count wrong here would hand ProcessEvent a short frame.
    struct SwapArgs {
        std::int32_t Index;
        ue::UObject* MaterialOverride;
        bool Flag;
    } args = {index, nullptr, false};
    return ue::CallFunction(comp, L"BPF_SwapOutfit", &args);
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
        const int outfit = ReadLocalOutfit(player);
        if (outfit >= 0) {
            local.outfit_index = outfit;
            local.outfit_valid = true;
        }
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
            if (!IsSafeToDress(puppet)) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    SC_LOG("run: refused to age that body -- it is the one controller 0 is "
                           "possessing, so it is YOURS, not your partner's");
                }
            } else if (puppet != aged_puppet || ages.age != aged_to) {
                const bool ok = WritePuppetAge(puppet, ages.age);
                aged_puppet = puppet;
                aged_to = ages.age;
                const bool refreshed = ok && RefreshAppearance(puppet);
                SC_LOG("run: partner's body aged to %d %s", ages.age,
                       !ok ? "FAILED -- BPF_SetCharacterAge did not dispatch"
                           : refreshed ? "and the model was rebuilt (OnStatsUpdated)"
                                       : "but OnStatsUpdated is unavailable -- the number "
                                         "changed and the face will not");
            }
        }
    }

    // Costume, same rule and the same reason as age: the puppet is a CLONE of
    // the local player, so without this it wears whatever this machine's player
    // is wearing and both characters look identical on both screens. Written to
    // the puppet only.
    if (coop::Get().sync_peer_age && coop::Get().mode == coop::Mode::Coop) {
        net::RunSnapshot outfit_state;
        ue::UObject* outfit_puppet = GetPuppet();
        if (outfit_puppet && net::GetPeerRunState(&outfit_state) && outfit_state.outfit_valid) {
            static ue::UObject* dressed_puppet = nullptr;
            static int dressed_as = INT_MIN;
            if (IsSafeToDress(outfit_puppet) &&
                (outfit_puppet != dressed_puppet || outfit_state.outfit_index != dressed_as)) {
                const bool ok = WritePuppetOutfit(outfit_puppet, outfit_state.outfit_index);
                dressed_puppet = outfit_puppet;
                dressed_as = outfit_state.outfit_index;
                SC_LOG("run: partner's outfit set to %d %s", outfit_state.outfit_index,
                       ok ? "" : "FAILED -- BPF_SwapOutfit did not dispatch");
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
