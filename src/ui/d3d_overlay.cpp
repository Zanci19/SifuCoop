// In-game overlay, drawn inside the game's own frame.
//
// The Win32 overlay window cannot appear over exclusive fullscreen -- nothing
// can, by design -- so the status line is instead rendered as part of Sifu's
// frame by hooking the swap chain's Present.
//
// Deliberately DISPLAY-ONLY: no WndProc hook, no input capture, no mouse. That
// removes the riskiest half of the usual approach and means the mod can never
// swallow a keypress the game needed. F1 is the only player-facing hotkey.

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <atomic>

#include "../../third_party/imgui/backends/imgui_impl_dx11.h"
#include "../../third_party/imgui/backends/imgui_impl_win32.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/minhook/include/MinHook.h"
#include "../core/log.h"
#include "../game/coop.h"
#include "../game/player2.h"
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
std::atomic<bool> g_menu_open{false};

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

void DrawLobbyTab(const MenuStatus& status) {
    const coop::Stats& stats = coop::GetStats();
    const bool native_network = coop::NativeNetworkActive();
    const ImVec4 good(0.45f, 0.90f, 0.45f, 1.f);
    const ImVec4 warn(0.95f, 0.72f, 0.30f, 1.f);

    if (native_network) {
        const ImVec4 bad(0.95f, 0.35f, 0.35f, 1.f);
        ImGui::TextColored(bad, "ENGINE NETWORKING - ALL CO-OP SYNC IS OFF");
        ImGui::TextWrapped("Player and enemy sync, damage and invites are disabled. "
                           "Set native_network=0 in SifuCoop.ini on both PCs and restart.");
        ImGui::Separator();
    }
    ImGui::TextColored(status.connected ? good : warn, "%s",
                       status.connected ? "CONNECTED" : "Waiting for player");
    if (status.connected && stats.rtt_ms >= 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d ms", stats.rtt_ms);
    }
    if (status.detail[0]) ImGui::TextColored(warn, "%s", status.detail);

    ImGui::TextDisabled("Use the host's ZeroTier IP.");
    if (status.connected) {
        ImGui::Text("Level: %s", status.my_level[0] ? status.my_level : "loading...");
        ImGui::SameLine();
        ImGui::TextDisabled("Partner: %s", status.peer_known ? "ready" : "loading...");
    }

    ImGui::SeparatorText("Connection");
    ImGui::RadioButton("Host", g_field_hosting);
    if (ImGui::IsItemClicked()) g_field_hosting = true;
    ImGui::SameLine();
    ImGui::RadioButton("Join", !g_field_hosting);
    if (ImGui::IsItemClicked()) g_field_hosting = false;

    ImGui::BeginDisabled(g_field_hosting);
    ImGui::InputText("Host ZeroTier IP", g_field_address, sizeof(g_field_address));
    ImGui::EndDisabled();
    ImGui::InputInt("Port", &g_field_port);
    ImGui::InputText("Shared passphrase", g_field_passphrase, sizeof(g_field_passphrase),
                     ImGuiInputTextFlags_Password);
    ImGui::TextDisabled("Same passphrase on both PCs.");

    if (ImGui::Button(native_network ? "Save connection" : "Save & Connect", ImVec2(180, 0))) {
        MenuRequests r;
        r.apply_network = true;
        r.host_mode = g_field_hosting;
        lstrcpynA(r.address, g_field_address, sizeof(r.address));
        lstrcpynA(r.passphrase, g_field_passphrase, sizeof(r.passphrase));
        r.port = g_field_port;
        PostRequests(r);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!status.connected || !status.hosting);
    if (ImGui::Button("Start co-op here")) {
        MenuRequests r;
        r.invite_peer = true;
        PostRequests(r);
    }
    ImGui::EndDisabled();

    if (native_network) {
        ImGui::SameLine();
        if (ImGui::Button(g_field_hosting ? "Start Engine Host" : "Join Engine Host")) {
            MenuRequests r;
            r.native_start = true;
            r.host_mode = g_field_hosting;
            lstrcpynA(r.address, g_field_address, sizeof(r.address));
            r.port = g_field_port;
            PostRequests(r);
        }
        ImGui::TextDisabled("Engine mode reloads the level; use it only after restart.");
    }

    // A standing invite from the host. It is deliberately an offer rather than
    // something that just happens: the host loading a level used to drag the
    // other player out of whatever they were doing without a prompt.
    if (status.invite_pending) {
        ImGui::SeparatorText("Invitation");
        ImGui::TextColored(warn, "Your partner is playing %s", status.invite_level);
        if (ImGui::Button("Join them", ImVec2(180, 0))) {
            MenuRequests r;
            r.accept_invite = true;
            PostRequests(r);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Always join automatically", &coop::Get().auto_join_level);
    }

    if (status.connected && status.together) {
        ImGui::SeparatorText("Together");
        // The "meet me at the boss" request from the play-testers, generalised:
        // rather than scripting a trigger per boss door, either player can close
        // the gap whenever they want to start something together.
        if (ImGui::Button("Teleport to partner", ImVec2(180, 0))) {
            MenuRequests r;
            r.teleport_to_peer = true;
            PostRequests(r);
        }
        ImGui::SameLine();
        if (status.friendly_confirmed) {
            ImGui::TextColored(good, "Friendly fire off");
        } else {
            ImGui::TextColored(warn, "Friendly fire unconfirmed - partner will not swing");
        }
    }

    if (status.connected && !status.together && !status.invite_pending) {
        ImGui::TextColored(warn, "%s", status.hosting ? "Press Start co-op here when ready."
                                                        : "Waiting for host to start co-op.");
    }
    if (stats.packets_rejected > 0) {
        ImGui::TextColored(warn, "Connection rejected: check the shared passphrase.");
    }
}

void DrawDebugTab() {
    const coop::Stats& stats = coop::GetStats();
    coop::Config& config = coop::Get();

    ImGui::Text("Network: %s", net::IsConnected() ? "connected" : "not connected");
    ImGui::Text("Ping: %d ms   Enemies: %d active / %d synced", stats.rtt_ms,
                stats.enemies_active, stats.enemies_driven);
    ImGui::Separator();
    ImGui::Checkbox("Detailed enemy log", &config.verbose_enemies);
    ImGui::Checkbox("Detailed combat log", &config.verbose_orders);
    ImGui::Checkbox("Adaptive smoothing", &config.adaptive_interp);
    ImGui::Checkbox("Partner swings visibly (needs friendly fire confirmed)",
                    &config.remote_player_attacks);
    ImGui::Checkbox("Hide partner's health bar", &config.hide_second_player_hud);
    ImGui::Checkbox("Join partner's level without asking", &config.auto_join_level);

    ImGui::Checkbox("Use Unreal engine networking (restart after saving)", &config.native_network);
    if (ImGui::Button("Save settings")) {
        MenuRequests r;
        r.save_config = true;
        PostRequests(r);
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump debug log")) {
        MenuRequests r;
        r.log_roster = true;
        PostRequests(r);
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart remote player")) {
        MenuRequests r;
        r.despawn_puppet = true;
        PostRequests(r);
    }
    ImGui::TextDisabled("Log: %%LOCALAPPDATA%%\\Sifu\\Saved\\Logs\\SifuCoop.log");
}

void DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(560, 390), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (!ImGui::Begin("SifuCoop", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const MenuStatus status = CopyStatus();
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Play")) {
            DrawLobbyTab(status);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
            DrawDebugTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::TextDisabled("F1 closes this menu");
    ImGui::End();
}

// While the menu is open, events ImGui actually consumes must not also reach
// Sifu. Forwarding every event made a GUI click double as a punch or a camera
// turn. F1 remains polled in PresentHook, so the player can always close the
// menu even if a window-message edge case occurs.
LRESULT CALLBACK WndProcHook(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_menu_open.load()) {
        ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        const ImGuiIO& io = ImGui::GetIO();
        switch (message) {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_INPUT:
                if (io.WantCaptureMouse) return 0;
                break;
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_CHAR:
            case WM_SYSCHAR:
            case WM_UNICHAR:
                if (io.WantCaptureKeyboard || io.WantTextInput) return 0;
                break;
            default:
                break;
        }
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

// Input is also fed by polling as a fallback. Some fullscreen/input-overlay
// combinations do not deliver mouse button messages to a subclass reliably;
// polling keeps the menu clickable in those cases while the WndProc path above
// prevents normally delivered GUI events from leaking into gameplay.
// Must run before ImGui::NewFrame(): queued events are consumed there.
void FeedMenuInput() {
    ImGuiIO& io = ImGui::GetIO();

    POINT cursor = {};
    if (GetCursorPos(&cursor) && ScreenToClient(g_window, &cursor)) {
        io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    }

    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);

    // Keyboard and text are delivered only through WndProc above. Polling them
    // here would duplicate WM_CHAR (digits) while Backspace arrives once.
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
        char ini[MAX_PATH] = {};
        char mode[16] = {};
        coop::IniPath(ini, sizeof(ini));
        GetPrivateProfileStringA("net", "mode", status.hosting ? "host" : "client", mode,
                                 sizeof(mode), ini);
        g_field_hosting = _stricmp(mode, "client") != 0;
        GetPrivateProfileStringA("net", "host", g_field_address, g_field_address,
                                 sizeof(g_field_address), ini);
        g_field_port = GetPrivateProfileIntA("net", "port", g_field_port, ini);
        if (g_field_port < 1 || g_field_port > 65535) g_field_port = 7777;
        GetPrivateProfileStringA("net", "passphrase", "", g_field_passphrase,
                                 sizeof(g_field_passphrase), ini);
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





