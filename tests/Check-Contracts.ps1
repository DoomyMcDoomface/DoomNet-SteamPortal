# Fails when a fact this host must match has moved in the player or the skin,
# or when the skeleton files that are supposed to stay empty have grown a body.
param()

$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent
$player = Join-Path (Split-Path $repo -Parent) "SteamTunes"
$skin = Join-Path (Split-Path $repo -Parent) "SteamSkin"

$failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$Message) {
    $failures.Add($Message) | Out-Null
    Write-Output "FAIL  $Message"
}

function Need-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        Fail "missing $Path"
        return $false
    }
    return $true
}

function Need-Text([string]$Path, [string]$Needle, [string]$Why) {
    if (-not (Need-File $Path)) { return }
    $text = [System.IO.File]::ReadAllText($Path)
    if (-not $text.Contains($Needle)) {
        Fail "$Why ($Needle) not in $Path"
    }
}

$pluginPath = Join-Path $player "plugin.json"
if (Need-File $pluginPath) {
    $plugin = Get-Content -LiteralPath $pluginPath -Raw | ConvertFrom-Json
    if ($plugin.name -ne "SteamMusicPlayer") { Fail "plugin.json name is '$($plugin.name)'" }
    if ($plugin.version -ne "1.14.1") { Fail "plugin.json version is '$($plugin.version)'" }
    if ($plugin.backendType -ne "lua") { Fail "plugin.json backendType is '$($plugin.backendType)'" }
}

$main = Join-Path $player "backend\main.lua"
$bootstrap = Join-Path $player "backend\assets\frontend\bootstrap.js"
$ui = Join-Path $player "backend\assets\frontend\steam-music-player.js"
$engine = Join-Path $player "backend\assets\frontend\audio-engine.js"
$state = Join-Path $player "backend\player_state.lua"
$skinJson = Join-Path $skin "skin.json"

Need-Text $main 'local CHUNK_SIZE = 512 * 1024' "audio chunk size"
Need-Text $main 'require("millennium")' "lua host module"
Need-Text $main "millennium.add_browser_js" "browser js registration"
Need-Text $main "millennium.call_frontend_method" "frontend push"
Need-Text $main "millennium.ready()" "ready call"
Need-Text $bootstrap 'var ASSET_REV = "20261001-version-1.14.1"' "asset revision"
Need-Text $bootstrap "mix-image.js" "bootstrap load order"
Need-Text $bootstrap "window.__steamMusicPlayerBootstrap" "bootstrap guard"
Need-Text $bootstrap "window !== window.top" "iframe bail"
Need-Text $ui 'var PLUGIN_NAME = "SteamMusicPlayer"' "ui plugin name"
Need-Text $ui 'var PLUGIN_VERSION = "1.14.1"' "about version"
Need-Text $ui "Millennium.callServerMethod(PLUGIN_NAME, method, argsArray || [])" "ui call shape"
Need-Text $ui "__SMP_RECEIVERS__" "frontend fan-out"
Need-Text $engine "Millennium.callServerMethod(PLUGIN_NAME, method, argsArray || [])" "engine call shape"
Need-Text $engine "AUDIO_CHUNK_TIMEOUT_MS = 8000" "chunk timeout"
Need-Text $engine "byteLength !== info.size" "staged size check"
Need-Text $state "fs.join(nil, ...)" "nil-safe join dependency"
Need-Text $skinJson '"UseDefaultPatches": true' "default patches flag"
Need-Text $skinJson '"TargetJs": "music-player-inject.js"' "skin player inject patch"

$spec = Join-Path $repo "own_steam_loader_a1dd8dbe.plan.md"
Need-Text $spec "D:\Documents\Repos\DoomNet-SteamPortal" "spec source root"
Need-Text $spec "steam_attach" "spec attach section"

$cmake = Join-Path $repo "CMakeLists.txt"
Need-Text $cmake "add_executable(DoomNet WIN32" "windows subsystem target"
Need-Text $cmake "src/wide_io.cpp" "wide path helper is not linked"
Need-Text $cmake "src/launch.cpp" "hidden launch helper is not linked"
Need-Text $cmake "src/shortcut.cpp" "shortcut helper is not linked"
Need-Text $cmake "src/payload.cpp" "payload copy is not linked"
Need-Text $cmake "src/dummy_child.cpp" "hidden child program is not built"
Need-Text (Join-Path $repo "src\launch.cpp") "Refusing to start steam.exe" "steam.exe launch guard"
if ((Need-File $cmake) -and (Select-String -LiteralPath $cmake -Pattern "find_package" -Quiet)) {
    Fail "CMakeLists.txt calls find_package; this skeleton must not fetch Lua or Millennium"
}
if ((Need-File $cmake) -and (Select-String -LiteralPath $cmake -Pattern "boot_script\.js" -Quiet) -and (Select-String -LiteralPath $cmake -Pattern "src/boot_script.js" -Quiet)) {
    # The comment may name the file. It must not be a compiled source line.
    $cmakeText = [System.IO.File]::ReadAllText($cmake)
    if ($cmakeText -match "(?m)^\s*src/boot_script\.js") {
        Fail "boot_script.js is listed as a compiled source"
    }
}

$attach = Join-Path $repo "src\steam_attach.cpp"
if (Need-File $attach) {
    $attachText = [System.IO.File]::ReadAllText($attach)
    if ($attachText -notmatch "void steam_attach_anchor\(\) \{\s*\}") {
        Fail "steam_attach.cpp anchor is no longer empty. Fill it only after the Lua host tests exist, and update this check in that same change."
    }
    foreach ($banned in @("WriteProcessMemory", "CreateRemoteThread", "VirtualAlloc", "MinHook", "addScriptToEvaluateOnNewDocument")) {
        if ($attachText.Contains($banned)) {
            Fail "steam_attach.cpp contains $banned while the anchor is still the scaffold"
        }
    }
}

$boot = Join-Path $repo "src\boot_script.js"
if (Need-File $boot) {
    $bootText = [System.IO.File]::ReadAllText($boot)
    if ($bootText -notmatch "callServerMethod") {
        Fail "boot_script.js does not define callServerMethod"
    }
    if ($bootText -notmatch "window !== window\.top") {
        Fail "boot_script.js does not skip iframes"
    }
    if ($bootText -match "127\.0\.0\.1") {
        Fail "boot_script.js mentions 127.0.0.1. The page binding is not HTTP."
    }
    if ($bootText -match "fetch\s*\(") {
        Fail "boot_script.js fetches. The page binding is not HTTP."
    }
}

$anchors = Join-Path $repo "src\anchors.h"
if (Need-File $anchors) {
    $names = [regex]::Matches([System.IO.File]::ReadAllText($anchors), "void (\w+_anchor)\(\);") | ForEach-Object { $_.Groups[1].Value }
    $cmakeText = ""
    if (Need-File $cmake) {
        $cmakeText = [System.IO.File]::ReadAllText($cmake)
    }
    foreach ($name in $names) {
        $fileName = switch ($name) {
            "log_anchor" { "log.cpp" }
            "config_anchor" { "config.cpp" }
            "steam_discover_anchor" { "steam_discover.cpp" }
            "state_thread_anchor" { "state_thread.cpp" }
            "browser_thread_anchor" { "browser_thread.cpp" }
            "steam_attach_anchor" { "steam_attach.cpp" }
            "stream_anchor" { "stream.cpp" }
            "host_fs_anchor" { "host_fs.cpp" }
            "host_json_anchor" { "host_json.cpp" }
            "host_logger_anchor" { "host_logger.cpp" }
            "host_utils_anchor" { "host_utils.cpp" }
            "host_millennium_anchor" { "host_millennium.cpp" }
            "toggle_anchor" { "toggle.cpp" }
            "installer_anchor" { "installer.cpp" }
            default { "" }
        }
        if (-not $fileName) {
            Fail "no file mapping for $name"
            continue
        }
        $path = Join-Path $repo "src\$fileName"
        Need-Text $path "void $name() {}" "empty anchor $name"
        if ($cmakeText -and -not $cmakeText.Contains("src/$fileName")) {
            Fail "CMakeLists.txt does not compile src/$fileName"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Output "----"
    Write-Output "$($failures.Count) failure(s)"
    exit 1
}

Write-Output "ok  player=$player"
Write-Output "ok  skin=$skin"
exit 0
