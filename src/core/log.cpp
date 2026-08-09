#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace sifucoop::log {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

// %LOCALAPPDATA%\Sifu\Saved\Logs\SifuCoop.log
void BuildLogPath(char* out, DWORD size) {
    char local[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        lstrcpynA(out, "SifuCoop.log", size);
        return;
    }
    snprintf(out, size, "%s\\Sifu\\Saved\\Logs\\SifuCoop.log", local);
}

}  // namespace

void Open() {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }

    char path[MAX_PATH] = {};
    BuildLogPath(path, MAX_PATH);

    g_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    Write("=== SifuCoop attached %04d-%02d-%02d %02d:%02d:%02d ===", st.wYear, st.wMonth,
          st.wDay, st.wHour, st.wMinute, st.wSecond);
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

    // stamps measured in ms
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    int n = snprintf(line, sizeof(line) - 2, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds);
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
    WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
    FlushFileBuffers(g_file);
    if (g_lock_ready) LeaveCriticalSection(&g_lock);

    OutputDebugStringA(line);
}

}  // namespace sifucoop::log
