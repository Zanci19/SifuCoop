#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstring>
#include <cstdio>

namespace sifucoop::log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

ClockFn g_shared_clock = nullptr;
ClockValidFn g_shared_clock_valid = nullptr;
char g_role_tag = '-';

// A log that is only ever appended to grows across every session the mod has
// ever run, and the file handed over for diagnosis is then mostly other
// evenings. One roll-over keeps the previous session available without letting
// the current one be buried.
constexpr LONGLONG kMaxLogBytes = 64ll * 1024 * 1024;

// %LOCALAPPDATA%\Sifu\Saved\Logs is created by the game on first run, but a
// fresh install (or a Steam machine that keeps its saves elsewhere) may not
// have it yet, and CreateFile does not make folders: the log silently never
// existed on the second test PC. Make the folders, and if that still fails,
// write beside dsound.dll, which is always writable by whoever installed it.
bool BuildLogPath(char* out, DWORD size) {
    char local[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    char folder[MAX_PATH] = {};
    const char* parts[] = {"Sifu", "Saved", "Logs"};
    lstrcpynA(folder, local, MAX_PATH);
    for (const char* part : parts) {
        const size_t used = strlen(folder);
        if (used + strlen(part) + 2 >= MAX_PATH) return false;
        folder[used] = '\\';
        lstrcpynA(folder + used + 1, part, static_cast<int>(MAX_PATH - used - 1));
        if (!CreateDirectoryA(folder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    snprintf(out, size, "%s\\SifuCoop.log", folder);
    return true;
}

bool BuildFallbackLogPath(char* out, DWORD size) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&BuildFallbackLogPath), &self)) {
        return false;
    }
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';
    snprintf(out, size, "%sSifuCoop.log", path);
    return true;
}

void RollOverIfHuge(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return;
    LARGE_INTEGER size = {};
    size.HighPart = static_cast<LONG>(info.nFileSizeHigh);
    size.LowPart = info.nFileSizeLow;
    if (size.QuadPart < kMaxLogBytes) return;

    char previous[MAX_PATH] = {};
    lstrcpynA(previous, path, MAX_PATH);
    char* dot = strrchr(previous, '.');
    if (!dot) return;
    lstrcpynA(dot, ".prev.log", static_cast<int>(MAX_PATH - (dot - previous)));
    DeleteFileA(previous);
    MoveFileA(path, previous);
}

HANDLE OpenLogFile(const char* path) {
    if (!path || !path[0]) return INVALID_HANDLE_VALUE;
    RollOverIfHuge(path);
    return CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}

void Open() {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }

    char path[MAX_PATH] = {};
    bool fallback = false;
    if (BuildLogPath(path, MAX_PATH)) g_file = OpenLogFile(path);
    if (g_file == INVALID_HANDLE_VALUE && BuildFallbackLogPath(path, MAX_PATH)) {
        g_file = OpenLogFile(path);
        fallback = true;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    Write("=== SifuCoop attached %04d-%02d-%02d %02d:%02d:%02d ===", st.wYear, st.wMonth,
          st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (fallback) {
        Write("log: %%LOCALAPPDATA%%\\Sifu\\Saved\\Logs was not writable -- logging beside "
              "dsound.dll instead (%s)",
              path);
    }
}

void Close() {
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void Write(const char* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) return;

    char line[2048];

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    // "t=" is the shared stamp; a line without one was written before the
    // session could estimate the offset and cannot be aligned with the peer.
    const unsigned long shared = SharedStamp();
    int n = 0;
    if (shared != 0) {
        n = snprintf(line, sizeof(line) - 2, "[%02d:%02d:%02d.%03d %c t=%lu] ", st.wHour,
                     st.wMinute, st.wSecond, st.wMilliseconds, g_role_tag, shared);
    } else {
        n = snprintf(line, sizeof(line) - 2, "[%02d:%02d:%02d.%03d %c t=-] ", st.wHour,
                     st.wMinute, st.wSecond, st.wMilliseconds, g_role_tag);
    }
    if (n < 0) n = 0;

    va_list args;
    va_start(args, fmt);
    int formatted = vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    va_end(args);
    if (formatted < 0) return;
    n += formatted;
    if (n > static_cast<int>(sizeof(line) - 2)) n = sizeof(line) - 2;
    line[n++] = '\n';

    if (g_lock_ready) EnterCriticalSection(&g_lock);
    DWORD written = 0;
    // No FlushFileBuffers here: the OS cache keeps the line through a process
    // crash, and a device flush per line stalled the game thread under
    // verbose logging.
    WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
    if (g_lock_ready) LeaveCriticalSection(&g_lock);

    OutputDebugStringA(line);
}

void SetSharedClock(ClockFn clock, ClockValidFn valid) {
    g_shared_clock = clock;
    g_shared_clock_valid = valid;
}

void SetRoleTag(char tag) { g_role_tag = tag; }

unsigned long SharedStamp() {
    if (!g_shared_clock || !g_shared_clock_valid) return 0;
    if (!g_shared_clock_valid()) return 0;
    return g_shared_clock();
}

}
