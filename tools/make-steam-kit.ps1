<#
    Assembles the self-contained Steam build kit and copies it wherever it is
    going.

    The kit is everything the OTHER machine needs to compile its own dsound.dll
    from ITS OWN Sifu-Win64-Shipping.pdb: sources, the build script, the offset
    generator, and the build definitions already recorded for other stores. It
    deliberately does NOT ship a compiled dll -- offsets are per-executable, and
    a dll built here is inert on a different Sifu build (the guard refuses to
    hook and logs "UNKNOWN BUILD").

    This used to be done by hand, which is why the kit on the USB stick and the
    one in the repo drifted apart. Now it is one command.

    Usage:
        .\tools\make-steam-kit.ps1                     # stage + zip locally
        .\tools\make-steam-kit.ps1 -Destination Z:\    # ...and push it there
#>
param(
    # Where to also place the kit, e.g. Z:\ (the other PC's share) or E:\ (a USB
    # stick). Skipped, with a plain message, if it is not reachable -- a network
    # share that happens to be offline is not a reason to fail the whole job.
    [string]$Destination = "",
    [string]$StageRoot = "$env:USERPROFILE",
    # Folder name to use at the destination. The Steam PC already keeps its copy
    # as "SifuCoopKit", and it has BUILT there -- so the name has to match or the
    # push lands beside its build instead of updating it.
    [string]$DestinationName = "",
    [switch]$NoZip,
    # Update the destination in place instead of replacing it.
    #
    # The destination is a machine that has already compiled: it holds a build/
    # and a dist/ of its own, and its builds/*.json records ITS executable's
    # offsets. Deleting that to drop a fresh copy on top would throw away the one
    # thing that machine produced and cannot be regenerated from here. Sources
    # and the shared build definitions are overwritten; anything the other
    # machine made is left where it is.
    [switch]$NoClean
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$KitName = "SifuCoop-SteamKit"
$Stage = Join-Path $StageRoot $KitName

# Exactly what the other machine needs, and nothing that would go stale.
$Dirs  = @("src", "tools", "builds", "third_party", "testclient", "launcher")
$Files = @("build.ps1", "STEAM-BUILD.md", "SETUP.md", "README.md", "LICENSE",
           "THIRD_PARTY.md")

if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force $Stage | Out-Null

foreach ($dir in $Dirs) {
    $source = Join-Path $Root $dir
    if (-not (Test-Path $source)) { throw "missing $source" }
    Copy-Item -Recurse -Force $source (Join-Path $Stage $dir)
}
foreach ($file in $Files) {
    $source = Join-Path $Root $file
    if (-not (Test-Path $source)) { throw "missing $source" }
    Copy-Item -Force $source $Stage
}

# The ini comes from dist/, not from the repo root: dist/ is the one with the
# end-user comments on every switch, and it is what a player actually reads.
Copy-Item -Force (Join-Path $Root "dist\SifuCoop.ini") $Stage

# Build leftovers help nobody and confuse a "why is this here" reading of the
# kit. __pycache__ in particular is bytecode for a different Python.
Get-ChildItem $Stage -Recurse -Directory -Filter "__pycache__" |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

# The offset count is the kit's own sanity check: STEAM-BUILD.md tells the other
# machine that a lower count than this means the two installs are different game
# patches, not a build problem. Print it here so the number is known before it
# leaves.
$epic = Join-Path $Stage "builds\epic.json"
if (Test-Path $epic) {
    $count = @((Get-Content $epic -Raw | ConvertFrom-Json).offsets.PSObject.Properties).Count
    Write-Host "kit carries the epic build definition: $count offsets" -ForegroundColor Cyan
}

Write-Host "staged $Stage" -ForegroundColor Green

$zip = Join-Path $StageRoot "$KitName.zip"
if (-not $NoZip) {
    Compress-Archive -Path "$Stage\*" -DestinationPath $zip -Force
    $megabytes = "{0:N1}" -f ((Get-Item $zip).Length / 1MB)
    Write-Host "zipped  $zip ($megabytes MB)"
}

if ($Destination) {
    if (-not (Test-Path $Destination)) {
        Write-Host "destination '$Destination' is not reachable -- kit left staged locally." `
            -ForegroundColor Yellow
        Write-Host "re-run with the same -Destination once it is up." -ForegroundColor Yellow
        return
    }
    $name = if ($DestinationName) { $DestinationName } else { $KitName }
    $target = Join-Path $Destination $name
    if ($NoClean) {
        New-Item -ItemType Directory -Force $target | Out-Null
        Copy-Item -Recurse -Force (Join-Path $Stage "*") $target
        Write-Host "updated in place: $target (its build/ and dist/ left alone)" `
            -ForegroundColor Green
    } else {
        if (Test-Path $target) { Remove-Item -Recurse -Force $target }
        Copy-Item -Recurse -Force $Stage $target
        if (-not $NoZip) { Copy-Item -Force $zip $Destination }
        Write-Host "copied to $target" -ForegroundColor Green
    }
}
