// dsound.dll proxy.
//
// Sifu-Win64-Shipping.exe statically imports DSOUND.dll by ORDINAL (1, 3, 6, 8,
// 11, 12), and no dsound.dll ships in the game folder -- so dropping ours in
// Binaries\Win64 injects us before main() while modifying zero game files.
// That last property matters: Epic's "verify files" would restore a renamed
// game DLL and silently break the mod, but it leaves an unknown extra file be.
//
// Every export is a bare `jmp` through a resolved pointer. A naked jump is
// signature-agnostic -- it touches no registers and no stack, so the real
// function sees the arguments exactly as the caller left them. GCC does not
// support __attribute__((naked)) on x86-64, hence the global asm blocks.

#include <windows.h>

#include "../core/log.h"

namespace {

HMODULE g_real_dsound = nullptr;

}  // namespace

// Fallback for every export, so a stub is never null.
//
// The trampolines are bare `jmp`s through a pointer. If resolving the real
// dsound ever failed those pointers would be null and the first call would jump
// to address 0 -- an immediate crash that looks to the user like "the game
// cannot start because of dsound". Pointing them at a stub that returns E_FAIL
// instead means the game gets an ordinary DirectSound error it can handle.
asm(".globl sifucoop_dsound_unavailable\n"
    "sifucoop_dsound_unavailable:\n"
    "\tmov $0x80004005, %eax\n"  // E_FAIL
    "\tret\n");

extern "C" void sifucoop_dsound_unavailable();

// One forwarding pointer per export, plus the trampoline that jumps through it.
#define SC_FORWARD(name)                                              \
    extern "C" {                                                      \
    void* g_ptr_##name = reinterpret_cast<void*>(&sifucoop_dsound_unavailable); \
    }                                                                 \
    asm(".globl " #name "\n"                                          \
        #name ":\n"                                                   \
        "\tjmp *g_ptr_" #name "(%rip)\n");

SC_FORWARD(DirectSoundCreate)            // @1  -- imported
SC_FORWARD(DirectSoundEnumerateA)        // @2
SC_FORWARD(DirectSoundEnumerateW)        // @3  -- imported
SC_FORWARD(DllCanUnloadNow)              // @4
SC_FORWARD(DllGetClassObject)            // @5
SC_FORWARD(DirectSoundCaptureCreate)     // @6  -- imported
SC_FORWARD(DirectSoundCaptureEnumerateA) // @7
SC_FORWARD(DirectSoundCaptureEnumerateW) // @8  -- imported
SC_FORWARD(GetDeviceID)                  // @9
SC_FORWARD(DirectSoundFullDuplexCreate)  // @10
SC_FORWARD(DirectSoundCreate8)           // @11 -- imported
SC_FORWARD(DirectSoundCaptureCreate8)    // @12 -- imported

#undef SC_FORWARD

namespace sifucoop::proxy {

// Ordinals verified against C:\Windows\System32\dsound.dll rather than assumed:
// the widely-quoted table has 9-12 in the wrong order.
bool Init() {
    char path[MAX_PATH] = {};
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return false;
    lstrcatA(path, "\\dsound.dll");

    g_real_dsound = LoadLibraryA(path);
    if (!g_real_dsound) {
        SC_LOG("proxy: FAILED to load %s (err %lu)", path, GetLastError());
        return false;
    }

    struct Entry {
        void** slot;
        WORD ordinal;
        const char* name;
    };
    const Entry entries[] = {
        {&g_ptr_DirectSoundCreate, 1, "DirectSoundCreate"},
        {&g_ptr_DirectSoundEnumerateA, 2, "DirectSoundEnumerateA"},
        {&g_ptr_DirectSoundEnumerateW, 3, "DirectSoundEnumerateW"},
        {&g_ptr_DllCanUnloadNow, 4, "DllCanUnloadNow"},
        {&g_ptr_DllGetClassObject, 5, "DllGetClassObject"},
        {&g_ptr_DirectSoundCaptureCreate, 6, "DirectSoundCaptureCreate"},
        {&g_ptr_DirectSoundCaptureEnumerateA, 7, "DirectSoundCaptureEnumerateA"},
        {&g_ptr_DirectSoundCaptureEnumerateW, 8, "DirectSoundCaptureEnumerateW"},
        {&g_ptr_GetDeviceID, 9, "GetDeviceID"},
        {&g_ptr_DirectSoundFullDuplexCreate, 10, "DirectSoundFullDuplexCreate"},
        {&g_ptr_DirectSoundCreate8, 11, "DirectSoundCreate8"},
        {&g_ptr_DirectSoundCaptureCreate8, 12, "DirectSoundCaptureCreate8"},
    };

    int missing = 0;
    for (const Entry& e : entries) {
        void* fn = reinterpret_cast<void*>(
            GetProcAddress(g_real_dsound, MAKEINTRESOURCEA(e.ordinal)));
        if (!fn) {
            SC_LOG("proxy: missing ordinal %u (%s)", e.ordinal, e.name);
            ++missing;
            continue;  // keep the E_FAIL stub rather than storing null
        }
        *e.slot = fn;
    }

    SC_LOG("proxy: forwarding to %s (%d/%d resolved)", path,
           static_cast<int>(sizeof(entries) / sizeof(entries[0])) - missing,
           static_cast<int>(sizeof(entries) / sizeof(entries[0])));
    return missing == 0;
}

}  // namespace sifucoop::proxy
