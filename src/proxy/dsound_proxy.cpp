












#include <windows.h>

#include "../core/log.h"

namespace {

HMODULE g_real_dsound = nullptr;

}








asm(".globl sifucoop_dsound_unavailable\n"
    "sifucoop_dsound_unavailable:\n"
    "\tmov $0x80004005, %eax\n"
    "\tret\n");

extern "C" void sifucoop_dsound_unavailable();


#define SC_FORWARD(name)                                              \
    extern "C" {                                                      \
    void* g_ptr_##name = reinterpret_cast<void*>(&sifucoop_dsound_unavailable); \
    }                                                                 \
    asm(".globl " #name "\n"                                          \
        #name ":\n"                                                   \
        "\tjmp *g_ptr_" #name "(%rip)\n");

SC_FORWARD(DirectSoundCreate)
SC_FORWARD(DirectSoundEnumerateA)
SC_FORWARD(DirectSoundEnumerateW)
SC_FORWARD(DllCanUnloadNow)
SC_FORWARD(DllGetClassObject)
SC_FORWARD(DirectSoundCaptureCreate)
SC_FORWARD(DirectSoundCaptureEnumerateA)
SC_FORWARD(DirectSoundCaptureEnumerateW)
SC_FORWARD(GetDeviceID)
SC_FORWARD(DirectSoundFullDuplexCreate)
SC_FORWARD(DirectSoundCreate8)
SC_FORWARD(DirectSoundCaptureCreate8)

#undef SC_FORWARD

namespace sifucoop::proxy {



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
            continue;
        }
        *e.slot = fn;
    }

    SC_LOG("proxy: forwarding to %s (%d/%d resolved)", path,
           static_cast<int>(sizeof(entries) / sizeof(entries[0])) - missing,
           static_cast<int>(sizeof(entries) / sizeof(entries[0])));
    return missing == 0;
}

}
