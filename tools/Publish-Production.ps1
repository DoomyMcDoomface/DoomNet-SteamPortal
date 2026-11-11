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

gh release upload production `
    (Join-Path $out "production-manifest.json") `
    (Join-Path $out "doomnet.zip") `
    (Join-Path $out "steamtunes.zip") `
    (Join-Path $out "steamskin.zip") `
    --clobber --repo DoomyMcDoomface/DoomNet-SteamPortal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "Uploaded the production update."
