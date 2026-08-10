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

// void ABaseCharacter::OnLocalPlayOrder(TSharedPtr<OrderBase>)
// TSharedPtr is 16 bytes (object pointer + reference controller), so under the
// MSVC x64 ABI it arrives by address rather than in a register.
using OnLocalPlayOrderFn = void(__fastcall*)(void* self, void* shared_ptr);

using SaveToFn = void(__fastcall*)(void* self, void* buffer);

// UOrderComponent::PlayOrder has a long parameter list (delegates, several
// uint8s, stack arguments). We only care about the first three registers, and
// re-declaring the rest correctly would be guesswork -- the same guesswork that
// crashed the game via GetType.
//
// So the detour is asm: it observes RCX/RDX/R8, then jumps to the trampoline
// with every register and the stack exactly as the caller left them. Nothing is
// interpreted, so nothing can be misdeclared.
extern "C" {
void* g_playorder_trampoline = nullptr;
}

extern "C" void sifucoop_on_playorder(void* self, unsigned int order_type,
                                      const void* net_order_struct,
                                      const void* play_order_infos);

asm(".globl sifucoop_playorder_detour\n"
    "sifucoop_playorder_detour:\n"
    "  sub  $0x48, %rsp\n"          // 16-byte aligned for the call
    "  mov  %rcx, 0x20(%rsp)\n"     // preserve the volatile argument registers
    "  mov  %rdx, 0x28(%rsp)\n"
    "  mov  %r8,  0x30(%rsp)\n"
    "  mov  %r9,  0x38(%rsp)\n"
    "  call sifucoop_on_playorder\n"  // args are already in rcx/rdx/r8
    "  mov  0x20(%rsp), %rcx\n"
    "  mov  0x28(%rsp), %rdx\n"
    "  mov  0x30(%rsp), %r8\n"
    "  mov  0x38(%rsp), %r9\n"
    "  add  $0x48, %rsp\n"          // stack restored: trampoline sees the
    "  jmp  *g_playorder_trampoline(%rip)\n");  // original frame untouched

extern "C" void sifucoop_playorder_detour();

// UOrderComponent::MultiCastPlayOrder_Implementation(
//     EOrderType, uint8, FBuffer, int64, uint8, FUniqueNetIdRepl, bool, uint8, uint8)
//   RCX = UOrderComponent* this
//   RDX = EOrderType
//   R8  = uint8
//   R9  = FBuffer*   (passed by hidden pointer: >8 bytes, so by address)
//
// This is the engine's remote-replay entry point, and its FBuffer is a
// *serialised* order -- built to cross a network, unlike the live C++ struct.
// Observing it tells us whether the game populates one during normal play.
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

// UAttackComponent::PrepareToLaunchAttack(FDelayedActionAttack const&) -> uint8
//   RCX = UAttackComponent* this
//   RDX = FDelayedActionAttack const*
//
// FDelayedActionAttack's layout is unknown, so rather than assuming field
// offsets we scan its first bytes for values that look like UObject pointers
// and ask each for its path name. The attack's identifying asset should be one
// of them -- and an asset path is the same string on the peer's machine.
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

// UAttackComponent::LaunchAttack(TSharedRef<OrderAttack>, EQuadrantTypes, bool)
//   RCX = UAttackComponent* this
//   RDX = TSharedRef<OrderAttack>*  (16 bytes, so by address)
//   R8  = EQuadrantTypes
//   R9  = bool
//
// This is the call that actually performs a chosen attack, downstream of
// whatever selected it. Observing what arrives here -- which OrderAttack object,
// which quadrant -- is how we find the real selector, rather than continuing to
// guess at fields in FDelayedActionAttack.
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

// --- Move selection ---------------------------------------------------------
//
// int FComboTransitions::GeNextAttackID(AFightingCharacter const*, UCombo const*,
//                                       EComboTransition, TArray<...>*) const
//
// This is where Sifu decides WHICH attack happens. The id it returns is what
// later appears at +0x50 of FDelayedActionAttack -- which is why stamping that
// field achieved nothing: it is the output, written after the decision.
//
// Overriding the return value is therefore the correct injection point, and it
// is unusually safe to hook: five arguments, all pointers or small integers,
// nothing passed or returned by value.
using GeNextAttackIDFn = int(__fastcall*)(const void* self, const void* character,
                                          const void* combo, unsigned int transition,
                                          void* trace);

GeNextAttackIDFn g_original_next_attack_id = nullptr;

// Sifu's AI task constructs a fresh FDelayedActionAttack from the destination
// enemy's own ability system and combo state. This is the only safe replay
// source: a delayed action copied from another fighter contains a private
// ability id at +0x24 and PrepareToLaunchAttack rejects it immediately.
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
void* g_replay_attack_component = nullptr;
void* g_replay_attack_target = nullptr;

void __fastcall SetNextAttackTargetHook(void* attack_component, void* target) {
    // UAttackBTTask::LaunchAttack normally copies FAIAttackHandler's local
    // target into the attack component. The joining side's brain is stopped,
    // so that handler can be stale. Substitute the host-mirrored body only for
    // the one synchronous replay call that armed this override.
    if (attack_component == g_replay_attack_component && g_replay_attack_target) {
        target = g_replay_attack_target;
    }
    g_original_set_next_attack_target(attack_component, target);
}

// The lethal-hit path already selected the archetype-, direction- and move-
// specific defender sequence. Observe it instead of guessing a generic corpse
// animation. Only the host publishes it; client health reconciliation re-enters
// Kill and must not echo the event back.
using HealthKillFn = void(__fastcall*)(void* health_component, std::int32_t behavior,
                                      ue::UObject* instigator, ue::UObject* death_animation,
                                      bool option_a, bool option_b);
HealthKillFn g_original_health_kill = nullptr;

void __fastcall HealthKillHook(void* health_component, std::int32_t behavior,
                               ue::UObject* instigator, ue::UObject* death_animation,
                               bool option_a, bool option_b) {
    std::uint32_t actor_hash = 0;
    char death_path[192] = {};
    if (net::GetRole() == net::Role::Host && net::IsConnected() &&
        coop::Get().echo_enemy_attacks && death_animation) {
        actor_hash = EnemyHashForHealthComponent(health_component);
        if (!actor_hash ||
            !ue::GetObjectPathName(death_animation, death_path, sizeof(death_path))) {
            actor_hash = 0;
        }
    }

    g_original_health_kill(health_component, behavior, instigator, death_animation,
                           option_a, option_b);

    if (actor_hash && death_path[0]) {
        net::SendAnimationSequence(death_path, actor_hash);
        SC_LOG("death: exact enemy sequence sent actor=%08X", actor_hash);
    }
}

// Armed just before a replayed attack is triggered, and consumed by the very
// next decision for that character. Deliberately one-shot and character-scoped
// so it can never leak into the local player's own combos.
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

// AFightingCharacter::PlayOrder(EOrderType, FNetOrderStruct const&,
//                               FPlayOrderInfos const&) -> uint8
//   RCX = AFightingCharacter* this
//   RDX = EOrderType
//   R8  = FNetOrderStruct const*
//   R9  = FPlayOrderInfos const*
// Every argument is a register, and all three we forward are pointers or a
// small enum -- nothing whose ABI has to be guessed.
using PlayOrderFn = unsigned char(__fastcall*)(void* self, unsigned int order_type,
                                               const void* net_order_struct,
                                               const void* play_order_infos);

bool g_mirror_enabled = true;
bool g_mirroring = false;
std::uintptr_t g_module_base = 0;

// FNetOrderStruct is polymorphic: its first 8 bytes are a per-order-type vtable
// pointer. That pointer is process-local and cannot be transmitted -- but both
// peers run the identical build, so its RVA is identical on both sides and the
// receiver can rebuild the pointer as (its own module base + rva).
//
// 64 bytes of tail is copied. The observed structs go quiet well before that,
// and a fixed generous size avoids needing each derived struct's exact length.
// 64 bytes was not enough. UOrderComponent::PlayOrder does not merely read the
// struct we pass -- it runs FNetOrderStructAttack's constructor over it, which
// read past the end of a 72-byte buffer into stack garbage and dereferenced it.
// The derived structs' exact sizes are unknown, so the payload is generously
// oversized and the destination is larger still, guaranteeing that anything the
// constructor touches lands in zeroed memory we own rather than in a neighbour.
constexpr std::size_t kOrderPayloadSize = 256;
constexpr std::size_t kOrderScratchSize = 1024;

struct OrderWire {
    std::uint32_t order_type = 0;
    std::uint32_t vtable_rva = 0;
    std::uint8_t payload[kOrderPayloadSize] = {};
};

// Copying a fixed 256 bytes could otherwise run off the end of the source
// allocation and fault on the *read* side.
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


extern "C" void sifucoop_on_playorder(void* self, unsigned int order_type,
                                      const void* net_order_struct,
                                      const void* play_order_infos) {
    static unsigned long long count = 0;

    // Replaying onto the puppet re-enters this hook; without this guard the
    // mirror would feed itself.
    if (g_mirroring) return;

    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    const bool from_player = player && player == static_cast<ue::UObject*>(self);

    if (++count <= 40) {
        SC_LOG("playorder: #%llu %s=%p type=%u netstruct=%p infos=%p", count,
               from_player ? "PLAYER" : "other", self, order_type & 0xFF, net_order_struct,
               play_order_infos);
    }

    if (!from_player || !g_mirror_enabled) return;

    // BUG FIX: mirroring is an OFFLINE rehearsal only. With a peer connected the
    // puppet represents *them*, so replaying our own moves onto it would make it
    // shadow us instead of showing what they actually did.
    if (net::IsConnected()) return;

    ue::UObject* puppet = GetPuppet();
    if (!puppet || puppet == player) return;

    // What is actually inside FNetOrderStruct decides how hard networking is.
    // If it is mostly zero, the order type alone may be enough to send.
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

    // Same process, so the FNetOrderStruct pointer is still valid. This proves
    // the replay mechanism before any serialisation exists -- if the puppet
    // performs the move, only transporting these bytes remains.
    // F7 routes the order through the wire format and back before replaying it.
    // Nothing is transmitted -- this proves the encode/decode is faithful while
    // both ends are still one process, so a failure over the network later is
    // unambiguously a transport problem rather than a serialisation one.
    // Byte-level reconstruction of FNetOrderStruct is abandoned, not merely
    // undersized. FNetOrderStructHitted contains FHitRequest -> FHitBox ->
    // FHitboxDataRow -> TSet<...>, and PlayOrder runs that constructor over
    // whatever we hand it. A TSet copy walks heap pointers that are invalid the
    // moment the object is rebuilt at a different address, so no payload size
    // makes this safe. The engine's own serialised form (FBuffer, via
    // MultiCastPlayOrder) exists precisely for this and is the correct target.
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

// Read-only observer. FBuffer is four TArrays of {void* Data; int32 Num; int32 Max},
// so each Num sits 8 bytes into its 16-byte slot.
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

// Heuristic UObject test. Calling GetPathName on a non-UObject would crash, so
// a candidate must be a readable, aligned pointer whose vtable lands inside the
// game module, and whose ClassPrivate is itself such a pointer. That is far
// stricter than "non-null" and rejects floats, handles and interior pointers.
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

// --- Intent replication ----------------------------------------------------
//
// The wire form of "which attack": a stable asset path plus two integers. This
// is what a peer would actually receive. Nothing here is a pointer.
struct AttackIntent {
    char tree_path[256] = {};
    std::int32_t index = 0;
    std::int32_t depth = 0;
    bool valid = false;
};

// Field offsets established by dumping and diffing real structs across moves.
constexpr int kOffsetDepth = 0x40;
constexpr int kOffsetTree = 0x48;
constexpr int kOffsetIndex = 0x50;
constexpr std::size_t kDelayedActionSize = 192;

AttackIntent g_last_intent;

// Each machine keeps a template captured from its own attacks, so the peer
// never needs ours -- only the three values above. Reconstructing on top of a
// locally-produced struct is what makes this work where raw byte transfer did
// not: every pointer in it is already valid in this process.
std::uint8_t g_attack_template[kDelayedActionSize] = {};
bool g_have_template = false;
// Whether the template we hold came from the local PLAYER's attack or from some
// other character's. A player-sourced one is preferred and is never replaced by
// an enemy's, but an enemy's is far better than none -- see the capture below.
bool g_template_from_player = false;

// A peer can attack before this process has seen its first local combat action.
// The delayed-action struct cannot be invented safely, so retain a small,
// current-level backlog and replay it as soon as the game's own action hook
// supplies a valid local template. No player punch is required.
struct DeferredPlayerOrder {
    std::uint32_t type = 0;
    std::int32_t index = 0;
    std::int32_t depth = 0;
};
constexpr int kDeferredPlayerOrderCount = 16;
DeferredPlayerOrder g_deferred_player_orders[kDeferredPlayerOrderCount] = {};
int g_deferred_player_order_count = 0;

using PrepareAttackFn = unsigned char(__fastcall*)(void* self, const void* delayed_action);

// DelayedActionAttack::ToString was tried and must not be tried again.
//
// It crashed inside FName::ToString reading null. The calling convention was
// right; the data was not. Our 192-byte template is a truncated copy of a
// struct containing an FName, so ToString walked a name index that does not
// exist. Any call that formats a *copy* of this struct has the same defect, so
// the answer was never a bigger copy -- it is to stop asking the game to
// interpret our reconstructions at all.

// Cached native attack-component lookup, declared before the LaunchAttack observer.
ue::UObject* LocalPlayerAttackComponent();

// LaunchAttack arrives before OrderAttack selected its UAnimSequence. Keep the
// exact object until OnStart, whose native PDB signature is void(this), then
// read the finished sequence and transmit only that cosmetic asset.
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
    net::SendAnimationSequence(path, actor_hash);
    if (actor_hash == 0) NotifyLocalSequenceSent();
    SC_LOG("attack: cosmetic sequence sent after OnStart actor=%08X", actor_hash);
}

// Read-only. The OrderAttack's vtable identifies its concrete type, and any
// small integers near the front of it are candidate move selectors.
extern "C" void sifucoop_on_launch_attack(void* self, const void* order_ref,
                                          unsigned int quadrant, unsigned int flag) {
    static unsigned long long count = 0;
    if (!order_ref) return;
    const bool log_this = ++count <= 30;

    // TSharedRef's first word is the object pointer.
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

    // This is pre-start, so GetAnimPlayed is expected to be null here. Arm the
    // object instead; OrderAttack::OnStart sends the sequence after Sifu fills it.
    const bool exact_local_component = self == LocalPlayerAttackComponent();
    if (exact_local_component && net::IsConnected() &&
        coop::Get().remote_player_attacks) {
        ArmCosmeticSequence(order, 0);
    } else if (!g_mirroring && net::GetRole() == net::Role::Host &&
               net::IsConnected() && coop::Get().echo_enemy_attacks) {
        const std::uint32_t enemy_hash = EnemyHashForAttackComponent(self);
        if (enemy_hash != 0) ArmCosmeticSequence(order, enemy_hash);
    }

    if (log_this) {
        SC_LOG("launch: #%llu quadrant=%u flag=%u vtable_rva=0x%08llX", count, quadrant & 0xFF,
               flag & 0xFF, static_cast<unsigned long long>(
                   vtable >= g_module_base ? vtable - g_module_base : 0));
        SC_LOG("launch:   words[2..9] %08X %08X %08X %08X %08X %08X %08X %08X", words[2],
               words[3], words[4], words[5], words[6], words[7], words[8], words[9]);
    }
}

// The local player's UAttackComponent, cached against the pawn pointer. Every
// character's attacks come through the hook below, so resolving this by
// reflection each time would mean a ProcessEvent per attack per enemy. The
// pawn is replaced on death, aging and level change, which invalidates the
// cache for free.
ue::UObject* LocalPlayerAttackComponent() {
    ue::UObject* world = ue::GetWorld();
    ue::UObject* local = world ? ue::GetPlayerCharacter(world, 0) : nullptr;

    static ue::UObject* cached_world = nullptr;
    static ue::UObject* cached_pawn = nullptr;
    static ue::UObject* cached_component = nullptr;

    // Keyed on the world as well as the pawn. Pawn addresses are recycled --
    // Sifu pools its characters -- so a new level's player can land on the
    // exact address the old one occupied, and a pawn-only check would then
    // happily compare against a component that no longer exists.
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

    // Replaying onto a puppet or a driven enemy re-enters this hook. Without
    // this guard the echo would feed itself and every replayed swing would be
    // rebroadcast as if it had originated here.
    if (!g_mirroring && LooksLikeUObject(tree)) {
        // Capture a replay template from ANY local character, not only from our
        // own player.
        //
        // Every remote animation -- the peer's swings and, on the joining side,
        // every enemy swing the host echoes -- is rebuilt on top of a locally
        // produced FDelayedActionAttack. Requiring that template to come from
        // the local player meant the joining player had to throw a punch before
        // a single enemy would visibly attack, and the template is discarded on
        // every respawn and level change, so the dead spell came back after each
        // one. Enemies swing constantly, so taking one from them primes the path
        // immediately. The template only supplies a valid same-process skeleton;
        // ApplyAttackTo overwrites the combo tree from the TARGET's own
        // component and the index/depth from the wire, so whose attack it was
        // originally does not leak into the replay.
        if (from_local_player || !g_have_template) {
            std::memcpy(g_attack_template, bytes, kDelayedActionSize);
            g_have_template = true;
            g_template_from_player = from_local_player;
        }
        if (from_local_player) {
            if (net::IsConnected() && coop::Get().remote_player_attacks) {
                // LaunchAttack identifies the exact player OrderAttack; TickPuppet
                // samples regular montages as a fallback for non-sequence moves.
                NotifyLocalAttackForCosmetic();
            }
            if (ue::GetObjectPathName(tree, g_last_intent.tree_path,
                                      sizeof(g_last_intent.tree_path))) {
                g_last_intent.index = index;
                g_last_intent.depth = depth;
                g_last_intent.valid = true;
            }
            net::SendOrderEvent(0, 0, index, depth);
            // OnStart sends the finished UAnimSequence; TickPuppet samples a
            // montage fallback. Neither route invokes a remote attack component.
        } else if (net::GetRole() == net::Role::Host && net::IsConnected() &&
                   coop::Get().echo_enemy_attacks) {
            // An enemy swung. The host owns that decision, so the joiner is
            // told about it rather than being left to watch enemies slide
            // around in silence -- which is the difference between a fight it
            // can react to and one it can only lose.
            const std::uint32_t hash = EnemyHashForAttackComponent(self);
            if (hash != 0) {
                net::SendOrderEvent(hash, 0, index, depth);
                ++coop::GetStats().attacks_echoed;
                if (coop::Get().verbose_orders) {
                    SC_LOG("orders: enemy %08X attacked index=0x%X depth=%d", hash, index,
                           depth);
                }
            }
        }
    }

    // The hex-dumping pass that established these field offsets is gone: it
    // produced kOffsetTree/kOffsetIndex/kOffsetDepth and has nothing left to
    // find. What remains is a short confirmation that the fields still read
    // sensibly on this build, which is what would break first if the game were
    // ever patched under us.
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

    // Every character of this class shares the vtable we patched, so most
    // calls here are enemies. Only the local player is interesting.
    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (!player || player != static_cast<ue::UObject*>(self)) return;

    // TSharedPtr's first word is the OrderBase*.
    void* order = shared_ptr ? *reinterpret_cast<void**>(shared_ptr) : nullptr;
    ++g_order_count;

    // Deliberately NOT calling OrderBase::GetType here.
    //
    // It returns FOrderType by value and two different guesses at its calling
    // convention both crashed the game inside GetType. Calling a by-value
    // returning C++ method through a hand-declared pointer is exactly the kind
    // of thing that fails destructively rather than loudly, and the replication
    // design does not need it: orders are serialised and replayed generically
    // via SaveTo/MultiCastPlayOrder, never matched on type. If a type is ever
    // needed, it should be read from the serialised buffer instead of by
    // calling a by-value getter.
    // SaveTo takes FBuffer BY REFERENCE -- plain pointer arguments, so unlike
    // GetType there is no by-value return convention to get wrong.
    //
    // FBuffer is four TArrays (data, FNames, AActor*, UObject*), each 16 bytes
    // as {void* Data; int32 Num; int32 Max}. The counts tell us whether orders
    // carry object references we would have to map across machines.
    alignas(16) unsigned char buffer[128] = {};
    if (order && g_save_to && g_order_count <= 20) {
        g_save_to(order, buffer);

        auto array_num = [&](int index) {
            return *reinterpret_cast<const std::int32_t*>(buffer + index * 16 + 8);
        };
        SC_LOG("order: #%llu order=%p bytes=%d fnames=%d actors=%d uobjects=%d",
               g_order_count, order, array_num(0), array_num(1), array_num(2), array_num(3));
        // Arrays allocated by SaveTo are intentionally leaked: this is a
        // capped diagnostic (first 20 orders), not production code.
        return;
    }

    // Gated. This used to log every single order the local player issued,
    // unconditionally and regardless of verbose_orders -- and the log writes
    // straight to disk, so each line is a blocking write on the game thread.
    // Sifu issues orders continuously (a held charge is a stream of
    // OrderChargeBuildUp), so playing normally meant synchronous file I/O in the
    // middle of combat forever.
    if (coop::Get().verbose_orders) {
        SC_LOG("order: #%llu order=%p", g_order_count, order);
    }
}

}  // namespace

bool InstallOrderHook(std::uintptr_t base, ue::UObject* any_character) {
    if (g_installed) return true;
    if (!any_character) return false;

    // One attempt only. The previous build retried every 30 frames and wrote
    // 401 identical failures, which buries anything useful in the log.
    static bool attempted = false;
    if (attempted) return false;
    attempted = true;

    g_save_to = reinterpret_cast<SaveToFn>(base + offsets::OrderBase_SaveTo);


    // Only ABaseCharacter and AFightingCharacter override this virtual, and
    // Blueprint subclasses reuse their parent's native vtable rather than
    // generating one -- so whichever class the live pawn is, its slot holds one
    // of these two addresses. Searching for either avoids having to compute a
    // slot index, which is fragile to get right from the binary alone.
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

// The captured template is a raw copy of a struct full of pointers into the
// level we were in when it was taken. Replaying it after a level change or a
// respawn would dereference freed objects, so it is dropped whenever the pawn
// that produced it is replaced.
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

// Makes `actor` throw the attack the peer reported.
//
// Rebuilding on top of a locally captured FDelayedActionAttack is what makes
// this work at all: every pointer in that struct is valid in this process,
// where a copy of the peer's bytes would have been a page of foreign addresses.
// Only the three portable fields are overwritten -- and the combo tree comes
// from the *target's own* component, so an enemy swings with an enemy's moveset
// and never has to be told which asset that is.
//
// The honest limitation: which strike comes out is still Sifu's decision, not
// ours. Selection happens inside the ability system, above the entry point we
// call, so the two screens agree that this character attacked and roughly when,
// but not necessarily on the exact strike. For a co-op fight against the game's
// own enemies that is the difference between polish and playability, and this
// is the playable half.
bool ApplyAttackTo(ue::UObject* actor, std::int32_t attack_index, std::int32_t attack_depth) {
    if (!actor || !g_have_template || !g_prepare_attack_trampoline) return false;

    Fighter fighter = ResolveFighter(actor);
    if (!fighter.attack) return false;

    // Prefer the character's own combo; fall back to whatever the template was
    // captured with, which at least resolves to a real asset.
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

    // Arm the selector override, then trigger the attack. The very next
    // decision for this character returns the peer's attack id instead of one
    // derived from our local combo state.
    g_forced_character = actor;
    g_forced_attack_id = attack_index;

    auto prepare = reinterpret_cast<PrepareAttackFn>(g_prepare_attack_trampoline);
    g_mirroring = true;
    prepare(fighter.attack, rebuilt);
    g_mirroring = false;

    // Never leave it armed: an unconsumed override would hijack whatever this
    // character did next.
    g_forced_character = nullptr;
    g_forced_attack_id = -1;
    return true;
}

// Replays a host enemy swing by asking the destination enemy's own AI task to
// build it. Unlike ApplyAttackTo (kept for explicit player-versus diagnostics),
// this never copies FDelayedActionAttack across fighters. The resulting local
// OrderAttack drives the real animation, hitbox, parry and avoid windows.
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

    // The PDB signature includes a UBlackboardComponent reference, but the
    // shipped function does not read R8 anywhere on this path. All state it
    // consumes comes from the character, attack component and AI fighting
    // component, and it creates the delayed action itself on the stack. Since
    // this peer's behavior-tree brain is stopped, explicitly run the handler's
    // selection step first; that seeds the combo manager's pending attack that
    // the BT task normally inherits from an earlier AI tick.
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
    // The template comes from OUR attacks, so until the local player throws one
    // there is nothing to build a remote attack on. Worth surfacing: otherwise
    // remote attacks silently do nothing and the mod looks broken.
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

void PumpRemoteOrders() {
    std::uint32_t actor_hash = 0;
    std::uint32_t order_type = 0;
    std::int32_t attack_index = 0;
    std::int32_t attack_depth = 0;

    // In Versus the remote player is supposed to hit you, so their attacks are
    // always replayed. In Co-op they are not: the replay is a real hitbox that
    // friendly-fires you, and their damage was already resolved on their own
    // machine, so echo_player_attacks gates it and defaults off.
    const bool versus = coop::Get().mode == coop::Mode::Versus;
    // Three ways in, in decreasing order of bluntness: Versus (hurting each
    // other is the point), the manual echo_player_attacks override, or the
    // guarded path -- remote attacks requested AND Sifu confirming that the two
    // players are marked friendly to one another.
    // Co-op player orders are never replayed through UAttackComponent. A
    // relationship flag does not prove every hitbox branch honors it. Cosmetic
    // montages are sent at the source; this path stays for Versus or an explicit
    // diagnostic override only.
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

        // An enemy's swing, relayed by the host. Only the joining side ever
        // receives these -- the host is the one deciding them.
        if (!coop::Get().echo_enemy_attacks) continue;
        // This path deliberately has no local-template prerequisite. Sifu's AI
        // task creates a valid delayed action from this exact enemy, so attacks
        // work from the first frame of a room even if the joining player has
        // not thrown a punch yet.
        if (ApplyEnemyAttack(actor_hash, attack_index, attack_depth)) {
            ++coop::GetStats().attacks_echoed;
        }
    }
}

// F6, offline: make the puppet throw the last attack we threw. This is the
// whole remote-attack path exercised without a peer, so a failure here is
// unambiguously local rather than a network problem.
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

// F6 on the nearest enemy instead of the puppet: the same path the joining
// side runs for every enemy swing, testable with one machine and one keypress.
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

    // AFightingCharacter::PlayOrder rather than UOrderComponent::PlayOrder: it
    // hands us the character itself in RCX, so identifying the local player and
    // replaying onto the puppet both become direct.
    auto* target = reinterpret_cast<void*>(base + offsets::AFightingCharacter_PlayOrder);

    // MinHook's HDE length disassembler works out how much of the prologue it
    // can relocate, instead of us hardcoding a byte count for one build.
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

    // Second hook: does the game populate a serialised FBuffer during normal
    // single-player play? If so, that is what we transmit instead of trying to
    // rebuild C++ objects byte-for-byte.
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

}  // namespace sifucoop::game





