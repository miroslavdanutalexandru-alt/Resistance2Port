# Chicxulub freeze repro run (RL 138705: hard freeze a few metres after Daedalus tears the roof off the last dome).
#   tools\chx_run.ps1 [-Exe <path>] [-Checkpoint cp_boss_catwalks1] [-Level last-1] [-Pack R2_100pct_newgameplus]
#                     [-Env @{NAME='value'}]
# Boots straight into Chicxulub (fast boot: Campaign > Load Level, one row above the bottom = "decks" checkpoint,
# which is still before the domes if the checkpoint switch below fails) and switches the start to the catwalks
# checkpoint, with god mode + infinite ammo and the CHX1 diagnostics (hang report after 20 s without progress,
# level-script [lua] lines, stutter log, RSX stall report). -Env adds/overrides game env vars (A/B switches).
# Game-run rules: refuses to start while any Resistance*.exe runs or D:\Games\tmp\gamerun.lock exists; takes the
# lock, copies the user's saves, installs the save pack. A separate hidden watcher (it survives closing this
# console) waits for every Resistance2.exe started from -Exe to exit, then puts the saves back and removes the lock.
# This script itself waits until the game has exited.
param([string]$Exe = '', [string]$Checkpoint = 'cp_boss_catwalks1', [string]$Level = 'last-1',
      [string]$Pack = 'R2_100pct_newgameplus', [string]$Log = '', [string]$Owner = 'chx_run', [hashtable]$Env = @{})

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$lock = 'D:\Games\tmp\gamerun.lock'
$GameDir = 'D:\Games\Resistance2Port'
if (-not $Exe) { $Exe = Join-Path $repo 'build\Resistance2.exe' }
$Exe = (Resolve-Path $Exe).Path
if (-not $Log) { $Log = Join-Path $GameDir 'logs\run_chx2.log' }
$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'Resistance*' })
if ($running) { throw "Resistance 2 is already running (PID $($running.Id -join ', ')). Close it first, then run this again." }
New-Item -ItemType Directory -Force (Split-Path $lock) | Out-Null
New-Item -ItemType File -Path $lock -Value "$Owner $(Get-Date -Format s)" -ErrorAction Stop | Out-Null   # fails if it exists

$save = Join-Path $GameDir 'gamedata\dev_hdd0\home\00000001\savedata'
$bk = Join-Path 'D:\Games\saves_backup\R2' ("{0}_chx_run\savedata" -f (Get-Date -Format yyyyMMdd_HHmmss))
$restore = "robocopy '$bk' '$save' /MIR /R:2 /W:1 /NJH /NJS /NFL /NDL /NP | Out-Null; " +
           "if (`$LASTEXITCODE -lt 8) { Remove-Item -Force '$lock' -ErrorAction SilentlyContinue }"
$launched = $false
try {
    & robocopy $save $bk /E /NJH /NJS /NFL /NDL /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "saves backup failed ($bk)" }
    $packDir = Join-Path 'D:\Games\saves_pack\R2' $Pack
    if (-not (Test-Path $packDir)) { throw "no save pack $packDir" }
    foreach ($d in Get-ChildItem $packDir -Directory) {
        & robocopy $d.FullName (Join-Path $save $d.Name) /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
    }
    Write-Host "Your saves are backed up to $bk (they are put back automatically when the game closes)."
    # watcher, started BEFORE the game: waits up to 3 min for a Resistance2.exe from $Exe, then until none is left
    # (also covers a relaunch), then restores the saves and removes the lock. Independent of this console.
    $w = "`$exe='$Exe'; `$t=Get-Date; " +
         "function g { @(Get-CimInstance Win32_Process -Filter `"Name='Resistance2.exe'`" | ?{ `$_.ExecutablePath -eq `$exe }) }; " +
         "while (-not (g) -and ((Get-Date)-`$t).TotalSeconds -lt 180) { Start-Sleep 1 }; " +
         "while (g) { Start-Sleep 2 }; Start-Sleep 2; $restore"
    Start-Process powershell -ArgumentList '-NoProfile', '-WindowStyle', 'Hidden', '-Command', $w -WindowStyle Hidden | Out-Null

    # CHX1 diagnostics; FREEZE_RELAUNCH off: a frozen game must stay frozen (the hang report is the point)
    $envs = @{ PORT_HANG_DUMP = '1'; PORT_HITCH_LOG = '1'; R2_LUA_LOG = '1'; RSX_STALL_DIAG_MS = '3000'
               PORT_CHEAT_GODMODE = '1'; PORT_CHEAT_AMMO = '1'; FREEZE_RELAUNCH = '' }
    foreach ($k in $Env.Keys) { $envs[$k] = [string]$Env[$k] }
    $out = & (Join-Path $PSScriptRoot 'fastboot.ps1') -Level $Level -Checkpoint $Checkpoint -Exe $Exe -GameDir $GameDir `
        -Log $Log -KeepSaves -Cutscenes -Env $envs 6>&1 | ForEach-Object { "$_" }
    $gpid = ($out | Where-Object { $_ -match '^PID (\d+)' } | Select-Object -First 1) -replace '^PID ', ''
    if (-not $gpid) { $out | Write-Host; throw 'the game did not start' }
    $launched = $true
} catch {
    if (-not $launched) {
        if (Test-Path $bk) { & robocopy $bk $save /MIR /NJH /NJS /NFL /NDL /NP | Out-Null }
        Remove-Item -Force $lock -ErrorAction SilentlyContinue
    }
    throw
}
Write-Host ''
Write-Host '=================================================================================================='
Write-Host ' The game starts and goes through the menus BY ITSELF (about 3 minutes). Do not press anything until'
Write-Host ' you can move. You start in Chicxulub, on the catwalks before the three domes (god mode is on).'
Write-Host ' Then: follow the objective marker ("Battle your way to the Fission Bomb"). Daedalus rips the roof off'
Write-Host ' each dome; after the 3rd / last one, keep walking forward toward the big door (where it froze for'
Write-Host ' RU553CK) and on into the boss arena.'
Write-Host ' If it FREEZES: wait 30 seconds (a report is written), then close the game (Alt+F4 or Task Manager).'
Write-Host ' If you get past the door into the arena without a freeze: play 1-2 minutes, then close the game.'
Write-Host " (If you start somewhere else, e.g. the ship decks, just play on to the domes.)"
Write-Host '=================================================================================================='
Write-Host "game PID $gpid, log $Log"
$t0 = Get-Date; $seen = @{}
$marks = '^\[fastboot\] (start checkpoint|PORT_BOOT_CHECKPOINT|gameplay)|closing Boss door|REGION H IS LOADING|set_Boss_Arena_cp|\[hang|\[freeze|watchdog'
while (Get-Process -Id $gpid -ErrorAction SilentlyContinue) {
    Start-Sleep 1
    foreach ($h in Select-String -Path $Log -Pattern $marks -ErrorAction SilentlyContinue) {
        if (-not $seen.ContainsKey($h.LineNumber)) { $seen[$h.LineNumber] = 1; Write-Host ('{0,6:N0}s  {1}' -f ((Get-Date) - $t0).TotalSeconds, $h.Line) }
    }
}
Write-Host 'The game has closed. Putting your saves back...'
for ($i = 0; $i -lt 30 -and (Test-Path $lock); $i++) { Start-Sleep 1 }   # the watcher restores them
if (Test-Path $lock) { Invoke-Expression $restore }
$hang = Get-ChildItem (Join-Path $GameDir 'logs\crash') -Filter '*hang*.txt' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt $t0 } | Sort-Object LastWriteTime | Select-Object -Last 1
Write-Host ("Saves restored: {0}. Log: {1}" -f $(if (Test-Path $lock) { 'NO - copy them back from ' + $bk } else { 'yes' }), $Log)
if ($hang) { Write-Host "Hang report: $($hang.FullName)" } else { Write-Host 'No hang report was written.' }
