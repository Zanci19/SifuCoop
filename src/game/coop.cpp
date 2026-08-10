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

bool g_native_network_at_startup = false;
// Whether the engine had the IpNetDriver config when it started, as opposed to
// us having just written it for next time.
bool g_native_driver_ready = false;
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

bool CreateDirectoryIfMissing(const char* path) {
    if (CreateDirectoryA(path, nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// THE reason `open <ip>:<port>` never connects.
//
// Sifu's packaged default GameNetDriver is not IpNetDriver -- it routes through
// EOS/Steam P2P, so an `open` against a plain IPv4 address is taken as a P2P
// session request and simply times out. No error, no refusal, nothing in the
// log: the listen server comes up and the client never arrives, which is exactly
// what the old sessions recorded 136 times over as
// "authoritative roster has 1 player(s); waiting for joiner".
//
// This existed once and was lost when the tree was overwritten, so the
// experiment was never finished rather than newly broken. The user config is
// read before this DLL is loaded at all, so writing it takes effect on the NEXT
// launch -- once, and then it stays.
void EnsureNativeIpNetDriverConfig() {
    char local_app_data[MAX_PATH] = {};
    if (GetEnvironmentVariableA("LOCALAPPDATA", local_app_data,
                                static_cast<DWORD>(sizeof(local_app_data))) == 0) {
        SC_LOG("native-net: could not locate LOCALAPPDATA for IP driver config");
        return;
    }

    char sifu_dir[MAX_PATH] = {};
    char saved_dir[MAX_PATH] = {};
    char config_dir[MAX_PATH] = {};
    char platform_dir[MAX_PATH] = {};
    char ini[MAX_PATH] = {};
    _snprintf(sifu_dir, sizeof(sifu_dir), "%s\\Sifu", local_app_data);
    _snprintf(saved_dir, sizeof(saved_dir), "%s\\Saved", sifu_dir);
    _snprintf(config_dir, sizeof(config_dir), "%s\\Config", saved_dir);
    _snprintf(platform_dir, sizeof(platform_dir), "%s\\WindowsNoEditor", config_dir);
    _snprintf(ini, sizeof(ini), "%s\\Engine.ini", platform_dir);

    if (!CreateDirectoryIfMissing(sifu_dir) || !CreateDirectoryIfMissing(saved_dir) ||
        !CreateDirectoryIfMissing(config_dir) || !CreateDirectoryIfMissing(platform_dir)) {
        SC_LOG("native-net: could not create IP driver config directory (error=%lu)",
               static_cast<unsigned long>(GetLastError()));
        return;
    }

    constexpr const char* kEngineSection = "/Script/Engine.Engine";

    // Was it already there when the engine read its config, or are we writing it
    // for the first time right now? That is the difference between "you can test
    // this" and "this cannot possibly work yet", and getting it wrong already
    // cost one test: the file was written during the very session it was then
    // tested in, so the engine had read the old config before the file existed
    // and the world could never have gone into listen mode.
    char existing[512] = {};
    GetPrivateProfileStringA(kEngineSection, "+NetDriverDefinitions", "", existing,
                             sizeof(existing), ini);
    g_native_driver_ready = strstr(existing, "IpNetDriver") != nullptr;

    const bool cleared = WritePrivateProfileStringA(kEngineSection, "!NetDriverDefinitions",
                                                    "ClearArray", ini) != FALSE;
    const bool wrote = WritePrivateProfileStringA(
        kEngineSection, "+NetDriverDefinitions",
        "(DefName=\"GameNetDriver\",DriverClassName=\"OnlineSubsystemUtils.IpNetDriver\","
        "DriverClassNameFallback=\"OnlineSubsystemUtils.IpNetDriver\")",
        ini) != FALSE;
    if (cleared && wrote) {
        if (g_native_driver_ready) {
            SC_LOG("native-net: UIpNetDriver is ACTIVE for this session -- hosting and "
                   "joining by IP can be tested now");
        } else {
            SC_LOG("native-net: UIpNetDriver written to %s, but this session already read "
                   "the old config -- RESTART THE GAME ONCE. Testing before that cannot "
                   "work and will report standalone", ini);
        }
    } else {
        SC_LOG("native-net: could not write UIpNetDriver config (error=%lu)",
               static_cast<unsigned long>(GetLastError()));
    }
}

}  // namespace

Config& Get() { return g_config; }
Stats& GetStats() { return g_stats; }
bool NativeNetworkActive() { return g_native_network_at_startup; }
bool NativeDriverReady() { return g_native_driver_ready; }

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
    // No longer forced off.
    //
    // This was hard-disabled with a log line and never revisited, and it is the
    // only route to what the mirror keeps approximating: one simulation, both
    // machines identical, enemies fighting both players with Sifu's own code.
    // The binary has every piece -- UWorld::Listen, UIpNetDriver::InitListen,
    // AThePlainesGameMode::PostLogin, APlayerController::ClientTravel -- and,
    // decisively, AFightingCharacter::GetLifetimeReplicatedProps exists, which
    // means Sifu's characters were built to replicate.
    //
    // Still default OFF, because whether the shipped game will actually accept
    // a login is unproven. Turning it on does not disable the UDP mirror; it
    // adds the engine path beside it so the two can be compared.
    g_config.native_network = ReadBool("native_network", false, ini);
    g_native_network_at_startup = g_config.native_network;
    if (g_config.native_network) {
        SC_LOG("coop: native UE4 networking ENABLED -- F1 -> Play to host or join");
        EnsureNativeIpNetDriverConfig();
    }
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
    // Was declared, documented and switchable in the overlay, but never read
    // from or written to the ini -- so the one thing that carries the peer's
    // dodges and traversal animations could not actually be configured.
    g_config.sync_montages = ReadBool("sync_montages", g_config.sync_montages, ini);
    g_config.report_damage = ReadBool("report_damage", g_config.report_damage, ini);
    g_config.mirror_peer_vitals = ReadBool("mirror_peer_vitals",
                                           g_config.mirror_peer_vitals, ini);
    g_config.sync_run_state = ReadBool("sync_run_state", g_config.sync_run_state, ini);
    g_config.peer_fights_locally =
        ReadBool("peer_fights_locally", g_config.peer_fights_locally, ini);
    g_config.mirror_hit_reactions =
        ReadBool("mirror_hit_reactions", g_config.mirror_hit_reactions, ini);
    g_config.fix_room_clear = ReadBool("fix_room_clear", g_config.fix_room_clear, ini);
    g_config.auto_follow_level = ReadBool("auto_follow_level",
                                          g_config.auto_follow_level, ini);
    g_config.auto_join_level = ReadBool("auto_join_level", g_config.auto_join_level, ini);
    g_config.adaptive_interp = ReadBool("adaptive_interp", g_config.adaptive_interp, ini);
    g_config.in_game_overlay = ReadBool("in_game_overlay", g_config.in_game_overlay, ini);
    g_config.selftest = ReadBool("selftest", g_config.selftest, ini);
    g_config.verbose_enemies = ReadBool("verbose_enemies", g_config.verbose_enemies, ini);
    g_config.verbose_orders = ReadBool("verbose_orders", g_config.verbose_orders, ini);

    g_config.interp_delay_ms = GetPrivateProfileIntA(kSection, "interp_delay_ms",
                                                     g_config.interp_delay_ms, ini);
    g_config.snapshot_hz = GetPrivateProfileIntA(kSection, "snapshot_hz",
                                                 g_config.snapshot_hz, ini);

    // Out-of-range values in a hand-edited ini would otherwise be silently
    // catastrophic: 0 Hz stops all sending, and a huge delay looks like a hang.
    if (g_config.interp_delay_ms < 0) g_config.interp_delay_ms = 0;
    if (g_config.interp_delay_ms > 500) g_config.interp_delay_ms = 500;
    // Below 30 Hz leaves the interpolation buffer starved on ordinary VPN jitter.
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

    // Was hardcoded false, so pressing "Save settings" -- or anything else that
    // writes the ini back -- silently reset the setting the user had just turned
    // on. Persist what is actually configured.
    WriteBool("native_network", g_config.native_network, ini);
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
    WriteBool("peer_fights_locally", g_config.peer_fights_locally, ini);
    WriteBool("mirror_hit_reactions", g_config.mirror_hit_reactions, ini);
    WriteBool("fix_room_clear", g_config.fix_room_clear, ini);
    WriteBool("auto_follow_level", g_config.auto_follow_level, ini);
    WriteBool("auto_join_level", g_config.auto_join_level, ini);
    WriteBool("adaptive_interp", g_config.adaptive_interp, ini);
    WriteBool("in_game_overlay", g_config.in_game_overlay, ini);
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

    // Only log a *change*: the same problem repeating every frame would bury
    // everything else, but the overlay still wants the current value.
    if (strcmp(text, g_stats.last_problem) != 0) SC_LOG("coop: %s", text);
    lstrcpynA(g_stats.last_problem, text, sizeof(g_stats.last_problem));
}

}  // namespace sifucoop::coop
