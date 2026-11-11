# Builds the tester zip and the production update, then replaces both on the
# production release. The pre-push hook runs this before a push of main or
# production. A failed upload stops the push.
param(
    [string]$Refs = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

function Test-UpdateRef([string]$remoteRef) {
    return $remoteRef -eq "refs/heads/main" -or $remoteRef -eq "refs/heads/production"
}

$publish = $false
$sha = ""
foreach ($line in ($Refs -split "`r?`n")) {
    $parts = @($line.Trim() -split '\s+' | Where-Object { $_ -ne "" })
    if ($parts.Count -lt 4) { continue }
    if (-not (Test-UpdateRef $parts[2])) { continue }
    if ($parts[1] -eq ("0" * 40)) { continue }
    $publish = $true
    if ($parts[2] -eq "refs/heads/production" -or $sha -eq "") { $sha = $parts[1] }
}

if ($Refs.Trim() -eq "") {
    $branch = (& git -C $repo rev-parse --abbrev-ref HEAD).Trim()
    if ($branch -eq "main" -or $branch -eq "production") {
        $publish = $true
        $sha = (& git -C $repo rev-parse HEAD).Trim()
    }
}

if (-not $publish) {
    Write-Output "This push does not update main or production, so the tester zip was left as it is."
    exit 0
}

$head = (& git -C $repo rev-parse HEAD).Trim()
if ($sha -ne "" -and $sha -ne $head) {
    Write-Output "The push is not the current checkout, so the tester zip was not published."
    exit 1
}

& (Join-Path $PSScriptRoot "Assemble-Tester.ps1")
if (-not (Test-Path -LiteralPath (Join-Path $repo "dist\DoomNet.zip"))) {
    Write-Output "dist\DoomNet.zip was not written, so the tester zip was not published."
    exit 1
}

& (Join-Path $PSScriptRoot "Publish-Production.ps1") -Upload
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "The tester zip and the production update now match this push."
