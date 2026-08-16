#pragma once

#include <cstdint>

namespace sifucoop::hooks {



bool InstallTickHook(std::uintptr_t module_base, void* gengine);

void RemoveTickHook();








float FrameDeltaSeconds();

}
