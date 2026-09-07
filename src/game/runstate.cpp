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

bool IsSafeToDress(ue::UObject* puppet) {
    if (!puppet) return false;
    ue::UObject* world = ue::GetWorld();
    ue::UObject* local_player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (local_player && puppet == local_player) return false;
    if (puppet == PrimaryPlayerPawn()) return false;
    return true;
}

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

bool WriteCharacterAge(ue::UObject* character, int years) {
    if (!character || years < 0) return false;
    struct StatsRet {
        ue::UObject* ReturnValue;
    } stats = {};
    if (!ue::CallFunction(character, L"BPF_GetStatsComponent", &stats) || !stats.ReturnValue) {
        return false;
    }
    struct AgeArg {
        int Age;
    } arg = {years};
    return ue::CallFunction(stats.ReturnValue, L"BPF_SetCharacterAge", &arg);
}

bool WritePuppetAge(ue::UObject* puppet, int years) {
    return puppet && IsSafeToDress(puppet) && WriteCharacterAge(puppet, years);
}

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

bool WritePuppetOutfit(ue::UObject* puppet, int index) {
    if (!puppet || index < 0 || !IsSafeToDress(puppet)) return false;
    ue::UObject* comp = PlayerFightingComponent(puppet);
    if (!comp) return false;

    if (offsets::M_UPlayerFightingComponent_iOutfitIndex != 0) {
        const std::int32_t exact_index = index;
        std::memcpy(reinterpret_cast<std::uint8_t*>(comp) +
                        offsets::M_UPlayerFightingComponent_iOutfitIndex,
                    &exact_index, sizeof(exact_index));
        if (ue::CallFunction(comp, L"OnRep_OutfitIndex", nullptr)) return true;
    }

    struct SwapArgs {
        std::int32_t Index;
        ue::UObject* MaterialOverride;
        bool Flag;
    } args = {index, nullptr, true};
    return ue::CallFunction(comp, L"BPF_SwapOutfit", &args);
}

bool RefreshPuppetVisualAge(ue::UObject* puppet, int age) {
    if (!puppet || !IsSafeToDress(puppet)) return false;
    ue::UObject* world = ue::GetWorld();
    if (!world) return false;
    ue::UObject* aging = ue::FindObjectByPath(
        L"/Game/Maps/Zoos/Newin/Aging/CharacterAging.Default__CharacterAging_C");
    if (!aging) return false;

    ue::UObject* local = ue::GetPlayerCharacter(world, 0);
    const int old_local_age = local ? ReadLocalAge(local) : -1;
    const bool staged =
        local && old_local_age >= 0 &&
        (old_local_age == age || WriteCharacterAge(local, age));
    if (staged) {
        struct FullParams {
            ue::UObject* Character;
            bool OnlyBodyAging;
            std::uint8_t Padding[7];
            ue::UObject* WorldContextObject;
        } full = {puppet, false, {}, world};
        static_assert(sizeof(FullParams) == 24,
                      "UpdateMorphTexAging parameter frame changed");
        const bool refreshed = ue::CallFunction(aging, L"UpdateMorphTexAging", &full);
        if (old_local_age != age) WriteCharacterAge(local, old_local_age);
        if (refreshed) return true;
    }

    ue::UObject* mesh = ue::GetSkeletalMeshComponent(puppet);
    if (!mesh) return false;
    struct Params {
        float Age;
        std::uint32_t Pad;
        ue::UObject* SkeletalMesh;
        ue::UObject* WorldContextObject;
    } params = {};
    static_assert(sizeof(Params) == 24, "updateMorphTargets parameter frame changed");
    const float clamped =
        age < 0 ? 0.f : (age > 50 ? 50.f : static_cast<float>(age));
    params.Age = clamped / 50.f;
    params.SkeletalMesh = mesh;
    params.WorldContextObject = world;
    return ue::CallFunction(aging, L"updateMorphTargets", &params);
}

bool ReadLocalWeaponPath(ue::UObject* player, char* out, int out_size) {
    out[0] = '\0';
    struct WeaponRet {
        ue::UObject* ReturnValue;
    } weapon = {};
    if (!ue::CallFunction(player, L"BPF_GetPickedUpWeapon", &weapon) || !weapon.ReturnValue) {
        return false;
    }
    struct DataRet {
        ue::UObject* ReturnValue;
    } data = {};
    if (!ue::CallFunction(weapon.ReturnValue, L"BPF_GetWeaponData", &data) || !data.ReturnValue) {
        return false;
    }
    return ue::GetObjectPathName(data.ReturnValue, out, out_size) && out[0] != '\0';
}

bool g_logged_local_probe = false;

int g_last_peer_age = INT_MIN;
int g_last_peer_outfit = INT_MIN;

constexpr const char* kCheatTags[] = {
    "Cheat.AICantDropWeapon",
    "Cheat.AutoAvoid",
    "Cheat.AutoDeflect",
    "Cheat.Autothrow",
    "Cheat.BulletTime",
    "Cheat.BulletTime.Action",
    "Cheat.BulletTime.Focus",
    "Cheat.BulletTime.Guard",
    "Cheat.ChooseAge",
    "Cheat.ChooseAge.20",
    "Cheat.ChooseAge.30",
    "Cheat.ChooseAge.40",
    "Cheat.ChooseAge.50",
    "Cheat.ChooseAge.60",
    "Cheat.ChooseAge.70",
    "Cheat.DamageChangeAge",
    "Cheat.DamageHealthOnly",
    "Cheat.DamageStructureOnly",
    "Cheat.Doppelgangers",
    "Cheat.EnemyRandomMoveset",
    "Cheat.EnvironmentDamage",
    "Cheat.FastEnemies",
    "Cheat.Firewalk",
    "Cheat.FirmGrip",
    "Cheat.FocusDisabled",
    "Cheat.ForceDifficulty",
    "Cheat.ForcedLastMan",
    "Cheat.FreeTakedown",
    "Cheat.GoldenBambooStick",
    "Cheat.GoldenBat",
    "Cheat.GoldenBroom",
    "Cheat.GoldenKodachi",
    "Cheat.GoldenMachete",
    "Cheat.GoldenMop",
    "Cheat.GoldenPipe",
    "Cheat.GoldenStaff",
    "Cheat.GoldenWoodenStick",
    "Cheat.HealthRecoveryLegacy",
    "Cheat.HighVoicePitch",
    "Cheat.InfiniteFocus",
    "Cheat.InfiniteLives",
    "Cheat.InfiniteStructure",
    "Cheat.InvisibleEnemies",
    "Cheat.LargeHitboxes",
    "Cheat.LethalWeapons",
    "Cheat.Lifeline",
    "Cheat.LifestealFoes",
    "Cheat.LockAllShrinesEffects",
    "Cheat.LockAllSkills",
    "Cheat.LowGravity",
    "Cheat.LowVoicePitch",
    "Cheat.MCDamageMultiplier.05",
    "Cheat.MCDamageMultiplier.075",
    "Cheat.MCDamageMultiplier.1",
    "Cheat.MCDamageMultiplier.2",
    "Cheat.MCDamageMultiplier.3",
    "Cheat.MCDamageMultiplier.4",
    "Cheat.MCDoubleDamageReceived",
    "Cheat.MCDoubleHealth",
    "Cheat.MCignoreGuard",
    "Cheat.MCinvicible",
    "Cheat.MCNoHealthRegen",
    "Cheat.MCOneHP",
    "Cheat.MoveSet.540JumpKick",
    "Cheat.MoveSet.BounceKick",
    "Cheat.MoveSet.CobraDoubleBite",
    "Cheat.MoveSet.Combo.Agile",
    "Cheat.MoveSet.Combo.Brawler",
    "Cheat.MoveSet.Combo.PakMei",
    "Cheat.MoveSet.DeadlyWaltz",
    "Cheat.MoveSet.DoubleHitUppercut",
    "Cheat.MoveSet.ElbowStrike",
    "Cheat.MoveSet.FalconPunch",
    "Cheat.MoveSet.FarGrab",
    "Cheat.MoveSet.FrontKick",
    "Cheat.MoveSet.GrabTakedown",
    "Cheat.MoveSet.GSPKickRunning",
    "Cheat.MoveSet.HammerKick",
    "Cheat.MoveSet.JumpKnee",
    "Cheat.MoveSet.LowKick360",
    "Cheat.MoveSet.NajaReverseLash",
    "Cheat.MoveSet.RisingFalcon",
    "Cheat.MoveSet.RushJumpKicks",
    "Cheat.MoveSet.ShoulderStrike",
    "Cheat.MoveSet.SpinTrickKicks",
    "Cheat.MoveSet.Sweep",
    "Cheat.MoveSet.ThrustPalm",
    "Cheat.MoveSet.TornadoKick",
    "Cheat.MoveSet.TripleHitPunish",
    "Cheat.MoveSet.YangGroundPunch",
    "Cheat.NoDeathCounterDecrement",
    "Cheat.NoEnvironmentalWeapons",
    "Cheat.NoPendant",
    "Cheat.OffensiveAvoid",
    "Cheat.OnePunchBreak",
    "Cheat.OnePunchMan",
    "Cheat.Randomizer",
    "Cheat.ReversedAging",
    "Cheat.SlapstickFights",
    "Cheat.SlowEnemies",
    "Cheat.SqueakyToy",
    "Cheat.StrongerArchetypes",
    "Cheat.StructureBreakingDeflect",
    "Cheat.UnlimitedThreats",
    "Cheat.UnlockAllShrineEffects",
    "Cheat.UnlockAllSkills",
    "Cheat.Vampire",
    "Cheat.Voices",
    "Cheat.WalkOnly",
    "Cheat.WeakerArchetype",
    "Cheat.WeaponMultiplier",
};
static_assert(sizeof(kCheatTags) / sizeof(kCheatTags[0]) == net::kCheatTagCount,
              "update cheat tag map when BP_CheatSettings changes");

bool CheatBit(const net::CheatSnapshot& snapshot, int index) {
    return (snapshot.active[index / 8] & (1u << (index % 8))) != 0;
}

void SetCheatBit(net::CheatSnapshot* snapshot, int index, bool active) {
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << (index % 8));
    if (active) snapshot->active[index / 8] |= mask;
    else snapshot->active[index / 8] &= static_cast<std::uint8_t>(~mask);
}

ue::UObject* CheatHelper() {
    return ue::FindObjectByPath(L"/Script/SCCore.Default__CheatManagerBlueprintHelper");
}

bool MakeCheatTag(const char* text, ue::FName* out) {
    wchar_t wide[96] = {};
    return text && out && MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 96) > 0 &&
           ue::MakeFName(wide, out);
}

bool ReadActivatedCheats(net::CheatSnapshot* out) {
    if (!out) return false;
    ue::UObject* helper = CheatHelper();
    if (!helper) return false;
    *out = {};
    for (int i = 0; i < net::kCheatTagCount; ++i) {
        struct Params {
            ue::FName cheat_wanted;
            std::uint8_t return_value;
            std::uint8_t padding[7];
        } params = {};
        if (!MakeCheatTag(kCheatTags[i], &params.cheat_wanted) ||
            !ue::CallFunction(helper, L"BPF_IsCheatActivated", &params)) {
            return false;
        }
        SetCheatBit(out, i, params.return_value != 0);
    }
    return true;
}

bool SetCheatActivated(int index, bool active) {
    if (index < 0 || index >= net::kCheatTagCount) return false;
    ue::UObject* helper = CheatHelper();
    if (!helper) return false;
    struct Params {
        ue::FName cheat;
    } params = {};
    if (!MakeCheatTag(kCheatTags[index], &params.cheat)) return false;
    return ue::CallFunction(helper, active ? L"BPF_ActivateCheat" : L"BPF_DeactivateCheat",
                            &params);
}

int CountCheats(const net::CheatSnapshot& snapshot) {
    int count = 0;
    for (int i = 0; i < net::kCheatTagCount; ++i) if (CheatBit(snapshot, i)) ++count;
    return count;
}

void TickCheatAuthority() {
    if (coop::Get().mode != coop::Mode::Coop) return;
    static net::CheatSnapshot client_selection_before_session = {};
    static bool client_selection_saved = false;
    static DWORD last_poll = 0;
    const DWORD now = GetTickCount();
    if (now - last_poll < 1000) return;
    last_poll = now;

    if (!net::IsConnected()) {
        if (!client_selection_saved) return;
        net::CheatSnapshot current = {};
        if (!ReadActivatedCheats(&current)) return;
        int restored = 0;
        for (int i = 0; i < net::kCheatTagCount; ++i) {
            const bool original = CheatBit(client_selection_before_session, i);
            if (CheatBit(current, i) == original) continue;
            if (SetCheatActivated(i, original)) ++restored;
        }
        client_selection_saved = false;
        SC_LOG("cheats: restored joiner's %d pre-session selection change(s)", restored);
        return;
    }

    if (net::GetRole() == net::Role::Host) {
        net::CheatSnapshot local = {};
        if (!ReadActivatedCheats(&local)) {
            static DWORD last_error = 0;
            if (now - last_error >= 10000) {
                last_error = now;
                SC_LOG("cheats: helper API unavailable; host settings were not sent");
            }
            return;
        }
        net::SendCheatState(local);
        static net::CheatSnapshot last_sent = {};
        static bool have_last_sent = false;
        if (!have_last_sent || memcmp(last_sent.active, local.active, sizeof(local.active)) != 0) {
            last_sent = local;
            have_last_sent = true;
            SC_LOG("cheats: host published %d selected modifier(s)/cheat(s)", CountCheats(local));
        }
        return;
    }

    net::CheatSnapshot wanted = {};
    if (!net::GetHostCheatState(&wanted)) return;
    net::CheatSnapshot local = {};
    if (!ReadActivatedCheats(&local)) {
        static DWORD last_error = 0;
        if (now - last_error >= 10000) {
            last_error = now;
            SC_LOG("cheats: helper API unavailable; host settings were not applied");
        }
        return;
    }
    if (!client_selection_saved) {
        client_selection_before_session = local;
        client_selection_saved = true;
    }
    int changed = 0;
    int failed = 0;
    for (int i = 0; i < net::kCheatTagCount; ++i) {
        const bool desired = CheatBit(wanted, i);
        if (CheatBit(local, i) == desired) continue;
        if (SetCheatActivated(i, desired)) ++changed;
        else ++failed;
    }
    if (changed || failed) {
        SC_LOG("cheats: host authority applied %d selection change(s)%s", changed,
               failed ? "; one or more game calls failed" : "");
    }
}
DWORD g_last_send = 0;

}

void TickRunState(ue::UObject* player) {
    if (!coop::Get().sync_run_state) return;
    if (!player) return;

    if (!net::IsConnected()) {
        TickCheatAuthority();

        if (!g_logged_local_probe) {
            const int age = ReadLocalAge(player);
            if (age >= 0) {
                SC_LOG("run: local read ok -- age=%d", age);
                g_logged_local_probe = true;
            }
        }
        return;
    }

    const DWORD now = GetTickCount();
    static ue::UObject* visual_age_puppet = nullptr;
    static ue::UObject* visual_age_world = nullptr;
    static DWORD visual_age_due = 0;
    static int visual_age_attempts = 0;
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
        if (ReadLocalWeaponPath(player, local.weapon_path, sizeof(local.weapon_path))) {
            local.has_weapon = true;
        }
        net::SendRunState(local);
    }

    net::RunSnapshot peer;
    if (net::GetPeerRunState(&peer)) {
        const int peer_age = peer.age_valid ? peer.age : INT_MIN;
        const int peer_outfit = peer.outfit_valid ? peer.outfit_index : INT_MIN;
        if (peer_age != g_last_peer_age || peer_outfit != g_last_peer_outfit) {
            g_last_peer_age = peer_age;
            g_last_peer_outfit = peer_outfit;
            char age_buf[16] = "n/a";
            char outfit_buf[16] = "n/a";
            if (peer_age != INT_MIN) wsprintfA(age_buf, "%d", peer_age);
            if (peer_outfit != INT_MIN) wsprintfA(outfit_buf, "%d", peer_outfit);
            SC_LOG("run: peer age=%s outfit=%s weapon=%s", age_buf, outfit_buf,
                   peer.has_weapon ? peer.weapon_path : "none");
        }
    }

    if (coop::Get().sync_peer_age && coop::Get().mode == coop::Mode::Coop) {
        net::RunSnapshot ages;
        ue::UObject* puppet = GetPuppet();
        if (puppet && net::GetPeerRunState(&ages) && ages.age_valid && ages.age >= 0) {
            static ue::UObject* aged_puppet = nullptr;
            static ue::UObject* aged_world = nullptr;
            static int aged_to = INT_MIN;
            static DWORD last_age_attempt = 0;
            if (!IsSafeToDress(puppet)) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    SC_LOG("run: refused to age that body -- it is the one controller 0 is "
                           "possessing, so it is YOURS, not your partner's");
                }
            } else if ((ue::GetWorld() != aged_world || puppet != aged_puppet ||
                        ages.age != aged_to) &&
                       now - last_age_attempt >= 1000) {
                last_age_attempt = now;
                const bool ok = WritePuppetAge(puppet, ages.age);
                if (ok) {
                    aged_puppet = puppet;
                    aged_world = ue::GetWorld();
                    aged_to = ages.age;
                    if (coop::Get().sync_peer_visual_age) {
                        visual_age_puppet = puppet;
                        visual_age_world = ue::GetWorld();
                        visual_age_due = now + 500;
                        visual_age_attempts = 0;
                    }
                }
                SC_LOG("run: partner's age stat set to %d %s", ages.age,
                       ok ? (coop::Get().sync_peer_visual_age
                                 ? "(visible aging refresh scheduled)"
                                 : "(stat only; visual refresh switch is off)")
                          : "FAILED -- BPF_SetCharacterAge did not dispatch");
            }
        }
    }

    if (coop::Get().sync_peer_age && coop::Get().mode == coop::Mode::Coop) {
        net::RunSnapshot outfit_state;
        ue::UObject* outfit_puppet = GetPuppet();
        if (outfit_puppet && net::GetPeerRunState(&outfit_state) && outfit_state.outfit_valid) {
            static ue::UObject* dressed_puppet = nullptr;
            static ue::UObject* dressed_world = nullptr;
            static int dressed_as = INT_MIN;
            static DWORD last_outfit_attempt = 0;
            if (IsSafeToDress(outfit_puppet) &&
                (ue::GetWorld() != dressed_world || outfit_puppet != dressed_puppet ||
                 outfit_state.outfit_index != dressed_as) &&
                now - last_outfit_attempt >= 1000) {
                last_outfit_attempt = now;
                const bool ok = WritePuppetOutfit(outfit_puppet, outfit_state.outfit_index);
                if (ok) {
                    dressed_puppet = outfit_puppet;
                    dressed_world = ue::GetWorld();
                    dressed_as = outfit_state.outfit_index;

                    if (coop::Get().sync_peer_visual_age) {
                        visual_age_puppet = outfit_puppet;
                        visual_age_world = ue::GetWorld();
                        visual_age_due = now + 500;
                        visual_age_attempts = 0;
                    }
                }
                SC_LOG("run: partner's outfit set to %d %s", outfit_state.outfit_index,
                       ok ? "" : "FAILED -- BPF_SwapOutfit did not dispatch");
            }
        }
    }

    if (visual_age_puppet && visual_age_due != 0 &&
        static_cast<LONG>(now - visual_age_due) >= 0) {

        if (ue::GetWorld() != visual_age_world || GetPuppet() != visual_age_puppet) {
            visual_age_puppet = nullptr;
            visual_age_world = nullptr;
            visual_age_due = 0;
            visual_age_attempts = 0;
        } else if (RefreshPuppetVisualAge(visual_age_puppet, g_last_peer_age)) {
            SC_LOG("run: partner's visible face/body aging refreshed");
            visual_age_puppet = nullptr;
            visual_age_world = nullptr;
            visual_age_due = 0;
            visual_age_attempts = 0;
        } else if (++visual_age_attempts < 10) {
            visual_age_due = now + 1000;
        } else {
            SC_LOG("run: partner visual aging unavailable -- CharacterAging CDO/function "
                   "did not resolve after 10 guarded attempts");
            visual_age_puppet = nullptr;
            visual_age_world = nullptr;
            visual_age_due = 0;
            visual_age_attempts = 0;
        }
    }

    TickCheatAuthority();

}

}
