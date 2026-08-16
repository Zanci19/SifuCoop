#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {
constexpr int kPath = 1001, kDetect = 1002, kBrowse = 1003, kReplaceConfig = 1004,
              kInstall = 1005, kStatus = 1006, kZeroTierNetwork = 1007,
              kZeroTierSetup = 1008, kZeroTierCentral = 1009;
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

bool IsNetworkId(const std::wstring& value) {
    return value.size() == 16 && std::all_of(value.begin(), value.end(),
        [](wchar_t ch) { return iswxdigit(ch) != 0; });
}

std::wstring FindZeroTierCli() {
    std::vector<std::wstring> roots = {
        GetEnvironment(L"ProgramFiles(x86)"), GetEnvironment(L"ProgramW6432"),
        GetEnvironment(L"ProgramFiles")};
    for (const auto& root : roots) {
        if (root.empty()) continue;
        const std::wstring folder = Join(root, L"ZeroTier\\One");
        for (const wchar_t* name : {L"zerotier-cli.bat", L"zerotier-cli.exe"}) {
            const std::wstring candidate = Join(folder, name);
            if (Exists(candidate)) return candidate;
        }
    }
    return L"";
}

bool RunElevatedAndWait(HWND owner, const std::wstring& executable,
                        const std::wstring& parameters, std::wstring& error) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        error = L"Administrator approval was cancelled or could not be started.\n\nWindows error " +
                std::to_wstring(GetLastError());
        return false;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    if (code == 0 || code == 3010) return true;
    error = L"The elevated ZeroTier command failed.\n\nExit code " + std::to_wstring(code);
    return false;
}

bool SetupZeroTier(HWND owner, const std::wstring& requestedNetwork, std::wstring& message) {
    const std::wstring network = Trim(requestedNetwork);
    if (!IsNetworkId(network)) {
        message = L"Enter the 16-character hexadecimal ZeroTier network ID from ZeroTier Central.";
        return false;
    }
    std::wstring cli = FindZeroTierCli();
    if (cli.empty()) {
        wchar_t tempDirectory[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, tempDirectory)) {
            message = L"Cannot create a temporary download location.\n\nWindows error " + std::to_wstring(GetLastError());
            return false;
        }
        wchar_t temporaryFile[MAX_PATH]{};
        if (!GetTempFileNameW(tempDirectory, L"SCZ", 0, temporaryFile)) {
            message = L"Cannot create a temporary MSI name.\n\nWindows error " + std::to_wstring(GetLastError());
            return false;
        }
        std::wstring msi = temporaryFile;
        DeleteFileW(msi.c_str());
        msi += L".msi";
        const HRESULT downloaded = URLDownloadToFileW(nullptr,
            L"https://download.zerotier.com/dist/ZeroTier%20One.msi", msi.c_str(), 0, nullptr);
        if (FAILED(downloaded)) {
            message = L"Could not download the official ZeroTier installer.\n\nHRESULT " + std::to_wstring(static_cast<long>(downloaded));
            return false;
        }
        std::wstring error;
        const bool installed = RunElevatedAndWait(owner, L"msiexec.exe",
            L"/i \"" + msi + L"\" /passive /norestart", error);
        DeleteFileW(msi.c_str());
        if (!installed) { message = error; return false; }
        cli = FindZeroTierCli();
        if (cli.empty()) {
            message = L"ZeroTier installed, but its command-line client was not found yet. Restart Windows, then run this setup again to join the network.";
            return false;
        }
    }
    std::wstring error;
    if (!RunElevatedAndWait(owner, cli, L"join " + network, error)) {
        message = error;
        return false;
    }
    message = L"ZeroTier is installed and this device requested to join network " + network +
              L".\n\nOpen ZeroTier Central and authorize this device. Repeat on the other player’s PC, then put the host’s ZeroTier managed IP in the SifuCoop client host field.";
    return true;
}

void OpenZeroTierCentral(HWND owner) {
    ShellExecuteW(owner, L"open", L"https://central.zerotier.com/", nullptr, nullptr, SW_SHOWNORMAL);
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

void SetStatus(HWND window, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(window, kStatus), text.c_str());
}

void DetectInto(HWND window, bool announce) {
    const auto found = DetectInstalls();
    if (found.empty()) {
        SetStatus(window, L"No Sifu installation was found automatically. Use Browse to select your Sifu folder.");
        return;
    }
    SetWindowTextW(GetDlgItem(window, kPath), found.front().c_str());
    SetStatus(window, std::wstring(L"Detected: ") + found.front() +
        (found.size() > 1 ? L"\n\nMore than one install was found; choose another folder with Browse if needed." : L""));
    if (announce) MessageBoxW(window, L"An installation was detected and selected.", L"SifuCoop installer", MB_OK | MB_ICONINFORMATION);
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
        SetStatus(window, L"Selected: " + std::wstring(path));
    }
    CoTaskMemFree(item);
}

void RunInstall(HWND window, bool commandLine) {
    const bool replaceConfig = IsDlgButtonChecked(window, kReplaceConfig) == BST_CHECKED;
    const bool setupZeroTier = IsDlgButtonChecked(window, kZeroTierSetup) == BST_CHECKED;
    const std::wstring supplied = ControlText(GetDlgItem(window, kPath));
    const std::wstring network = ControlText(GetDlgItem(window, kZeroTierNetwork));
    if (setupZeroTier && !IsNetworkId(Trim(network))) {
        const std::wstring error = L"Enter a 16-character hexadecimal ZeroTier network ID, or untick ZeroTier setup.";
        MessageBoxW(window, error.c_str(), L"SifuCoop installer", MB_OK | MB_ICONERROR);
        SetStatus(window, error);
        return;
    }

    InstallResult result = Install(supplied, replaceConfig);
    if (!result.ok && result.needsElevation && !commandLine) {
        const std::wstring executable = Join(ModuleDirectory(), L"SifuCoopInstaller.exe");
        std::wstring params = L"--install \"" + supplied + L"\"";
        if (replaceConfig) params += L" --replace-config";
        if (setupZeroTier) params += L" --zerotier \"" + Trim(network) + L"\"";
        const intptr_t launch = reinterpret_cast<intptr_t>(
            ShellExecuteW(window, L"runas", executable.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL));
        if (launch > 32) {
            SetStatus(window, L"Administrator approval requested. Complete the elevated installer window.");
            return;
        }
    }
    if (result.ok && setupZeroTier) {
        std::wstring zeroTier;
        if (!SetupZeroTier(window, network, zeroTier)) {
            result.message += L"\n\nSifuCoop installed, but ZeroTier setup did not finish:\n" + zeroTier;
            MessageBoxW(window, result.message.c_str(), L"SifuCoop installer", MB_OK | MB_ICONWARNING);
            SetStatus(window, zeroTier);
            return;
        }
        result.message += L"\n\n" + zeroTier;
    }
    if (result.ok) {
        MessageBoxW(window, result.message.c_str(), L"SifuCoop installer", MB_OK | MB_ICONINFORMATION);
        SetStatus(window, setupZeroTier ? L"SifuCoop and ZeroTier setup complete." : L"Install complete.");
    } else {
        MessageBoxW(window, result.message.c_str(), L"SifuCoop installer", MB_OK | MB_ICONERROR);
        SetStatus(window, result.message);
    }
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
        CreateWindowW(L"STATIC", L"SifuCoop installer", WS_CHILD | WS_VISIBLE,
                      20, 18, 480, 25, window, nullptr, g_instance, nullptr);
        CreateWindowW(L"STATIC", L"Sifu folder or Binaries\\Win64 folder:", WS_CHILD | WS_VISIBLE,
                      20, 48, 430, 20, window, nullptr, g_instance, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        20, 69, 470, 24, window, reinterpret_cast<HMENU>(kPath), g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Auto-detect", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      20, 103, 120, 28, window, reinterpret_cast<HMENU>(kDetect), g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      150, 103, 100, 28, window, reinterpret_cast<HMENU>(kBrowse), g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Replace SifuCoop.ini (back it up first)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                      20, 140, 310, 22, window, reinterpret_cast<HMENU>(kReplaceConfig), g_instance, nullptr);
        CreateWindowW(L"STATIC", L"Existing dsound.dll is always backed up. Existing SifuCoop.ini is kept unless checked above.",
                      WS_CHILD | WS_VISIBLE, 20, 164, 500, 28, window, nullptr, g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Also install ZeroTier and join this network", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                      20, 202, 310, 22, window, reinterpret_cast<HMENU>(kZeroTierSetup), g_instance, nullptr);
        CreateWindowW(L"STATIC", L"ZeroTier network ID (16 hex characters):", WS_CHILD | WS_VISIBLE,
                      40, 228, 300, 20, window, nullptr, g_instance, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        40, 249, 280, 24, window, reinterpret_cast<HMENU>(kZeroTierNetwork), g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Open ZeroTier Central (login / authorize)", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      330, 246, 160, 28, window, reinterpret_cast<HMENU>(kZeroTierCentral), g_instance, nullptr);
        CreateWindowW(L"STATIC", L"After both PCs join, authorize both devices in ZeroTier Central. The client then uses the host’s ZeroTier managed IP.",
                      WS_CHILD | WS_VISIBLE, 20, 282, 480, 34, window, nullptr, g_instance, nullptr);
        CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                      20, 325, 470, 54, window, reinterpret_cast<HMENU>(kStatus), g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      380, 395, 110, 30, window, reinterpret_cast<HMENU>(kInstall), g_instance, nullptr);
        DetectInto(window, false);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kDetect: DetectInto(window, true); return 0;
        case kBrowse: Browse(window); return 0;
        case kZeroTierCentral: OpenZeroTierCentral(window); return 0;
        case kInstall: RunInstall(window, false); return 0;
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_instance = instance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && _wcsicmp(argv[1], L"--install") == 0) {
        bool replace = false;
        std::wstring zeroTierNetwork;
        for (int i = 3; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--replace-config") == 0) replace = true;
            if (_wcsicmp(argv[i], L"--zerotier") == 0 && i + 1 < argc)
                zeroTierNetwork = argv[++i];
        }
        InstallResult result = Install(argv[2], replace);
        if (result.ok && !zeroTierNetwork.empty()) {
            std::wstring zeroTier;
            if (!SetupZeroTier(nullptr, zeroTierNetwork, zeroTier)) {
                result.ok = false;
                result.message += L"\n\nSifuCoop installed, but ZeroTier setup did not finish:\n" + zeroTier;
            } else {
                result.message += L"\n\n" + zeroTier;
            }
        }
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
    HWND window = CreateWindowExW(0, klass, L"SifuCoop installer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 530, 480, nullptr, nullptr, instance, nullptr);
    if (!window) {
        MessageBoxW(nullptr, L"Could not create the installer window.",
                    L"SifuCoop installer", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }


    ShowWindow(window, show ? show : SW_SHOWNORMAL);
    UpdateWindow(window);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
