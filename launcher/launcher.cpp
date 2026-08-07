// SifuCoop launcher.
//
// Connecting is inherently a pre-launch decision -- you need the peer's address
// before the game starts -- so the setup UI lives here rather than in-game.
// Being a separate process, it also cannot crash Sifu.
//
// Plain Win32: no dependencies, no runtime to install, builds with the same
// MinGW toolchain as the mod.

// winsock2.h must precede windows.h, or windows.h pulls in the incompatible
// winsock 1 headers first.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <shlwapi.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr wchar_t kDefaultGameDir[] =
    L"C:\\Program Files\\Epic Games\\Sifu\\Sifu\\Binaries\\Win64";

enum : int {
    kIdHostRadio = 101,
    kIdJoinRadio = 102,
    kIdAddressEdit = 103,
    kIdPortEdit = 104,
    kIdSaveButton = 105,
    kIdLaunchButton = 106,
    kIdStatusLabel = 107,
    kIdAddressLabel = 108,
    kIdLocalIps = 109,
    kIdGameDirEdit = 110,
    kIdPassphraseEdit = 111,
};

HWND g_host_radio = nullptr;
HWND g_join_radio = nullptr;
HWND g_address_edit = nullptr;
HWND g_port_edit = nullptr;
HWND g_status = nullptr;
HWND g_local_ips = nullptr;
HWND g_game_dir = nullptr;
HWND g_address_label = nullptr;
HWND g_passphrase_edit = nullptr;

void GetIniPath(wchar_t* out, int count) {
    wchar_t dir[MAX_PATH] = {};
    GetWindowTextW(g_game_dir, dir, MAX_PATH);
    if (dir[0] == L'\0') wcsncpy(dir, kDefaultGameDir, MAX_PATH - 1);
    _snwprintf(out, count, L"%s\\SifuCoop.ini", dir);
}

void SetStatus(const wchar_t* text) { SetWindowTextW(g_status, text); }

bool IsHosting() { return SendMessageW(g_host_radio, BM_GETCHECK, 0, 0) == BST_CHECKED; }

// Hosting means telling the peer which address to use, and a machine on a VPN
// has several. ZeroTier hands out 10.x / 172.16-31.x, so those are flagged --
// the alternative is the user guessing from ipconfig output.
void RefreshLocalAddresses() {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    char hostname[256] = {};
    wchar_t text[1024] = L"Your addresses:\r\n";

    if (gethostname(hostname, sizeof(hostname)) == 0) {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* results = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &results) == 0) {
            for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
                auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
                char ip[64] = {};
                inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

                const unsigned long host_order = ntohl(addr->sin_addr.s_addr);
                const unsigned int a = (host_order >> 24) & 0xFF;
                const unsigned int b = (host_order >> 16) & 0xFF;
                const bool vpn = (a == 10) || (a == 172 && b >= 16 && b <= 31);

                wchar_t line[128] = {};
                _snwprintf(line, 128, L"   %hs%s\r\n", ip, vpn ? L"   <- likely ZeroTier" : L"");
                wcsncat(text, line, 1023 - wcslen(text));
            }
            freeaddrinfo(results);
        }
    }
    SetWindowTextW(g_local_ips, text);
    WSACleanup();
}

void LoadSettings() {
    wchar_t ini[MAX_PATH] = {};
    GetIniPath(ini, MAX_PATH);

    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"net", L"mode", L"host", mode, 32, ini);
    const bool hosting = (_wcsicmp(mode, L"client") != 0);

    SendMessageW(g_host_radio, BM_SETCHECK, hosting ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_join_radio, BM_SETCHECK, hosting ? BST_UNCHECKED : BST_CHECKED, 0);

    wchar_t address[128] = {};
    GetPrivateProfileStringW(L"net", L"host", L"", address, 128, ini);
    SetWindowTextW(g_address_edit, address);

    wchar_t port[16] = {};
    const UINT port_value = GetPrivateProfileIntW(L"net", L"port", 7777, ini);
    _snwprintf(port, 16, L"%u", port_value);
    SetWindowTextW(g_port_edit, port);

    wchar_t passphrase[64] = {};
    GetPrivateProfileStringW(L"net", L"passphrase", L"", passphrase, 64, ini);
    SetWindowTextW(g_passphrase_edit, passphrase);
}

bool SaveSettings() {
    wchar_t ini[MAX_PATH] = {};
    GetIniPath(ini, MAX_PATH);

    wchar_t dir[MAX_PATH] = {};
    GetWindowTextW(g_game_dir, dir, MAX_PATH);
    if (!PathFileExistsW(dir)) {
        SetStatus(L"Game folder not found - check the path above.");
        return false;
    }

    const bool hosting = IsHosting();

    wchar_t address[128] = {};
    GetWindowTextW(g_address_edit, address, 128);
    if (!hosting && address[0] == L'\0') {
        SetStatus(L"Joining needs the host's address.");
        return false;
    }

    wchar_t port[16] = {};
    GetWindowTextW(g_port_edit, port, 16);
    if (port[0] == L'\0') wcscpy(port, L"7777");

    wchar_t passphrase[64] = {};
    GetWindowTextW(g_passphrase_edit, passphrase, 64);

    WritePrivateProfileStringW(L"net", L"mode", hosting ? L"host" : L"client", ini);
    WritePrivateProfileStringW(L"net", L"host", address[0] ? address : L"127.0.0.1", ini);
    WritePrivateProfileStringW(L"net", L"port", port, ini);
    WritePrivateProfileStringW(L"net", L"passphrase", passphrase, ini);

    // The mod itself must be present, or none of this does anything.
    wchar_t dll[MAX_PATH] = {};
    _snwprintf(dll, MAX_PATH, L"%s\\dsound.dll", dir);
    if (!PathFileExistsW(dll)) {
        SetStatus(L"Saved - but dsound.dll is MISSING from that folder.");
        return true;
    }

    SetStatus(hosting ? L"Saved. Hosting - give your peer an address above."
                      : L"Saved. Will join the address above.");
    return true;
}

void LaunchGame() {
    if (!SaveSettings()) return;

    wchar_t dir[MAX_PATH] = {};
    GetWindowTextW(g_game_dir, dir, MAX_PATH);

    // Prefer the Epic shim in the install root: launching the shipping exe
    // directly can bypass Epic's startup and fail on entitlement checks.
    wchar_t root[MAX_PATH] = {};
    wcsncpy(root, dir, MAX_PATH - 1);
    for (int i = 0; i < 2; ++i) {
        wchar_t* slash = wcsrchr(root, L'\\');
        if (slash) *slash = L'\0';
    }
    wchar_t* slash = wcsrchr(root, L'\\');
    if (slash) *slash = L'\0';

    wchar_t shim[MAX_PATH] = {};
    _snwprintf(shim, MAX_PATH, L"%s\\Sifu.exe", root);

    const wchar_t* target = PathFileExistsW(shim) ? shim : nullptr;
    if (!target) {
        SetStatus(L"Saved, but Sifu.exe was not found - launch the game yourself.");
        return;
    }

    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", target, nullptr, root, SW_SHOWNORMAL));
    if (result <= 32) {
        SetStatus(L"Saved, but launching failed - start Sifu yourself.");
        return;
    }
    SetStatus(L"Launching Sifu...");
}

void UpdateAddressFieldState() {
    const bool hosting = IsHosting();
    EnableWindow(g_address_edit, !hosting);
    SetWindowTextW(g_address_label,
                   hosting ? L"(hosting - your peer enters YOUR address)" : L"Host's address:");
}

HWND MakeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id) {
    return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
}

void CreateControls(HWND window) {
    MakeLabel(window, L"Sifu install folder (Binaries\\Win64):", 16, 12, 320, 18, 0);
    g_game_dir = CreateWindowW(L"EDIT", kDefaultGameDir,
                               WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 16, 32, 420,
                               22, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdGameDirEdit)),
                               nullptr, nullptr);

    g_host_radio = CreateWindowW(L"BUTTON", L"Host a game",
                                 WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 16, 68,
                                 140, 22, window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHostRadio)),
                                 nullptr, nullptr);
    g_join_radio = CreateWindowW(L"BUTTON", L"Join a game",
                                 WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 168, 68, 140, 22,
                                 window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdJoinRadio)),
                                 nullptr, nullptr);

    g_address_label = MakeLabel(window, L"Host's address:", 16, 100, 260, 18, kIdAddressLabel);
    g_address_edit = CreateWindowW(L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 16, 120,
                                   260, 22, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAddressEdit)),
                                   nullptr, nullptr);

    MakeLabel(window, L"Port:", 296, 100, 60, 18, 0);
    g_port_edit = CreateWindowW(L"EDIT", L"7777",
                                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 296, 120, 80, 22,
                                window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPortEdit)),
                                nullptr, nullptr);

    // Both players must type the same passphrase. It keys the authentication on
    // every packet, so a mismatch is a silent refusal to connect rather than a
    // confusing half-working session -- which is exactly why it is on the first
    // screen rather than buried in the ini.
    MakeLabel(window, L"Passphrase (both players must match):", 16, 154, 300, 18, 0);
    g_passphrase_edit =
        CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 16, 174,
                      420, 22, window,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPassphraseEdit)), nullptr,
                      nullptr);
    MakeLabel(window,
              L"Leave empty only on a VPN or LAN. Set one before forwarding a port.", 16, 200,
              420, 18, 0);

    g_local_ips = CreateWindowW(L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY,
                                16, 224, 420, 92, window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLocalIps)),
                                nullptr, nullptr);

    CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE, 16, 328, 100, 28, window,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSaveButton)), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Save && Launch Sifu", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                  128, 328, 180, 28, window,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLaunchButton)), nullptr,
                  nullptr);

    g_status = MakeLabel(window, L"", 16, 366, 420, 40, kIdStatusLabel);

    // Use the system UI font; the default is the ancient bitmap one.
    HFONT font = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    EnumChildWindows(
        window,
        [](HWND child, LPARAM param) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(param), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font));
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            CreateControls(window);
            LoadSettings();
            RefreshLocalAddresses();
            UpdateAddressFieldState();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kIdSaveButton:
                    SaveSettings();
                    return 0;
                case kIdLaunchButton:
                    LaunchGame();
                    return 0;
                case kIdHostRadio:
                case kIdJoinRadio:
                    UpdateAddressFieldState();
                    return 0;
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"SifuCoopLauncher";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    RegisterClassExW(&wc);

    HWND window = CreateWindowExW(0, wc.lpszClassName, L"SifuCoop - Setup",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 470, 460, nullptr, nullptr,
                                  instance, nullptr);
    if (!window) return 1;

    ShowWindow(window, SW_SHOW);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return 0;
}
