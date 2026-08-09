#pragma once

#include <cstdint>

namespace sifucoop::hooks {

bool InstallTickHook(std::uintptr_t module_base, void* gengine);
void RemoveTickHook();

// for different hz screens
float FrameDeltaSeconds();

}  // namespace sifucoop::hooks
