<#
PGO step 3+4: merge the recorded profiles and build the profile-optimised exe.

  1. instrumented build   powershell -ExecutionPolicy Bypass -File scripts\pgo_build_instrumented.ps1
                          -> build_pgo_gen\<Game>.exe (game runtime, hand-written code, lifted SPU code AND the
                             RPCS3 renderer, build_rpcs3_pgo_gen, are instrumented; the normal build\ is untouched)
  2. training             tools\pgo_train.bat (plays build_pgo_gen\<Game>.exe; profiles -> pgo\raw\*.profraw)
  3+4. this script        merges pgo\raw\*.profraw -> pgo\<r2|r3>.profdata, then build_cpu.bat pgo-use
                          -> build_pgo\<Game>.exe (AVX2 + ThinLTO + PGO; renderer in build_rpcs3_pgo_use)

Then compare: set PORT_GAME_EXE=build_pgo\<Game>.exe before run.bat (or the launcher), same save and spot.

Options:
  -AllRuns      also merge the older pgo\run_*\*.profraw sessions (scratch\pgo\pgo_train.ps1)
  -MergeOnly    merge and show the profile summary, do not build
  -NoLto        PGO without ThinLTO for the runtime (the renderer keeps its own setting from build_cpu.bat)
  -Jobs N       compile jobs (PORT_JOBS, default 4)
Never starts or stops the game. Builds take the repo's scratch\BUILD.lock like build_locked.ps1.
#>
param([switch]$AllRuns, [switch]$MergeOnly, [switch]$NoLto, [int]$Jobs = 4, [int]$TimeoutMin = 120)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$cpu = Get-Content (Join-Path $repo 'build_cpu.bat') -Raw
if ($cpu -notmatch 'pgo\\(r\d)\.profdata') { throw "build_cpu.bat: profile name not found" }
$tag = $Matches[1]
$game = if ($cpu -match 'build_cpu\\(\w+)\.exe') { $Matches[1] } else { throw "build_cpu.bat: exe name not found" }

$llvm = 'C:\Program Files\LLVM\bin'
$profdata = Join-Path $llvm 'llvm-profdata.exe'
if (-not (Test-Path $profdata)) { throw "llvm-profdata.exe not found in $llvm" }

$raw = @(Get-ChildItem (Join-Path $repo 'pgo\raw') -Filter *.profraw -ErrorAction SilentlyContinue)
if ($AllRuns) { $raw += @(Get-ChildItem (Join-Path $repo 'pgo') -Directory -Filter 'run_*' -ErrorAction SilentlyContinue | ForEach-Object { Get-ChildItem $_.FullName -Filter *.profraw }) }
$raw = @($raw | Where-Object { $_.Length -gt 0 })
if (-not $raw) { Write-Output "No .profraw in pgo\raw - build with scripts\pgo_build_instrumented.ps1, then play tools\pgo_train.bat."; exit 1 }
$gen = Join-Path $repo "build_pgo_gen\$game.exe"
if (Test-Path $gen) {
    $old = @($raw | Where-Object { $_.LastWriteTime -lt (Get-Item $gen).LastWriteTime })
    if ($old) { Write-Output "Note: $($old.Count) profile(s) predate build_pgo_gen\$game.exe (recorded with an older instrumented build; stale functions are ignored)." }
}
$out = Join-Path $repo "pgo\$tag.profdata"
Write-Output "Merging $($raw.Count) profile(s) ($([math]::Round(($raw | Measure-Object Length -Sum).Sum / 1MB)) MB) -> pgo\$tag.profdata"
# --failure-mode=warn: a profile cut short by a crash or a kill is skipped instead of failing the merge
& $profdata merge --failure-mode=warn -o $out @($raw.FullName)
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $out)) { Write-Output "llvm-profdata merge FAILED"; exit 1 }
$show = & $profdata show $out 2>&1 | Out-String
$show.Trim() -split "`n" | Where-Object { $_ -match 'Total functions|Maximum function count|Maximum internal block count' } | ForEach-Object { Write-Output "  $($_.Trim())" }
# the renderer must be in the profile, else build_rpcs3_pgo_use gets nothing from it (MSVC-mangled names: ...@rsx@@...)
$all = & $profdata show --all-functions $out 2>&1 | Select-String -SimpleMatch '@rsx@@' | Measure-Object | ForEach-Object Count
Write-Output "  renderer (rsx::) functions with counters: $all"
if ($all -eq 0) { Write-Output "  WARNING: no rsx:: functions in the profile - was build_pgo_gen built with the instrumented renderer (build_rpcs3_pgo_gen)?" }
if ($MergeOnly) { exit 0 }

# --- build (serialised with build_locked.ps1 through the same lock file) ---
$lock = Join-Path $repo 'scratch\BUILD.lock'
New-Item -ItemType Directory -Force (Split-Path $lock) | Out-Null
$t0 = Get-Date; $fs = $null
while (-not $fs) {
    try { $fs = [IO.File]::Open($lock, 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { if (((Get-Date) - $t0).TotalMinutes -gt $TimeoutMin) { Write-Output "LOCK TIMEOUT"; exit 2 }; Start-Sleep 10 }
}
try {
    $target = Join-Path $repo "build_pgo\$game.exe"
    while (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -ieq $target }) {
        if (((Get-Date) - $t0).TotalMinutes -gt $TimeoutMin) { Write-Output "build_pgo\$game.exe is still running - build skipped"; exit 3 }
        Start-Sleep 15
    }
    $env:PORT_JOBS = "$Jobs"
    if ($NoLto) { $env:PORT_LTO = '0' }
    # A new profile must reach every object. OBJECT_DEPENDS on the .profdata (both CMakeLists) does that per
    # object; on top of it, a changed profile cleans the two profile-optimised folders, so nothing built from
    # the previous profile can survive (also objects configured before the dependency existed).
    $hash = (Get-FileHash $out -Algorithm SHA256).Hash
    $stamp = Join-Path $repo "pgo\$tag.profdata.built.sha256"
    $last = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { '' }
    if ($hash -ne $last) {
        $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
        if (-not $cmake) { $cmake = @("$repo\tools_cache\cmake\bin\cmake.exe", "$env:ProgramFiles\CMake\bin\cmake.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1 }
        if (-not $cmake) { Write-Output "cmake not found (needed to clean the PGO folders)"; exit 1 }
        foreach ($d in 'build_pgo', 'build_rpcs3_pgo_use') {
            $bd = Join-Path $repo $d
            if (Test-Path (Join-Path $bd 'build.ninja')) {
                Write-Output "Profile changed: cleaning $d\"
                & $cmake --build $bd --target clean | Out-Null
                if ($LASTEXITCODE -ne 0) { Write-Output "cmake clean of $d FAILED"; exit 1 }
            }
        }
        Remove-Item $stamp -ErrorAction SilentlyContinue
    } else { Write-Output "Profile unchanged since the last pgo-use build (incremental build)." }
    $log = Join-Path $repo "scratch\pgo_use_$(Get-Date -f MMdd_HHmmss).log"
    Write-Output "Building build_pgo\$game.exe (build_cpu.bat pgo-use; log $log) ..."
    $p = Start-Process cmd.exe -ArgumentList ('/s /c ""' + "$repo\build_cpu.bat" + '" pgo-use nopause > "' + $log + '" 2>&1"') -WorkingDirectory $repo -PassThru -WindowStyle Hidden
    $p.WaitForExit()
    Get-Content $log -Tail 8
    if ($p.ExitCode -ne 0) { Write-Output "PGO build FAILED (exit $($p.ExitCode))"; exit $p.ExitCode }
    Set-Content $stamp $hash
    Write-Output "Done: $target"
    Write-Output "Run it with: set PORT_GAME_EXE=$target  (then run.bat / the launcher)"
} finally { $fs.Close() }
