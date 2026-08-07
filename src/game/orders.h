#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {

// Hooks ABaseCharacter::OnLocalPlayOrder so we can observe every Order the
// local player performs. Orders are how Sifu expresses attacks, hit reactions,
// traversal and animation -- montages are barely used -- so this is the signal
// M4/M5 need to replicate.
//
// The hook is installed on the character class vtable, which every character
// shares, so the callback filters to the local player before doing anything.
bool InstallOrderHook(std::uintptr_t module_base, ue::UObject* any_character);

bool IsOrderHookInstalled();

// Inline-hooks UOrderComponent::PlayOrder via MinHook. Unlike OnLocalPlayOrder,
// this sees the EOrderType and the FNetOrderStruct the game itself uses to
// replicate an order.
bool InstallPlayOrderHook(std::uintptr_t module_base);

// Whether an attack has been captured yet. Remote attacks are rebuilt on top of
// one produced in this process, so until something has attacked there is
// nothing to replay.
bool HaveAttackTemplate();

// Makes `actor` throw the last captured attack -- the exact path the joining
// side runs for every enemy swing the host reports. Returns false if there is
// no template, or the actor has no attack component.
bool ApplyAttackToActor(ue::UObject* actor);

// F6: reconstruct a captured attack from its portable description and invoke it
// on the puppet, exactly as a peer would. Offline rehearsal of the remote path.
void ReplayAttackOnPuppet();

// F7: the same, but on the nearest active enemy -- which is the path the
// joining side runs for every enemy swing the host reports. Being able to fire
// it from one machine is the only way this half gets tested without two.
void ReplayAttackOnNearestEnemy();

// Applies a move reported by the peer to the puppet.
void ApplyRemoteOrder(std::uint32_t order_type, std::int32_t attack_index,
                      std::int32_t attack_depth);

// Drains everything the peer did since the last frame and applies it: their own
// moves onto the puppet, and -- on the joining side -- the host's enemies'
// attacks onto the matching driven enemies. Without the second half a client
// sees enemies slide silently into range and then take damage from nothing.
void PumpRemoteOrders();

// Discards the captured attack template. Must be called when the local pawn is
// replaced (level change, death/aging respawn): the template is a raw copy of a
// struct full of pointers into the old level, and replaying it afterwards would
// dereference freed objects.
void InvalidateAttackTemplate();

}  // namespace sifucoop::game
