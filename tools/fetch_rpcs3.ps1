# Downloads RPCS3's source at the pinned commit (plus the third-party code the RSX/Vulkan build needs) and applies
# this port's patch (ps3recomp\patches\*-rpcs3.patch). RPCS3 is GPL-2.0; see its LICENSE.
#
# Made to get through the ways this has failed on players' PCs (Resistance 2 port issue #13, Resistance 3 port #5):
#  - git's own messages are shown and kept in logs\fetch_rpcs3.log, followed by a plain-English hint;
#  - every download is tried again with a growing pause;
#  - RPCS3: shallow fetch of the pinned commit -> full fetch (protocol v2, HTTP/1.1) -> GitHub source archive (.zip);
#  - its submodules: all at once (shallow) -> one at a time (shallow) -> one at a time (full fetch, no --depth, for
#    servers that refuse "unadvertised object" requests) -> GitHub source archive of the exact pinned commit;
#  - every finished piece is stamped (.port_fetch\*.ok), so a run that stopped half way continues where it stopped
#    and a damaged or half-downloaded piece is cleaned and downloaded again;
#  - on exFAT/FAT32, network and other non-NTFS drives Git can't check who owns a repository, so the work is done in
#    a local NTFS folder (%LOCALAPPDATA%\PS3PortSetup) and the finished source (without .git) is copied in;
#  - "detected dubious ownership": that one folder is added to the global safe.directory list (git's own suggested
#    fix), and recorded in tools_cache\installed_by_setup.txt so "setup.ps1 -RemoveTools" can take it out again.
# Dot-source with -Library to load the functions without running anything (tools\test_setup_tooling.py does).
param(
    [string]$Dest = (Join-Path $PSScriptRoot '..\ps3recomp\third_party\rpcs3'),
    [string]$LogFile = (Join-Path $PSScriptRoot '..\logs\fetch_rpcs3.log'),
    [int]$Tries = 3,              # per download method
    [double]$RetryDelay = 5,      # seconds before the 2nd try; doubles after that
    [string]$StageRoot = '',      # where to work when the port is on a non-NTFS drive (default %LOCALAPPDATA%\PS3PortSetup)
    [switch]$NoStaging,           # work in place even on exFAT/FAT32/network drives
    [switch]$NoArchive,           # never fall back to GitHub's source archives
    [string]$Patch = '',          # default: the one ps3recomp\patches\*-rpcs3.patch
    [string]$Manifest = (Join-Path $PSScriptRoot '..\tools_cache\installed_by_setup.txt'),
    [switch]$Library
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell started from a PowerShell 7 window inherits pwsh's module paths and then cannot load its own
# modules (Get-FileHash 'not recognized'): keep only Windows PowerShell's own paths (the same block as in setup.ps1).
if ($PSVersionTable.PSVersion.Major -lt 6) {
    $pmp = @(($env:PSModulePath -split ';') | Where-Object { $_ -and $_ -notmatch '\\PowerShell\\(7[^\\]*\\)?Modules\\?$' })
    $pmpSys = Join-Path $PSHOME 'Modules'
    if (-not @($pmp | Where-Object { $_.TrimEnd('\') -eq $pmpSys }).Count) { $pmp += $pmpSys }
    $env:PSModulePath = $pmp -join ';'
}

# scripts\regen_patches.ps1 and tools\test_setup_tooling.py read this pin: keep the line as $script:Commit = '<40 hex>'
$script:Commit  = 'eee3f0f7c285f2b8aa18fec278a2146c5cd874eb'
$script:RepoUrl = 'https://github.com/RPCS3/rpcs3.git'
$script:RepoArchive = "https://codeload.github.com/RPCS3/rpcs3/zip/$script:Commit"
$script:MarkerRel = 'rpcs3\Emu\RSX\VK\VKR2Upscaler.cpp'   # created by the patch: present = finished
# The submodules the RSX/Vulkan build needs, pinned by RPCS3's commit above (Name = the name in its .gitmodules).
$script:Subs = @(
    @{ Path = '3rdparty/asmjit/asmjit';                 Name = 'asmjit';                                Repo = 'asmjit/asmjit';                                Sha = '416f7356967c1f66784dc1580fe157f9406d8bff' },
    @{ Path = '3rdparty/glslang/glslang';               Name = '3rdparty/glslang';                      Repo = 'KhronosGroup/glslang';                         Sha = 'f0bd0257c308b9a26562c1a30c4748a0219cc951' },
    @{ Path = '3rdparty/GPUOpen/VulkanMemoryAllocator'; Name = '3rdparty/GPUOpen/VulkanMemoryAllocator'; Repo = 'GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator'; Sha = '0f1a757da579831283413295cc7f3b47eb521c77' },
    @{ Path = '3rdparty/stblib/stb';                    Name = '3rdparty/stblib/stb';                   Repo = 'nothings/stb';                                 Sha = '013ac3beddff3dbffafd5177e7972067cd2b5083' },
    @{ Path = '3rdparty/yaml-cpp/yaml-cpp';             Name = '3rdparty/yaml-cpp';                     Repo = 'RPCS3/yaml-cpp';                               Sha = '51a5d623e3fde1f58829a56ba910f1cb33596222' }
)
# Not from the RPCS3 submodule set: pugixml 1.15 and Vulkan-Headers 1.4.344.
$script:Extras = @(
    @{ Path = '3rdparty/pugixml';        Key = 'pugixml';        Repo = 'zeux/pugixml';                Tag = 'v1.15';    Url = 'https://github.com/zeux/pugixml.git' },
    @{ Path = '3rdparty/Vulkan-Headers'; Key = 'vulkan-headers'; Repo = 'KhronosGroup/Vulkan-Headers'; Tag = 'v1.4.344'; Url = 'https://github.com/KhronosGroup/Vulkan-Headers.git' }
)
# Every setting is passed on the command line (git hands -c settings on to the git processes it starts for the
# submodules), so nothing depends on a repository's own config. safe.directory=* covers exFAT/USB/network folders
# on Git 2.39 and newer (older Git ignores it on the command line; see Repair-DubiousOwnership).
$script:GitCfg = @('-c', 'safe.directory=*', '-c', 'core.autocrlf=false', '-c', 'core.longpaths=true',
                   '-c', 'advice.detachedHead=false', '-c', 'init.defaultBranch=master')
$script:GitText = New-Object System.Collections.Generic.List[string]   # everything git said, for Get-FetchFailureHint
$script:SafeDirsAdded = @{}

function Write-FetchLog([string]$Text, [switch]$Quiet) {
    if (-not $Quiet) { Write-Host $Text }
    if ($script:LogFile) {
        try { Add-Content -LiteralPath $script:LogFile -Value ('{0:HH:mm:ss} {1}' -f (Get-Date), $Text) -Encoding UTF8 } catch {}
    }
}

# --- git and downloads (the tests replace Invoke-GitRaw and Get-UrlFile) -------------------------------------
function Invoke-GitRaw([string[]]$GitArgs) {
    # stderr is merged into the output and turned into plain text: with -ErrorAction Stop Windows PowerShell would
    # otherwise stop at git's first progress line, and without the merge the messages never reach Setup's log.
    $ErrorActionPreference = 'Continue'
    $cfg = $script:GitCfg
    $lines = @(& git @cfg @GitArgs 2>&1 | ForEach-Object { "$_" })
    [pscustomobject]@{ Code = $LASTEXITCODE; Lines = $lines }
}
function Invoke-Git {
    $gitArgs = @(foreach ($a in $args) { foreach ($b in @($a)) { "$b" } })   # arrays (submodule lists) flattened
    for ($attempt = 1; ; $attempt++) {
        Write-FetchLog "     > git $($gitArgs -join ' ')" -Quiet
        $r = Invoke-GitRaw $gitArgs
        foreach ($l in $r.Lines) { if ("$l".Trim()) { Write-FetchLog "     git: $l"; $script:GitText.Add("$l") } }
        if ($r.Code -eq 0) { return }
        if ($attempt -eq 1 -and (Repair-DubiousOwnership ($r.Lines -join "`n"))) { continue }
        throw "git $($gitArgs -join ' ') failed (exit code $($r.Code))"
    }
}
# git: "detected dubious ownership in repository at 'F:/x'" ... "git config --global --add safe.directory F:/x".
# Adds exactly the folder git names (never "*") and records it so it can be removed again.
function Repair-DubiousOwnership([string]$Text) {
    if ($Text -notmatch 'dubious ownership') { return $false }
    $dir = $null
    if ($Text -match "safe\.directory\s+'?([^'\r\n]+?)'?\s*(\r|\n|$)") { $dir = $Matches[1].Trim() }
    elseif ($Text -match "repository at '([^']+)'") { $dir = $Matches[1] }
    if (-not $dir -or $dir -eq '*' -or $script:SafeDirsAdded.ContainsKey($dir)) { return $false }
    $script:SafeDirsAdded[$dir] = $true
    Write-FetchLog "   Git doesn't trust the owner of $dir (common on exFAT/FAT32, USB and network drives)."
    Write-FetchLog "   Adding just that folder to Git's safe.directory list and trying again..."
    $r = Invoke-GitRaw @('config', '--global', '--add', 'safe.directory', $dir)
    if ($r.Code -ne 0) { return $false }
    Add-SetupManifest "gitconfig|safe.directory|$dir"
    return $true
}
function Add-SetupManifest([string]$Line) {
    $m = [IO.Path]::GetFullPath($script:Manifest)
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path $m) | Out-Null
        if (-not ((Test-Path -LiteralPath $m) -and (Get-Content -LiteralPath $m) -contains $Line)) { Add-Content -LiteralPath $m -Value $Line -Encoding UTF8 }
    } catch {}
}
function Get-UrlFile([string]$Url, [string]$OutFile) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $OutFile
}

# --- helpers --------------------------------------------------------------------------------------------------
function Invoke-WithRetry([string]$What, [scriptblock]$Action, [scriptblock]$Cleanup, [int]$Count = $script:Tries) {
    for ($i = 1; ; $i++) {
        try { & $Action; return }
        catch {
            if ($i -ge [Math]::Max(1, $Count)) { throw }
            $wait = $script:RetryDelay * [Math]::Pow(2, $i - 1)
            Write-FetchLog "   $What failed (try $i of $Count): $($_.Exception.Message)"
            Write-FetchLog "   Trying again in $wait s..."
            if ($Cleanup) { try { & $Cleanup } catch {} }
            if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
        }
    }
}
# Tries each @{ Name; Do; Cleanup; Tries } in turn; returns the Name of the one that worked.
function Invoke-FirstWorking([string]$What, [object[]]$Methods) {
    $errors = @()
    foreach ($m in $Methods) {
        if ($m.Cleanup) { try { & $m.Cleanup } catch {} }
        $n = $script:Tries; if ($m.ContainsKey('Tries')) { $n = $m.Tries }
        try { Invoke-WithRetry -What "$What ($($m.Name))" -Action $m.Do -Cleanup $m.Cleanup -Count $n; return $m.Name }
        catch {
            $errors += "$($m.Name): $($_.Exception.Message)"
            Write-FetchLog "   $What ($($m.Name)) did not work: $($_.Exception.Message)"
        }
    }
    throw "Could not download $What. Tried: $($errors -join '; ')"
}
function Clear-Dir([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        # git marks its pack files read-only; -Force removes them anyway
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}
function Test-DirHasFiles([string]$Path) {
    (Test-Path -LiteralPath $Path) -and [bool](Get-ChildItem -LiteralPath $Path -Force | Where-Object { $_.Name -ne '.git' } | Select-Object -First 1)
}
function Get-StampPath([string]$Work, [string]$Key) { Join-Path $Work (".port_fetch\" + ($Key -replace '[\\/:]', '_') + '.ok') }
function Test-Stamp([string]$Work, [string]$Key, [string]$Value) {
    $p = Get-StampPath $Work $Key
    (Test-Path -LiteralPath $p) -and ((Get-Content -LiteralPath $p -Raw).Trim() -eq $Value)
}
function Set-Stamp([string]$Work, [string]$Key, [string]$Value) {
    $p = Get-StampPath $Work $Key
    New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null
    Set-Content -LiteralPath $p -Value $Value -Encoding ASCII
}
function Remove-Stamp([string]$Work, [string]$Key) { Remove-Item -LiteralPath (Get-StampPath $Work $Key) -Force -ErrorAction SilentlyContinue }

# Downloads a GitHub source archive and unpacks it (without its top folder) into $Into.
function Expand-GitHubArchive([string]$Url, [string]$Into, [string]$DlDir) {
    New-Item -ItemType Directory -Force -Path $DlDir | Out-Null
    $zip = Join-Path $DlDir (($Url -replace '^https://[^/]+/', '' -replace '[^A-Za-z0-9._-]', '_') + '.zip')
    $ok = $false
    if (Test-Path -LiteralPath $zip) {   # from an earlier try: reuse it if it opens
        try { Add-Type -AssemblyName System.IO.Compression.FileSystem; ([IO.Compression.ZipFile]::OpenRead($zip)).Dispose(); $ok = $true }
        catch { Remove-Item -LiteralPath $zip -Force }
    }
    if (-not $ok) {
        Write-FetchLog "     downloading $Url"
        Get-UrlFile $Url "$zip.part"
        Move-Item -LiteralPath "$zip.part" -Destination $zip -Force
    }
    $tmp = "$Into.unzip"
    Clear-Dir $tmp
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $tmp)
    $top = @(Get-ChildItem -LiteralPath $tmp -Force)
    $src = $tmp; if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $src = $top[0].FullName }
    New-Item -ItemType Directory -Force -Path $Into | Out-Null
    Get-ChildItem -LiteralPath $src -Force | Move-Item -Destination $Into -Force
    Clear-Dir $tmp
}

# --- where to work --------------------------------------------------------------------------------------------
function Get-DriveFacts([string]$Path) {
    $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path))
    if ($root -like '\\*') { return @{ Root = $root; Type = 'Network'; Format = '' } }
    try { $d = New-Object IO.DriveInfo $root; return @{ Root = $root; Type = "$($d.DriveType)"; Format = "$($d.DriveFormat)" } }
    catch { return @{ Root = $root; Type = 'Unknown'; Format = '' } }
}
# $null when Git works normally there; otherwise a description of the drive.
function Get-DriveProblem([hashtable]$Facts) {
    if ($Facts.Type -eq 'Network') { return "a network drive or folder ($($Facts.Root))" }
    if ($Facts.Format -and $Facts.Format -notin 'NTFS', 'ReFS') {
        return "a $($Facts.Format) drive ($($Facts.Root); exFAT and FAT32 are usual on USB sticks and external disks)"
    }
    return $null
}
function Get-StageDir([string]$Dest, [string]$Root) {
    if (-not $Root) { $Root = Join-Path $env:LOCALAPPDATA 'PS3PortSetup' }
    $md5 = [Security.Cryptography.MD5]::Create()
    $h = -join ($md5.ComputeHash([Text.Encoding]::UTF8.GetBytes($Dest.ToLowerInvariant())) | Select-Object -First 6 | ForEach-Object { $_.ToString('x2') })
    Join-Path $Root "rpcs3_$h"
}

# --- the pieces -----------------------------------------------------------------------------------------------
function Get-FetchMode([string]$Work) {
    $p = Get-StampPath $Work 'base'
    if (Test-Path -LiteralPath $p) { return ((Get-Content -LiteralPath $p -Raw).Trim() -split '\|')[1] }
    return $null
}
function Get-Rpcs3Base([string]$Work, [string]$DlDir) {
    foreach ($mode in 'git', 'archive') {
        if ((Test-Stamp $Work 'base' "$script:Commit|$mode") -and (Test-Path -LiteralPath (Join-Path $Work 'rpcs3'))) {
            Write-FetchLog "   RPCS3 $($script:Commit.Substring(0,7)): already downloaded."
            return $mode
        }
    }
    Write-FetchLog "   Downloading RPCS3 $($script:Commit.Substring(0,7))..."
    $init = {
        # also cleans what a stopped or failed run left behind
        Clear-Dir $Work
        Invoke-Git init -q $Work
        Invoke-Git -C $Work config core.autocrlf false   # for anyone running git in this folder later
        Invoke-Git -C $Work remote add origin $script:RepoUrl
    }
    $methods = @(
        @{ Name = 'shallow fetch'; Cleanup = $init; Do = {
            Invoke-Git -C $Work fetch -q --depth 1 --no-tags origin $script:Commit
            Invoke-Git -C $Work checkout -q -f FETCH_HEAD } },
        @{ Name = 'full fetch'; Cleanup = $init; Tries = 2; Do = {
            Write-FetchLog "   Downloading RPCS3's full history instead (larger, can take several minutes)..."
            Invoke-Git -C $Work -c protocol.version=2 -c http.version=HTTP/1.1 fetch -q --no-tags origin
            Invoke-Git -C $Work checkout -q -f $script:Commit } }
    )
    if (-not $script:NoArchive) {
        $methods += @{ Name = 'source archive'; Cleanup = { Clear-Dir $Work }; Do = {
            Write-FetchLog "   Downloading RPCS3's source archive from GitHub instead..."
            Expand-GitHubArchive $script:RepoArchive $Work $DlDir
            Invoke-Git init -q $Work } }   # its own repository, so git apply works there like anywhere else
    }
    $how = Invoke-FirstWorking 'RPCS3' $methods
    $mode = 'git'; if ($how -eq 'source archive') { $mode = 'archive' }
    Set-Stamp $Work 'base' "$script:Commit|$mode"
    return $mode
}
function Clear-Submodule([string]$Work, [hashtable]$S) {
    Clear-Dir (Join-Path $Work ($S.Path -replace '/', '\'))
    Clear-Dir (Join-Path $Work (".git\modules\" + ($S.Name -replace '/', '\')))
}
function Get-Submodules([string]$Work, [string]$Mode, [string]$DlDir) {
    $pending = @($script:Subs | Where-Object {
        -not ((Test-Stamp $Work "sub_$($_.Path)" $_.Sha) -and (Test-DirHasFiles (Join-Path $Work ($_.Path -replace '/', '\')))) })
    if (-not $pending.Count) { Write-FetchLog "   Third-party code: already downloaded."; return }
    Write-FetchLog "   Downloading $($pending.Count) third-party libraries..."
    foreach ($s in $pending) { Clear-Submodule $Work $s }   # leftovers of a stopped run
    if ($Mode -eq 'git' -and $pending.Count -gt 1) {
        try {
            Invoke-Git -C $Work submodule update -q --init --depth 1 --jobs 5 '--' @($pending | ForEach-Object { $_.Path })
            foreach ($s in $pending) { Set-Stamp $Work "sub_$($s.Path)" $s.Sha }
            return
        } catch {
            Write-FetchLog "   Downloading them together did not work ($($_.Exception.Message)); trying one at a time..."
        }
    }
    foreach ($s in $pending) {
        $sub = $s; $methods = @()
        $clean = { Clear-Submodule $Work $sub }
        if ($Mode -eq 'git') {
            $methods += @{ Name = 'shallow'; Cleanup = $clean; Do = { Invoke-Git -C $Work -c http.version=HTTP/1.1 submodule update -q --init --depth 1 '--' $sub.Path } }
            $methods += @{ Name = 'full fetch'; Cleanup = $clean; Tries = 1; Do = { Invoke-Git -C $Work -c protocol.version=2 -c http.version=HTTP/1.1 submodule update -q --init '--' $sub.Path } }
        }
        if (-not $script:NoArchive) {
            $url = "https://codeload.github.com/$($sub.Repo)/zip/$($sub.Sha)"
            $methods += @{ Name = 'source archive'; Cleanup = $clean; Do = { Expand-GitHubArchive $url (Join-Path $Work ($sub.Path -replace '/', '\')) $DlDir } }
        }
        [void](Invoke-FirstWorking $sub.Path $methods)
        Set-Stamp $Work "sub_$($sub.Path)" $sub.Sha
    }
}
function Get-Extras([string]$Work, [string]$DlDir) {
    foreach ($e in $script:Extras) {
        $x = $e; $dir = Join-Path $Work ($x.Path -replace '/', '\')
        if ((Test-Stamp $Work $x.Key $x.Tag) -and (Test-DirHasFiles $dir)) { continue }
        $clean = { Clear-Dir $dir }
        $methods = @(@{ Name = 'clone'; Cleanup = $clean; Do = { Invoke-Git clone -q --depth 1 --branch $x.Tag $x.Url $dir } })
        if (-not $script:NoArchive) {
            $url = "https://codeload.github.com/$($x.Repo)/zip/refs/tags/$($x.Tag)"
            $methods += @{ Name = 'source archive'; Cleanup = $clean; Do = { Expand-GitHubArchive $url $dir $DlDir } }
        }
        [void](Invoke-FirstWorking "$($x.Repo) $($x.Tag)" $methods)
        Set-Stamp $Work $x.Key $x.Tag
    }
}
# Files the patch creates ("--- /dev/null" followed by "+++ b/<path>").
function Get-PatchNewFiles([string]$Patch) {
    $prev = ''; $out = @()
    foreach ($l in [IO.File]::ReadLines($Patch)) {
        if ($prev -eq '--- /dev/null' -and $l -match '^\+\+\+ b/(.+?)\s*$') { $out += $Matches[1] }
        $prev = $l
    }
    $out
}
function Test-GitOk([string[]]$GitArgs) { (Invoke-GitRaw $GitArgs).Code -eq 0 }
# $true when the patch is applied; $false when the source is damaged and has to be downloaded again.
function Add-PortPatch([string]$Work, [string]$Mode, [string]$Patch) {
    $hash = (Get-FileHash -LiteralPath $Patch -Algorithm SHA256).Hash
    if ((Test-Stamp $Work 'patch' $hash) -and (Test-Path -LiteralPath (Join-Path $Work $script:MarkerRel))) { return $true }
    $check = @('-C', $Work, 'apply', '--check', '--whitespace=nowarn', $Patch)
    if (-not (Test-GitOk $check)) {
        if (Test-GitOk @('-C', $Work, 'apply', '-R', '--check', '--whitespace=nowarn', $Patch)) {   # applied by an earlier run
            Set-Stamp $Work 'patch' $hash; return $true
        }
        Write-FetchLog "   The port's patch does not fit the downloaded source (a stopped earlier run?). Resetting it..."
        foreach ($f in Get-PatchNewFiles $Patch) { Remove-Item -LiteralPath (Join-Path $Work ($f -replace '/', '\')) -Force -ErrorAction SilentlyContinue }
        if ($Mode -ne 'git' -or -not (Test-GitOk @('-C', $Work, 'checkout', '-q', '-f', 'HEAD', '--', '.')) -or -not (Test-GitOk $check)) { return $false }
    }
    Invoke-Git -C $Work apply --whitespace=nowarn $Patch
    Set-Stamp $Work 'patch' $hash
    return $true
}

# --- why it failed --------------------------------------------------------------------------------------------
function Get-FetchFailureHint([string]$Text) {
    $h = @()
    if ($Text -match 'dubious ownership|not in a git directory|safe\.directory') {
        $h += "Git can't check who owns the folder. This happens on exFAT/FAT32 drives (USB sticks, external disks) and network drives: move the port folder to an NTFS drive, e.g. C:\Games, and run Setup again." }
    if ($Text -match "Could not resolve (host|proxy)|Couldn't resolve|getaddrinfo") {
        $h += "This PC could not look up github.com: no internet, or DNS, a VPN or a firewall is blocking it. Check that https://github.com opens in a browser." }
    if ($Text -match 'Failed to connect|Connection (timed out|refused|reset)|Operation timed out|Timed out') {
        $h += "The connection to github.com was blocked or timed out: a firewall, VPN, proxy or antivirus may be blocking git.exe." }
    if ($Text -match '(?i)proxy|\b407\b') {
        $h += "A proxy is involved. If your network needs one, tell Git about it: git config --global http.proxy http://host:port" }
    if ($Text -match '(?i)SSL|schannel|certificate|TLS') {
        $h += "The secure connection to GitHub failed. Usual causes: antivirus 'HTTPS/web scanning', a company proxy, or a wrong date/time on the PC." }
    if ($Text -match 'unadvertised object|not our ref|upload-pack: not our ref') {
        $h += "The server refused to send that exact version on its own (some proxies and mirrors do). Setup also tried full downloads and GitHub's source archives." }
    if ($Text -match 'RPC failed|early EOF|curl \d+|HTTP/2 stream|unexpected disconnect|remote end hung up|fetch-pack: invalid index-pack') {
        $h += "The connection dropped in the middle of a download (unstable Wi-Fi, VPN or provider). Run Setup again: it continues where it stopped." }
    if ($Text -match '(?i)Filename too long|path too long') {
        $h += "A path got too long for Windows. Move the port to a short folder such as C:\Games\Port and run Setup again." }
    if ($Text -match '(?i)No space left|not enough space|disk full|ENOSPC') {
        $h += "The drive is full. Free some space (the RPCS3 source needs about 1 GB while downloading) and run Setup again." }
    if ($Text -match '(?i)Permission denied|Access is denied|unable to (create|unlink)|cannot lock|being used by another process|Device or resource busy') {
        $h += "Windows would not let Git write a file. Antivirus or OneDrive/cloud sync is often holding it: keep the port out of OneDrive/Documents, or exclude the folder in your antivirus." }
    if (-not $h) { $h += "Check that https://github.com opens in a browser, then run Setup again (it continues where it stopped)." }
    $h
}

# --- the whole job --------------------------------------------------------------------------------------------
function Invoke-FetchRpcs3 {
    $Dest = [IO.Path]::GetFullPath($script:Dest)
    if ($script:LogFile) {
        $script:LogFile = [IO.Path]::GetFullPath($script:LogFile)
        try { New-Item -ItemType Directory -Force -Path (Split-Path $script:LogFile) | Out-Null } catch { $script:LogFile = '' }
    }
    if (Test-Path -LiteralPath (Join-Path $Dest $script:MarkerRel)) { Write-Host "   RPCS3 source already present."; return }
    $Patch = $script:Patch
    if (-not $Patch) {
        $patches = @(Get-ChildItem -Path (Join-Path $PSScriptRoot '..\ps3recomp\patches') -Filter '*-rpcs3.patch' -File)
        if ($patches.Count -ne 1) { throw "Expected exactly one ps3recomp\patches\*-rpcs3.patch, found $($patches.Count)." }
        $Patch = $patches[0].FullName
    }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "git was not found. Install Git (winget install Git.Git) and run Setup again." }
    Write-FetchLog "   ---- fetch_rpcs3 $(Get-Date -Format s) into $Dest" -Quiet
    Write-FetchLog "   $((Invoke-GitRaw @('--version')).Lines -join ' ')" -Quiet

    $Work = $Dest
    $problem = Get-DriveProblem (Get-DriveFacts $Dest)
    if ($problem -and -not $script:NoStaging) {
        $Work = Get-StageDir $Dest $script:StageRoot
        if (Get-DriveProblem (Get-DriveFacts $Work)) { $Work = $Dest }
        else {
            Write-FetchLog "   The port is on $problem. Git can't check who owns repositories there,"
            Write-FetchLog "   so the source is downloaded to $Work first and then copied into the port."
        }
    } elseif ($problem) { Write-FetchLog "   Note: the port is on $problem; if Git fails, move the port to an NTFS drive such as C:\Games." }
    $DlDir = Join-Path $PSScriptRoot '..\tools_cache\rpcs3_download'   # source archives, only if git can't fetch
    if ($Work -ne $Dest) { $DlDir = "$Work.download" }
    $DlDir = [IO.Path]::GetFullPath($DlDir)

    for ($round = 1; ; $round++) {
        $mode = Get-Rpcs3Base $Work $DlDir
        Get-Submodules $Work $mode $DlDir
        Get-Extras $Work $DlDir
        if (Add-PortPatch $Work $mode $Patch) { break }
        if ($round -ge 2) { throw "The port's patch does not apply to the downloaded RPCS3 source." }
        Write-FetchLog "   Downloading RPCS3 again..."
        Clear-Dir $Work
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Work $script:MarkerRel))) { throw "The patched source is incomplete ($script:MarkerRel is missing)." }

    if ($Work -ne $Dest) {
        Write-FetchLog "   Copying the source into the port folder..."
        Clear-Dir $Dest
        & robocopy $Work $Dest /E /XD .git .port_fetch /XF .git /R:2 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Could not copy the source from $Work to $Dest (robocopy exit code $LASTEXITCODE)." }
        $global:LASTEXITCODE = 0
        Clear-Dir $Work
    }
    Clear-Dir $DlDir
    $tc = Split-Path $DlDir   # an empty tools_cache\ is not left behind
    if ((Split-Path -Leaf $tc) -eq 'tools_cache' -and (Test-Path -LiteralPath $tc) -and -not (Get-ChildItem -LiteralPath $tc -Force)) { Remove-Item -LiteralPath $tc -Force -ErrorAction SilentlyContinue }
    Write-FetchLog "   RPCS3 $($script:Commit.Substring(0,7)) downloaded and patched."
}

if ($Library) { return }
try { Invoke-FetchRpcs3; exit 0 }
catch {
    Write-FetchLog "   FAILED: $($_.Exception.Message)"
    Write-FetchLog "   What probably went wrong:"
    foreach ($h in Get-FetchFailureHint (($script:GitText -join "`n") + "`n" + $_.Exception.Message)) { Write-FetchLog "    - $h" }
    if ($script:LogFile) { Write-FetchLog "   Everything Git said is in $script:LogFile" }
    exit 1
}
