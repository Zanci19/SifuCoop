#include "selftest.h"

#include <windows.h>

#include <cmath>

#include "../core/log.h"
#include "../net/protocol.h"
#include "../ue/reflection.h"
#include "actors.h"
#include "coop.h"
#include "enemies.h"
#include "orders.h"
#include "puppet.h"

namespace sifucoop::game {
namespace {

namespace ue = sifucoop::ue;
namespace coop = sifucoop::coop;
namespace net = sifucoop::net;








enum class Round {
    AiDriven,
    ModDriven,
    Done,
};

enum class Step {
    WaitForTarget,
    Settle,
    Measure,
    Watch,
    Report,
};

Round g_round = Round::AiDriven;
Step g_step = Step::WaitForTarget;

std::uint32_t g_target_hash = 0;
DWORD g_step_started = 0;
DWORD g_last_swing = 0;

float g_health_before = 0.f;
float g_health_low = 0.f;
int g_swings_sent = 0;

bool g_enabled = false;
bool g_announced = false;



constexpr float kStageDistance = 130.f;
constexpr DWORD kSettleMs = 1200;
constexpr DWORD kWatchMs = 6000;
constexpr DWORD kSwingIntervalMs = 900;

const char* RoundName(Round round) {
    switch (round) {
        case Round::AiDriven: return "AI-driven (baseline)";
        case Round::ModDriven: return "mod-driven (the joining player's path)";
        default: return "done";
    }
}

void Enter(Step step) {
    g_step = step;
    g_step_started = GetTickCount();
}




bool StageEnemy(ue::UObject* player, ue::UObject* enemy) {
    ue::FVector player_location = {};
    ue::FRotator player_rotation = {};
    if (!ue::GetActorLocation(player, &player_location)) return false;
    if (!ue::GetActorRotation(player, &player_rotation)) return false;

    SetActorPresent(enemy, true);





    const ue::FRotator facing = {0.f, player_rotation.Yaw + 180.f, 0.f};
    for (int step = 0; step < 8; ++step) {
        const float degrees = player_rotation.Yaw + step * 45.f;
        const float radians = degrees * 3.14159265f / 180.f;
        ue::FVector at = player_location;
        at.X += kStageDistance * cosf(radians);
        at.Y += kStageDistance * sinf(radians);

        ue::FRotator look = facing;
        look.Yaw = degrees + 180.f;
        if (TeleportActor(enemy, at, look)) return true;
    }
    return false;
}



std::uint32_t PickTarget() {
    EnemyRow rows[net::kMaxTrackedEnemies];
    const int count = GetEnemyRows(rows, net::kMaxTrackedEnemies);

    std::uint32_t best = 0;
    float best_distance = 1e9f;
    for (int i = 0; i < count; ++i) {
        if (!rows[i].active || rows[i].down) continue;
        if (rows[i].distance >= best_distance) continue;
        best_distance = rows[i].distance;
        best = rows[i].hash;
    }
    return best;
}

void ReportRound() {
    const float lost = g_health_before - g_health_low;
    const bool landed = lost > 0.5f;

    SC_LOG("selftest: === %s ===", RoundName(g_round));
    SC_LOG("selftest:   swings issued   %d", g_swings_sent);
    SC_LOG("selftest:   health          %.1f -> %.1f  (lost %.1f)", g_health_before,
           g_health_low, lost);
    SC_LOG("selftest:   VERDICT         %s", landed ? "HIT -- attacks damage the player"
                                                    : "NO DAMAGE");

    if (g_round == Round::ModDriven) {
        if (landed) {
            SC_LOG("selftest: CONCLUSION -- a replayed enemy attack damages the local "
                   "player. The joining side can fight a real shared encounter; damage "
                   "stays local and nothing needs to change.");
            coop::ReportProblem("self-test: replayed enemy attacks DO damage the player");
        } else {
            SC_LOG("selftest: CONCLUSION -- replayed enemy attacks do NOT damage the local "
                   "player. Damage to players must become host-authoritative: the host "
                   "already knows what its enemies did to the puppet, and the same "
                   "running-total mechanism that carries enemy damage would carry it back.");
            coop::ReportProblem("self-test: replayed enemy attacks do NOT damage the player");
        }
    }
}

}

void InitSelfTest() {
    g_enabled = coop::Get().selftest;
    if (!g_enabled) return;
    SC_LOG("selftest: ENABLED -- an enemy will be teleported next to you and made to "
           "attack. Turn selftest off in SifuCoop.ini for normal play.");
}

bool SelfTestActive() {
    return g_enabled && g_round != Round::Done && g_step != Step::WaitForTarget;
}

void TickSelfTest() {
    if (!g_enabled || g_round == Round::Done) return;

    ue::UObject* world = ue::GetWorld();
    ue::UObject* player = world ? ue::GetPlayerCharacter(world, 0) : nullptr;
    if (!player || !ActorsReady()) return;

    const DWORD now = GetTickCount();
    Fighter mine = ResolveFighter(player);
    if (!mine.health) return;

    switch (g_step) {
        case Step::WaitForTarget: {




            if (g_round == Round::ModDriven && !HaveAttackTemplate()) {
                if (!g_announced) {
                    g_announced = true;
                    SC_LOG("selftest: waiting for an attack template before the driven "
                           "round -- throw a punch, or let round one land one");
                }
                return;
            }







            static bool instructed = false;
            const std::uint32_t target = PickTarget();
            if (target == 0) {
                if (!instructed) {
                    instructed = true;
                    SC_LOG("selftest: waiting -- walk into a fight and stand near an enemy, "
                           "and the test runs automatically (nothing to press)");
                }
                return;
            }
            instructed = false;
            ue::UObject* enemy = FindEnemyByHash(target);
            if (!enemy) return;
            if (!StageEnemy(player, enemy)) {
                static DWORD last = 0;
                if (now - last > 3000 || last == 0) {
                    last = now;
                    SC_LOG("selftest: could not place an enemy next to you -- no room on "
                           "any side; move to a more open spot");
                }
                return;
            }

            g_target_hash = target;
            g_swings_sent = 0;
            g_announced = false;

            if (g_round == Round::ModDriven) {


                StopBrain(enemy);
            }

            SC_LOG("selftest: staging %s against enemy %08X", RoundName(g_round), target);
            Enter(Step::Settle);
            break;
        }

        case Step::Settle:
            if (now - g_step_started < kSettleMs) return;
            Enter(Step::Measure);
            break;

        case Step::Measure:
            g_health_before = GetHealth(mine);
            g_health_low = g_health_before;
            SC_LOG("selftest: player at %.1f health, watching for %lums", g_health_before,
                   kWatchMs);
            g_last_swing = 0;
            Enter(Step::Watch);
            break;

        case Step::Watch: {
            const float health = GetHealth(mine);
            if (health < g_health_low) g_health_low = health;

            ue::UObject* enemy = FindEnemyByHash(g_target_hash);




            if (enemy && now - g_last_swing > kSwingIntervalMs / 2) {
                ue::FVector player_location = {};
                ue::FVector enemy_location = {};
                if (ue::GetActorLocation(player, &player_location) &&
                    ue::GetActorLocation(enemy, &enemy_location)) {
                    const float dx = enemy_location.X - player_location.X;
                    const float dy = enemy_location.Y - player_location.Y;
                    if (sqrtf(dx * dx + dy * dy) > kStageDistance * 2.5f) {
                        StageEnemy(player, enemy);
                    }
                }
            }

            if (g_round == Round::ModDriven && enemy && now - g_last_swing > kSwingIntervalMs) {
                g_last_swing = now;
                if (ApplyAttackToActor(enemy)) ++g_swings_sent;
            }

            if (now - g_step_started < kWatchMs) return;
            Enter(Step::Report);
            break;
        }

        case Step::Report:
            ReportRound();
            g_round = (g_round == Round::AiDriven) ? Round::ModDriven : Round::Done;
            if (g_round == Round::Done) {
                SC_LOG("selftest: finished. Set selftest=0 in SifuCoop.ini.");
            }
            Enter(Step::WaitForTarget);
            break;
    }
}

}
