// Finding the other PC without anybody reading an IP address aloud.
//
// Nothing here touches the protocol. SifuCoop already connects to whatever
// address it is given, and a tailnet address is an ordinary address: the mod
// still sends its own authenticated UDP, and Tailscale is only the thing that
// makes that address reachable. So this file does one job -- ask the
// Tailscale client who is on the tailnet -- and the Setup tab offers the
// answer as a list to click instead of a field to type.
//
// The approach is the one mashburnn/tailcoop uses for the same game: shell
// out to the `tailscale` CLI rather than link anything, and fall back to
// reading the adapters when the CLI cannot be run.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include "tailscale.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>

#include "../core/log.h"

namespace sifucoop::net::tailscale {
namespace {

// Long enough that a slow `tailscale status` still lands, short enough that a
// hung one cannot wedge the worker for a whole session.
constexpr DWORD kCommandTimeoutMs = 5000;
constexpr DWORD kCacheLifetimeMs = 5000;

CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

Device g_devices[kMaxDevices];
int g_device_count = 0;
char g_self_ip[48] = {};
bool g_have_answer = false;
volatile LONG g_refreshing = 0;
DWORD g_last_refresh_ms = 0;

void EnsureLock() {
    if (g_lock_ready) return;
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
}

// `tailscale.exe` is not on PATH for every install, so the default location
// is tried first and PATH is the fallback rather than the other way round.
void TailscaleCommand(const wchar_t* args, wchar_t* out, int size) {
    wchar_t program_files[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramFiles", program_files, MAX_PATH)) {
        wchar_t path[MAX_PATH] = {};
        _snwprintf(path, MAX_PATH - 1, L"%s\\Tailscale\\tailscale.exe", program_files);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            _snwprintf(out, size - 1, L"\"%s\" %s", path, args);
            return;
        }
    }
    _snwprintf(out, size - 1, L"tailscale.exe %s", args);
}

// Runs the CLI with no console window and returns its output. An empty
// string means it could not be run, timed out, or said nothing -- the
// callers treat all three the same way, because all three mean "ask the
// adapters instead".
bool RunCapture(const wchar_t* args, char* out, int size) {
    if (size > 0) out[0] = '\0';

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return false;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = nullptr;

    wchar_t command[MAX_PATH + 64] = {};
    TailscaleCommand(args, command, MAX_PATH + 64);

    PROCESS_INFORMATION pi = {};
    const BOOL started = CreateProcessW(nullptr, command, nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_pipe);
    if (!started) {
        CloseHandle(read_pipe);
        return false;
    }

    int written = 0;
    bool ok = true;
    const DWORD deadline = GetTickCount() + kCommandTimeoutMs;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available > 0) {
            char chunk[1024];
            DWORD read = 0;
            if (!ReadFile(read_pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
            for (DWORD i = 0; i < read && written < size - 1; ++i) out[written++] = chunk[i];
        } else if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            // Drain whatever the child wrote just before exiting.
            for (;;) {
                if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr)) break;
                if (available == 0) break;
                char chunk[1024];
                DWORD read = 0;
                if (!ReadFile(read_pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
                for (DWORD i = 0; i < read && written < size - 1; ++i) out[written++] = chunk[i];
            }
            break;
        }
        if (static_cast<LONG>(GetTickCount() - deadline) >= 0) {
            TerminateProcess(pi.hProcess, 1);
            written = 0;
            ok = false;
            break;
        }
    }

    out[written] = '\0';
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_pipe);
    return ok && written > 0;
}

bool LooksLikeIpv4Start(char c) { return c >= '0' && c <= '9'; }

// `tailscale status` prints one device per line:
//
//   100.x.y.z   laptop   someone@  windows  -
//   100.a.b.c   desktop  someone@  windows  offline
//
// with this PC first. Anything that does not start with a digit is a header,
// a warning or a blank line.
void ParseStatus(const char* text) {
    Device parsed[kMaxDevices];
    int count = 0;
    bool first_device = true;
    int dropped = 0;

    const char* line = text;
    while (*line) {
        const char* end = line;
        while (*end && *end != '\n' && *end != '\r') ++end;
        const int length = static_cast<int>(end - line);

        if (length > 0 && LooksLikeIpv4Start(line[0])) {
            char ip[48] = {};
            char name[48] = {};
            int field = 0;
            int at = 0;
            int name_ended = length;

            for (int i = 0; i <= length; ++i) {
                const bool space = i == length || line[i] == ' ' || line[i] == '\t';
                if (!space) {
                    if (field == 0 && at < static_cast<int>(sizeof(ip)) - 1) ip[at++] = line[i];
                    if (field == 1 && at < static_cast<int>(sizeof(name)) - 1) name[at++] = line[i];
                    continue;
                }
                if (at > 0) {
                    if (field == 1) name_ended = i;
                    ++field;
                    at = 0;
                }
            }

            // Only the status column says whether a device is reachable, and
            // the scan starts past the hostname so a machine someone called
            // "offline-box" is not written off as unreachable.
            bool offline = false;
            for (int i = name_ended; i + 7 <= length; ++i) {
                if (memcmp(line + i, "offline", 7) == 0) {
                    offline = true;
                    break;
                }
            }

            if (name[0] && ip[0]) {
                if (count < kMaxDevices) {
                    Device& device = parsed[count++];
                    lstrcpynA(device.name, name, sizeof(device.name));
                    lstrcpynA(device.ip, ip, sizeof(device.ip));
                    device.self = first_device;
                    // This PC is always reachable from itself; Tailscale does
                    // not print a status word for it.
                    device.online = first_device || !offline;
                } else {
                    ++dropped;
                }
                first_device = false;
            }
        }

        line = end;
        while (*line == '\n' || *line == '\r') ++line;
    }

    if (dropped > 0) {
        SC_LOG("tailscale: %d devices past the first %d were dropped", dropped, kMaxDevices);
    }

    // Online first, so the partner who is actually sitting at a PC is at the
    // top of the list rather than below six sleeping phones. Stable within
    // each group: Tailscale's own order is alphabetical and worth keeping.
    Device ordered[kMaxDevices];
    int ordered_count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < count; ++i) {
            if (parsed[i].online != (pass == 0)) continue;
            ordered[ordered_count++] = parsed[i];
        }
    }

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < ordered_count; ++i) g_devices[i] = ordered[i];
    g_device_count = ordered_count;
    LeaveCriticalSection(&g_lock);
}

// 100.64.0.0/10 is the range Tailscale hands out, so an address in it on an
// adapter that is up is this PC's tailnet address even when the CLI is
// missing. Scoring prefers an adapter that is both up and named like
// Tailscale's over one that merely happens to carry such an address.
void ReadAdapterIp(char* out, int size) {
    if (size > 0) out[0] = '\0';

    ULONG bytes = 16 * 1024;
    unsigned char* buffer = nullptr;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; ++tries) {
        delete[] buffer;
        buffer = new unsigned char[bytes];
        result = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer), &bytes);
    }
    if (result != NO_ERROR) {
        delete[] buffer;
        return;
    }

    int best_score = -1;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer); adapter;
         adapter = adapter->Next) {
        bool named = false;
        const wchar_t* fields[2] = {adapter->FriendlyName, adapter->Description};
        for (const wchar_t* field : fields) {
            for (const wchar_t* p = field; p && *p && !named; ++p) {
                if (towlower(*p) != L't') continue;
                const wchar_t* want = L"tailscale";
                const wchar_t* q = p;
                bool match = true;
                for (int i = 0; want[i]; ++i, ++q) {
                    if (!*q || towlower(*q) != want[i]) {
                        match = false;
                        break;
                    }
                }
                named = match;
            }
        }
        const int score = (adapter->OperStatus == IfOperStatusUp ? 2 : 0) + (named ? 1 : 0);
        if (score <= best_score) continue;

        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr) continue;
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
            const std::uint32_t ip = ntohl(
                reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr.s_addr);
            if ((ip & 0xFFC00000u) != 0x64400000u) continue;
            _snprintf(out, size - 1, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255u,
                      (ip >> 8) & 255u, ip & 255u);
            best_score = score;
            break;
        }
    }

    delete[] buffer;
}

DWORD WINAPI RefreshThread(LPVOID) {
    char status[8192] = {};
    const bool have_cli = RunCapture(L"status", status, sizeof(status));
    if (have_cli) ParseStatus(status);

    char ip[48] = {};
    char raw[128] = {};
    if (RunCapture(L"ip -4", raw, sizeof(raw))) {
        int at = 0;
        for (const char* p = raw; *p && *p != '\r' && *p != '\n'; ++p) {
            if (at < static_cast<int>(sizeof(ip)) - 1) ip[at++] = *p;
        }
        ip[at] = '\0';
    }
    if (!LooksLikeIpv4Start(ip[0])) {
        // No CLI here. A game running under CrossOver or Wine against the
        // host OS's Tailscale is the case this covers: the adapter is
        // present, the command is not.
        ReadAdapterIp(ip, sizeof(ip));
    }

    EnterCriticalSection(&g_lock);
    lstrcpynA(g_self_ip, ip, sizeof(g_self_ip));
    if (!have_cli) g_device_count = 0;
    const bool first = !g_have_answer;
    g_have_answer = true;
    g_last_refresh_ms = GetTickCount();
    const int found = g_device_count;
    LeaveCriticalSection(&g_lock);

    if (first) {
        SC_LOG("tailscale: %s, this PC is %s, %d device(s) on the tailnet",
               have_cli ? "client found" : "no tailscale command here",
               ip[0] ? ip : "not on a tailnet", found);
    }

    InterlockedExchange(&g_refreshing, 0);
    return 0;
}

void Kick() {
    EnsureLock();
    if (InterlockedCompareExchange(&g_refreshing, 1, 0) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, RefreshThread, nullptr, 0, nullptr);
    if (!thread) {
        InterlockedExchange(&g_refreshing, 0);
        return;
    }
    CloseHandle(thread);
}

}

void Poll() {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    const bool stale = !g_have_answer ||
                       static_cast<LONG>(GetTickCount() - g_last_refresh_ms) >=
                           static_cast<LONG>(kCacheLifetimeMs);
    LeaveCriticalSection(&g_lock);
    if (stale) Kick();
}

void RequestRefresh() { Kick(); }

void GetSelfIp(char* out, int size) {
    if (!out || size <= 0) return;
    out[0] = '\0';
    if (!g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    lstrcpynA(out, g_self_ip, size);
    LeaveCriticalSection(&g_lock);
}

int GetDevices(Device* out, int max) {
    if (!out || max <= 0 || !g_lock_ready) return 0;
    EnterCriticalSection(&g_lock);
    int count = g_device_count;
    if (count > max) count = max;
    for (int i = 0; i < count; ++i) out[i] = g_devices[i];
    LeaveCriticalSection(&g_lock);
    return count;
}

bool HaveAnswer() {
    if (!g_lock_ready) return false;
    EnterCriticalSection(&g_lock);
    const bool answered = g_have_answer;
    LeaveCriticalSection(&g_lock);
    return answered;
}

}
