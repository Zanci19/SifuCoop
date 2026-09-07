#pragma once

#include <cstdint>

namespace sifucoop::coop {

enum class Mode : int {
    Coop = 0,
    Versus = 1,
};

struct Config {
    Mode mode = Mode::Coop;

    bool sync_enemies = true;
    bool suppress_client_ai = true;
    bool sync_enemy_vitals = true;
    bool park_extra_enemies = true;
    bool echo_enemy_attacks = true;

    bool echo_player_attacks = false;

    bool friendly_relationship = false;

    bool real_second_player = false;

    bool second_player_disable_splitscreen = true;

    bool hide_second_player_hud = true;

    bool second_player_in_gameplay_only = true;

    bool sync_montages = true;

    bool report_damage = true;
    bool mirror_peer_vitals = true;

    bool sync_run_state = true;

    bool mirror_hit_reactions = false;

    bool sync_peer_age = true;

    bool director_targets_partner = false;

    bool client_simulates_enemies = false;

    bool peer_fights_locally = true;

    bool retarget_from_down_peer = false;
    bool observer_cosmetic_enemy_attacks_only = false;
    bool sync_enemy_death_animations = false;
    bool use_engine_outfit_refresh = false;
    bool sync_peer_visual_age = false;

    bool remote_player_attacks = true;

    bool force_enemy_engage = false;

    bool puppet_invincible = true;

    bool puppet_ignores_pawn_collision = true;

    bool auto_follow_level = true;

    bool auto_join_level = false;
    bool adaptive_interp = true;

    int interp_delay_ms = 60;
    int snapshot_hz = 60;

    bool in_game_overlay = true;

    bool menu_exclusive_input = false;

    bool verbose_enemies = false;
    bool verbose_orders = false;

    bool selftest = false;
};

struct Stats {
    int rtt_ms = -1;
    int rtt_jitter_ms = 0;
    std::uint32_t packets_sent = 0;
    std::uint32_t packets_received = 0;
    std::uint32_t packets_dropped = 0;
    std::uint32_t packets_rejected = 0;

    std::uint32_t snapshots_received = 0;
    std::uint32_t bytes_per_second_in = 0;
    std::uint32_t bytes_per_second_out = 0;

    int enemies_known = 0;
    int enemies_active = 0;
    int enemies_driven = 0;
    int enemies_unmatched = 0;

    std::uint32_t attacks_echoed = 0;
    std::uint32_t damage_reports = 0;
    float damage_reported_total = 0.f;
    float damage_applied_total = 0.f;

    char last_problem[160] = {};
};

Config& Get();
Stats& GetStats();

void Load();
void Save();

void IniPath(char* out, int out_size);

void ReportProblem(const char* format, ...);

}
