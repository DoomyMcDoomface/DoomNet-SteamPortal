# Builds the production zips and manifest. -Upload sends them to the production release.
param(
    [switch]$Upload
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path $repo "dist\production"
New-Item -ItemType Directory -Path $out -Force | Out-Null
& (Join-Path $PSScriptRoot "Write-ProductionManifest.ps1") `
    -DoomNetExe (Join-Path $repo "build\Release\DoomNet.exe") `
    -PluginJson (Join-Path (Split-Path $repo -Parent) "SteamTunes\plugin.json") `
    -SkinJson (Join-Path (Split-Path $repo -Parent) "SteamSkin\skin.json") `
    -OutFile (Join-Path $out "production-manifest.json")

if (-not $Upload) {
    Write-Output "Manifest and zips are in $out"
    return
}

$files = @(
    (Join-Path $out "production-manifest.json"),
    (Join-Path $out "doomnet.zip"),
    (Join-Path $out "steamtunes.zip"),
    (Join-Path $out "steamskin.zip")
)
$tester = Join-Path $repo "dist\DoomNet.zip"
if (Test-Path -LiteralPath $tester) {
    $setup = Join-Path $out "DoomNet-setup.zip"
    Copy-Item -LiteralPath $tester -Destination $setup -Force
    $files += $setup
}
$existing = @(gh release view production --repo DoomyMcDoomface/DoomNet-SteamPortal --json assets --jq ".assets[].name")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($file in $files) {
    $name = Split-Path -Leaf $file
    $flag = @()
    if ($existing -contains $name) { $flag = @("--clobber") }
    gh release upload production $file @flag --repo DoomyMcDoomface/DoomNet-SteamPortal
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Output "Uploaded $name"
}
Write-Output "Uploaded the production update."
exit 0
