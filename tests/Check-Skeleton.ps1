# Re-checks the skeleton binary. Does not start Steam and does not write outside this repo.
param(
    [string]$Exe = ""
)

$ErrorActionPreference = "Stop"

function Get-PeInfo([string]$Path) {
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $fs.Position = 0x3C
        $pe = $br.ReadInt32()
        $fs.Position = $pe
        $sig = $br.ReadUInt32()
        if ($sig -ne 0x00004550) {
            throw "not a PE file"
        }
        $fs.Position = $pe + 24
        $magic = $br.ReadUInt16()
        if ($magic -ne 0x10B -and $magic -ne 0x20B) {
            throw "unexpected optional magic $magic"
        }
        $fs.Position = $pe + 24 + 68
        $subsystem = $br.ReadUInt16()
        $dllChars = $br.ReadUInt16()
        return @{ Subsystem = $subsystem; DllCharacteristics = $dllChars }
    } finally {
        $fs.Dispose()
    }
}

function Invoke-One([string]$Path, [int]$TimeoutMs) {
    $proc = Start-Process -FilePath $Path -PassThru -WindowStyle Hidden
    if (-not $proc.WaitForExit($TimeoutMs)) {
        try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch {}
        throw "DoomNet did not exit within ${TimeoutMs}ms (pid $($proc.Id))"
    }
    return $proc.ExitCode
}

if (-not $Exe) {
    $Exe = Join-Path $PSScriptRoot "..\build\Release\DoomNet.exe"
}
$Exe = (Resolve-Path -LiteralPath $Exe).Path

$failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$Message) {
    $failures.Add($Message) | Out-Null
    Write-Output "FAIL  $Message"
}

Write-Output "exe   $Exe"
$length = (Get-Item -LiteralPath $Exe).Length
Write-Output "size  $length bytes (logged only, no cap: the host embeds Lua and a JSON library)"

$pe = Get-PeInfo $Exe
Write-Output ("pe    subsystem={0} dllchars=0x{1:X4}" -f $pe.Subsystem, $pe.DllCharacteristics)
if ($pe.Subsystem -ne 2) {
    Fail "Subsystem is $($pe.Subsystem), expected 2 (WINDOWS_GUI). A console subsystem would flash a window."
}
foreach ($flag in @(
    @{ Name = "DYNAMICBASE"; Mask = 0x0040 },
    @{ Name = "NXCOMPAT"; Mask = 0x0100 },
    @{ Name = "HIGH_ENTROPY_VA"; Mask = 0x0020 },
    @{ Name = "GUARD_CF"; Mask = 0x4000 }
)) {
    if (($pe.DllCharacteristics -band $flag.Mask) -eq 0) {
        Fail "missing $($flag.Name)"
    }
}

$before = Get-Process -Name DoomNet -ErrorAction SilentlyContinue
if ($before) {
    Fail "DoomNet was already running before the test ($($before.Id -join ', '))"
}

$dataRoot = Join-Path $env:TEMP ("doomnet-skeleton-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $dataRoot | Out-Null
$env:DOOMNET_ROOT = $dataRoot
$script:launches = 0

$freeBefore = [int64](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory

foreach ($warm in 1..3) {
    $code = Invoke-One $Exe 2000
    $script:launches++
    if ($code -ne 0) {
        Fail "warmup $warm exit code $code"
    }
}

$samples = New-Object System.Collections.Generic.List[int]
for ($i = 0; $i -lt 100; $i++) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $code = Invoke-One $Exe 2000
    $script:launches++
    $sw.Stop()
    $samples.Add([int]$sw.ElapsedMilliseconds) | Out-Null
    if ($code -ne 0) {
        Fail "run $i exit code $code"
        break
    }
}

$sorted = @($samples | Sort-Object)
$max = $sorted[$sorted.Count - 1]
$p50 = $sorted[[int]($sorted.Count / 2)]
$sum = 0
foreach ($sample in $samples) { $sum += $sample }
Write-Output ("seq   n={0} p50={1}ms max={2}ms total={3}ms" -f $samples.Count, $p50, $max, $sum)
if ($max -gt 2000) {
    Fail "a run took ${max}ms (limit 2000ms)"
}

$parallel = @()
for ($i = 0; $i -lt 32; $i++) {
    $parallel += Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
    $script:launches++
}
$parallelOk = $true
foreach ($proc in $parallel) {
    if (-not $proc.WaitForExit(2000)) {
        try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch {}
        Fail "parallel pid $($proc.Id) did not exit within 2000ms"
        $parallelOk = $false
        continue
    }
    if ($proc.ExitCode -ne 0) {
        Fail "parallel pid $($proc.Id) exit code $($proc.ExitCode)"
        $parallelOk = $false
    }
}
if ($parallelOk) {
    Write-Output "par   32 processes exited 0"
}

Start-Sleep -Milliseconds 200
$deadline = [DateTime]::UtcNow.AddSeconds(2)
$left = @(Get-Process -Name DoomNet -ErrorAction SilentlyContinue)
while ($left.Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100
    $left = @(Get-Process -Name DoomNet -ErrorAction SilentlyContinue)
}
if ($left.Count -gt 0) {
    $detail = @()
    foreach ($proc in $left) {
        try {
            $proc.Refresh()
            $detail += "$($proc.Id) ws=$($proc.WorkingSet64)"
        } catch {
            $detail += "$($proc.Id)"
        }
        try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch {}
    }
    Fail "$($left.Count) DoomNet process(es) still resident: $($detail -join ', ')"
} else {
    Write-Output "live  none"
}

$peak = 0
$seen = 0
for ($i = 0; $i -lt 30; $i++) {
    $proc = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
    $script:launches++
    while (-not $proc.HasExited) {
        try {
            $proc.Refresh()
            $seen++
            if ($proc.WorkingSet64 -gt $peak) {
                $peak = $proc.WorkingSet64
            }
        } catch {
            break
        }
    }
    if (-not $proc.WaitForExit(2000)) {
        try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch {}
        Fail "working-set sample $i did not exit"
        break
    }
    if ($proc.ExitCode -ne 0) {
        Fail "working-set sample $i exit code $($proc.ExitCode)"
        break
    }
}
Write-Output ("ws    samples-observed={0} peak={1} bytes" -f $seen, $peak)
if ($peak -gt 32MB) {
    Fail "peak working set $peak bytes exceeds 32MB"
}

$logPath = Join-Path $dataRoot "logs\doomnet.log"
if (-not (Test-Path -LiteralPath $logPath)) {
    Fail "log was not created under DOOMNET_ROOT"
} else {
    $raw = [System.IO.File]::ReadAllText($logPath)
    $lines = @($raw -split "`n" | Where-Object { $_ -ne "" })
    $bad = 0
    foreach ($line in $lines) {
        if ($line -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z (info|warn|error) .+\r?$') {
            $bad++
        }
    }
    Write-Output ("log   lines={0} launches={1} malformed={2}" -f $lines.Count, $script:launches, $bad)
    if ($bad -ne 0) {
        Fail "$bad log line(s) were torn or not a single record"
    }
    $started = @($lines | Where-Object { $_ -match 'Process started' }).Count
    $owners = @($lines | Where-Object { $_ -match 'Acquired Local\\DoomNet' }).Count
    $duplicates = @($lines | Where-Object { $_ -match 'already held' }).Count
    $finished = @($lines | Where-Object { $_ -match 'Startup checks finished' }).Count
    $steamLines = @($lines | Where-Object { $_ -match ' steam: ' }).Count
    $configLines = @($lines | Where-Object { $_ -match ' config: ' }).Count
    Write-Output ("log   started={0} owners={1} duplicates={2} finished={3} steam={4} config={5}" -f $started, $owners, $duplicates, $finished, $steamLines, $configLines)
    if ($started -ne $script:launches) {
        Fail "startup lines $started != launches $($script:launches)"
    }
    if (($owners + $duplicates) -ne $script:launches) {
        Fail "owner ($owners) plus duplicate ($duplicates) != launches $($script:launches)"
    }
    if ($finished -ne $owners) {
        Fail "finished lines $finished != owners $owners. A duplicate ran the rest of startup, or an owner stopped early."
    }
    if ($steamLines -lt $owners) {
        Fail "steam step was missing for an owner"
    }
    if ($configLines -lt $owners) {
        Fail "config step was missing for an owner"
    }
}

$freeAfter = [int64](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory
$droppedKb = $freeBefore - $freeAfter
Write-Output "mem   free-physical delta ${droppedKb}KB (logged only; the OS cache moves more than this process allocates)"

if ($failures.Count -gt 0) {
    Write-Output "----"
    Write-Output "$($failures.Count) failure(s)"
    exit 1
}

Write-Output "ok"
exit 0
