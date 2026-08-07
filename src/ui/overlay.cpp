#include "overlay.h"

#include <windows.h>

#include <cstring>

#include "../core/log.h"

namespace sifucoop::ui {
namespace {

constexpr wchar_t kClassName[] = L"SifuCoopOverlay";
constexpr int kWidth = 340;
constexpr int kHeight = 28;

HWND g_window = nullptr;
HANDLE g_thread = nullptr;
volatile bool g_running = false;

CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
char g_text[128] = "SifuCoop: starting";

void CopyText(char* out, int size) {
    if (!g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    lstrcpynA(out, g_text, size);
    LeaveCriticalSection(&g_lock);
}

void Paint(HWND window) {
    PAINTSTRUCT ps = {};
    HDC dc = BeginPaint(window, &ps);

    RECT rect = {};
    GetClientRect(window, &rect);
    HBRUSH background = CreateSolidBrush(RGB(12, 12, 12));
    FillRect(dc, &rect, background);
    DeleteObject(background);

    char text[128] = {};
    CopyText(text, sizeof(text));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 200, 120));
    HFONT font = CreateFontA(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HGDIOBJ previous = SelectObject(dc, font);

    RECT text_rect = rect;
    text_rect.left += 8;
    DrawTextA(dc, text, -1, &text_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    SelectObject(dc, previous);
    DeleteObject(font);
    EndPaint(window, &ps);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        Paint(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

DWORD WINAPI OverlayThread(LPVOID) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Topmost, transparent to input (so clicks reach the game), and never
    // activated -- it must never steal focus from Sifu.
    g_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kClassName, L"SifuCoop", WS_POPUP, 24, 24, kWidth, kHeight, nullptr, nullptr,
        wc.hInstance, nullptr);

    if (!g_window) {
        SC_LOG("overlay: CreateWindowEx failed (%lu)", GetLastError());
        return 0;
    }

    SetLayeredWindowAttributes(g_window, 0, 215, LWA_ALPHA);
    ShowWindow(g_window, SW_SHOWNOACTIVATE);

    MSG message = {};
    while (g_running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        InvalidateRect(g_window, nullptr, FALSE);
        Sleep(250);  // status text does not need to be redrawn per frame
    }

    DestroyWindow(g_window);
    g_window = nullptr;
    return 0;
}

}  // namespace

void StartOverlay() {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    if (g_thread) return;
    g_running = true;
    g_thread = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    SC_LOG("overlay: started");
}

void StopOverlay() {
    g_running = false;
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

void SetOverlayText(const char* text) {
    if (!text) return;
    // Feed both: the in-game overlay when it is available, and the window as a
    // fallback. The window hides itself once the in-game one is running so the
    // same line is never drawn twice.
    SetInGameOverlayText(text);
    if (!g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    lstrcpynA(g_text, text, sizeof(g_text));
    LeaveCriticalSection(&g_lock);
}

}  // namespace sifucoop::ui

