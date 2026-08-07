#pragma once

#include <cstdint>

namespace sifucoop::hooks {

// Swaps UGameEngine::Tick in GEngine's vtable to give us a game-thread frame
// callback. `gengine` must be the live GEngine pointer, not the global's slot.
bool InstallTickHook(std::uintptr_t module_base, void* gengine);

void RemoveTickHook();

}  // namespace sifucoop::hooks
