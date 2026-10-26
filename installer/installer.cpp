#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <string>
#include <vector>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

namespace {
constexpr int kPath = 1001, kDetect = 1002, kBrowse = 1003, kReplaceConfig = 1004,
              kStatus = 1006, kHeader = 1007, kSubtitle = 1008, kPathLabel = 1009,
              kNote = 1010, kStatusIcon = 1011, kSeparator = 1012;
// The Install button answers to IDOK so that Return activates it, the way it
// does in any dialog.
constexpr int kInstall = IDOK;

HINSTANCE g_instance;

std::wstring Parent(std::wstring value) {
    const size_t slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : value.substr(0, slash);
}

std::wstring Join(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    return left + (left.back() == L'\\' || left.back() == L'/' ? L"" : L"\\") + right;
}
bool Exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

std::wstring Trim(std::wstring value) {
    const auto notSpace = [](wchar_t ch) { return !iswspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        return value.substr(1, value.size() - 2);
    return value;
}

std::wstring GetEnvironment(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return L"";
    std::wstring result(length, L'\0');
    GetEnvironmentVariableW(name, result.data(), length);
    result.resize(length - 1);
    return result;
}

bool ReadText(const std::wstring& path, std::string& result) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size > 8 * 1024 * 1024) { CloseHandle(file); return false; }
    result.resize(size);
    DWORD read = 0;
    const bool ok = size == 0 || ReadFile(file, result.data(), size, &read, nullptr);
    CloseHandle(file);
    return ok && read == size;
}

std::wstring UnescapePath(const std::string& text) {
    std::wstring result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char next = text[++i];
            result.push_back(next == '\\' ? L'\\' : static_cast<unsigned char>(next));
        } else {
            result.push_back(static_cast<unsigned char>(text[i]));
        }
    }
    return result;
}

std::vector<std::wstring> ValuesAfterKey(const std::string& text, const std::string& key) {
    std::vector<std::wstring> result;
    size_t from = 0;
    while ((from = text.find(key, from)) != std::string::npos) {
        size_t quote = text.find('"', from + key.size());
        if (quote == std::string::npos) break;
        ++quote;
        std::string value;
        bool escaped = false;
        for (; quote < text.size(); ++quote) {
            const char ch = text[quote];
            if (ch == '"' && !escaped) break;
            value += ch;
            escaped = ch == '\\' && !escaped;
            if (ch != '\\') escaped = false;
        }
        if (!value.empty()) result.push_back(UnescapePath(value));
        from = quote + 1;
    }
    return result;
}

bool IsBinDirectory(const std::wstring& path) {
    return Exists(Join(path, L"Sifu-Win64-Shipping.exe"));
}

std::wstring ToBinDirectory(std::wstring supplied) {
    supplied = Trim(supplied);
    if (IsBinDirectory(supplied)) return supplied;
    const std::wstring bin = Join(supplied, L"Binaries\\Win64");
    if (IsBinDirectory(bin)) return bin;


    const std::wstring nestedBin = Join(supplied, L"Sifu\\Binaries\\Win64");
    return IsBinDirectory(nestedBin) ? nestedBin : L"";
}

void AddCandidate(std::vector<std::wstring>& values, const std::wstring& supplied) {
    const std::wstring bin = ToBinDirectory(supplied);
    if (bin.empty()) return;
    if (std::find(values.begin(), values.end(), bin) == values.end()) values.push_back(bin);
}

std::wstring ReadRegistryPath(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    wchar_t data[MAX_PATH * 4] = {};
    DWORD size = sizeof(data);
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, data, &size) != ERROR_SUCCESS)
        return L"";
    return data;
}

std::vector<std::wstring> DetectInstalls() {
    std::vector<std::wstring> found;
    const std::wstring programData = GetEnvironment(L"ProgramData");
    std::string epic;
    if (!programData.empty() &&
        ReadText(Join(programData, L"Epic\\UnrealEngineLauncher\\LauncherInstalled.dat"), epic)) {
        for (const auto& value : ValuesAfterKey(epic, "\"InstallLocation\"")) AddCandidate(found, value);
    }
    AddCandidate(found, Join(GetEnvironment(L"ProgramFiles"), L"Epic Games\\Sifu"));

    std::vector<std::wstring> steamRoots;
    steamRoots.push_back(ReadRegistryPath(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", L"SteamPath"));
    steamRoots.push_back(ReadRegistryPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"));
    steamRoots.push_back(Join(GetEnvironment(L"ProgramFiles(x86)"), L"Steam"));
    for (const auto& steam : steamRoots) {
        if (steam.empty()) continue;
        AddCandidate(found, Join(steam, L"steamapps\\common\\Sifu"));
        std::string libraries;
        if (!ReadText(Join(steam, L"steamapps\\libraryfolders.vdf"), libraries)) continue;
        for (const auto& library : ValuesAfterKey(libraries, "\"path\""))
            AddCandidate(found, Join(library, L"steamapps\\common\\Sifu"));
    }
    return found;
}

bool IsSifuRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"Sifu-Win64-Shipping.exe") == 0 ||
                _wcsicmp(entry.szExeFile, L"Sifu.exe") == 0) { running = true; break; }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

std::wstring ModuleDirectory() {
    DWORD size = MAX_PATH;
    for (;;) {
        std::wstring path(size, L'\0');
        const DWORD used = GetModuleFileNameW(nullptr, path.data(), size);
        if (!used) return L"";
        if (used < size - 1) { path.resize(used); return Parent(path); }
        size *= 2;
    }
}

bool CopyToStage(const std::wstring& source, const std::wstring& target, std::wstring& error) {
    const std::wstring stage = target + L".sifucoop.new";
    DeleteFileW(stage.c_str());
    if (CopyFileW(source.c_str(), stage.c_str(), FALSE)) return true;
    error = L"Cannot copy " + source + L"\n\n" + std::to_wstring(GetLastError());
    return false;
}

std::wstring Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t value[48]{};
    swprintf_s(value, L"%04u-%02u-%02u_%02u-%02u-%02u",
              time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return value;
}

bool MakeDirectory(const std::wstring& path, std::wstring& error) {
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS) return true;
    SetLastError(static_cast<DWORD>(result));
    error = L"Cannot create backup folder:\n" + path + L"\n\n" + std::to_wstring(result);
    return false;
}

bool BackupIfPresent(const std::wstring& file, const std::wstring& backup, std::wstring& error) {
    if (!Exists(file)) return true;
    if (CopyFileW(file.c_str(), backup.c_str(), TRUE)) return true;
    error = L"Cannot back up:\n" + file + L"\n\n" + std::to_wstring(GetLastError());
    return false;
}

struct InstallResult { bool ok = false; bool needsElevation = false; std::wstring message; };

InstallResult Install(const std::wstring& supplied, bool replaceConfig) {
    InstallResult result;
    const std::wstring target = ToBinDirectory(supplied);
    if (target.empty()) { result.message = L"Choose the Sifu folder, or its Binaries\\Win64 folder."; return result; }
    if (IsSifuRunning()) { result.message = L"Close Sifu before installing the mod."; return result; }

    const std::wstring sourceRoot = ModuleDirectory();
    const std::wstring sourceDll = Join(sourceRoot, L"dsound.dll");
    const std::wstring sourceIni = Join(sourceRoot, L"SifuCoop.ini");
    if (!Exists(sourceDll) || !Exists(sourceIni)) {
        result.message = L"Run SifuCoopInstaller.exe from the extracted release folder, beside dsound.dll and SifuCoop.ini.";
        return result;
    }

    const std::wstring targetDll = Join(target, L"dsound.dll");
    const std::wstring targetIni = Join(target, L"SifuCoop.ini");
    const bool installIni = replaceConfig || !Exists(targetIni);
    const bool needBackup = Exists(targetDll) || (installIni && Exists(targetIni));
    std::wstring backupRoot, error;
    if (needBackup) {
        backupRoot = Join(Join(target, L"SifuCoop-backup"), Timestamp());
        if (!MakeDirectory(backupRoot, error)) {
            result.needsElevation = GetLastError() == ERROR_ACCESS_DENIED;
            result.message = error; return result;
        }
        if (!BackupIfPresent(targetDll, Join(backupRoot, L"dsound.dll"), error) ||
            (installIni && !BackupIfPresent(targetIni, Join(backupRoot, L"SifuCoop.ini"), error))) {
            result.needsElevation = GetLastError() == ERROR_ACCESS_DENIED;
            result.message = error; return result;
        }
    }

    if (!CopyToStage(sourceDll, targetDll, error) ||
        (installIni && !CopyToStage(sourceIni, targetIni, error))) {
        result.needsElevation = GetLastError() == ERROR_ACCESS_DENIED;
        result.message = error; return result;
    }
    if (installIni && !MoveFileExW((targetIni + L".sifucoop.new").c_str(), targetIni.c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.needsElevation = GetLastError() == ERROR_ACCESS_DENIED;
        result.message = L"Cannot install SifuCoop.ini.\n\n" + std::to_wstring(GetLastError()); return result;
    }
    if (!MoveFileExW((targetDll + L".sifucoop.new").c_str(), targetDll.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.needsElevation = GetLastError() == ERROR_ACCESS_DENIED;
        result.message = L"Cannot install dsound.dll.\n\n" + std::to_wstring(GetLastError()); return result;
    }
    result.ok = true;
    result.message = L"SifuCoop installed successfully.\n\nTarget:\n" + target +
        (backupRoot.empty() ? L"\n\nNo existing proxy files needed a backup."
                            : L"\n\nBackup:\n" + backupRoot);
    return result;
}

std::wstring ControlText(HWND control) {
    const int size = GetWindowTextLengthW(control);
    std::wstring result(size + 1, L'\0');
    GetWindowTextW(control, result.data(), size + 1);
    result.resize(size);
    return result;
}

// ---------------------------------------------------------------------------
// Presentation
//
// Everything below is layout. It is written against DPI-independent pixels and
// scaled through Scale(), because a hard-coded pixel layout is wrong on every
// display that is not 96 DPI -- which by now is most of them.
// ---------------------------------------------------------------------------

UINT g_dpi = 96;
HFONT g_font = nullptr;        // Segoe UI 9pt: the shell's own UI font
HFONT g_headerFont = nullptr;  // Segoe UI 14pt semibold

int Scale(int value) { return MulDiv(value, static_cast<int>(g_dpi), 96); }

UINT DpiForWindow(HWND window) {
    // Win10 1607+. On anything older the process is at best system-DPI aware
    // and the desktop value is the right answer anyway.
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn) {
        const UINT dpi = fn(window);
        if (dpi) return dpi;
    }
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

bool MessageFontForDpi(UINT dpi, LOGFONTW* out) {
    using SpiForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static auto fn = reinterpret_cast<SpiForDpiFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (fn && fn(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        *out = metrics.lfMessageFont;
        return true;
    }
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return false;
    }
    // The unscaled call returns the font for the primary display, so it has to
    // be scaled by hand for any other DPI.
    *out = metrics.lfMessageFont;
    out->lfHeight = MulDiv(out->lfHeight, static_cast<int>(dpi), 96);
    return true;
}

void BuildFonts(UINT dpi) {
    if (g_font) DeleteObject(g_font);
    if (g_headerFont) DeleteObject(g_headerFont);
    g_font = nullptr;
    g_headerFont = nullptr;

    LOGFONTW body{};
    if (!MessageFontForDpi(dpi, &body)) {
        lstrcpynW(body.lfFaceName, L"Segoe UI", LF_FACESIZE);
        body.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
    }
    g_font = CreateFontIndirectW(&body);

    LOGFONTW header = body;
    header.lfHeight = -MulDiv(14, static_cast<int>(dpi), 72);
    header.lfWeight = FW_SEMIBOLD;
    g_headerFont = CreateFontIndirectW(&header);
}

void ApplyFonts(HWND window) {
    const int ids[] = {kPath, kDetect, kBrowse, kReplaceConfig, kInstall,
                       kStatus, kSubtitle, kPathLabel, kNote};
    for (int id : ids) {
        HWND control = GetDlgItem(window, id);
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
    HWND header = GetDlgItem(window, kHeader);
    if (header) {
        SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(g_headerFont), TRUE);
    }
}

void ApplyStatusIcon(HWND window, const wchar_t* which) {
    HWND control = GetDlgItem(window, kStatusIcon);
    if (!control) return;
    const int size = Scale(16);
    HICON icon = static_cast<HICON>(
        LoadImageW(nullptr, which, IMAGE_ICON, size, size, LR_SHARED));
    SendMessageW(control, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
    ShowWindow(control, icon ? SW_SHOW : SW_HIDE);
}

void Layout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;

    const int margin = Scale(16);
    const int gap = Scale(8);
    const int buttonWidth = Scale(96);
    const int buttonHeight = Scale(27);
    const int footer = Scale(20) + buttonHeight + Scale(16);
    const int content = width - margin * 2;

    HDWP defer = BeginDeferWindowPos(12);
    const auto place = [&](int id, int x, int y, int cx, int cy) {
        HWND control = GetDlgItem(window, id);
        if (control) {
            defer = DeferWindowPos(defer, control, nullptr, x, y, cx, cy,
                                   SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    int y = margin;
    place(kHeader, margin, y, content, Scale(26));
    y += Scale(26) + Scale(2);
    place(kSubtitle, margin, y, content, Scale(18));
    y += Scale(18) + Scale(18);

    place(kPathLabel, margin, y, content, Scale(17));
    y += Scale(17) + Scale(3);
    const int browseWidth = Scale(92);
    place(kPath, margin, y + Scale(2), content - browseWidth - gap, Scale(23));
    place(kBrowse, width - margin - browseWidth, y, browseWidth, buttonHeight);
    y += buttonHeight + Scale(14);

    place(kReplaceConfig, margin, y, content, Scale(20));
    y += Scale(20) + Scale(4);
    place(kNote, margin + Scale(20), y, content - Scale(20), Scale(32));
    y += Scale(32) + Scale(10);

    const int statusTop = y;
    const int statusBottom = height - footer;
    place(kStatusIcon, margin, statusTop + Scale(1), Scale(16), Scale(16));
    place(kStatus, margin + Scale(16) + gap, statusTop,
          content - Scale(16) - gap, statusBottom > statusTop ? statusBottom - statusTop : 0);

    place(kSeparator, 0, height - footer, width, Scale(1));
    const int buttonTop = height - footer + Scale(20);
    place(kInstall, width - margin - buttonWidth, buttonTop, buttonWidth, buttonHeight);
    place(kDetect, width - margin - buttonWidth * 2 - gap, buttonTop, buttonWidth, buttonHeight);

    EndDeferWindowPos(defer);
    InvalidateRect(window, nullptr, TRUE);
}

void SetStatus(HWND window, const std::wstring& text, const wchar_t* icon = IDI_INFORMATION) {
    SetWindowTextW(GetDlgItem(window, kStatus), text.c_str());
    ApplyStatusIcon(window, icon);
}

void DetectInto(HWND window, bool announce) {
    const auto found = DetectInstalls();
    if (found.empty()) {
        SetStatus(window,
                  L"No Sifu installation was found automatically. Use Browse to select "
                  L"your Sifu folder.",
                  IDI_WARNING);
        return;
    }
    SetWindowTextW(GetDlgItem(window, kPath), found.front().c_str());
    SetStatus(window, std::wstring(L"Detected: ") + found.front() +
        (found.size() > 1 ? L"\n\nMore than one install was found; choose another folder with Browse if needed." : L""));
    if (announce) MessageBoxW(window, L"An installation was detected and selected.", L"SifuCoop installer", MB_OK | MB_ICONINFORMATION);
}

// Typing a path is not rare -- people paste one -- and saying whether it
// resolved only after Install is pressed is a needless round trip.
void ValidatePath(HWND window) {
    const std::wstring supplied = Trim(ControlText(GetDlgItem(window, kPath)));
    if (supplied.empty()) {
        SetStatus(window, L"Choose your Sifu folder, or press Auto-detect.");
        return;
    }
    const std::wstring resolved = ToBinDirectory(supplied);
    if (resolved.empty()) {
        SetStatus(window,
                  L"No Sifu-Win64-Shipping.exe under this folder. Pick the Sifu folder "
                  L"itself, or its Binaries\\Win64 folder.",
                  IDI_WARNING);
        return;
    }
    SetStatus(window, L"Ready to install into:\n" + resolved);
}

void Browse(HWND window) {
    BROWSEINFOW browse{};
    browse.hwndOwner = window;
    browse.lpszTitle = L"Select your Sifu game folder (the folder containing Binaries)";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return;
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(item, path)) {
        SetWindowTextW(GetDlgItem(window, kPath), path);
        ValidatePath(window);
    }
    CoTaskMemFree(item);
}

void RunInstall(HWND window, bool commandLine) {
    const bool replaceConfig = IsDlgButtonChecked(window, kReplaceConfig) == BST_CHECKED;
    const std::wstring supplied = ControlText(GetDlgItem(window, kPath));

    InstallResult result = Install(supplied, replaceConfig);
    if (!result.ok && result.needsElevation && !commandLine) {
        const std::wstring executable = Join(ModuleDirectory(), L"SifuCoopInstaller.exe");
        std::wstring params = L"--install \"" + supplied + L"\"";
        if (replaceConfig) params += L" --replace-config";
        const intptr_t launch = reinterpret_cast<intptr_t>(
            ShellExecuteW(window, L"runas", executable.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL));
        if (launch > 32) {
            SetStatus(window,
                      L"Administrator approval requested. Complete the elevated installer "
                      L"window.");
            return;
        }
    }
    if (result.ok) {
        MessageBoxW(window, result.message.c_str(), L"SifuCoop installer", MB_OK | MB_ICONINFORMATION);
        SetStatus(window, L"Install complete. Configure networking in Sifu (F1) or "
                          L"SifuCoopLauncher.exe.");
    } else {
        MessageBoxW(window, result.message.c_str(), L"SifuCoop installer", MB_OK | MB_ICONERROR);
        SetStatus(window, result.message, IDI_ERROR);
    }
}

void CreateControls(HWND window) {
    const auto make = [&](const wchar_t* klass, const wchar_t* text, DWORD style, int id,
                          DWORD exStyle = 0) {
        return CreateWindowExW(exStyle, klass, text, style | WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                               window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               g_instance, nullptr);
    };

    make(L"STATIC", L"Install SifuCoop", SS_LEFT | SS_NOPREFIX, kHeader);
    make(L"STATIC", L"Choose your Sifu installation.", SS_LEFT | SS_NOPREFIX, kSubtitle);

    make(L"STATIC", L"Sifu folder", SS_LEFT, kPathLabel);
    HWND path = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kPath, WS_EX_CLIENTEDGE);
    SendMessageW(path, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"C:\\...\\Sifu\\Binaries\\Win64"));
    make(L"BUTTON", L"Browse...", WS_TABSTOP, kBrowse);

    make(L"BUTTON", L"Replace SifuCoop.ini", WS_TABSTOP | BS_AUTOCHECKBOX, kReplaceConfig);
    make(L"STATIC",
         L"An existing dsound.dll is always backed up first. An existing SifuCoop.ini is "
         L"kept unless this is ticked, and is backed up either way.",
         SS_LEFT | SS_NOPREFIX, kNote);

    make(L"STATIC", L"", SS_ICON | SS_REALSIZECONTROL, kStatusIcon);
    make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, kStatus);

    make(L"STATIC", L"", SS_ETCHEDHORZ, kSeparator);
    make(L"BUTTON", L"Auto-detect", WS_TABSTOP, kDetect);
    make(L"BUTTON", L"Install", WS_TABSTOP | BS_DEFPUSHBUTTON, kInstall);
}

// lparam is NAMED and forwarded. It was discarded, and DefWindowProcW was
// called with a hardcoded 0 -- which breaks WM_NCCREATE, the message Windows
// sends BEFORE WM_CREATE carrying the CREATESTRUCT in lparam. DefWindowProc
// needs that pointer; given 0 it returns FALSE, and a FALSE from WM_NCCREATE
// makes CreateWindowEx abandon the window and return null. That is the whole of
// "Could not create the installer window": the window procedure was rejecting
// its own window before any child control was ever reached.
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_dpi = DpiForWindow(window);
        BuildFonts(g_dpi);
        CreateControls(window);
        ApplyFonts(window);
        Layout(window);
        DetectInto(window, false);
        return 0;

    case WM_SIZE:
        Layout(window);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        // Window size, not client: the default client height of 320 plus the
        // frame. Smaller than this and the status line loses its second row.
        info->ptMinTrackSize.x = Scale(470);
        info->ptMinTrackSize.y = Scale(360);
        return 0;
    }

    case WM_DPICHANGED: {
        // Dragged to a display with a different scale. The suggested rectangle
        // is the same window at the new scale; everything inside it has to be
        // rebuilt at that scale too.
        g_dpi = HIWORD(wparam);
        BuildFonts(g_dpi);
        ApplyFonts(window);
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(window);
        return 0;
    }

    // Static text draws on the dialog background rather than white, and the
    // secondary lines are drawn in the system's grey so the header and the
    // status line carry the emphasis.
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
        SetBkMode(dc, TRANSPARENT);
        if (id == kSubtitle || id == kNote) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        } else {
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        }
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kDetect: DetectInto(window, true); return 0;
        case kBrowse: Browse(window); return 0;
        case kInstall: RunInstall(window, false); return 0;
        case IDCANCEL: DestroyWindow(window); return 0;
        case kPath:
            if (HIWORD(wparam) == EN_CHANGE) ValidatePath(window);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        if (g_headerFont) { DeleteObject(g_headerFont); g_headerFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_instance = instance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && _wcsicmp(argv[1], L"--install") == 0) {
        bool replace = false;
        for (int i = 3; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--replace-config") == 0) replace = true;
        }
        InstallResult result = Install(argv[2], replace);
        MessageBoxW(nullptr, result.message.c_str(), L"SifuCoop installer",
                    MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONERROR));
        LocalFree(argv);
        CoUninitialize();
        return result.ok ? 0 : 1;
    }
    if (argv) LocalFree(argv);

    const wchar_t* klass = L"SifuCoopInstallerWindow";
    WNDCLASSW wc{};
    wc.hInstance = instance; wc.lpszClassName = klass; wc.lpfnWndProc = WindowProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    const ATOM registered = RegisterClassW(&wc);
    if (!registered && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Could not register the installer window class.",
                    L"SifuCoop installer", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    // The window is sized in DIPs against the DPI it will open at, so it is the
    // same apparent size on every display.
    const UINT dpi = DpiForWindow(nullptr);
    // Sized to the layout above: the content ends around 257 DIP and the
    // footer band is 63, so anything taller is dead space above the buttons.
    RECT wanted{0, 0, MulDiv(520, static_cast<int>(dpi), 96),
                MulDiv(320, static_cast<int>(dpi), 96)};
    AdjustWindowRectEx(&wanted, WS_OVERLAPPEDWINDOW, FALSE, 0);

    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, klass, L"SifuCoop installer",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        wanted.right - wanted.left, wanted.bottom - wanted.top, nullptr, nullptr, instance, nullptr);
    if (!window) {
        MessageBoxW(nullptr, L"Could not create the installer window.",
                    L"SifuCoop installer", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }


    ShowWindow(window, show ? show : SW_SHOWNORMAL);
    UpdateWindow(window);
    MSG msg;
    // IsDialogMessage is what makes Tab, the arrow keys, Return and Escape work
    // in a plain window with child controls. Without it the WS_TABSTOP flags on
    // every control meant nothing and the keyboard did not move focus at all.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
