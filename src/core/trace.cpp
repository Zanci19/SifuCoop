#include "trace.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../game/actors.h"
#include "../game/coop.h"
#include "../game/puppet.h"
#include "../net/session.h"
#include "../ue/reflection.h"
#include "hooks.h"
#include "log.h"

namespace sifucoop::trace {
namespace {

namespace ue = sifucoop::ue;
namespace net = sifucoop::net;
namespace game = sifucoop::game;

unsigned long g_last_bucket = 0;
bool g_have_bucket = false;
bool g_forced = false;
net::Role g_last_role = net::Role::Offline;

// "/Game/Animations/MainChar/Attack/foo.foo_C" -> "foo". The full path is
// already in the event lines; a digest only has to say which clip, and a
// short token keeps the line diffable by eye.
void Leaf(const char* path, char* out, int out_size) {
    if (out_size <= 0) return;
    out[0] = '\0';
    if (!path || !path[0]) {
        lstrcpynA(out, "-", out_size);
        return;
    }
    const char* start = strrchr(path, '/');
    start = start ? start + 1 : path;
    lstrcpynA(out, start, out_size);
    char* dot = strchr(out, '.');
    if (dot) *dot = '\0';
    if (!out[0]) lstrcpynA(out, "-", out_size);
}

void ActorAnim(ue::UObject* actor, char* out, int out_size, float* position) {
    lstrcpynA(out, "-", out_size);
    *position = 0.f;
    if (!actor) return;
    ue::AnimState state = {};
    if (!ue::ReadAnimState(actor, &state) || !state.montage) return;
    char path[192] = {};
    if (!ue::GetObjectPathName(state.montage, path, sizeof(path))) return;
    Leaf(path, out, out_size);
    *position = state.position;
}

float Speed2D(const ue::FVector& v) { return sqrtf(v.X * v.X + v.Y * v.Y); }

char RoleLetter(net::Role role) {
    switch (role) {
        case net::Role::Host: return 'H';
        case net::Role::Client: return 'C';
        default: return '-';
    }
}

}

void RequestDigest() { g_forced = true; }

void Tick() {
    // The role letter goes into every log line, so it is maintained even when
    // the digest itself is switched off -- a log with no role cannot be told
    // apart from its partner's.
    const net::Role role = net::GetRole();
    if (role != g_last_role) {
        g_last_role = role;
        log::SetRoleTag(RoleLetter(role));
    }

    const coop::Config& config = coop::Get();
    if (!config.trace) return;

    int hz = config.trace_hz;
    if (hz < 1) hz = 1;
    if (hz > 30) hz = 30;
    const unsigned long period = 1000ul / static_cast<unsigned long>(hz);

    // Bucketing on the SHARED clock (not the local one) is what makes the two
    // logs line up: both machines cross the same boundary at the same instant
    // and print a line keyed to the same number.
    const unsigned long now = net::SharedClockMs();
    const unsigned long bucket = now / period;
    if (!g_forced && g_have_bucket && bucket == g_last_bucket) return;
    g_last_bucket = bucket;
    g_have_bucket = true;
    g_forced = false;

    ue::UObject* world = ue::GetWorld();
    if (!world) return;
    ue::UObject* player = ue::GetPlayerCharacter(world, 0);
    if (!player) return;

    ue::FVector me = {};
    ue::FRotator me_rot = {};
    if (!ue::GetActorLocation(player, &me) || !ue::GetActorRotation(player, &me_rot)) return;

    ue::FVector me_velocity = {};
    game::GetActorVelocity(player, &me_velocity);

    float health = -1.f, max_health = -1.f, guard = -1.f;
    int down = -1;
    const game::Fighter mine = game::ResolveFighter(player);
    if (mine.health) {
        health = game::GetHealth(mine);
        max_health = game::GetMaxHealth(mine);
        guard = game::GetGuard(mine);
        down = game::IsDown(mine) ? 1 : 0;
    }

    char my_anim[48] = {};
    float my_anim_pos = 0.f;
    ActorAnim(player, my_anim, sizeof(my_anim), &my_anim_pos);

    // What the wire says the partner is doing, and where their body actually
    // is on this screen. The gap between the two is the chase error; the gap
    // between THIS line's pup= and the peer log's me= at the same t= is the
    // real divergence, which no single log can show.
    ue::FVector wire = {};
    ue::FRotator wire_rot = {};
    ue::FVector wire_velocity = {};
    const bool have_wire =
        net::IsConnected() && net::GetPeerTransform(&wire, &wire_rot, &wire_velocity);

    ue::UObject* puppet = game::GetPuppet();
    ue::FVector body = {};
    const bool have_body = puppet && ue::GetActorLocation(puppet, &body);

    float chase_error = -1.f;
    if (have_wire && have_body) {
        const float dx = wire.X - body.X;
        const float dy = wire.Y - body.Y;
        const float dz = wire.Z - body.Z;
        chase_error = sqrtf(dx * dx + dy * dy + dz * dz);
    }

    char peer_anim[48] = {};
    float peer_anim_pos = 0.f;
    ActorAnim(puppet, peer_anim, sizeof(peer_anim), &peer_anim_pos);

    net::PeerVitals vitals = {};
    const bool have_vitals = net::GetPeerVitals(&vitals);

    char level[128] = {};
    char level_leaf[64] = {};
    if (ue::GetCurrentLevelPath(level, sizeof(level))) {
        Leaf(level, level_leaf, sizeof(level_leaf));
    } else {
        lstrcpynA(level_leaf, "-", sizeof(level_leaf));
    }

    const coop::Stats& stats = coop::GetStats();

    SC_LOG(
        "dgst f=%lu lvl=%s me=%.0f,%.0f,%.0f yaw=%.0f v=%.0f hp=%.1f/%.1f gd=%.1f dn=%d "
        "anim=%s@%.2f wire=%s%.0f,%.0f,%.0f wv=%.0f pup=%s%.0f,%.0f,%.0f err=%.1f "
        "panim=%s@%.2f php=%.1f/%.1f pgd=%.1f pdn=%d plvl=%d "
        "en=%d/%d/%d/%d rtt=%d/%d interp=%d io=%u/%u sn=%u drop=%u rej=%u "
        "dmg=%u/%.1f/%.1f",
        static_cast<unsigned long>(sifucoop::hooks::FrameCount()), level_leaf, me.X, me.Y,
        me.Z, me_rot.Yaw,
        Speed2D(me_velocity), health, max_health, guard, down, my_anim, my_anim_pos,
        have_wire ? "" : "?", wire.X, wire.Y, wire.Z, Speed2D(wire_velocity),
        have_body ? "" : "?", body.X, body.Y, body.Z, chase_error, peer_anim, peer_anim_pos,
        have_vitals ? vitals.health : -1.f, have_vitals ? vitals.max_health : -1.f,
        have_vitals ? vitals.guard : -1.f,
        have_vitals ? (vitals.is_down ? 1 : 0) : -1,
        have_vitals ? (vitals.in_level ? 1 : 0) : -1, stats.enemies_known,
        stats.enemies_active, stats.enemies_driven, stats.enemies_unmatched, stats.rtt_ms,
        stats.rtt_jitter_ms, net::GetInterpolationDelayMs(), stats.bytes_per_second_in,
        stats.bytes_per_second_out, stats.snapshots_received, stats.packets_dropped,
        stats.packets_rejected, stats.damage_reports, stats.damage_reported_total,
        stats.damage_applied_total);
}

}
