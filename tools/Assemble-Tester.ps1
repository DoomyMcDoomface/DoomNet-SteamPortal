# Builds the folder a tester can be sent. Run from the DoomNet repo.
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build (Join-Path $repo "build") --config Release --target DoomNet
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$fx = "C:\Windows\Microsoft.NET\Framework64\v4.0.30319"
$setupExe = Join-Path $repo "build\Release\DoomNetSetup.exe"
$setupCs = Join-Path $repo "tools\DoomNetSetup.cs"
$setupManifest = Join-Path $repo "tools\setup.manifest"
& (Join-Path $fx "csc.exe") /nologo /target:winexe /platform:x64 "/out:$setupExe" "/win32manifest:$setupManifest" "/reference:$fx\WPF\PresentationFramework.dll" "/reference:$fx\WPF\PresentationCore.dll" "/reference:$fx\WPF\WindowsBase.dll" "/reference:$fx\System.Xaml.dll" "/reference:$fx\System.Windows.Forms.dll" $setupCs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$release = Join-Path $repo "build\Release"
$out = Join-Path $repo "dist\DoomNet"
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null
Copy-Item -LiteralPath (Join-Path $release "DoomNet.exe") -Destination (Join-Path $out "DoomNet.exe")
Copy-Item -LiteralPath (Join-Path $release "DoomNetSetup.exe") -Destination (Join-Path $out "DoomNetSetup.exe")
Copy-Item -LiteralPath (Join-Path $release "version.dll") -Destination (Join-Path $out "version.dll")
Copy-Item -LiteralPath (Join-Path $repo "tools\wizard.xaml") -Destination (Join-Path $out "wizard.xaml")

function Copy-Tree([string]$From, [string]$To) {
    New-Item -ItemType Directory -Path $To -Force | Out-Null
    & robocopy $From $To /E /NFL /NDL /NJH /NJS /XD .git node_modules .vs
    if ($LASTEXITCODE -ge 8) { throw "Could not copy $From" }
}

$player = Join-Path (Split-Path $repo -Parent) "SteamTunes"
$skin = Join-Path (Split-Path $repo -Parent) "SteamSkin"
New-Item -ItemType Directory -Path (Join-Path $out "payload\player") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $player "plugin.json") -Destination (Join-Path $out "payload\player\plugin.json") -Force
Copy-Tree (Join-Path $player "backend") (Join-Path $out "payload\player\backend")
Copy-Tree $skin (Join-Path $out "payload\skin")

@"
1. Quit Steam. Also exit Steam from the icon in the taskbar corner.
2. Double-click DoomNetSetup.exe. Do not use Run as administrator.
3. Leave the Steam folder as it is, choose what you want, and click Install.
4. Click Yes when Windows asks for permission. That step puts DoomNet on Steam.
5. A folder opens with the installed files. Then open Steam. DoomNet has no window of its own. It shows up inside Steam.
6. Later fixes download by themselves. Quit Steam, including the taskbar icon, and open Steam again. Do not download a new zip.
"@ | Set-Content -LiteralPath (Join-Path $out "Read me.txt") -Encoding utf8

$zip = Join-Path $repo "dist\DoomNet.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $out "*") -DestinationPath $zip
Write-Output "Wrote $zip"
