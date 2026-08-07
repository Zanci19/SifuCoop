<#
    Builds dsound.dll (the SifuCoop proxy) and optionally deploys it.

    Usage:
        .\build.ps1              # regenerate offsets + build
        .\build.ps1 -Deploy      # ...and copy into the game folder
        .\build.ps1 -NoGen       # skip the PDB pass (fast rebuild)
#>
param(
    [switch]$Deploy,
    [switch]$NoGen,
    [string]$LocalBuildName = "epic",
    # Where THIS machine's Sifu lives. Defaults to the Epic install; on a Steam
    # machine pass -GameDir "...\steamapps\common\Sifu\Sifu\Binaries\Win64"
    # -LocalBuildName steam to record that build alongside the others.
    [string]$GameDir = "C:\Program Files\Epic Games\Sifu\Sifu\Binaries\Win64",
    # Extra directories to prepend to PATH for this run, semicolon-separated.
    # Point it at a PORTABLE toolchain when winget/Store is unavailable, e.g.
    # -ToolchainBin "C:\winlibs\mingw64\bin;C:\python312". Prepended AFTER the
    # registry PATH is rebuilt below, so it always wins.
    [string]$ToolchainBin = ""
)

$ErrorActionPreference = "Stop"

$Root    = $PSScriptRoot
$Exe     = Join-Path $GameDir "Sifu-Win64-Shipping.exe"
$Pdb     = Join-Path $GameDir "Sifu-Win64-Shipping.pdb"
$OutDir  = Join-Path $Root "build"

# PATH is not refreshed in an existing shell after an install, so rebuild it
# from the registry first...
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
            [Environment]::GetEnvironmentVariable("Path", "User")
# ...then let a portable toolchain override it (survives the rebuild above).
if ($ToolchainBin) { $env:Path = $ToolchainBin + ";" + $env:Path }

if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    throw ("g++ not found. Install the WinLibs UCRT toolchain (portable zip from " +
           "winlibs.com, no winget needed) and pass its bin folder with " +
           "-ToolchainBin `"C:\winlibs\mingw64\bin`".")
}

New-Item -ItemType Directory -Force $OutDir | Out-Null

if (-not $NoGen) {
    if (-not (Test-Path $Pdb)) { throw "PDB not found at $Pdb" }
    Write-Host "[1/3] regenerating offsets from PDB..." -ForegroundColor Cyan
    $BuildsDir = Join-Path $Root "builds"
    New-Item -ItemType Directory -Force $BuildsDir | Out-Null
    # Record THIS machine's executable as a build definition, then combine every
    # known build into one header so a single DLL serves Epic and Steam alike.
    python (Join-Path $Root "tools\pdbdump\pdbdump.py") $Pdb `
        --exe $Exe --emit-build $LocalBuildName (Join-Path $BuildsDir "$LocalBuildName.json")
    if ($LASTEXITCODE -ne 0) { throw "pdbdump failed" }
    python (Join-Path $Root "tools\pdbdump\pdbdump.py") $Pdb `
        --gen (Join-Path $Root "src\core\offsets.g.h") --builds $BuildsDir
    if ($LASTEXITCODE -ne 0) { throw "pdbdump failed" }
}

# The packet authentication is the one part of this where "it appears to work"
# proves nothing: a broken HMAC either rejects everything, which is obvious, or
# accepts everything, which is invisible and defeats the entire point. So the
# published vectors run on every build, before anything is shipped.
Write-Host "[2/3] verifying crypto against published test vectors..." -ForegroundColor Cyan
$cryptoTest = Join-Path $OutDir "cryptotest.exe"
cmd /c "g++ -O2 -std=c++17 -static -Wall -Wextra -o `"$cryptoTest`" `"$(Join-Path $Root 'tools\cryptotest\cryptotest.cpp')`" `"$(Join-Path $Root 'src\net\crypto.cpp')`" 2>&1"
if ($LASTEXITCODE -ne 0) { throw "cryptotest failed to compile" }
$vectorOutput = cmd /c "`"$cryptoTest`" 2>&1"
if ($LASTEXITCODE -ne 0) {
    $vectorOutput | ForEach-Object { Write-Host $_ }
    throw "crypto test vectors FAILED -- refusing to build an unauthenticated protocol"
}
Write-Host ($vectorOutput | Select-Object -Last 1) -ForegroundColor Green

Write-Host "[3/3] compiling dsound.dll..." -ForegroundColor Cyan

$sources = @(
    "src\dllmain.cpp"
    "src\core\log.cpp"
    "src\core\hooks.cpp"
    "src\ue\reflection.cpp"
    "src\game\actors.cpp"
    "src\game\coop.cpp"
    "src\game\selftest.cpp"
    "src\game\puppet.cpp"
    "src\game\runstate.cpp"
    "src\net\crypto.cpp"
    "src\net\session.cpp"
    "src\\game\\orders.cpp"
    "src\\game\\enemies.cpp"
    "src\\ui\\overlay.cpp"
    "src\\ui\\d3d_overlay.cpp"
    # Dear ImGui (MIT), display-only: no input backend hooks are installed.
    "third_party\imgui\imgui.cpp"
    "third_party\imgui\imgui_draw.cpp"
    "third_party\imgui\imgui_tables.cpp"
    "third_party\imgui\imgui_widgets.cpp"
    "third_party\imgui\backends\imgui_impl_dx11.cpp"
    "third_party\imgui\backends\imgui_impl_win32.cpp"
    # MinHook (MIT). Brings its own HDE length disassembler, which is the part
    # that makes relocating an arbitrary prologue safe.
    "third_party\minhook\src\buffer.c"
    "third_party\minhook\src\hook.c"
    "third_party\minhook\src\trampoline.c"
    "third_party\minhook\src\hde\hde64.c"
    "src\proxy\dsound_proxy.cpp"
) | ForEach-Object { Join-Path $Root $_ }

$dll = Join-Path $OutDir "dsound.dll"

# -static links libstdc++/libgcc in, so the DLL has no MinGW runtime deps.
# -s strips symbols; the DLL is small and we debug via the log, not a debugger.
#
# Routed through cmd.exe on purpose: Windows PowerShell 5.1 wraps a native
# command's stderr in ErrorRecords and reports failure even on success, which
# hides the compiler diagnostics we actually need to read.
# ImGui's own sources include "imgui.h" unqualified, so its directory has to be
# on the include path rather than reached by relative path.
$imguiInc = "-I`"$(Join-Path $Root 'third_party\imgui')`" -I`"$(Join-Path $Root 'third_party\imgui\backends')`""
$flags = "-O2 -std=c++17 -static -static-libgcc -static-libstdc++ $imguiInc " +
         "-Wall -Wextra -s -lkernel32 -luser32 -lws2_32 -lgdi32 -ld3d11 -ldxgi " +
         "-ld3dcompiler -ldwmapi"
$srcArgs = ($sources | ForEach-Object { "`"$_`"" }) -join " "
$defFile = Join-Path $Root "src\proxy\dsound.def"
$logFile = Join-Path $OutDir "build.log"

cmd /c "g++ -shared -o `"$dll`" $srcArgs `"$defFile`" $flags > `"$logFile`" 2>&1"
$compileOk = ($LASTEXITCODE -eq 0)

if (Test-Path $logFile) {
    $output = Get-Content $logFile
    if ($output) { $output | ForEach-Object { Write-Host $_ } }
}
if (-not $compileOk) { throw "compile failed (see $logFile)" }

$size = [math]::Round((Get-Item $dll).Length / 1KB, 1)
Write-Host "built $dll ($size KB)" -ForegroundColor Green

# The test client stands in for a second player, so the netcode can be tested
# without a second copy of Sifu.
$testClient = Join-Path $OutDir "testclient.exe"
$tcLog = Join-Path $OutDir "testclient.build.log"
$tcSources = @(
    "testclient\testclient.cpp"
    # The bot has to sign its packets like any other peer, so it links the same
    # implementation the game does rather than a second copy that could drift.
    "src\net\crypto.cpp"
) | ForEach-Object { "`"$(Join-Path $Root $_)`"" }
cmd /c "g++ -o `"$testClient`" $($tcSources -join ' ') $flags > `"$tcLog`" 2>&1"
if ($LASTEXITCODE -ne 0) {
    Get-Content $tcLog | ForEach-Object { Write-Host $_ }
    # A running test client locks the exe. That must not fail the build -- the
    # DLL is the deliverable and the protocol rarely changes.
    Write-Host "WARNING: testclient not rebuilt (running? close it and rebuild)" -ForegroundColor Yellow
} else {
    Write-Host "built $testClient" -ForegroundColor Green
}

# Setup GUI. -mwindows makes it a windowed app rather than a console one.
$launcher = Join-Path $OutDir "SifuCoopLauncher.exe"
$lnLog = Join-Path $OutDir "launcher.build.log"
cmd /c "g++ -o `"$launcher`" `"$(Join-Path $Root 'launcher\launcher.cpp')`" $flags -mwindows -municode -lshlwapi > `"$lnLog`" 2>&1"
if ($LASTEXITCODE -ne 0) {
    Get-Content $lnLog | ForEach-Object { Write-Host $_ }
    Write-Host "WARNING: launcher not rebuilt" -ForegroundColor Yellow
} else {
    Write-Host "built $launcher" -ForegroundColor Green
}

# Package the shippable subset: everything a player needs, nothing they don't.
# Kept separate from build\ so source and build artefacts never get handed out
# by accident.
$Dist = Join-Path $Root "dist"
New-Item -ItemType Directory -Force $Dist | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Dist "tools") | Out-Null

Copy-Item $dll (Join-Path $Dist "dsound.dll") -Force
if (Test-Path $launcher) { Copy-Item $launcher (Join-Path $Dist "SifuCoopLauncher.exe") -Force }
if (Test-Path $testClient) { Copy-Item $testClient (Join-Path $Dist "tools\testclient.exe") -Force }
Copy-Item (Join-Path $Root "SETUP.md") (Join-Path $Dist "SETUP.md") -Force

# Default config, only if absent -- never clobber one a user has edited.
$distIni = Join-Path $Dist "SifuCoop.ini"
if (-not (Test-Path $distIni)) {
    @"
; SifuCoop configuration. Edit by hand, use SifuCoopLauncher.exe, or press F1
; in game -- the Tuning tab writes this file back.
;
;   mode       = off | host | client
;   host       = the HOST's address (client mode only)
;   port       = UDP port; the host binds it, the client connects to it
;   passphrase = shared secret; every packet is authenticated with it.
;                Both players must use exactly the same one. Leaving it empty
;                still works and still authenticates -- it just uses a key
;                everyone else also has, which is fine on a VPN or LAN and not
;                fine on a forwarded port.
;
; For hole punching over the internet, both players also set:
;   punch      = the OTHER player's public address:port (F1 -> Internet finds it)
;   local_port = the same fixed number on both machines
[net]
mode=host
host=127.0.0.1
port=7777
passphrase=

; Everything below defaults to on. Each line names one part of the machinery;
; turning one off is how you find out which half is misbehaving.
;   versus              = 1 to spar against each other instead of co-operating
;   echo_enemy_attacks  = replay the host's enemy swings on the joining screen
;   report_damage       = let the joining player hurt what the host can see
;   park_extra_enemies  = hide enemies the host has not activated
;   adaptive_interp     = size the smoothing buffer from measured ping
[coop]
versus=0
sync_enemies=1
suppress_client_ai=1
sync_enemy_vitals=1
park_extra_enemies=1
echo_enemy_attacks=1
; Replaying the remote PLAYER's attacks is off: the replay is a real hitbox and
; same-faction does not stop it hurting you, so with this on your co-op partner's
; swings kill you. Their damage already resolved on their own machine. Versus
; mode replays them regardless (hitting each other is the point there).
echo_player_attacks=0
report_damage=1
mirror_peer_vitals=1
auto_follow_level=1
adaptive_interp=1
interp_delay_ms=60
snapshot_hz=60

; The F1 menu draws by hooking the game's swap chain. That is the riskiest
; thing this mod does and it is purely cosmetic -- set this to 0 if the game
; crashes or misbehaves graphically, and everything else keeps working through
; this file and the hotkeys.
in_game_overlay=1

; Diagnostic. With this on, once you are standing in a fight the mod teleports
; an enemy next to you and makes it attack twice -- once under its own AI, once
; the way a joining player's screen drives it -- then logs whether each landed.
; It answers, on one machine, whether a replayed enemy attack actually damages
; you. Leave it 0 for normal play; it moves enemies around.
selftest=0
"@ | Set-Content $distIni -Encoding ascii
}

Write-Host "packaged -> $Dist" -ForegroundColor Green

if ($Deploy) {
    if (-not (Test-Path $GameDir)) { throw "game folder not found: $GameDir" }
    Copy-Item $dll (Join-Path $GameDir "dsound.dll") -Force
    Write-Host "deployed to $GameDir" -ForegroundColor Green
    Write-Host "remove with: Remove-Item '$GameDir\dsound.dll'" -ForegroundColor DarkGray
}




