# measure.ps1 - measures the running cost of the livewall engine.
#
#   .\tools\measure.ps1                                 current wallpaper, as configured
#   .\tools\measure.ps1 -Wallpaper builtin:dunes -Render  force rendering even if the desktop is covered
#   .\tools\measure.ps1 -Resolution 2                   shader resolution: 0 auto, 1 full, 2 half
#
# Restarts the engine, waits for it to settle, then samples for -Seconds.
# -Render temporarily turns off "pause when covered" so the cost of actually
# drawing is measured; the original config is always restored.
param([string]$Wallpaper = '', [switch]$Render, [int]$Seconds = 10, [int]$Resolution = -1)

$exe = Join-Path $PSScriptRoot '..\build\livewall.exe' | Resolve-Path
$cfg = Join-Path $env:APPDATA 'livewall\config.ini'
$backup = Get-Content $cfg -Raw -Encoding Unicode

function Set-Key($text, $key, $value) { $text -replace "(?m)^$key=.*$", "$key=$value" }
function Write-Config($text) { # the file can be briefly locked by indexers / antivirus
    for ($i = 0; $i -lt 20; $i++) {
        try { [IO.File]::WriteAllText($cfg, $text, [Text.Encoding]::Unicode); return } catch { Start-Sleep -Milliseconds 150 }
    }
    throw "could not write $cfg"
}

try {
    $t = $backup
    if ($Wallpaper) { $t = Set-Key $t 'current' $Wallpaper }
    if ($Render) { $t = Set-Key $t 'pause_when_covered' '0' }
    if ($Resolution -ge 0) {
        $t = $t -replace '(?m)^(scale|resolution)=.*\r?\n', ''
        $t = $t -replace '(?m)^\[engine\]\r?\n', "[engine]`r`nresolution=$Resolution`r`n"
    }
    Write-Config $t

    Start-Process $exe -ArgumentList "-quit" -Wait
    $p = Start-Process $exe -PassThru
    Start-Sleep 4

    $q = Get-Process -Id $p.Id
    $cpu0 = $q.TotalProcessorTime
    $gpu = "\GPU Engine(pid_$($p.Id)_*)\Running Time"
    $g0 = (Get-Counter $gpu -ErrorAction SilentlyContinue).CounterSamples | Measure-Object CookedValue -Sum
    Start-Sleep $Seconds
    $q.Refresh()
    $g1 = (Get-Counter $gpu -ErrorAction SilentlyContinue).CounterSamples | Measure-Object CookedValue -Sum

    $pws = (Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -Filter "IDProcess=$($p.Id)").WorkingSetPrivate
    $cpu = ($q.TotalProcessorTime - $cpu0).TotalMilliseconds / ($Seconds * 1000) * 100 / [Environment]::ProcessorCount
    $gpums = ($g1.Sum - $g0.Sum) / 10000 / $Seconds # running time is in 100 ns units
    $name = if ($Wallpaper) { Split-Path $Wallpaper -Leaf } else { '(configured)' }
    "{0,-16} privateWS={1,5:N1} MB  threads={2,2}  CPU={3,6:N3} %  GPU={4,6:N2} ms/s" -f `
        $name, ($pws / 1MB), $q.Threads.Count, $cpu, $gpums
}
finally {
    Write-Config $backup
    if ($Render -or $Wallpaper) { Start-Process $exe -ArgumentList "-quit" -Wait; Start-Process $exe; Start-Sleep 2 }
}
