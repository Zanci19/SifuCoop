#include "orders.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../../third_party/minhook/include/MinHook.h"
#include "../core/log.h"
#include "../core/offsets.g.h"
#include "../net/protocol.h"
#include "../net/session.h"
#include "actors.h"
#include "coop.h"
#include "enemies.h"
#include "puppet.h"

namespace sifucoop::game {
namespace {

namespace offsets = sifucoop::offsets;
namespace ue = sifucoop::ue;
namespace net = sifucoop::net;
namespace coop = sifucoop::coop;




using OnLocalPlayOrderFn = void(__fastcall*)(void* self, void* shared_ptr);

using SaveToFn = void(__fastcall*)(void* self, void* buffer);









extern "C" {
void* g_playorder_trampoline = nullptr;
}

extern "C" void sifucoop_on_playorder(void* self, unsigned int order_type,
                                      const void* net_order_struct,
                                      const void* play_order_infos);

asm(".globl sifucoop_playorder_detour\n"
    "sifucoop_playorder_detour:\n"
    "  sub  $0x48, %rsp\n"
    "  mov  %rcx, 0x20(%rsp)\n"
    "  mov  %rdx, 0x28(%rsp)\n"
    "  mov  %r8,  0x30(%rsp)\n"
    "  mov  %r9,  0x38(%rsp)\n"
    "  call sifucoop_on_playorder\n"
    "  mov  0x20(%rsp), %rcx\n"
    "  mov  0x28(%rsp), %rdx\n"
    "  mov  0x30(%rsp), %r8\n"
    "  mov  0x38(%rsp), %r9\n"
    "  add  $0x48, %rsp\n"
    "  jmp  *g_playorder_trampoline(%rip)\n");

extern "C" void sifucoop_playorder_detour();











extern "C" {
void* g_multicast_trampoline = nullptr;
}

extern "C" void sifucoop_on_multicast(void* self, unsigned int order_type, unsigned int flag,
                                      const void* buffer);

asm(".globl sifucoop_multicast_detour\n"
    "sifucoop_multicast_detour:\n"
    "  sub  $0x48, %rsp\n"
    "  mov  %rcx, 0x20(%rsp)\n"
    "  mov  %rdx, 0x28(%rsp)\n"
    "  mov  %r8,  0x30(%rsp)\n"
    "  mov  %r9,  0x38(%rsp)\n"
    "  call sifucoop_on_multicast\n"
    "  mov  0x20(%rsp), %rcx\n"
    "  mov  0x28(%rsp), %rdx\n"
    "  mov  0x30(%rsp), %r8\n"
    "  mov  0x38(%rsp), %r9\n"
    "  add  $0x48, %rsp\n"
    "  jmp  *g_multicast_trampoline(%rip)\n");

extern "C" void sifucoop_multicast_detour();









extern "C" {
void* g_prepare_attack_trampoline = nullptr;
}

extern "C" void sifucoop_on_prepare_attack(void* self, const void* delayed_action);

asm(".globl sifucoop_prepare_attack_detour\n"
    "sifucoop_prepare_attack_detour:\n"
    "  sub  $0x48, %rsp\n"
    "  mov  %rcx, 0x20(%rsp)\n"
    "  mov  %rdx, 0x28(%rsp)\n"
    "  mov  %r8,  0x30(%rsp)\n"
    "  mov  %r9,  0x38(%rsp)\n"
    "  call sifucoop_on_prepare_attack\n"
    "  mov  0x20(%rsp), %rcx\n"
    "  mov  0x28(%rsp), %rdx\n"
    "  mov  0x30(%rsp), %r8\n"
    "  mov  0x38(%rsp), %r9\n"
    "  add  $0x48, %rsp\n"
    "  jmp  *g_prepare_attack_trampoline(%rip)\n");

extern "C" void sifucoop_prepare_attack_detour();











extern "C" {
void* g_launch_attack_trampoline = nullptr;
}

extern "C" void sifucoop_on_launch_attack(void* self, const void* order_ref,
                                          unsigned int quadrant, unsigned int flag);

asm(".globl sifucoop_launch_attack_detour\n"
    "sifucoop_launch_attack_detour:\n"
    "  sub  $0x48, %rsp\n"
    "  mov  %rcx, 0x20(%rsp)\n"
    "  mov  %rdx, 0x28(%rsp)\n"
    "  mov  %r8,  0x30(%rsp)\n"
    "  mov  %r9,  0x38(%rsp)\n"
    "  call sifucoop_on_launch_attack\n"
    "  mov  0x20(%rsp), %rcx\n"
    "  mov  0x28(%rsp), %rdx\n"
    "  mov  0x30(%rsp), %r8\n"
    "  mov  0x38(%rsp), %r9\n"
    "  add  $0x48, %rsp\n"
    "  jmp  *g_launch_attack_trampoline(%rip)\n");

extern "C" void sifucoop_launch_attack_detour();













using GeNextAttackIDFn = int(__fastcall*)(const void* self, const void* character,
                                          const void* combo, unsigned int transition,
                                          void* trace);

GeNextAttackIDFn g_original_next_attack_id = nullptr;





using LaunchAIAttackFn = unsigned char(__fastcall*)(void* character, void* attack_component,
                                                    void* blackboard,
                                                    void* ai_fighting_component);
using GetAttackHandlerFn = void*(__fastcall*)(void* ai_fighting_component);
using PrepareNextAIAttackFn = unsigned char(__fastcall*)(void* attack_handler);
using SetNextAttackTargetFn = void(__fastcall*)(void* attack_component, void* target);

LaunchAIAttackFn g_launch_ai_attack = nullptr;
GetAttackHandlerFn g_get_attack_handler = nullptr;
PrepareNextAIAttackFn g_prepare_next_ai_attack = nullptr;
SetNextAttackTargetFn g_original_set_next_attack_target = nullptr;
using SetIsDownFn = void(__fastcall*)(void* health_component, bool down);
SetIsDownFn g_original_set_is_down = nullptr;
void* g_replay_attack_component = nullptr;
void* g_replay_attack_target = nullptr;

void __fastcall SetNextAttackTargetHook(void* attack_component, void* target) {




    if (attack_component == g_replay_attack_component && g_replay_attack_target) {
        target = g_replay_attack_target;
    }
    g_original_set_next_attack_target(attack_component, target);
}


void __fastcall SetIsDownHook(void* health_component, bool down) {

    if (!down && EnemyMustRemainDead(health_component)) {
        static unsigned int suppressed = 0;
        if (++suppressed <= 12 || coop::Get().verbose_enemies) {
            SC_LOG("death: suppressed native stand-up for replicated corpse %08X",
                   EnemyHashForHealthComponent(health_component));
        }
        return;
    }
    g_original_set_is_down(health_component, down);
}






using HealthKillFn = void(__fastcall*)(void* health_component, std::int32_t behavior,
                                      ue::UObject* instigator, ue::UObject* death_animation,
                                      bool option_a, bool option_b);
HealthKillFn g_original_health_kill = nullptr;
void* g_replicated_kill_health = nullptr;
ue::UObject* g_replicated_kill_instigator = nullptr;

void __fastcall HealthKillHook(void* health_component, std::int32_t behavior,
                               ue::UObject* instigator, ue::UObject* death_animation,
                               bool option_a, bool option_b) {
    if (!instigator && health_component == g_replicated_kill_health &&
        g_replicated_kill_instigator) {
        instigator = g_replicated_kill_instigator;
        static bool first = true;
        if (first) {
            first = false;
            SC_LOG("death: synchronized lethal hit received its remote instigator");
        }
    }
    std::uint32_t actor_hash = 0;
    char death_path[192] = {};
    const net::Role role = net::GetRole();
    const bool publish_death = (role == net::Role::Host && coop::Get().echo_enemy_attacks) ||
                               (role == net::Role::Client &&
                                coop::Get().sync_enemy_death_animations);
    if (net::IsConnected() && publish_death && death_animation) {
        actor_hash = EnemyHashForHealthComponent(health_component);
        if (!actor_hash || !EnemyActionsAreLocallyAuthoritative(actor_hash, true) ||
            !ue::GetObjectPathName(death_animation, death_path, sizeof(death_path))) {
            actor_hash = 0;
        }
    }

    g_original_health_kill(health_component, behavior, instigator, death_animation,
                           option_a, option_b);

    if (actor_hash && death_path[0]) {
        net::SendAnimationSequence(death_path, actor_hash, net::AnimationSemantic::Death);
        SC_LOG("death: exact enemy sequence sent actor=%08X", actor_hash);
    }
}




const void* g_forced_character = nullptr;
int g_forced_attack_id = -1;

int __fastcall GeNextAttackIDHook(const void* self, const void* character, const void* combo,
                                  unsigned int transition, void* trace) {
    if (g_forced_attack_id >= 0 && character == g_forced_character) {
        const int forced = g_forced_attack_id;
        g_forced_attack_id = -1;
        g_forced_character = nullptr;

        static unsigned long long count = 0;
        if (++count <= 20) SC_LOG("select: forcing attack id %d", forced);
        return forced;
    }
    return g_original_next_attack_id(self, character, combo, transition, trace);
}









using PlayOrderFn = unsigned char(__fastcall*)(void* self, unsigned int order_type,
                                               const void* net_order_struct,
                                               const void* play_order_infos);

bool g_mirror_enabled = true;
bool g_mirroring = false;
std::uintptr_t g_module_base = 0;














constexpr std::size_t kOrderPayloadSize = 256;
constexpr std::size_t kOrderScratchSize = 1024;

struct OrderWire {
    std::uint32_t order_type = 0;
    std::uint32_t vtable_rva = 0;
    std::uint8_t payload[kOrderPayloadSize] = {};
};



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














const char* OrderTypeName(unsigned int type) {
    static const char* kNames[] = {
        "Attack", "Dodge", "ParryVictim", "Hitted", "Guard", "Avoided", "FreezeFrame",
        "WeaponAction", "TakedownInstigator", "TakedownVictim", "KnockedDown", "Dizzy", "Pushed",
        "PlayAnim", "Parry", "GrabInstigator", "GrabVictim", "FightingStateRecovery",
        "DownBeforeStandup", "Standup", "UseMovable", "ThrowObject", "PushObject", "PickUpObject",
        "DropObject", "PushInstigator", "PushVictim", "FallFromPushed", "FallReception",
        "FallGetUp", "Reaction", "IdleExit", "Traversal", "StructureBroken",
        "AttackEnvInstigator", "AttackEnvVictim", "SwapWeaponHand", "Fidget", "FallOnSlope",
        "PrepFocus", "SynchronizedAttackInstigator", "SynchronizedAttackVictim", "WallJumpEntry",
        "WallJumpAttack", "ParryInstigator", "TraversalClimb", "ParryFromDown",
        "DeflectSBInstigator", "AnimSync", "TraversalCinematic", "RagingBull", "Avoid",
        "FallOnSlopeRecovery", "Dash", "FallOnSlopeEntry", "AttackActionGeneric", "ChargeBuildUp",
        "OpeningDoor", "HittedGeneric", "TraversalPush", "TraversalPushInstigator",
        "TraversalDropDown", "RainDash", "Incapacipated", "Jiggle", "Deflected", "Taunt",
        "PlayBlendSpace", "TargetReactionBlendSpace", "MoveToWithPhysWalking", "Count", "None"};
    constexpr unsigned int kCount = sizeof(kNames) / sizeof(kNames[0]);
    return type < kCount ? kNames[type] : "?";
}




bool IsReactionOrder(unsigned int type) {
    switch (type) {
        case 3:
        case 58:
        case 12:
        case 10:
        case 11:
        case 65:
        case 2:
        case 5:
        case 33:
        case 27:
        case 30:
        case 68:
            return true;
        default: return false;
    }
}




















struct OrderTypeStat {
    unsigned int type = 0;
    unsigned long long total = 0;
    unsigned long long from_player = 0;
    unsigned long long while_hit = 0;
    bool used = false;
};
constexpr int kOrderTypeSlots = 64;
OrderTypeStat g_order_census[kOrderTypeSlots];



DWORD g_hit_window_until = 0;




const void* g_reaction_actor = nullptr;
DWORD g_reaction_armed_ms = 0;
unsigned int g_reaction_order_type = 0;


void NoteOrderType(unsigned int type, bool from_player, bool in_hit_window) {
    for (int i = 0; i < kOrderTypeSlots; ++i) {
        OrderTypeStat& slot = g_order_census[i];
        if (slot.used && slot.type != type) continue;
        if (!slot.used) {
            slot.used = true;
            slot.type = type;
        }
        ++slot.total;
        if (from_player) ++slot.from_player;
        if (in_hit_window) ++slot.while_hit;
        return;
    }
}

void DumpOrderCensus() {
    char line[1600];
    int n = 0;
    int shown = 0;
    for (int i = 0; i < kOrderTypeSlots && n < static_cast<int>(sizeof(line)) - 24; ++i) {
        const OrderTypeStat& slot = g_order_census[i];
        if (!slot.used) continue;
        ++shown;
        n += snprintf(line + n, sizeof(line) - n, "%u=%s:%llu/p%llu/h%llu ", slot.type,
                      OrderTypeName(slot.type), slot.total, slot.from_player, slot.while_hit);
    }
    if (shown == 0) return;


    SC_LOG("orders: census type:total/player/hit -- %s", line);
}

extern "C" void sifucoop_on_playorder(void* self, unsigned int order_type,
                                      const void* net_order_struct,
                                      const void* play_order_infos) {
    static unsigned long long count = 0;



    if (g_mirroring) return;

    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    const bool from_player = player && player == static_cast<ue::UObject*>(self);

    const DWORD order_now = GetTickCount();
    const bool in_hit_window = static_cast<LONG>(g_hit_window_until - order_now) > 0;
    NoteOrderType(order_type & 0xFF, from_player, in_hit_window);




    if (IsReactionOrder(order_type & 0xFF)) {
        g_reaction_actor = self;
        g_reaction_armed_ms = order_now;
        g_reaction_order_type = order_type & 0xFF;
        NoteEnemyReactionOrder(self, order_type & 0xFF);
    }

    ++count;
    if (count <= 40 || in_hit_window) {
        SC_LOG("playorder: #%llu %s=%p type=%u %s netstruct=%p infos=%p%s", count,
               from_player ? "PLAYER" : "other", self, order_type & 0xFF,
               OrderTypeName(order_type & 0xFF), net_order_struct, play_order_infos,
               in_hit_window ? "  <-- WITHIN the window after YOU were hit" : "");
    }

    if (!from_player || !g_mirror_enabled) return;




    if (net::IsConnected()) return;

    ue::UObject* puppet = GetPuppet();
    if (!puppet || puppet == player) return;



    static unsigned long long dumped = 0;
    if (net_order_struct && ++dumped <= 10) {
        const auto* bytes = static_cast<const unsigned char*>(net_order_struct);
        char hex[160];
        int n = 0;
        for (int i = 0; i < 32; ++i) {
            n += snprintf(hex + n, sizeof(hex) - n, "%02X ", bytes[i]);
        }
        SC_LOG("netstruct type=%u: %s", order_type & 0xFF, hex);
    }


















    const void* payload = net_order_struct;

    g_mirroring = true;
    auto original = reinterpret_cast<PlayOrderFn>(g_playorder_trampoline);
    const unsigned char result = original(puppet, order_type, payload, play_order_infos);
    g_mirroring = false;

    static unsigned long long mirrored = 0;
    if (++mirrored <= 20) {
        SC_LOG("mirror: -> puppet type=%u result=%u", order_type & 0xFF, result);
    }
}



extern "C" void sifucoop_on_multicast(void* self, unsigned int order_type, unsigned int flag,
                                      const void* buffer) {
    static unsigned long long count = 0;
    if (++count > 30) return;

    if (!buffer) {
        SC_LOG("multicast: #%llu comp=%p type=%u flag=%u buffer=null", count, self,
               order_type & 0xFF, flag & 0xFF);
        return;
    }

    auto array_num = [&](int index) {
        return *reinterpret_cast<const std::int32_t*>(static_cast<const std::uint8_t*>(buffer) +
                                                      index * 16 + 8);
    };
    SC_LOG("multicast: #%llu comp=%p type=%u bytes=%d fnames=%d actors=%d uobjects=%d", count,
           self, order_type & 0xFF, array_num(0), array_num(1), array_num(2), array_num(3));
}





bool LooksLikeUObject(const void* candidate) {
    const auto address = reinterpret_cast<std::uintptr_t>(candidate);
    if (address < 0x10000 || (address & 7) != 0) return false;
    if (!RangeReadable(candidate, 0x20)) return false;

    const auto vtable = *reinterpret_cast<const std::uintptr_t*>(candidate);
    if (vtable < g_module_base || vtable - g_module_base > 0x10000000u) return false;

    const auto class_private =
        *reinterpret_cast<const std::uintptr_t*>(static_cast<const std::uint8_t*>(candidate) +
                                                 0x10);
    if (class_private < 0x10000 || (class_private & 7) != 0) return false;
    if (!RangeReadable(reinterpret_cast<const void*>(class_private), 8)) return false;

    const auto class_vtable = *reinterpret_cast<const std::uintptr_t*>(class_private);
    return class_vtable >= g_module_base && class_vtable - g_module_base <= 0x10000000u;
}





struct AttackIntent {
    char tree_path[256] = {};
    std::int32_t index = 0;
    std::int32_t depth = 0;
    bool valid = false;
};


constexpr int kOffsetDepth = 0x40;
constexpr int kOffsetTree = 0x48;
constexpr int kOffsetIndex = 0x50;
constexpr std::size_t kDelayedActionSize = 192;

AttackIntent g_last_intent;





std::uint8_t g_attack_template[kDelayedActionSize] = {};
bool g_have_template = false;



bool g_template_from_player = false;





struct DeferredPlayerOrder {
    std::uint32_t type = 0;
    std::int32_t index = 0;
    std::int32_t depth = 0;
};
constexpr int kDeferredPlayerOrderCount = 16;
DeferredPlayerOrder g_deferred_player_orders[kDeferredPlayerOrderCount] = {};
int g_deferred_player_order_count = 0;

using PrepareAttackFn = unsigned char(__fastcall*)(void* self, const void* delayed_action);











ue::UObject* LocalPlayerAttackComponent();




using OrderAttackOnStartFn = void(__fastcall*)(void* order);
OrderAttackOnStartFn g_original_order_attack_on_start = nullptr;

struct PendingCosmeticSequence {
    const void* order = nullptr;
    DWORD armed_ms = 0;
    std::uint32_t actor_hash = 0;
};

constexpr int kPendingCosmeticSequenceCount = 16;
PendingCosmeticSequence g_pending_cosmetic_sequences[kPendingCosmeticSequenceCount] = {};

void ArmCosmeticSequence(const void* order, std::uint32_t actor_hash) {
    if (!order) return;
    const DWORD now = GetTickCount();
    for (PendingCosmeticSequence& pending : g_pending_cosmetic_sequences) {
        if (pending.order == order || pending.order == nullptr || now - pending.armed_ms > 1000) {
            pending = {order, now, actor_hash};
            return;
        }
    }
    g_pending_cosmetic_sequences[0] = {order, now, actor_hash};
}



























using OrderHittedOnStartFn = void(__fastcall*)(void* order);
OrderHittedOnStartFn g_original_order_hitted_on_start = nullptr;

































struct PendingReaction {
    std::uint32_t actor_hash = 0;
    unsigned int order_type = 0;
    DWORD until_ms = 0;
    ue::UObject* last_sent = nullptr;
};
constexpr int kPendingReactionCount = 8;
PendingReaction g_pending_reactions[kPendingReactionCount] = {};
struct UObjectArray {
    ue::UObject** data = nullptr;
    std::int32_t num = 0;
    std::int32_t max = 0;
};


UObjectArray g_hit_anim_histories[kPendingReactionCount] = {};

void __fastcall OrderHittedOnStartHook(void* order) {
    g_original_order_hitted_on_start(order);
    if (!order || g_mirroring || !net::IsConnected()) return;
    if (!coop::Get().mirror_hit_reactions) return;

    const DWORD now = GetTickCount();
    const void* actor = g_reaction_actor;
    const unsigned int type = g_reaction_order_type;

    if (!actor || now - g_reaction_armed_ms > 100) return;
    g_reaction_actor = nullptr;





    const std::uint32_t actor_hash = EnemyHashForActor(actor);
    if (actor_hash == 0) return;
    if (!EnemyActionsAreLocallyAuthoritative(actor_hash)) return;

    for (PendingReaction& pending : g_pending_reactions) {
        if (pending.actor_hash != actor_hash && pending.until_ms != 0 &&
            static_cast<LONG>(pending.until_ms - now) > 0) {
            continue;
        }
        pending = {actor_hash, type, now + 400, nullptr};
        return;
    }
    g_pending_reactions[0] = {actor_hash, type, now + 400, nullptr};
}


















void PumpReactionCaptures() {
    if (!coop::Get().mirror_hit_reactions || !net::IsConnected()) return;
    const DWORD now = GetTickCount();
    for (int i = 0; i < kPendingReactionCount; ++i) {
        PendingReaction& pending = g_pending_reactions[i];
        UObjectArray& history = g_hit_anim_histories[i];
        if (!pending.actor_hash || pending.until_ms == 0) continue;
        if (static_cast<LONG>(pending.until_ms - now) <= 0) {


            if (!pending.last_sent) {
                static unsigned int missed = 0;
                if (++missed <= 5 || coop::Get().verbose_orders) {
                    SC_LOG("reaction: %s on %08X -- HitAnimHistory produced no new sequence",
                           OrderTypeName(pending.order_type), pending.actor_hash);
                }
            }
            pending = {};
            continue;
        }



        ue::UObject* actor = FindEnemyByHash(pending.actor_hash);
        if (!actor) continue;
        if (!ue::CallFunction(actor, L"GetHitAnimHistory", &history)) continue;
        if (!history.data || history.num <= 0 || history.num > history.max ||
            history.max > 256) continue;
        ue::UObject* sequence = history.data[history.num - 1];



        if (!sequence || !ue::ObjectClassIs(sequence, "AnimSequence")) continue;
        if (sequence == pending.last_sent) continue;
        pending.last_sent = sequence;

        char path[192] = {};
        if (!ue::GetObjectPathName(sequence, path, sizeof(path))) continue;


        net::SendAnimationSequence(path, pending.actor_hash,
                                   net::AnimationSemantic::Reaction);
        static unsigned int sent = 0;
        if (++sent <= 8 || coop::Get().verbose_orders) {
            SC_LOG("reaction: %s body sequence sent for %08X '%s'",
                   OrderTypeName(pending.order_type), pending.actor_hash, path);
        }
    }
}

void __fastcall OrderAttackOnStartHook(void* order) {
    g_original_order_attack_on_start(order);
    if (!order || g_mirroring || !net::IsConnected() ||
        offsets::OrderAttack_GetAnimPlayed == 0) {
        return;
    }

    std::uint32_t actor_hash = 0;
    bool armed = false;
    for (PendingCosmeticSequence& pending : g_pending_cosmetic_sequences) {
        if (pending.order != order) continue;
        actor_hash = pending.actor_hash;
        pending = {};
        armed = true;
        break;
    }
    if (!armed) return;
    if (actor_hash == 0 && !coop::Get().remote_player_attacks) return;
    if (actor_hash != 0 && !coop::Get().echo_enemy_attacks) return;

    using GetAnimPlayedFn = ue::UObject*(__fastcall*)(const void*);
    auto get_anim =
        reinterpret_cast<GetAnimPlayedFn>(g_module_base + offsets::OrderAttack_GetAnimPlayed);
    ue::UObject* sequence = get_anim(order);
    char path[192] = {};
    if (!sequence || !ue::GetObjectPathName(sequence, path, sizeof(path))) {
        SC_LOG("attack: OrderAttack OnStart had no portable sequence");
        return;
    }
    net::SendAnimationSequence(path, actor_hash, net::AnimationSemantic::Attack);
    if (actor_hash == 0) NotifyLocalSequenceSent();
    SC_LOG("attack: cosmetic sequence sent after OnStart actor=%08X", actor_hash);
}



extern "C" void sifucoop_on_launch_attack(void* self, const void* order_ref,
                                          unsigned int quadrant, unsigned int flag) {
    static unsigned long long count = 0;
    if (!order_ref) return;
    const bool log_this = ++count <= 30;


    const void* order = *reinterpret_cast<const void* const*>(order_ref);
    if (!order || !RangeReadable(order, 64)) {
        if (log_this) {
            SC_LOG("launch: #%llu comp=%p quadrant=%u flag=%u (order unreadable)", count, self,
                   quadrant & 0xFF, flag & 0xFF);
        }
        return;
    }

    const auto vtable = *reinterpret_cast<const std::uintptr_t*>(order);
    const auto* words = reinterpret_cast<const std::uint32_t*>(order);



    const bool exact_local_component = self == LocalPlayerAttackComponent();
    if (exact_local_component && net::IsConnected() &&
        coop::Get().remote_player_attacks) {
        ArmCosmeticSequence(order, 0);
    } else if (!g_mirroring && net::IsConnected() && coop::Get().echo_enemy_attacks) {
        const std::uint32_t enemy_hash = EnemyHashForAttackComponent(self);
        if (enemy_hash != 0 && EnemyActionsAreLocallyAuthoritative(enemy_hash)) {
            ArmCosmeticSequence(order, enemy_hash);
        }
    }

    if (log_this) {
        SC_LOG("launch: #%llu quadrant=%u flag=%u vtable_rva=0x%08llX", count, quadrant & 0xFF,
               flag & 0xFF, static_cast<unsigned long long>(
                   vtable >= g_module_base ? vtable - g_module_base : 0));
        SC_LOG("launch:   words[2..9] %08X %08X %08X %08X %08X %08X %08X %08X", words[2],
               words[3], words[4], words[5], words[6], words[7], words[8], words[9]);
    }
}






ue::UObject* LocalPlayerAttackComponent() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* local = world ? ue::GetPlayerCharacter(world, 0) : nullptr;

    static ue::UObject* cached_world = nullptr;
    static ue::UObject* cached_pawn = nullptr;
    static ue::UObject* cached_component = nullptr;





    if (local != cached_pawn || world != cached_world) {
        cached_world = world;
        cached_pawn = local;
        cached_component = local ? ResolveFighter(local).attack : nullptr;
    }
    return cached_component;
}

extern "C" void sifucoop_on_prepare_attack(void* self, const void* delayed_action) {
    static unsigned long long count = 0;
    if (!delayed_action) return;
    if (!RangeReadable(delayed_action, kDelayedActionSize)) return;

    const auto* bytes = static_cast<const std::uint8_t*>(delayed_action);
    auto* tree = *reinterpret_cast<ue::UObject* const*>(bytes + kOffsetTree);
    const std::int32_t index = *reinterpret_cast<const std::int32_t*>(bytes + kOffsetIndex);
    const std::int32_t depth = *reinterpret_cast<const std::int32_t*>(bytes + kOffsetDepth);

    const bool from_local_player = (self == LocalPlayerAttackComponent());




    if (!g_mirroring && LooksLikeUObject(tree)) {














        if (from_local_player || !g_have_template) {
            std::memcpy(g_attack_template, bytes, kDelayedActionSize);
            g_have_template = true;
            g_template_from_player = from_local_player;
        }
        if (from_local_player) {
            if (net::IsConnected() && coop::Get().remote_player_attacks) {


                NotifyLocalAttackForCosmetic();
            }
            if (ue::GetObjectPathName(tree, g_last_intent.tree_path,
                                      sizeof(g_last_intent.tree_path))) {
                g_last_intent.index = index;
                g_last_intent.depth = depth;
                g_last_intent.valid = true;
            }
            net::SendOrderEvent(0, 0, index, depth);


        } else if (net::IsConnected() && coop::Get().echo_enemy_attacks) {





            const std::uint32_t hash = EnemyHashForAttackComponent(self);
            if (hash != 0 && EnemyActionsAreLocallyAuthoritative(hash)) {
                net::SendOrderEvent(hash, 0, index, depth);
                ++coop::GetStats().attacks_echoed;
                if (coop::Get().verbose_orders) {
                    SC_LOG("orders: enemy %08X attacked index=0x%X depth=%d", hash, index,
                           depth);
                }
            }
        }
    }






    if (++count > 8) return;
    SC_LOG("attack: #%llu comp=%p %s index=0x%X depth=%d tree=%p", count, self,
           from_local_player ? "PLAYER" : "other ", index, depth,
           static_cast<const void*>(tree));
}

OnLocalPlayOrderFn g_original = nullptr;
SaveToFn g_save_to = nullptr;
void** g_vtable = nullptr;
int g_slot = -1;
bool g_installed = false;

unsigned long long g_order_count = 0;

void __fastcall OnLocalPlayOrderHook(void* self, void* shared_ptr) {
    g_original(self, shared_ptr);



    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (!player || player != static_cast<ue::UObject*>(self)) return;


    void* order = shared_ptr ? *reinterpret_cast<void**>(shared_ptr) : nullptr;
    ++g_order_count;

















    alignas(16) unsigned char buffer[128] = {};
    if (order && g_save_to && g_order_count <= 20) {
        g_save_to(order, buffer);

        auto array_num = [&](int index) {
            return *reinterpret_cast<const std::int32_t*>(buffer + index * 16 + 8);
        };
        SC_LOG("order: #%llu order=%p bytes=%d fnames=%d actors=%d uobjects=%d",
               g_order_count, order, array_num(0), array_num(1), array_num(2), array_num(3));


        return;
    }







    if (coop::Get().verbose_orders) {
        SC_LOG("order: #%llu order=%p", g_order_count, order);
    }
}

}

bool InstallOrderHook(std::uintptr_t base, ue::UObject* any_character) {
    if (g_installed) return true;
    if (!any_character) return false;



    static bool attempted = false;
    if (attempted) return false;
    attempted = true;

    g_save_to = reinterpret_cast<SaveToFn>(base + offsets::OrderBase_SaveTo);







    const void* candidates[] = {
        reinterpret_cast<void*>(base + offsets::ABaseCharacter_OnLocalPlayOrder),
        reinterpret_cast<void*>(base + offsets::AFightingCharacter_OnLocalPlayOrder),
    };

    auto** vtable = *reinterpret_cast<void***>(any_character);

    for (int i = 0; i < 1024 && g_slot < 0; ++i) {
        if (IsBadReadPtr(&vtable[i], sizeof(void*))) break;
        for (const void* candidate : candidates) {
            if (vtable[i] == candidate) {
                g_slot = i;
                break;
            }
        }
    }

    if (g_slot < 0) {
        SC_LOG("order: OnLocalPlayOrder not found in the pawn's vtable -- not hooking");
        return false;
    }

    const auto existing = reinterpret_cast<std::uintptr_t>(vtable[g_slot]) - base;
    SC_LOG("order: slot %d, override rva 0x%08llX (%s)", g_slot,
           static_cast<unsigned long long>(existing),
           existing == offsets::AFightingCharacter_OnLocalPlayOrder ? "AFightingCharacter"
                                                                    : "ABaseCharacter");

    DWORD old_protect = 0;
    if (!VirtualProtect(&vtable[g_slot], sizeof(void*), PAGE_READWRITE, &old_protect)) {
        SC_LOG("order: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }

    g_original = reinterpret_cast<OnLocalPlayOrderFn>(vtable[g_slot]);
    vtable[g_slot] = reinterpret_cast<void*>(&OnLocalPlayOrderHook);
    VirtualProtect(&vtable[g_slot], sizeof(void*), old_protect, &old_protect);

    g_vtable = vtable;
    g_installed = true;
    SC_LOG("order: hook installed at vtable slot %d -- watching local player orders", g_slot);
    return true;
}

bool IsOrderHookInstalled() { return g_installed; }

void ArmReplicatedKillInstigator(void* health_component, ue::UObject* instigator) {
    g_replicated_kill_health = health_component;
    g_replicated_kill_instigator = instigator;
}

void ClearReplicatedKillInstigator() {
    g_replicated_kill_health = nullptr;
    g_replicated_kill_instigator = nullptr;
}





void InvalidateAttackTemplate() {
    const bool had_template = g_have_template;
    g_have_template = false;
    g_template_from_player = false;
    g_deferred_player_order_count = 0;
    g_last_intent.valid = false;
    if (had_template) {
        SC_LOG("order: attack template invalidated (pawn changed)");
    }
}
















bool ApplyAttackTo(ue::UObject* actor, std::int32_t attack_index, std::int32_t attack_depth) {
    if (!actor || !g_have_template || !g_prepare_attack_trampoline) return false;

    Fighter fighter = ResolveFighter(actor);
    if (!fighter.attack) return false;



    ue::UObject* tree = GetDefaultCombo(fighter);
    if (!tree) {
        wchar_t wide_path[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, g_last_intent.tree_path, -1, wide_path, 256);
        tree = ue::FindObjectByPath(wide_path);
    }
    if (!tree) return false;

    alignas(16) std::uint8_t rebuilt[kDelayedActionSize];
    std::memcpy(rebuilt, g_attack_template, kDelayedActionSize);
    *reinterpret_cast<ue::UObject**>(rebuilt + kOffsetTree) = tree;
    *reinterpret_cast<std::int32_t*>(rebuilt + kOffsetIndex) = attack_index;
    *reinterpret_cast<std::int32_t*>(rebuilt + kOffsetDepth) = attack_depth;




    g_forced_character = actor;
    g_forced_attack_id = attack_index;

    auto prepare = reinterpret_cast<PrepareAttackFn>(g_prepare_attack_trampoline);
    g_mirroring = true;
    prepare(fighter.attack, rebuilt);
    g_mirroring = false;



    g_forced_character = nullptr;
    g_forced_attack_id = -1;
    return true;
}





bool ApplyEnemyAttack(std::uint32_t actor_hash, std::int32_t attack_index,
                      std::int32_t attack_depth) {
    (void)attack_depth;
    if (!g_launch_ai_attack || !g_get_attack_handler || !g_prepare_next_ai_attack) return false;

    EnemyAttackContext context = {};
    if (!PrepareMirroredEnemyAttack(actor_hash, &context)) return false;
    void* attack_handler = g_get_attack_handler(context.ai_fighting);
    if (!attack_handler) return false;

    g_forced_character = context.actor;
    g_forced_attack_id = attack_index;
    g_replay_attack_component = context.attack_component;
    g_replay_attack_target = context.target;








    g_mirroring = true;
    const unsigned char prepared = g_prepare_next_ai_attack(attack_handler);
    const unsigned char result = prepared
        ? g_launch_ai_attack(context.actor, context.attack_component, nullptr,
                             context.ai_fighting)
        : 0;
    g_mirroring = false;

    g_replay_attack_component = nullptr;
    g_replay_attack_target = nullptr;
    g_forced_character = nullptr;
    g_forced_attack_id = -1;

    if (!result) {
        static unsigned long long rejected = 0;
        if (++rejected <= 10 || coop::Get().verbose_orders) {
            SC_LOG("remote: enemy %08X native attack %s rejected (index=0x%X)",
                   actor_hash, prepared ? "launch" : "selection", attack_index);
        }
        return false;
    }

    static unsigned long long applied = 0;
    if (++applied <= 20 || coop::Get().verbose_orders) {
        SC_LOG("remote: enemy %08X native attack launched (index=0x%X target=%s)",
               actor_hash, attack_index, context.target ? "mirrored" : "local lock");
    }
    return true;
}

void WarnNoTemplate() {



    static bool warned = false;
    if (warned) return;
    warned = true;
    SC_LOG("order: a remote attack arrived before we had a template -- "
           "throw one attack yourself to prime it");
    coop::ReportProblem("throw one attack to prime remote animations");
}

bool HaveAttackTemplate() { return g_have_template; }

bool ApplyAttackToActor(ue::UObject* actor) {
    return ApplyAttackTo(actor, g_last_intent.index, g_last_intent.depth);
}

void ApplyRemoteOrder(std::uint32_t order_type, std::int32_t attack_index,
                      std::int32_t attack_depth) {
    if (!g_have_template) {
        WarnNoTemplate();
        return;
    }
    if (!ApplyAttackTo(GetPuppet(), attack_index, attack_depth)) {
        static unsigned long long rejected = 0;
        if (++rejected <= 5) {
            SC_LOG("remote: peer attack could not be replayed (no valid puppet/action)");
            coop::ReportProblem("remote attack animation could not start");
        }
        return;
    }

    static unsigned long long applied = 0;
    if (++applied <= 20) {
        SC_LOG("remote: peer attacked type=%u index=0x%X depth=%d", order_type, attack_index,
               attack_depth);
    }
}




void WatchLocalPlayerForHits() {
    if (!coop::Get().verbose_orders) return;

    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (!player) return;
    const Fighter fighter = ResolveFighter(player);
    if (!fighter.health) return;

    const float health = GetHealth(fighter);
    const DWORD now = GetTickCount();

    static ue::UObject* watched = nullptr;
    static float last_health = -1.f;
    if (watched != player) {
        watched = player;
        last_health = health;
        return;
    }
    const float drop = last_health - health;
    last_health = health;
    if (drop > 0.05f) {


        g_hit_window_until = now + 1000;
        SC_LOG("orders: YOU took %.1f (health %.0f) -- logging every order for 1s", drop,
               health);
    }

    static DWORD last_census = 0;
    if (now - last_census >= 10000) {
        last_census = now;
        DumpOrderCensus();
    }
}

void PumpRemoteOrders() {
    WatchLocalPlayerForHits();
    PumpReactionCaptures();

    std::uint32_t actor_hash = 0;
    std::uint32_t order_type = 0;
    std::int32_t attack_index = 0;
    std::int32_t attack_depth = 0;





    const bool versus = coop::Get().mode == coop::Mode::Versus;








    const bool replay_player = versus || coop::Get().echo_player_attacks;

    if (g_have_template && replay_player && g_deferred_player_order_count > 0) {
        SC_LOG("remote: replaying %d peer attacks held until local template",
               g_deferred_player_order_count);
        for (int i = 0; i < g_deferred_player_order_count; ++i) {
            const DeferredPlayerOrder& deferred = g_deferred_player_orders[i];
            ApplyRemoteOrder(deferred.type, deferred.index, deferred.depth);
        }
        g_deferred_player_order_count = 0;
    }

    while (net::PopOrderEvent(&actor_hash, &order_type, &attack_index, &attack_depth)) {
        if (actor_hash == 0) {
            if (replay_player) {
                if (g_have_template) {
                    ApplyRemoteOrder(order_type, attack_index, attack_depth);
                } else if (g_deferred_player_order_count < kDeferredPlayerOrderCount) {
                    g_deferred_player_orders[g_deferred_player_order_count++] =
                        {order_type, attack_index, attack_depth};
                    SC_LOG("remote: peer attack queued until local template (%d/%d)",
                           g_deferred_player_order_count, kDeferredPlayerOrderCount);
                }
            }
            continue;
        }



        if (!coop::Get().echo_enemy_attacks) continue;










        if (coop::Get().observer_cosmetic_enemy_attacks_only &&
            coop::Get().mode == coop::Mode::Coop) {
            continue;
        }






        if (EnemyActionsAreLocallyAuthoritative(actor_hash)) continue;
        if (ApplyEnemyAttack(actor_hash, attack_index, attack_depth)) {
            ++coop::GetStats().attacks_echoed;
        }
    }
}




void ReplayAttackOnPuppet() {
    if (!g_last_intent.valid || !g_have_template) {
        SC_LOG("replay: no attack captured yet -- throw a punch first");
        return;
    }
    ue::UObject* puppet = GetPuppet();
    if (!puppet) {
        SC_LOG("replay: no puppet (press F9 first)");
        return;
    }
    const bool ok = ApplyAttackTo(puppet, g_last_intent.index, g_last_intent.depth);
    SC_LOG("replay: index=0x%X depth=%d -> %s", g_last_intent.index, g_last_intent.depth,
           ok ? "sent" : "FAILED");
}



void ReplayAttackOnNearestEnemy() {
    if (!g_last_intent.valid) {
        SC_LOG("replay: no attack captured yet -- throw a punch first");
        return;
    }
    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (!player) return;

    ue::FVector mine = {};
    if (!ue::GetActorLocation(player, &mine)) return;

    EnemyRow rows[net::kMaxTrackedEnemies];
    const int count = GetEnemyRows(rows, net::kMaxTrackedEnemies);

    std::uint32_t best_hash = 0;
    float best_distance = 1e9f;
    for (int i = 0; i < count; ++i) {
        if (!rows[i].active || rows[i].down) continue;
        if (rows[i].distance >= best_distance) continue;
        best_distance = rows[i].distance;
        best_hash = rows[i].hash;
    }

    ue::UObject* enemy = best_hash ? FindEnemyByHash(best_hash) : nullptr;
    if (!enemy) {
        SC_LOG("replay: no active enemy nearby");
        return;
    }
    const bool ok = ApplyEnemyAttack(best_hash, g_last_intent.index, g_last_intent.depth);
    SC_LOG("replay: enemy %08X at %.0f units -> %s", best_hash, best_distance,
           ok ? "attacking" : "FAILED");
}

bool InstallPlayOrderHook(std::uintptr_t base) {
    static bool attempted = false;
    if (attempted) return g_playorder_trampoline != nullptr;
    attempted = true;

    g_module_base = base;

    if (MH_Initialize() != MH_OK) {
        SC_LOG("order: MH_Initialize failed");
        return false;
    }




    auto* target = reinterpret_cast<void*>(base + offsets::AFightingCharacter_PlayOrder);



    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&sifucoop_playorder_detour),
                                     &g_playorder_trampoline);
    if (status != MH_OK) {
        SC_LOG("order: MH_CreateHook failed (%d)", static_cast<int>(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        SC_LOG("order: MH_EnableHook failed (%d)", static_cast<int>(status));
        return false;
    }

    SC_LOG("order: PlayOrder hook ACTIVE at %p (trampoline %p)", target, g_playorder_trampoline);




    auto* multicast =
        reinterpret_cast<void*>(base + offsets::UOrderComponent_MultiCastPlayOrder_Impl);
    if (MH_CreateHook(multicast, reinterpret_cast<void*>(&sifucoop_multicast_detour),
                      &g_multicast_trampoline) == MH_OK &&
        MH_EnableHook(multicast) == MH_OK) {
        SC_LOG("order: MultiCastPlayOrder hook ACTIVE at %p", multicast);
    } else {
        SC_LOG("order: MultiCastPlayOrder hook FAILED");
    }

    auto* selector =
        reinterpret_cast<void*>(base + offsets::FComboTransitions_GeNextAttackID);
    if (MH_CreateHook(selector, reinterpret_cast<void*>(&GeNextAttackIDHook),
                      reinterpret_cast<void**>(&g_original_next_attack_id)) == MH_OK &&
        MH_EnableHook(selector) == MH_OK) {
        SC_LOG("order: move selector hook ACTIVE at %p", selector);
    } else {
        SC_LOG("order: move selector hook FAILED -- attacks will not match");
    }

    if (offsets::UHealthComponent_Kill != 0) {
        auto* health_kill = reinterpret_cast<void*>(base + offsets::UHealthComponent_Kill);
        if (MH_CreateHook(health_kill, reinterpret_cast<void*>(&HealthKillHook),
                          reinterpret_cast<void**>(&g_original_health_kill)) == MH_OK &&
            MH_EnableHook(health_kill) == MH_OK) {
            SC_LOG("death: exact sequence observer ACTIVE at %p", health_kill);
        } else {
            g_original_health_kill = nullptr;
            SC_LOG("death: exact sequence observer FAILED -- generic down fallback remains");
        }
    }

    if (offsets::UCharacterHealthComponent_SetIsDown != 0) {
        auto* set_is_down = reinterpret_cast<void*>(
            base + offsets::UCharacterHealthComponent_SetIsDown);
        if (MH_CreateHook(set_is_down, reinterpret_cast<void*>(&SetIsDownHook),
                          reinterpret_cast<void**>(&g_original_set_is_down)) == MH_OK &&
            MH_EnableHook(set_is_down) == MH_OK) {
            SC_LOG("death: replicated-corpse stand-up guard ACTIVE at %p", set_is_down);
        } else {
            g_original_set_is_down = nullptr;
            SC_LOG("death: replicated-corpse stand-up guard FAILED");
        }
    }


    g_launch_ai_attack = reinterpret_cast<LaunchAIAttackFn>(
        base + offsets::UAttackBTTask_LaunchAttack);

    g_get_attack_handler = reinterpret_cast<GetAttackHandlerFn>(
        base + offsets::UAIFightingComponent_GetAttackHandler);
    g_prepare_next_ai_attack = reinterpret_cast<PrepareNextAIAttackFn>(
        base + offsets::FAIAttackHandler_PrepareNextAttack);
    auto* set_next_target = reinterpret_cast<void*>(
        base + offsets::UAttackComponent_SetNextAttackTarget);
    if (MH_CreateHook(set_next_target, reinterpret_cast<void*>(&SetNextAttackTargetHook),
                      reinterpret_cast<void**>(&g_original_set_next_attack_target)) == MH_OK &&
        MH_EnableHook(set_next_target) == MH_OK) {
        SC_LOG("order: mirrored enemy target hook ACTIVE at %p", set_next_target);
    } else {
        g_original_set_next_attack_target = nullptr;
        SC_LOG("order: mirrored enemy target hook FAILED -- attacks use local AI lock");
    }

    SC_LOG("order: native per-enemy attack replay ready at %p",
           reinterpret_cast<void*>(g_launch_ai_attack));

    auto* launch = reinterpret_cast<void*>(base + offsets::UAttackComponent_LaunchAttack);
    if (MH_CreateHook(launch, reinterpret_cast<void*>(&sifucoop_launch_attack_detour),
                      &g_launch_attack_trampoline) == MH_OK &&
        MH_EnableHook(launch) == MH_OK) {
        SC_LOG("order: LaunchAttack hook ACTIVE at %p", launch);
    } else {
        SC_LOG("order: LaunchAttack hook FAILED");
    }

    if (offsets::OrderAttack_OnStart != 0) {
        auto* on_start = reinterpret_cast<void*>(base + offsets::OrderAttack_OnStart);
        if (MH_CreateHook(on_start, reinterpret_cast<void*>(&OrderAttackOnStartHook),
                          reinterpret_cast<void**>(&g_original_order_attack_on_start)) == MH_OK &&
            MH_EnableHook(on_start) == MH_OK) {
            SC_LOG("order: OrderAttack::OnStart hook ACTIVE at %p", on_start);
        } else {
            g_original_order_attack_on_start = nullptr;
            SC_LOG("order: OrderAttack::OnStart hook FAILED -- cosmetic attacks unavailable");
        }
    } else {
        SC_LOG("order: OrderAttack::OnStart unavailable on this build");
    }

    if (offsets::OrderHitted_OnStart != 0) {
        auto* hitted = reinterpret_cast<void*>(base + offsets::OrderHitted_OnStart);
        if (MH_CreateHook(hitted, reinterpret_cast<void*>(&OrderHittedOnStartHook),
                          reinterpret_cast<void**>(&g_original_order_hitted_on_start)) == MH_OK &&
            MH_EnableHook(hitted) == MH_OK) {
            SC_LOG("order: OrderHitted::OnStart hook ACTIVE at %p -- enemy hit reactions",
                   hitted);
        } else {
            g_original_order_hitted_on_start = nullptr;
            SC_LOG("order: OrderHitted::OnStart hook FAILED -- enemy hit reactions unavailable");
        }
    } else {
        SC_LOG("order: OrderHitted::OnStart unavailable on this build -- REBUILD the offset "
               "table for this game folder");
    }

    auto* prepare =
        reinterpret_cast<void*>(base + offsets::UAttackComponent_PrepareToLaunchAttack);
    if (MH_CreateHook(prepare, reinterpret_cast<void*>(&sifucoop_prepare_attack_detour),
                      &g_prepare_attack_trampoline) == MH_OK &&
        MH_EnableHook(prepare) == MH_OK) {
        SC_LOG("order: PrepareToLaunchAttack hook ACTIVE at %p", prepare);
    } else {
        SC_LOG("order: PrepareToLaunchAttack hook FAILED");
    }

    return true;
}

}





