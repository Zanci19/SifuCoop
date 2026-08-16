#include "coop.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../core/log.h"

namespace sifucoop::coop {
namespace {

Config g_config;
Stats g_stats;

const char* kSection = "coop";

bool ReadBool(const char* key, bool fallback, const char* ini) {
    return GetPrivateProfileIntA(kSection, key, fallback ? 1 : 0, ini) != 0;
}

void WriteBool(const char* key, bool value, const char* ini) {
    WritePrivateProfileStringA(kSection, key, value ? "1" : "0", ini);
}

void WriteInt(const char* key, int value, const char* ini) {
    char text[16] = {};
    _snprintf(text, sizeof(text), "%d", value);
    WritePrivateProfileStringA(kSection, key, text, ini);
}

}

Config& Get() { return g_config; }
Stats& GetStats() { return g_stats; }

void IniPath(char* out, int out_size) {
    if (!out || out_size <= 0) return;
    out[0] = '\0';
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) return;
    char* last_slash = strrchr(path, '\\');
    if (last_slash) *(last_slash + 1) = '\0';
    strncat(path, "SifuCoop.ini", MAX_PATH - strlen(path) - 1);
    lstrcpynA(out, path, out_size);
}

void Load() {
    char ini[MAX_PATH] = {};
    IniPath(ini, sizeof(ini));
    if (!ini[0]) return;

    g_config.mode = GetPrivateProfileIntA(kSection, "versus", 0, ini) != 0 ? Mode::Versus
                                                                          : Mode::Coop;
    g_config.sync_enemies = ReadBool("sync_enemies", g_config.sync_enemies, ini);
    g_config.suppress_client_ai = ReadBool("suppress_client_ai",
                                           g_config.suppress_client_ai, ini);
    g_config.sync_enemy_vitals = ReadBool("sync_enemy_vitals",
                                          g_config.sync_enemy_vitals, ini);
    g_config.park_extra_enemies = ReadBool("park_extra_enemies",
                                           g_config.park_extra_enemies, ini);
    g_config.echo_enemy_attacks = ReadBool("echo_enemy_attacks",
                                           g_config.echo_enemy_attacks, ini);
    g_config.echo_player_attacks = ReadBool("echo_player_attacks",
                                            g_config.echo_player_attacks, ini);
    g_config.friendly_relationship = ReadBool("friendly_relationship",
                                              g_config.friendly_relationship, ini);
    g_config.real_second_player = ReadBool("real_second_player",
                                           g_config.real_second_player, ini);
    g_config.second_player_disable_splitscreen =
        ReadBool("second_player_disable_splitscreen",
                 g_config.second_player_disable_splitscreen, ini);
    g_config.hide_second_player_hud =
        ReadBool("hide_second_player_hud", g_config.hide_second_player_hud, ini);
    g_config.second_player_in_gameplay_only =
        ReadBool("second_player_in_gameplay_only",
                 g_config.second_player_in_gameplay_only, ini);
    g_config.remote_player_attacks =
        ReadBool("remote_player_attacks", g_config.remote_player_attacks, ini);
    g_config.force_enemy_engage =
        ReadBool("force_enemy_engage", g_config.force_enemy_engage, ini);
    g_config.puppet_invincible =
        ReadBool("puppet_invincible", g_config.puppet_invincible, ini);
    g_config.puppet_ignores_pawn_collision = ReadBool(
        "puppet_ignores_pawn_collision", g_config.puppet_ignores_pawn_collision, ini);



    g_config.sync_montages = ReadBool("sync_montages", g_config.sync_montages, ini);
    g_config.report_damage = ReadBool("report_damage", g_config.report_damage, ini);
    g_config.mirror_peer_vitals = ReadBool("mirror_peer_vitals",
                                           g_config.mirror_peer_vitals, ini);
    g_config.sync_run_state = ReadBool("sync_run_state", g_config.sync_run_state, ini);
    g_config.client_simulates_enemies =
        ReadBool("client_simulates_enemies", g_config.client_simulates_enemies, ini);
    g_config.peer_fights_locally =
        ReadBool("peer_fights_locally", g_config.peer_fights_locally, ini);
    g_config.retarget_from_down_peer =
        ReadBool("retarget_from_down_peer", g_config.retarget_from_down_peer, ini);
    g_config.observer_cosmetic_enemy_attacks_only = ReadBool(
        "observer_cosmetic_enemy_attacks_only",
        g_config.observer_cosmetic_enemy_attacks_only, ini);
    g_config.sync_enemy_death_animations = ReadBool(
        "sync_enemy_death_animations", g_config.sync_enemy_death_animations, ini);
    g_config.use_engine_outfit_refresh = ReadBool(
        "use_engine_outfit_refresh", g_config.use_engine_outfit_refresh, ini);
    g_config.sync_peer_visual_age =
        ReadBool("sync_peer_visual_age", g_config.sync_peer_visual_age, ini);
    g_config.mirror_hit_reactions =
        ReadBool("mirror_hit_reactions", g_config.mirror_hit_reactions, ini);
    g_config.sync_peer_age = ReadBool("sync_peer_age", g_config.sync_peer_age, ini);
    g_config.director_targets_partner =
        ReadBool("director_targets_partner", g_config.director_targets_partner, ini);




    if (g_config.real_second_player) {
        SC_LOG("coop: disabling unsafe real_second_player experiment");
        g_config.real_second_player = false;
    }




    if (g_config.force_enemy_engage || g_config.director_targets_partner) {
        SC_LOG("coop: disabling unsafe director experiment "
               "(force_enemy_engage=%d director_targets_partner=%d)",
               g_config.force_enemy_engage, g_config.director_targets_partner);
        g_config.force_enemy_engage = false;
        g_config.director_targets_partner = false;
    }

    g_config.auto_follow_level = ReadBool("auto_follow_level",
                                          g_config.auto_follow_level, ini);
    g_config.auto_join_level = ReadBool("auto_join_level", g_config.auto_join_level, ini);
    g_config.adaptive_interp = ReadBool("adaptive_interp", g_config.adaptive_interp, ini);
    g_config.in_game_overlay = ReadBool("in_game_overlay", g_config.in_game_overlay, ini);
    g_config.menu_exclusive_input =
        ReadBool("menu_exclusive_input", g_config.menu_exclusive_input, ini);
    g_config.selftest = ReadBool("selftest", g_config.selftest, ini);
    g_config.verbose_enemies = ReadBool("verbose_enemies", g_config.verbose_enemies, ini);
    g_config.verbose_orders = ReadBool("verbose_orders", g_config.verbose_orders, ini);

    g_config.interp_delay_ms = GetPrivateProfileIntA(kSection, "interp_delay_ms",
                                                     g_config.interp_delay_ms, ini);
    g_config.snapshot_hz = GetPrivateProfileIntA(kSection, "snapshot_hz",
                                                 g_config.snapshot_hz, ini);



    if (g_config.interp_delay_ms < 0) g_config.interp_delay_ms = 0;
    if (g_config.interp_delay_ms > 500) g_config.interp_delay_ms = 500;

    if (g_config.snapshot_hz < 30) g_config.snapshot_hz = 30;
    if (g_config.snapshot_hz > 60) g_config.snapshot_hz = 60;

    SC_LOG("coop: mode=%s enemies=%d ai_off=%d vitals=%d attacks=%d damage=%d "
           "park=%d follow=%d adaptive=%d",
           g_config.mode == Mode::Coop ? "CO-OP" : "VERSUS", g_config.sync_enemies,
           g_config.suppress_client_ai, g_config.sync_enemy_vitals,
           g_config.echo_enemy_attacks, g_config.report_damage,
           g_config.park_extra_enemies, g_config.auto_follow_level,
           g_config.adaptive_interp);
}

void Save() {
    char ini[MAX_PATH] = {};
    IniPath(ini, sizeof(ini));
    if (!ini[0]) return;

    WriteBool("versus", g_config.mode == Mode::Versus, ini);
    WriteBool("sync_enemies", g_config.sync_enemies, ini);
    WriteBool("suppress_client_ai", g_config.suppress_client_ai, ini);
    WriteBool("sync_enemy_vitals", g_config.sync_enemy_vitals, ini);
    WriteBool("park_extra_enemies", g_config.park_extra_enemies, ini);
    WriteBool("echo_enemy_attacks", g_config.echo_enemy_attacks, ini);
    WriteBool("echo_player_attacks", g_config.echo_player_attacks, ini);
    WriteBool("friendly_relationship", g_config.friendly_relationship, ini);
    WriteBool("real_second_player", g_config.real_second_player, ini);
    WriteBool("second_player_disable_splitscreen",
              g_config.second_player_disable_splitscreen, ini);
    WriteBool("hide_second_player_hud", g_config.hide_second_player_hud, ini);
    WriteBool("second_player_in_gameplay_only",
              g_config.second_player_in_gameplay_only, ini);
    WriteBool("remote_player_attacks", g_config.remote_player_attacks, ini);
    WriteBool("force_enemy_engage", g_config.force_enemy_engage, ini);
    WriteBool("puppet_invincible", g_config.puppet_invincible, ini);
    WriteBool("puppet_ignores_pawn_collision", g_config.puppet_ignores_pawn_collision, ini);
    WriteBool("sync_montages", g_config.sync_montages, ini);
    WriteBool("report_damage", g_config.report_damage, ini);
    WriteBool("mirror_peer_vitals", g_config.mirror_peer_vitals, ini);
    WriteBool("sync_run_state", g_config.sync_run_state, ini);
    WriteBool("client_simulates_enemies", g_config.client_simulates_enemies, ini);
    WriteBool("peer_fights_locally", g_config.peer_fights_locally, ini);
    WriteBool("retarget_from_down_peer", g_config.retarget_from_down_peer, ini);
    WriteBool("observer_cosmetic_enemy_attacks_only",
              g_config.observer_cosmetic_enemy_attacks_only, ini);
    WriteBool("sync_enemy_death_animations", g_config.sync_enemy_death_animations, ini);
    WriteBool("use_engine_outfit_refresh", g_config.use_engine_outfit_refresh, ini);
    WriteBool("sync_peer_visual_age", g_config.sync_peer_visual_age, ini);
    WriteBool("mirror_hit_reactions", g_config.mirror_hit_reactions, ini);
    WriteBool("sync_peer_age", g_config.sync_peer_age, ini);
    WriteBool("director_targets_partner", g_config.director_targets_partner, ini);
    WriteBool("auto_follow_level", g_config.auto_follow_level, ini);
    WriteBool("auto_join_level", g_config.auto_join_level, ini);
    WriteBool("adaptive_interp", g_config.adaptive_interp, ini);
    WriteBool("in_game_overlay", g_config.in_game_overlay, ini);
    WriteBool("menu_exclusive_input", g_config.menu_exclusive_input, ini);
    WriteBool("verbose_enemies", g_config.verbose_enemies, ini);
    WriteBool("verbose_orders", g_config.verbose_orders, ini);
    WriteInt("interp_delay_ms", g_config.interp_delay_ms, ini);
    WriteInt("snapshot_hz", g_config.snapshot_hz, ini);
}

void ReportProblem(const char* format, ...) {
    char text[160] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf(text, sizeof(text) - 1, format, args);
    va_end(args);



    if (strcmp(text, g_stats.last_problem) != 0) SC_LOG("coop: %s", text);
    lstrcpynA(g_stats.last_problem, text, sizeof(g_stats.last_problem));
}

}
