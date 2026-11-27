# Chicxulub freeze repro run (RL 138705: hard freeze a few metres after Daedalus tears the roof off the last dome).
#   tools\chx_run.ps1 [-Exe <path>] [-Checkpoint cp_boss_catwalks1] [-Pack R2_100pct_newgameplus] [-Seconds 600]
# Boots straight into Chicxulub (fast boot: Campaign > Load Level, last row) at the catwalks checkpoint, with god
# mode + infinite ammo and the CHX1 diagnostics (hang report after 20 s without progress, level-script [lua] lines,
# stutter log, RSX stall report). The hero still has to walk to the domes; the script only gets him there fast.
# Game-run rules: refuses to start while any Resistance*.exe runs or D:\Games\tmp\gamerun.lock exists; takes the lock,
# installs the save pack (all levels unlocked) after copying the user's saves, and when the game exits (close only
# the printed PID) puts the saves back and removes the lock.
param([string]$Exe = '', [string]$Checkpoint = 'cp_boss_catwalks1', [string]$Pack = 'R2_100pct_newgameplus',
      [int]$Seconds = 600, [string]$Log = '', [string]$Owner = 'chx_run')

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$lock = 'D:\Games\tmp\gamerun.lock'
$GameDir = 'D:\Games\Resistance2Port'
if (-not $Exe) { $Exe = Join-Path $repo 'build\Resistance2.exe' }
if (-not $Log) { $Log = Join-Path $GameDir 'logs\run_chx2.log' }
$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'Resistance*' })
if ($running) { throw "a game is running (PID $($running.Id -join ', ')): not starting" }
New-Item -ItemType File -Path $lock -Value "$Owner $(Get-Date -Format s)" -ErrorAction Stop | Out-Null   # fails if it exists
try {
    $save = Join-Path $GameDir 'gamedata\dev_hdd0\home\00000001\savedata'
    $bk = Join-Path 'D:\Games\saves_backup\R2' ("{0}_chx_run\savedata" -f (Get-Date -Format yyyyMMdd_HHmmss))
    & robocopy $save $bk /E /NJH /NJS /NFL /NDL /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "saves backup failed ($bk)" }
    foreach ($d in Get-ChildItem (Join-Path 'D:\Games\saves_pack\R2' $Pack) -Directory) {
        & robocopy $d.FullName (Join-Path $save $d.Name) /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
    }
    Write-Host "saves: copied to $bk, pack $Pack installed"
    # CHX1 diagnostics (the fast boot script reads settings.bat first; these are set in this process after it)
    $diag = [ordered]@{ PORT_HANG_DUMP = '1'; PORT_HITCH_LOG = '1'; R2_LUA_LOG = '1'; RSX_STALL_DIAG_MS = '3000'
                        PORT_CHEAT_GODMODE = '1'; PORT_CHEAT_AMMO = '1' }
    foreach ($k in $diag.Keys) { [Environment]::SetEnvironmentVariable($k, $diag[$k], 'Process') }
    $out = & (Join-Path $PSScriptRoot 'fastboot.ps1') -Level chicxulub -Checkpoint $Checkpoint -Exe $Exe -GameDir $GameDir `
        -Log $Log -KeepSaves -Cutscenes 6>&1 | ForEach-Object { "$_" }
    $out | Write-Host
    $gpid = ($out | Where-Object { $_ -match '^PID (\d+)' } | Select-Object -First 1) -replace '^PID ', ''
    if (-not $gpid) { throw 'the game did not start' }
    # watcher: saves back + lock removed when the game exits
    $w = "Wait-Process -Id $gpid -ErrorAction SilentlyContinue; Start-Sleep 2; " +
         "robocopy '$bk' '$save' /MIR /NJH /NJS /NFL /NDL /NP | Out-Null; Remove-Item -Force '$lock' -ErrorAction SilentlyContinue"
    Start-Process powershell -ArgumentList '-NoProfile', '-WindowStyle', 'Hidden', '-Command', $w -WindowStyle Hidden | Out-Null
    Write-Host "game PID $gpid; saves restored + lock removed when it exits. Log $Log"
} catch {
    if (-not $gpid) {
        if ($bk -and (Test-Path $bk)) { & robocopy $bk $save /MIR /NJH /NJS /NFL /NDL /NP | Out-Null }
        Remove-Item -Force $lock -ErrorAction SilentlyContinue
    }
    throw
}
# progress: the checkpoint override, gameplay, then the script markers around the freeze spot
$t0 = Get-Date; $seen = @{}
$marks = '^\[fastboot\] (start checkpoint|PORT_BOOT_CHECKPOINT|gameplay)|closing Boss door|REGION H IS LOADING|set_Boss_Arena_cp|hang|HANG'
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and (Get-Process -Id $gpid -ErrorAction SilentlyContinue)) {
    Start-Sleep 1
    foreach ($h in Select-String -Path $Log -Pattern $marks -ErrorAction SilentlyContinue) {
        if (-not $seen.ContainsKey($h.LineNumber)) { $seen[$h.LineNumber] = 1; Write-Host ('{0,6:N1}s  {1}' -f ((Get-Date) - $t0).TotalSeconds, $h.Line) }
    }
}
