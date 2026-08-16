



#include <windows.h>

#include <cstdio>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "dsound.dll";

    printf("loading %s\n", path);
    HMODULE module = LoadLibraryA(path);
    if (!module) {
        const DWORD error = GetLastError();
        char message[512] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       error, 0, message, sizeof(message), nullptr);
        printf("FAILED: error %lu (0x%08lX)\n  %s\n", error, error, message);
        return 1;
    }

    printf("loaded OK at %p\n", static_cast<void*>(module));
    for (WORD ordinal = 1; ordinal <= 12; ++ordinal) {
        FARPROC fn = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
        printf("  ordinal %2u -> %p%s\n", ordinal, reinterpret_cast<void*>(fn),
               fn ? "" : "   MISSING");
    }
    FreeLibrary(module);
    return 0;
}
