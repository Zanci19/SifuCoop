// In-game overlay, drawn inside the game's own frame.
//
// The Win32 overlay window cannot appear over exclusive fullscreen -- nothing
// can, by design -- so the status line is instead rendered as part of Sifu's
// frame by hooking the swap chain's Present.
//
// Deliberately DISPLAY-ONLY: no WndProc hook, no input capture, no mouse. That
// removes the riskiest half of the usual approach and means the mod can never
// swallow a keypress the game needed. F2 continues to be read by polling.

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../../third_party/imgui/backends/imgui_impl_dx11.h"
#include "../../third_party/imgui/backends/imgui_impl_win32.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/minhook/include/MinHook.h"
#include "../core/log.h"
#include "../game/coop.h"
#include "../game/levels.g.h"
#include "../net/session.h"
#include "overlay.h"

// imgui_impl_win32.h leaves this declaration inside an #if 0 for the
// application to provide, so it has to be declared here -- at global scope, or
// it would become a static of the anonymous namespace and never link.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message,
                                                             WPARAM wparam, LPARAM lparam);

namespace sifucoop::ui {

namespace levels = sifucoop::levels;
namespace coop = sifucoop::coop;
namespace net = sifucoop::net;

namespace {

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT,
                                            UINT);

PresentFn g_original_present = nullptr;
ResizeBuffersFn g_original_resize_buffers = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;
HWND g_window = nullptr;

// The swap chain we attached to, and the ONLY one we will ever touch.
//
// The hook is installed on IDXGISwapChain's vtable, which is shared by every
// swap chain in the process -- and Sifu is not alone in here. DLSS and the
// Epic overlay each bring their own, and a game can legitimately present on
// more than one. Without this check the overlay would bind a render target
// view created from the game's back buffer while some other swap chain was
// presenting, possibly on a different device entirely. D3D11 does not
// diagnose that; it dereferences something that is not what it expects and
// faults inside itself, with no frame of ours anywhere on the stack.
IDXGISwapChain* g_swap_chain = nullptr;

bool g_initialised = false;
bool g_failed = false;

CRITICAL_SECTION g_text_lock;
bool g_text_lock_ready = false;
char g_text[192] = "SifuCoop";

void CopyText(char* out, int size) {
    if (!g_text_lock_ready) {
        lstrcpynA(out, g_text, size);
        return;
    }
    EnterCriticalSection(&g_text_lock);
    lstrcpynA(out, g_text, size);
    LeaveCriticalSection(&g_text_lock);
}

LRESULT CALLBACK WndProcHook(HWND, UINT, WPARAM, LPARAM);
void DrawMenu();

WNDPROC g_original_wndproc = nullptr;

bool CreateRenderTarget(IDXGISwapChain* swap_chain) {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void**>(&back_buffer))) ||
        !back_buffer) {
        return false;
    }
    const HRESULT hr = g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
    back_buffer->Release();
    return SUCCEEDED(hr);
}

bool InitialiseFrom(IDXGISwapChain* swap_chain) {
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device),
                                     reinterpret_cast<void**>(&g_device)))) {
        SC_LOG("d3d: swap chain is not D3D11 -- overlay disabled");
        return false;
    }
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc = {};
    swap_chain->GetDesc(&desc);
    g_window = desc.OutputWindow;

    if (!CreateRenderTarget(swap_chain)) {
        SC_LOG("d3d: could not create render target -- overlay disabled");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // never write imgui.ini into the game folder
    io.LogFilename = nullptr;
    // No input: we do not own the mouse or keyboard, the game does.
    // Keyboard navigation is enabled as a fallback: if the game recentres or
    // captures the cursor, the menu is still usable with arrows and Enter.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = true;  // draw our own; the game hides the system one
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        SC_LOG("d3d: ImGui backend init failed -- overlay disabled");
        return false;
    }

    g_original_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProcHook)));

    SC_LOG("d3d: in-game overlay active (hwnd %p, %ux%u), F1 opens the menu",
           static_cast<void*>(g_window), desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

// --- Menu state -------------------------------------------------------------

CRITICAL_SECTION g_menu_lock;
bool g_menu_lock_ready = false;
bool g_menu_open = false;

MenuRequests g_requests;
bool g_requests_pending = false;

// Live state, published by the game thread for the menu to display.
MenuStatus g_status;

constexpr int kMaxSyncRows = 96;
SyncRow g_sync_rows[kMaxSyncRows];
int g_sync_row_count = 0;

// Editable fields.
char g_field_address[64] = "127.0.0.1";
char g_field_passphrase[64] = {};
int g_field_port = 7777;
bool g_field_hosting = true;
int g_selected_level = 0;
bool g_fields_loaded = false;
char g_public_address[64] = {};

void PostRequests(const MenuRequests& requests) {
    EnterCriticalSection(&g_menu_lock);
    g_requests = requests;
    g_requests_pending = true;
    LeaveCriticalSection(&g_menu_lock);
}

MenuStatus CopyStatus() {
    if (!g_menu_lock_ready) return g_status;
    EnterCriticalSection(&g_menu_lock);
    MenuStatus copy = g_status;
    LeaveCriticalSection(&g_menu_lock);
    return copy;
}

int CopySyncRows(SyncRow* out, int max_out) {
    if (!g_menu_lock_ready) return 0;
    EnterCriticalSection(&g_menu_lock);
    const int count = g_sync_row_count < max_out ? g_sync_row_count : max_out;
    for (int i = 0; i < count; ++i) out[i] = g_sync_rows[i];
    LeaveCriticalSection(&g_menu_lock);
    return count;
}

// A gauge that reads at a glance. Sifu's own bars are the reference: full is
// unremarkable, empty is the thing you need to notice.
void VitalBar(const char* label, float value, float maximum, const ImVec4& colour) {
    const float fraction = maximum > 0.f ? value / maximum : 0.f;
    char text[48] = {};
    if (maximum > 0.f) {
        snprintf(text, sizeof(text), "%.0f / %.0f", value, maximum);
    } else {
        snprintf(text, sizeof(text), "unknown");
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colour);
    ImGui::ProgressBar(fraction < 0.f ? 0.f : (fraction > 1.f ? 1.f : fraction),
                       ImVec2(180, 0), text);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
}

// Every toggle carries the consequence of turning it off, because that is the
// only reason anyone opens this tab: something looks wrong and they are trying
// to find which half of the machinery is responsible.
void ConfigToggle(const char* label, bool* value, const char* explanation) {
    ImGui::Checkbox(label, value);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", explanation);
    ImGui::Indent();
    ImGui::TextDisabled("%s", explanation);
    ImGui::Unindent();
}

void DrawLobbyTab(const MenuStatus& status) {
    const coop::Stats& stats = coop::GetStats();

    ImGui::TextColored(status.connected ? ImVec4(0.45f, 0.90f, 0.45f, 1.f)
                                        : ImVec4(0.90f, 0.65f, 0.35f, 1.f),
                       "%s", status.connected ? "CONNECTED" : "Not connected");
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", status.offline ? "networking off"
                                               : (status.hosting ? "hosting" : "joining"));
    if (status.connected) {
        ImGui::SameLine();
        if (stats.rtt_ms < 0) {
            ImGui::TextDisabled("| measuring ping...");
        } else {
            const ImVec4 colour = stats.rtt_ms < 60    ? ImVec4(0.45f, 0.90f, 0.45f, 1.f)
                                  : stats.rtt_ms < 140 ? ImVec4(0.95f, 0.85f, 0.40f, 1.f)
                                                       : ImVec4(0.95f, 0.45f, 0.40f, 1.f);
            ImGui::TextColored(colour, "| %d ms (+/- %d)", stats.rtt_ms, stats.rtt_jitter_ms);
        }
    }

    if (status.detail[0]) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.f), "! %s", status.detail);
    }

    ImGui::Separator();
    ImGui::Text("You:  %s", status.my_level[0] ? status.my_level : "-");
    ImGui::SameLine(260);
    VitalBar("health", status.my_health, status.my_max_health,
             ImVec4(0.80f, 0.25f, 0.25f, 1.f));

    ImGui::Text("Peer: %s", status.peer_level[0] ? status.peer_level : "-");
    ImGui::SameLine(260);
    if (status.peer_known) {
        VitalBar(status.peer_down ? "DOWN" : "health", status.peer_health,
                 status.peer_max_health, ImVec4(0.30f, 0.50f, 0.85f, 1.f));
    } else {
        ImGui::TextDisabled("no vitals reported yet");
    }

    if (status.connected && !status.together) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.40f, 1.f),
                           "You are in different levels -- you will not see each other.");
    }

    ImGui::Separator();
    ImGui::SeparatorText("Connection");
    ImGui::RadioButton("Host", g_field_hosting);
    if (ImGui::IsItemClicked()) g_field_hosting = true;
    ImGui::SameLine();
    ImGui::RadioButton("Join", !g_field_hosting);
    if (ImGui::IsItemClicked()) g_field_hosting = false;

    ImGui::BeginDisabled(g_field_hosting);
    ImGui::InputText("Host address", g_field_address, sizeof(g_field_address));
    ImGui::EndDisabled();
    ImGui::InputInt("Port", &g_field_port);

    ImGui::InputText("Passphrase", g_field_passphrase, sizeof(g_field_passphrase));
    if (g_field_passphrase[0] == '\0') {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.40f, 1.f),
                           "No passphrase: anyone who can reach this port can join.");
        ImGui::TextDisabled("Fine on a VPN or LAN. Set one before forwarding a port.");
    } else {
        ImGui::TextDisabled("Both players must type exactly the same passphrase.");
    }

    if (ImGui::Button("Apply & (re)connect", ImVec2(200, 0))) {
        MenuRequests r;
        r.apply_network = true;
        r.host_mode = g_field_hosting;
        lstrcpynA(r.address, g_field_address, sizeof(r.address));
        lstrcpynA(r.passphrase, g_field_passphrase, sizeof(r.passphrase));
        r.port = g_field_port;
        PostRequests(r);
    }
    if (stats.packets_rejected > 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.f),
                           "%u packets rejected -- usually a mismatched passphrase.",
                           stats.packets_rejected);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!status.connected || !status.hosting);
    if (ImGui::Button("Bring peer here", ImVec2(160, 0))) {
        MenuRequests r;
        r.invite_peer = true;
        PostRequests(r);
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Mode");
    coop::Config& config = coop::Get();
    int mode = static_cast<int>(config.mode);
    if (ImGui::RadioButton("Co-op (fight the level together)", &mode, 0)) {
        config.mode = coop::Mode::Coop;
    }
    if (ImGui::RadioButton("Versus (spar against each other)", &mode, 1)) {
        config.mode = coop::Mode::Versus;
    }
    ImGui::TextDisabled(
        "Faction decides this. Takes effect the next time the remote character\n"
        "is spawned -- use Respawn below after switching.");

    ImGui::Spacing();
    if (ImGui::Button("Respawn remote character")) {
        MenuRequests r;
        r.despawn_puppet = true;
        PostRequests(r);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(it comes back on its own within a second)");

    if (status.puppet_alive) {
        ImGui::TextDisabled("factions: you=%d remote=%d %s", status.faction_mine,
                            status.faction_puppet,
                            status.faction_mine == status.faction_puppet ? "(allied)"
                                                                         : "(hostile)");
    }
}

void DrawSyncTab() {
    const coop::Stats& stats = coop::GetStats();

    ImGui::Text("known %d   active %d   driven %d   unmatched %d", stats.enemies_known,
                stats.enemies_active, stats.enemies_driven, stats.enemies_unmatched);
    if (stats.enemies_unmatched > 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.40f, 1.f),
                           "The host is reporting enemies this machine does not have. "
                           "Usually means the two of you are not in the same level.");
    }
    ImGui::Text("damage: dealt %.0f reported, %.0f applied from peer, %u reports",
                stats.damage_reported_total, stats.damage_applied_total, stats.damage_reports);
    ImGui::Text("attacks echoed: %u", stats.attacks_echoed);

    ImGui::Separator();

    SyncRow rows[kMaxSyncRows];
    const int count = CopySyncRows(rows, kMaxSyncRows);

    if (ImGui::BeginTable("enemies", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0, 260))) {
        ImGui::TableSetupColumn("enemy", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("dist", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("health", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("ai", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        for (int i = 0; i < count; ++i) {
            const SyncRow& row = rows[i];
            // A pooled enemy is not in the fight; showing every one of the
            // sixty a level pre-spawns would bury the handful that matter.
            if (!row.active) continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.0f", row.distance);
            ImGui::TableSetColumnIndex(2);
            if (row.max_health > 0.f) {
                ImGui::Text("%.0f/%.0f", row.health, row.max_health);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableSetColumnIndex(3);
            if (row.down) {
                ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.70f, 1.f), "down");
            } else if (row.driven) {
                ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.45f, 1.f), "driven");
            } else {
                ImGui::TextUnformatted("local");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(row.ai_stopped ? "off" : "on");
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("Dump full roster to the log")) {
        MenuRequests r;
        r.log_roster = true;
        PostRequests(r);
    }
}

void DrawTuningTab() {
    coop::Config& config = coop::Get();

    ImGui::TextWrapped(
        "Everything below is on by default. Turn one off if something misbehaves -- "
        "that is the fastest way to find which half is responsible, and it takes "
        "effect immediately.");
    ImGui::Separator();

    ImGui::SeparatorText("Enemies");
    ConfigToggle("Drive enemies from the host", &config.sync_enemies,
                 "Off: each machine's enemies stand where its own game puts them.");
    ConfigToggle("Stop enemy AI on the joining side", &config.suppress_client_ai,
                 "Off: both machines run their own AI and the fight diverges within "
                 "seconds.");
    ConfigToggle("Follow the host's enemy health", &config.sync_enemy_vitals,
                 "Off: health bars drift apart, but enemies still die when the host "
                 "says so.");
    ConfigToggle("Hide enemies the host has not activated", &config.park_extra_enemies,
                 "Off: enemies your own game spawned stand around inert.");
    ConfigToggle("Replay the host's enemy attacks", &config.echo_enemy_attacks,
                 "Off: enemies move but never swing on the joining screen. This is the "
                 "least proven feature here -- turn it off first if enemies behave "
                 "strangely.");

    ImGui::SeparatorText("Damage");
    ConfigToggle("Report my hits to the host", &config.report_damage,
                 "Off: the joining player cannot hurt anything the host can see.");
    ConfigToggle("Show the peer's real health", &config.mirror_peer_vitals,
                 "Off: the other player's character always looks untouched.");

    ImGui::SeparatorText("Remote player's attacks (EXPERIMENTAL)");
    ConfigToggle("Replay the remote player's attacks", &config.echo_player_attacks,
                 "Off (safe default): the remote player moves but does not swing on "
                 "your screen. On: they swing -- but their punches can currently hurt "
                 "you (friendly fire), unless the option below actually works.");
    ConfigToggle("Make the remote player friendly (unverified)",
                 &config.friendly_relationship,
                 "Sets your relationship to the puppet 'friendly' in Sifu's own system so "
                 "its swings should pass through you, like two enemies not hurting each "
                 "other. UNVERIFIED that Sifu's melee honours this. Test: spawn a puppet "
                 "(F9), replay an attack on it (F6) next to you, watch your health.");

    ImGui::SeparatorText("Session");
    ConfigToggle("Follow the host between levels", &config.auto_follow_level,
                 "Off: you have to be invited into each level by hand.");
    ConfigToggle("Exchange age / room-clear / weapon", &config.sync_run_state,
                 "Off: nothing about the other player's run is shared. On: informational "
                 "only -- shown below and in the log; nothing is applied to your game.");

    // Peer run-state readout: proves the exchange end-to-end at a glance.
    if (net::IsConnected()) {
        net::RunSnapshot peer;
        if (net::GetPeerRunState(&peer)) {
            ImGui::Indent();
            if (peer.age_valid) {
                ImGui::Text("peer age: %d", peer.age);
            } else {
                ImGui::TextDisabled("peer age: -");
            }
            if (peer.room_clear_valid) {
                ImGui::Text("peer room-clear: %.0f%%", peer.room_clear_percent * 100.f);
            } else {
                ImGui::TextDisabled("peer room-clear: -");
            }
            ImGui::TextDisabled("peer weapon: %s",
                                peer.has_weapon ? peer.weapon_path : "none");
            ImGui::Unindent();
        } else {
            ImGui::Indent();
            ImGui::TextDisabled("waiting for the peer's run state...");
            ImGui::Unindent();
        }
    }

    ImGui::SeparatorText("Latency");
    ConfigToggle("Size the buffer from measured ping", &config.adaptive_interp,
                 "Off: the fixed delay below is used instead.");
    ImGui::BeginDisabled(config.adaptive_interp);
    ImGui::SliderInt("Interpolation delay (ms)", &config.interp_delay_ms, 0, 250);
    ImGui::EndDisabled();
    ImGui::SliderInt("Snapshot rate (Hz)", &config.snapshot_hz, 20, 120);
    ImGui::TextDisabled(
        "More delay is smoother and later; less is sharper and jumpier.\n"
        "Your own character is never delayed, whatever this says.");

    ImGui::SeparatorText("Logging");
    ImGui::Checkbox("Log every enemy event", &config.verbose_enemies);
    ImGui::Checkbox("Log every attack event", &config.verbose_orders);

    ImGui::Separator();
    if (ImGui::Button("Save these settings", ImVec2(200, 0))) {
        MenuRequests r;
        r.save_config = true;
        PostRequests(r);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("writes SifuCoop.ini next to the game exe");
}

void DrawLevelsTab() {
    ImGui::TextWrapped(
        "Travel to a level and pull your peer with you. Only the host should start; "
        "the joiner follows automatically, including through later level changes.");
    ImGui::Separator();

    ImGui::BeginChild("levels", ImVec2(0, 280), true);
    const char* last_category = nullptr;
    for (int i = 0; i < levels::kLevelCount; ++i) {
        const auto& level = levels::kLevels[i];
        if (!last_category || strcmp(last_category, level.category) != 0) {
            last_category = level.category;
            ImGui::SeparatorText(level.category);
        }
        if (ImGui::Selectable(level.name, g_selected_level == i)) g_selected_level = i;
    }
    ImGui::EndChild();

    const auto& chosen = levels::kLevels[g_selected_level];
    ImGui::TextDisabled("%s", chosen.package);

    if (ImGui::Button("Start here (travel + invite peer)", ImVec2(320, 0))) {
        MenuRequests r;
        r.travel = true;
        lstrcpynA(r.level, chosen.package, sizeof(r.level));
        PostRequests(r);
    }
}

void DrawInternetTab() {
    ImGui::TextWrapped(
        "Two machines behind home routers cannot reach each other by default. "
        "There are three ways round it; try them in this order.");

    ImGui::SeparatorText("1. A private network  (recommended)");
    ImGui::TextWrapped(
        "ZeroTier or Tailscale. Both of you install it and join the same network; each "
        "machine gets a 10.x address that simply works, with no router configuration and "
        "nothing exposed to the internet. Put that 10.x address in the Lobby tab.");

    ImGui::SeparatorText("2. The host forwards a port");
    ImGui::TextWrapped(
        "On the host's router, forward UDP 7777 to the host's machine. The joiner then "
        "connects to the host's public address. Reliable, but it does expose that port -- "
        "set a passphrase in the Lobby tab before doing this.");

    ImGui::SeparatorText("3. Hole punching  (no router access needed)");
    ImGui::TextWrapped(
        "Both of you press the button below and send each other the address it prints. "
        "Put the other person's address in SifuCoop.ini as punch=ADDRESS:PORT, and set "
        "local_port to the same number on both machines. Then connect at the same time. "
        "This works on most home routers and fails on some -- if it does not connect "
        "within a minute, fall back to option 1.");

    ImGui::Spacing();
    if (ImGui::Button("Find my public address", ImVec2(220, 0))) {
        MenuRequests r;
        r.discover_address = true;
        PostRequests(r);
    }
    ImGui::SameLine();
    if (g_public_address[0]) {
        ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.45f, 1.f), "%s", g_public_address);
    } else {
        ImGui::TextDisabled("asks a public STUN server; nothing is sent until you press it");
    }
    if (g_public_address[0]) {
        ImGui::TextDisabled("Send that to the other player. It is what your router looks");
        ImGui::TextDisabled("like from outside, for the exact port this game is using.");
    }

    ImGui::SeparatorText("Whichever you use");
    ImGui::BulletText("Both players need the same Sifu build and the same passphrase.");
    ImGui::BulletText("The host needs inbound UDP allowed through Windows Firewall.");
    ImGui::BulletText("Every packet is authenticated; unsigned ones are dropped unread.");
}

void DrawNetworkTab() {
    const coop::Stats& stats = coop::GetStats();
    if (stats.rtt_ms < 0) {
        ImGui::Text("round trip     not measured yet");
    } else {
        ImGui::Text("round trip     %d ms (jitter %d ms)", stats.rtt_ms, stats.rtt_jitter_ms);
    }
    ImGui::Text("packets        %u sent, %u received", stats.packets_sent,
                stats.packets_received);
    ImGui::Text("lost inbound   %u snapshots", stats.packets_dropped);
    ImGui::Text("bandwidth      %.1f KB/s out, %.1f KB/s in",
                stats.bytes_per_second_out / 1024.f, stats.bytes_per_second_in / 1024.f);
    ImGui::Separator();
    ImGui::TextWrapped(
        "Loss of a few snapshots is normal and invisible -- the buffer covers it. "
        "Steady loss above a few percent shows up as the other player stuttering, and "
        "is worth a look at the VPN rather than at these settings.");
    ImGui::Separator();
    ImGui::TextDisabled("Full log: %%LOCALAPPDATA%%\\Sifu\\Saved\\Logs\\SifuCoop.log");
}

void DrawHowToTab() {
    ImGui::TextWrapped(
        "1. Both players install the mod: dsound.dll and SifuCoop.ini in "
        "Sifu\\Binaries\\Win64, next to Sifu-Win64-Shipping.exe.\n\n"
        "2. Both join the same ZeroTier network and authorise each device at "
        "my.zerotier.com. Each machine gets a 10.x address.\n\n"
        "3. One player picks Host. The other picks Join and types the host's 10.x "
        "address. Press Apply on both.\n\n"
        "4. When this tab's Lobby shows CONNECTED, the host loads a level -- through "
        "Sifu's own menus, or from the Levels tab. The joiner is pulled in.\n\n"
        "5. Fight. The host's game decides what the enemies do; both of you can hurt "
        "them; each of you decides your own damage, so nobody's parry is punished by "
        "the network.");

    ImGui::Separator();
    ImGui::SeparatorText("Keys");
    ImGui::BulletText("F1      this menu");
    ImGui::BulletText("F2      bring your peer into the level you are in");
    ImGui::BulletText("F6      make the remote character replay your last attack");
    ImGui::BulletText("F7      make the nearest enemy replay your last attack");
    ImGui::BulletText("F8      follow mode (offline rehearsal)");
    ImGui::BulletText("F9/F10  spawn / despawn the remote character by hand");
    ImGui::BulletText("INSERT  dump every tracked character to the log");
    ImGui::TextDisabled("F3-F5 are avoided: the Epic overlay owns F3.");

    ImGui::SeparatorText("What is and is not synchronised");
    ImGui::BulletText("Both players, their positions, attacks, health and death: yes.");
    ImGui::BulletText("Enemy positions, health and death: yes, decided by the host.");
    ImGui::BulletText("Enemy attacks: replayed on the joining side, but the exact");
    ImGui::Indent();
    ImGui::TextDisabled("strike may differ -- Sifu picks it from its own combo state.");
    ImGui::Unindent();
    ImGui::BulletText("Shrines, upgrades, age and save progress: not shared. Each");
    ImGui::Indent();
    ImGui::TextDisabled("player keeps their own, as in singleplayer.");
    ImGui::Unindent();
    ImGui::BulletText("Bosses and cutscenes: untested, expect them to go their own way.");
}

void DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (!ImGui::Begin("SifuCoop", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const MenuStatus status = CopyStatus();

    {
        char line[192] = {};
        CopyText(line, sizeof(line));
        ImGui::TextColored(ImVec4(0.98f, 0.80f, 0.45f, 1.f), "%s", line);
        ImGui::Separator();
    }

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Lobby")) {
            DrawLobbyTab(status);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Levels")) {
            DrawLevelsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sync")) {
            DrawSyncTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tuning")) {
            DrawTuningTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Internet")) {
            DrawInternetTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Network")) {
            DrawNetworkTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("How to")) {
            DrawHowToTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::TextDisabled("F1 closes this menu");
    ImGui::End();
}

// Input is SHARED while the menu is open: ImGui sees it and the game still sees
// it. Nothing is swallowed, so the mod can never leave the player unable to act
// -- at the cost of the game reacting underneath (moving the mouse still turns
// the camera, clicking a button may also throw a punch).
LRESULT CALLBACK WndProcHook(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_menu_open) {
        ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
    }
    return CallWindowProcW(g_original_wndproc, window, message, wparam, lparam);
}

// Sifu recentres the cursor every frame to drive the camera, which pins the
// pointer to the middle of the screen -- so no button could ever be clicked, no
// matter how the input is routed. Suppressing the recentre (and the clip that
// confines it) only while the menu is open is what actually makes it clickable.
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

SetCursorPosFn g_original_set_cursor_pos = nullptr;
ClipCursorFn g_original_clip_cursor = nullptr;

BOOL WINAPI SetCursorPosHook(int x, int y) {
    if (g_menu_open) return TRUE;  // pretend it worked; leave the cursor alone
    return g_original_set_cursor_pos(x, y);
}

BOOL WINAPI ClipCursorHook(const RECT* rect) {
    if (g_menu_open) return g_original_clip_cursor(nullptr);  // unconfine
    return g_original_clip_cursor(rect);
}

// Input is fed by POLLING rather than through the window procedure.
//
// ImGui takes mouse position from GetCursorPos, but mouse *buttons* only from
// WndProc messages -- and our subclass was not receiving them, so the menu drew
// but could not be clicked. Polling depends on nothing but the cursor being
// free to move, which the SetCursorPos hook guarantees, and it cannot take
// input away from the game because it never touches the message stream.
//
// Must run before ImGui::NewFrame(): queued events are consumed there.
void FeedMenuInput() {
    ImGuiIO& io = ImGui::GetIO();

    POINT cursor = {};
    if (GetCursorPos(&cursor) && ScreenToClient(g_window, &cursor)) {
        io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    }

    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);

    // Enough typing support for an address field: digits, dot, backspace. Full
    // text entry would need the message stream; the launcher covers that case.
    struct KeyMap {
        int vkey;
        char character;
        ImGuiKey imgui_key;
    };
    static const KeyMap keys[] = {
        {'0', '0', ImGuiKey_0}, {'1', '1', ImGuiKey_1}, {'2', '2', ImGuiKey_2},
        {'3', '3', ImGuiKey_3}, {'4', '4', ImGuiKey_4}, {'5', '5', ImGuiKey_5},
        {'6', '6', ImGuiKey_6}, {'7', '7', ImGuiKey_7}, {'8', '8', ImGuiKey_8},
        {'9', '9', ImGuiKey_9}, {VK_OEM_PERIOD, '.', ImGuiKey_Period},
        {VK_DECIMAL, '.', ImGuiKey_KeypadDecimal},
        {VK_BACK, 0, ImGuiKey_Backspace}, {VK_RETURN, 0, ImGuiKey_Enter},
        {VK_TAB, 0, ImGuiKey_Tab}, {VK_LEFT, 0, ImGuiKey_LeftArrow},
        {VK_RIGHT, 0, ImGuiKey_RightArrow}, {VK_UP, 0, ImGuiKey_UpArrow},
        {VK_DOWN, 0, ImGuiKey_DownArrow},
    };
    static bool was_down[sizeof(keys) / sizeof(keys[0])] = {};

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const bool down = (GetAsyncKeyState(keys[i].vkey) & 0x8000) != 0;
        if (down != was_down[i]) {
            io.AddKeyEvent(keys[i].imgui_key, down);
            if (down && keys[i].character) io.AddInputCharacter(keys[i].character);
            was_down[i] = down;
        }
    }
}

void ReleaseRenderTarget() {
    if (g_render_target) {
        g_render_target->Release();
        g_render_target = nullptr;
    }
}

// ResizeBuffers fails with DXGI_ERROR_INVALID_CALL while ANY reference to the
// back buffer is outstanding -- and our render target view is exactly that.
// UE resizes when switching to fullscreen and treats the failure as fatal, so
// holding that view across a resize crashed the game on startup. Release it
// here; Present recreates it on the next frame.
HRESULT __stdcall ResizeBuffersHook(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width,
                                    UINT height, DXGI_FORMAT format, UINT flags) {
    // Only ours. Releasing our view because somebody else's swap chain resized
    // would drop it for no reason; worse, not releasing it when *ours* resizes
    // makes ResizeBuffers fail with DXGI_ERROR_INVALID_CALL, which UE treats as
    // fatal -- that is a crash already in this game's history.
    if (swap_chain == g_swap_chain) ReleaseRenderTarget();
    return g_original_resize_buffers(swap_chain, buffer_count, width, height, format, flags);
}

HRESULT __stdcall PresentHook(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    if (!g_failed && !g_initialised) {
        g_initialised = InitialiseFrom(swap_chain);
        if (!g_initialised) g_failed = true;  // never retry a broken init every frame
        if (g_initialised) g_swap_chain = swap_chain;
    }

    // Anything that is not the swap chain we attached to is passed straight
    // through, untouched. See g_swap_chain: drawing into someone else's chain
    // is a fault inside D3D11 with none of our code on the stack.
    if (swap_chain != g_swap_chain) {
        return g_original_present(swap_chain, sync_interval, flags);
    }

    // Recreate after a resize (fullscreen toggle, resolution change).
    if (g_initialised && !g_render_target && g_device) {
        if (!CreateRenderTarget(swap_chain)) {
            return g_original_present(swap_chain, sync_interval, flags);
        }
    }

    // F1 is polled here as well as handled in the WndProc hook. If that hook
    // ever misbehaves and swallows input, this still closes the menu and hands
    // control back to the game -- the failsafe for the one change in this mod
    // that can take input away from the player.
    if (g_initialised) {
        static bool f1_was_down = false;
        const bool f1_down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (f1_down && !f1_was_down) {
            g_menu_open = !g_menu_open;
            ImGui::GetIO().MouseDrawCursor = g_menu_open;
            SC_LOG("menu: %s", g_menu_open ? "opened" : "closed");
        }
        f1_was_down = f1_down;
    }

    if (g_initialised && g_render_target) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (g_menu_open) FeedMenuInput();
        ImGui::NewFrame();
        // The status line is part of the menu now; showing it permanently was
        // clutter during play.
        if (g_menu_open) DrawMenu();
        ImGui::Render();

        // Bind only the back buffer; the game's own state is restored by it on
        // the next frame, and we touch nothing else.
        g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return g_original_present(swap_chain, sync_interval, flags);
}

// Present cannot be found by symbol: it is a COM virtual. Creating a throwaway
// swap chain gives a real vtable to read the address from, which is the
// standard way and stays correct across driver and Windows versions.
void* FindPresent(void** out_resize_buffers) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SifuCoopDummy";
    RegisterClassExW(&wc);

    HWND dummy = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy) return nullptr;

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = dummy;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc,
        &swap_chain, &device, nullptr, &context);

    void* present = nullptr;
    if (SUCCEEDED(hr) && swap_chain) {
        // IDXGISwapChain vtable: 8 = Present, 13 = ResizeBuffers.
        void** vtable = *reinterpret_cast<void***>(swap_chain);
        present = vtable[8];
        *out_resize_buffers = vtable[13];
    } else {
        SC_LOG("d3d: could not create a probe swap chain (0x%08lX)", hr);
    }

    if (swap_chain) swap_chain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(dummy);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return present;
}

}  // namespace

bool StartInGameOverlay() {
    if (!g_text_lock_ready) {
        InitializeCriticalSection(&g_text_lock);
        g_text_lock_ready = true;
    }
    if (!g_menu_lock_ready) {
        InitializeCriticalSection(&g_menu_lock);
        g_menu_lock_ready = true;
    }

    void* resize_buffers = nullptr;
    void* present = FindPresent(&resize_buffers);
    if (!present) {
        SC_LOG("d3d: Present not found -- falling back to the window overlay");
        return false;
    }

    // MinHook is already initialised by the order hooks; calling twice is safe.
    MH_Initialize();
    if (MH_CreateHook(present, reinterpret_cast<void*>(&PresentHook),
                      reinterpret_cast<void**>(&g_original_present)) != MH_OK ||
        MH_EnableHook(present) != MH_OK) {
        SC_LOG("d3d: failed to hook Present -- falling back to the window overlay");
        return false;
    }

    // Without this second hook the first fullscreen transition kills the game.
    if (resize_buffers &&
        MH_CreateHook(resize_buffers, reinterpret_cast<void*>(&ResizeBuffersHook),
                      reinterpret_cast<void**>(&g_original_resize_buffers)) == MH_OK &&
        MH_EnableHook(resize_buffers) == MH_OK) {
        SC_LOG("d3d: Present + ResizeBuffers hooked");

        // Without these the menu draws but cannot be clicked, because the game
        // pins the cursor to the screen centre every frame for camera control.
        if (MH_CreateHook(reinterpret_cast<void*>(&SetCursorPos),
                          reinterpret_cast<void*>(&SetCursorPosHook),
                          reinterpret_cast<void**>(&g_original_set_cursor_pos)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(&SetCursorPos)) == MH_OK) {
            SC_LOG("d3d: cursor recentring suppressed while the menu is open");
        } else {
            SC_LOG("d3d: SetCursorPos hook FAILED -- the menu may not be clickable");
        }
        if (MH_CreateHook(reinterpret_cast<void*>(&ClipCursor),
                          reinterpret_cast<void*>(&ClipCursorHook),
                          reinterpret_cast<void**>(&g_original_clip_cursor)) == MH_OK) {
            MH_EnableHook(reinterpret_cast<void*>(&ClipCursor));
        }
    } else {
        SC_LOG("d3d: ResizeBuffers hook FAILED -- disabling overlay, a fullscreen "
               "switch would crash the game");
        MH_DisableHook(present);
        return false;
    }
    return true;
}

bool TakeMenuRequests(MenuRequests* out) {
    if (!g_menu_lock_ready || !out) return false;
    EnterCriticalSection(&g_menu_lock);
    const bool pending = g_requests_pending;
    if (pending) {
        *out = g_requests;
        g_requests_pending = false;
    }
    LeaveCriticalSection(&g_menu_lock);
    return pending;
}

void SetMenuStatus(const MenuStatus& status) {
    if (!g_menu_lock_ready) return;
    EnterCriticalSection(&g_menu_lock);
    g_status = status;

    // Seed the editable fields from the live configuration once, so the menu
    // opens showing what is actually in effect rather than defaults.
    if (!g_fields_loaded) {
        g_fields_loaded = true;
        g_field_hosting = status.hosting;
    }
    lstrcpynA(g_public_address, status.public_address, sizeof(g_public_address));
    LeaveCriticalSection(&g_menu_lock);
}

void SetSyncRows(const SyncRow* rows, int count) {
    if (!g_menu_lock_ready || !rows) return;
    if (count < 0) count = 0;
    if (count > kMaxSyncRows) count = kMaxSyncRows;
    EnterCriticalSection(&g_menu_lock);
    for (int i = 0; i < count; ++i) g_sync_rows[i] = rows[i];
    g_sync_row_count = count;
    LeaveCriticalSection(&g_menu_lock);
}

bool IsMenuOpen() { return g_menu_open; }

void SetInGameOverlayText(const char* text) {
    if (!text) return;
    if (!g_text_lock_ready) return;
    EnterCriticalSection(&g_text_lock);
    lstrcpynA(g_text, text, sizeof(g_text));
    LeaveCriticalSection(&g_text_lock);
}

}  // namespace sifucoop::ui





