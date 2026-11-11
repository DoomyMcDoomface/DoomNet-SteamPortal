# Writes the production manifest and the three stored zips the updater downloads.
# The workflow that calls this runs only on the production branch.
param(
    [Parameter(Mandatory = $true)][string]$DoomNetExe,
    [Parameter(Mandatory = $true)][string]$PluginJson,
    [Parameter(Mandatory = $true)][string]$SkinJson,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [string]$DoomNetVersion = "1.16.0"
)

$ErrorActionPreference = "Stop"

function Add-U16([System.Collections.Generic.List[byte]]$Out, [int]$Value) {
    $Out.Add([byte]($Value -band 255))
    $Out.Add([byte](($Value -shr 8) -band 255))
}

function Add-U32([System.Collections.Generic.List[byte]]$Out, [uint32]$Value) {
    $Out.Add([byte]($Value -band 255))
    $Out.Add([byte](($Value -shr 8) -band 255))
    $Out.Add([byte](($Value -shr 16) -band 255))
    $Out.Add([byte](($Value -shr 24) -band 255))
}

function Write-StoreZip([string]$Path, [object[]]$Files) {
    $locals = New-Object System.Collections.Generic.List[byte]
    $central = New-Object System.Collections.Generic.List[byte]
    $count = 0
    foreach ($file in $Files) {
        $nameBytes = [Text.Encoding]::UTF8.GetBytes([string]$file.Name)
        $data = [IO.File]::ReadAllBytes([string]$file.Path)
        if ($data.Length -gt 64MB) { throw "$($file.Path) is larger than the updater accepts" }
        $offset = [uint32]$locals.Count
        Add-U32 $locals 0x04034b50
        Add-U16 $locals 20
        Add-U16 $locals 0
        Add-U16 $locals 0
        Add-U16 $locals 0
        Add-U16 $locals 0
        Add-U32 $locals 0
        Add-U32 $locals ([uint32]$data.Length)
        Add-U32 $locals ([uint32]$data.Length)
        Add-U16 $locals $nameBytes.Length
        Add-U16 $locals 0
        $locals.AddRange($nameBytes)
        $locals.AddRange($data)
        Add-U32 $central 0x02014b50
        Add-U16 $central 20
        Add-U16 $central 20
        Add-U16 $central 0
        Add-U16 $central 0
        Add-U16 $central 0
        Add-U16 $central 0
        Add-U32 $central 0
        Add-U32 $central ([uint32]$data.Length)
        Add-U32 $central ([uint32]$data.Length)
        Add-U16 $central $nameBytes.Length
        Add-U16 $central 0
        Add-U16 $central 0
        Add-U16 $central 0
        Add-U16 $central 0
        Add-U32 $central 0
        Add-U32 $central $offset
        $central.AddRange($nameBytes)
        $count++
    }
    $zip = New-Object System.Collections.Generic.List[byte]
    $zip.AddRange($locals)
    $cdOff = [uint32]$zip.Count
    $zip.AddRange($central)
    Add-U32 $zip 0x06054b50
    Add-U16 $zip 0
    Add-U16 $zip 0
    Add-U16 $zip $count
    Add-U16 $zip $count
    Add-U32 $zip ([uint32]$central.Count)
    Add-U32 $zip $cdOff
    Add-U16 $zip 0
    [IO.File]::WriteAllBytes($Path, $zip.ToArray())
}

function Hash-File([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Select-PayloadFiles([string]$Root, [string]$Prefix) {
    $files = @()
    Get-ChildItem -LiteralPath $Root -Recurse -File -Force | ForEach-Object {
        $relative = $_.FullName.Substring($Root.Length).TrimStart('\')
        if ($relative -match '(^|\\)(\.git|node_modules|data)(\\|$)') { return }
        $files += [pscustomobject]@{ Name = ($Prefix + ($relative -replace '\\', '/')); Path = $_.FullName }
    }
    return $files
}

$outDir = Split-Path -Parent $OutFile
if ([string]::IsNullOrWhiteSpace($outDir)) { $outDir = "." }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$exeDir = Split-Path -Parent $DoomNetExe
$dll = Join-Path $exeDir "version.dll"
if (-not (Test-Path -LiteralPath $dll)) { throw "version.dll is not beside $DoomNetExe" }
$doomnetZip = Join-Path $outDir "doomnet.zip"
Write-StoreZip $doomnetZip @(
    [pscustomobject]@{ Name = "DoomNet.exe"; Path = $DoomNetExe },
    [pscustomobject]@{ Name = "version.dll"; Path = $dll }
)

$playerRoot = Split-Path -Parent $PluginJson
$playerFiles = @([pscustomobject]@{ Name = "plugin.json"; Path = $PluginJson })
$playerFiles += Select-PayloadFiles (Join-Path $playerRoot "backend") "backend/"
$playerZip = Join-Path $outDir "steamtunes.zip"
Write-StoreZip $playerZip $playerFiles

$skinRoot = Split-Path -Parent $SkinJson
$skinFiles = Select-PayloadFiles $skinRoot ""
$skinZip = Join-Path $outDir "steamskin.zip"
Write-StoreZip $skinZip $skinFiles

function Piece([string]$ZipPath, [string]$FileName, [string]$Version) {
    $hash = Hash-File $ZipPath
    if ([string]::IsNullOrWhiteSpace($Version)) { $Version = $hash.Substring(0, 12) }
    return [ordered]@{
        version = $Version
        url = "https://github.com/DoomyMcDoomface/DoomNet-SteamPortal/releases/download/production/$FileName"
        sha256 = $hash
    }
}

$manifest = [ordered]@{
    doomnet = Piece $doomnetZip "doomnet.zip" $DoomNetVersion
    steamtunes = Piece $playerZip "steamtunes.zip"
    steamskin = Piece $skinZip "steamskin.zip"
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $OutFile -Encoding utf8
Write-Output "Wrote $OutFile"
Write-Output "Wrote $doomnetZip"
Write-Output "Wrote $playerZip"
Write-Output "Wrote $skinZip"
