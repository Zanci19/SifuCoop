#pragma once

#include <cstdint>

namespace sifucoop::hooks {

// Swaps UGameEngine::Tick in GEngine's vtable to give us a game-thread frame
// callback. `gengine` must be the live GEngine pointer, not the global's slot.
bool InstallTickHook(std::uintptr_t module_base, void* gengine);

void RemoveTickHook();

// Seconds of game time in the frame currently being processed, straight from
// the engine's own Tick argument.
//
// Anything that smooths towards a target must scale by this rather than assume
// a frame is a fixed slice of time. Two machines running the same build at 165
// and 60 frames per second would otherwise converge at rates differing by
// nearly three times, so each would see the other as the jittery one.
float FrameDeltaSeconds();

}  // namespace sifucoop::hooks
