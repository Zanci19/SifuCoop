#pragma once

#include <cstdint>

#include "../ue/reflection.h"

namespace sifucoop::game {




















void InitPlayer2(std::uintptr_t module_base);



ue::UObject* CreateSecondPlayer();


void RemoveSecondPlayer();



ue::UObject* GetSecondPlayerPawn();


bool SecondPlayerActive();










ue::UObject* MaintainSecondPlayer();





ue::UObject* PrimaryPlayerPawn();

}
