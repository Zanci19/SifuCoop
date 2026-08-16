param(



    [string]$Destination = "",
    [string]$StageRoot = "$env:USERPROFILE",



    [string]$DestinationName = "",
    [switch]$NoZip,








    [switch]$NoClean
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$KitName = "SifuCoop-SteamKit"
$Stage = Join-Path $StageRoot $KitName


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



Copy-Item -Force (Join-Path $Root "dist\SifuCoop.ini") $Stage



Get-ChildItem $Stage -Recurse -Directory -Filter "__pycache__" |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue





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
