# EVBots stand: shared paths, flags and helpers. Dot-source it: . "$PSScriptRoot\common.ps1"
# ASCII only: Windows PowerShell 5.1 reads BOM-less scripts as ANSI.
# 16.10.2026 (build 170): replaces the scattered trash\2026-10-1x scripts (run_bare, start_run, join_client,
# finish_run, run_cycle). Old scripts are kept in trash untouched.
$OutputEncoding = [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Continue'

$Kit        = 'C:\Games\EVBots-dev\stand'
$Game       = 'C:\Games\BF-Portable\Battlefield_3_Server'
$Instance   = $Game + '\Instance'
$ClientGame = 'C:\Games\Battlefield 3'
$Log        = $Kit + '\logs\EVBots.log'
$StateFile  = $Kit + '\logs\run-state.json'
$BuildDll   = 'C:\Games\EVBots-dev\evbots-dll\build\EVBots.dll'
$ActDll     = $Kit + '\activation\EVBots.dll'
# 23.10: ev-core.dll - EVBots core (analogue of vu-core.dll): server window console/panel; loaded before EVBots.dll.
$CoreBuild  = 'C:\Games\EVBots-dev\ev-core\build\ev-core.dll'
$CoreDll    = $Kit + '\activation\ev-core.dll'
# 23.10: own EVBots loader (EVBots-dev\loader, sources there): several DLLs per process, client gets activation.dll first.
# The old activation-loader.exe (one DLL, no sources) stays in activation\ as a fallback.
$Loader     = $Kit + '\activation\evbots-loader.exe'
$Today      = (Get-Date).ToString('yyyy-MM-dd')
$RunDir     = 'C:\Games\trash\' + $Today + '\run'      # outputs of this day's runs (temporary -> trash)
$BackupDir  = 'C:\Games\backup\' + $Today              # rule 15: working DLL before every deploy
# Network ports (defaults match a single local stand). Change these to run another stack in parallel.
$BackendHttpPort = 8080
$RedirectorPort  = 42127
$BlazePort       = 10041
$WebPort         = 80
$GamePort        = 25300
$RemoteAdminPort = 47900
$ServerAddr      = '127.0.0.1:' + $GamePort
$Http            = 'http://127.0.0.1:' + $BackendHttpPort
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

# 26.10 (build 238): engine frequency like VU -high60/-high120 (ev-core EvFreq.cpp): run\frequency.txt holds 30, 60 or 120.
# No file or anything else -> 30 (regular). Server and client get it as EVBOTS_FREQUENCY.
$Frequency = '30'
$FrequencyFile = Join-Path $PSScriptRoot 'frequency.txt'
if (Test-Path $FrequencyFile) {
    $fq = ([string](Get-Content $FrequencyFile -TotalCount 1)).Trim()
    if ($fq -eq '60' -or $fq -eq '120') { $Frequency = $fq }
}

# Server env flags that the code still reads (checked against src after builds 168-169).
# Dead flags dropped: AI_PLAYER, DUMP_INPUT, SPM_SPAWN, SPM_POINTS. SPM_CUSTOM / PTS_REG / SQUAD_HOOK are on by default.
$ServerEnv = [ordered]@{
    'BF3ACT_MODE'           = 'dedicated'
    'BF3ACT_DRM'            = '0'
    'BF3ACT_LOG'            = $Log
    'BF3ACT_DNSREDIR'       = 'gosredirector.ea.com,gosredirector.online.ea.com,gosredirector.scert.ea.com,gosredirector.stest.ea.com,bf3.gos.ea.com'  # 20.10: + bf3.gos.ea.com (server.xml from backend on 127.0.0.1:80)
    'EVBOTS_LOG_VERBOSE' = '1'   # 22.10 (build 211): full diagnostic log on the stand; friends' installs log quietly
    'EVBOTS_BRIDGE'      = '1'   # Lua -> engine bridge (real engine players for bots)
    'EVBOTS_BRIDGE_ENGINE' = '1'
    'EVBOTS_SPM_PROBE'   = '0'
    'EVBOTS_SPM_MGR'     = '1'   # enables SpmProbe (manager pointer)
    'EVBOTS_SPM_TICK'    = '1'   # ServerPlayerManager::update hook: bridge tick
    'EVBOTS_SPM_SOLDIER' = '1'   # spawnSoldier hook: spawn reference
    'EVBOTS_SPM_SQUAD'   = '0'
    'EVBOTS_SPM_ONCREATE' = '0'  # crashes with bots (12.10), keep off
    'EVBOTS_SPM_CREATE'  = '0'
    'EVBOTS_LIVE_SEARCH' = '1'   # ResourceManager search from the live engine database
    'EVBOTS_EBX_INDEX'   = ($Kit + '\activation\ebx_index.bin')
    'EVBOTS_FREQUENCY'   = $Frequency   # build 238: 30 / 60 / 120 Hz (run\frequency.txt)
}

function Write-Step([string]$msg) { Write-Output ((Get-Date).ToString('HH:mm:ss') + '  ' + $msg) }

function Get-Proc([string]$name) { Get-Process -Name $name -ErrorAction SilentlyContinue | Select-Object -First 1 }

function Get-FileMd5([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    return (Get-FileHash -Algorithm MD5 -Path $path).Hash.ToLower()
}

function Get-LogLength { if (Test-Path $Log) { return (Get-Item $Log).Length } else { return 0 } }

# Reads the log from a byte offset without locking it (the server keeps it open).
# If the file got rotated (shorter than the offset), reads from the start.
function Read-LogSince([long]$offset) {
    if (-not (Test-Path $Log)) { return @() }
    $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
    $fs = [System.IO.File]::Open($Log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
    try {
        if ($offset -gt $fs.Length) { $offset = 0 }
        [void]$fs.Seek($offset, [System.IO.SeekOrigin]::Begin)
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
        $text = $sr.ReadToEnd()
    } finally { $fs.Dispose() }
    return ($text -split "`r?`n")
}

function Save-RunState([hashtable]$s) { ($s | ConvertTo-Json) | Set-Content -Path $StateFile -Encoding UTF8 }

function Get-RunState {
    if (-not (Test-Path $StateFile)) { return $null }
    try { return (Get-Content $StateFile -Raw -Encoding UTF8 | ConvertFrom-Json) } catch { return $null }
}

function Get-Json([string]$path) {
    try {
        $r = Invoke-WebRequest -Uri ($Http + $path) -TimeoutSec 3 -UseBasicParsing
        return ([regex]::Match($r.Content, '(?s)\{.*\}')).Value | ConvertFrom-Json
    } catch { return $null }
}

# Waits until the scriptblock returns a truthy value; returns that value or $null on timeout.
function Wait-For([scriptblock]$cond, [int]$timeoutSec, [int]$stepMs = 1000) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $v = & $cond
        if ($v) { return $v }
        Start-Sleep -Milliseconds $stepMs
    }
    return $null
}

# Stops server and backend and waits until they are really gone (the DLL lock goes away with the server).
function Stop-Stand {
    foreach ($n in 'bf3_server_Final', 'EV Backend') {
        Get-Process -Name $n -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force }
    }
    $gone = Wait-For { -not (Get-Proc 'bf3_server_Final') -and -not (Get-Proc 'EV Backend') } 20 500
    return [bool]$gone
}

# Build 267: profile helpers are side-effect free until Backup/Write is called.
# Tests load these function ASTs without dot-sourcing this file's live paths.
function Get-StandRunningClient {
    foreach ($name in @('bf3', 'zbf3')) { $client = Get-Proc $name; if ($client) { return $client } }
    return $null
}

function Get-StandModProfile([string]$Name, [string]$ModsRoot, [string]$UiAssets) {
    if ($Name -eq 'FunBots') { $Name = 'Standard' }
    if ($Name -notin @('Standard', 'Royale')) { throw ('Unknown mod profile: ' + $Name) }
    if (-not $ModsRoot) {
        $ModsRoot = if ($Name -eq 'Royale') { 'D:\Games-cache\royale\working\Mods' } else { $Instance + '\Admin\Mods' }
    }
    if ($Name -eq 'Royale' -and -not $UiAssets) { $UiAssets = 'D:\Games-cache\royale\ui-assets-csharp' }
    [pscustomobject]@{
        Name = $Name
        ModsRoot = [IO.Path]::GetFullPath($ModsRoot).TrimEnd('\')
        UiAssets = $(if ($Name -eq 'Royale') { [IO.Path]::GetFullPath($UiAssets).TrimEnd('\') } else { '' })
        # Royale depends on VEManager and VU-BattleRoyale-Compass (mod.json). Loader resolves deps, but list explicitly.
        ActiveMods = $(if ($Name -eq 'Royale') { 'VEManager;VU-BattleRoyale-Compass;VU-BattleRoyale' } else { 'fun-bots' })
    }
}

function Assert-StandFile([string]$Path) {
    if (-not [IO.File]::Exists($Path) -or (Get-Item -LiteralPath $Path -ErrorAction Stop).Length -eq 0) {
        throw ('Required file is missing or empty: ' + $Path)
    }
}

function Read-StandJson([string]$Path) {
    Assert-StandFile $Path
    return ([IO.File]::ReadAllText($Path, (New-Object Text.UTF8Encoding($false, $true))) | ConvertFrom-Json -ErrorAction Stop)
}

# VU Docs caret: same major and >= lower bound, including 0.x (not npm caret).
function Test-StandVersion([string]$Actual, [string]$Required) {
    $parse = {
        param([string]$value)
        if ($value -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$') { throw ('Invalid SemVer: ' + $value) }
        $pre = @(); if ($Matches[4]) { $pre = @($Matches[4] -split '\.') }
        foreach ($part in $pre) { if ($part -match '^0[0-9]+$') { throw ('Invalid SemVer: ' + $value) } }
        [pscustomobject]@{ Core = @([decimal]$Matches[1], [decimal]$Matches[2], [decimal]$Matches[3]); Pre = $pre }
    }
    $a = & $parse $Actual
    $op = ''; $text = $Required.Trim()
    if ($text.StartsWith('^')) { $op = '^'; $text = $text.Substring(1).Trim() }
    $b = & $parse $text
    $cmp = 0
    for ($i = 0; $i -lt 3 -and $cmp -eq 0; $i++) { $cmp = $a.Core[$i].CompareTo($b.Core[$i]) }
    if ($cmp -eq 0) {
        if ($a.Pre.Count -eq 0 -and $b.Pre.Count -gt 0) { $cmp = 1 }
        elseif ($b.Pre.Count -eq 0 -and $a.Pre.Count -gt 0) { $cmp = -1 }
        else {
            for ($i = 0; $i -lt [Math]::Min($a.Pre.Count, $b.Pre.Count) -and $cmp -eq 0; $i++) {
                $an = $a.Pre[$i] -match '^[0-9]+$'; $bn = $b.Pre[$i] -match '^[0-9]+$'
                if ($an -and $bn) { $cmp = ([decimal]$a.Pre[$i]).CompareTo([decimal]$b.Pre[$i]) }
                elseif ($an -ne $bn) { $cmp = if ($an) { -1 } else { 1 } }
                else { $cmp = [string]::CompareOrdinal($a.Pre[$i], $b.Pre[$i]) }
            }
            if ($cmp -eq 0) { $cmp = $a.Pre.Count.CompareTo($b.Pre.Count) }
        }
    }
    if ($op -eq '^') { return $a.Core[0] -eq $b.Core[0] -and $cmp -ge 0 }
    return $cmp -eq 0
}

function Assert-StandVuic([string]$Path) {
    Assert-StandFile $Path
    $stream = [IO.File]::OpenRead($Path)
    $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($stream.Length -lt 12 -or $stream.Length -gt 268435456 -or
            [Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne 'CIUV' -or $reader.ReadUInt32() -ne 2) { throw ('Invalid CIUV v2: ' + $Path) }
        $count = $reader.ReadUInt32()
        if ($count -eq 0 -or $count -gt 100000 -or 12L + 20L * $count -gt $stream.Length) { throw ('Invalid UI index: ' + $Path) }
        for ($i = 0; $i -lt $count; $i++) {
            [void]$reader.ReadUInt32(); $stored = $reader.ReadUInt32(); [void]$reader.ReadUInt32(); [void]$reader.ReadUInt32(); $offset = $reader.ReadUInt32()
            if ([long]$offset + $stored -gt $stream.Length) { throw ('Truncated UI archive: ' + $Path) }
        }
    } finally { $reader.Dispose(); $stream.Dispose() }
}

function Assert-StandUiAssets([string]$Root) {
    $manifest = Read-StandJson (Join-Path $Root 'manifest.json')
    if ($manifest.Schema -ne 1 -or $manifest.Game -ne $ClientGame -or -not $manifest.Images -or -not $manifest.Inputs) {
        throw ('Invalid or foreign game UI cache: ' + $Root)
    }
    foreach ($inputFile in $manifest.Inputs) {
        $file = Get-Item -LiteralPath $inputFile.Path -ErrorAction Stop
        if ($file.PSIsContainer -or $file.Length -ne $inputFile.Length -or $file.LastWriteTimeUtc.Ticks -ne $inputFile.Modified) { throw ('Stale UI cache input: ' + $inputFile.Path) }
    }
    foreach ($icon in $manifest.Images) {
        if ($icon.Name -notmatch '^ui/[a-zA-Z0-9_/ -]+$' -or $icon.Name.Contains('//') -or
            @($icon.Name.Split('/') | Where-Object { $_ -ne $_.Trim() }).Count -gt 0 -or
            $icon.Sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Invalid UI cache image entry' }
        $path = Join-Path $Root ($icon.Name.Replace('/', '\') + '.png')
        Assert-StandFile $path
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256 -ErrorAction Stop).Hash -ne $icon.Sha256) { throw ('UI cache hash mismatch: ' + $path) }
    }
}

function Assert-StandProfileFiles($Profile) {
    if ($Profile.Name -eq 'Standard') {
        if (-not (Test-Path -LiteralPath (Join-Path $Profile.ModsRoot 'fun-bots') -PathType Container)) { throw ('No fun-bots in ' + $Profile.ModsRoot) }
        return
    }
    $folders = @('VU-BattleRoyale', 'VEManager', 'VU-BattleRoyale-Compass')
    $mods = @{}
    foreach ($folder in $folders) {
        $dir = Join-Path $Profile.ModsRoot $folder
        $mod = Read-StandJson (Join-Path $dir 'mod.json')
        if ($mod.HasVeniceEXT -isnot [bool] -or -not $mod.HasVeniceEXT -or $mod.Version -isnot [string] -or
            -not (Test-StandVersion $mod.Version $mod.Version)) { throw ('Invalid mod manifest: ' + $folder) }
        foreach ($side in @('client', 'server')) { Assert-StandFile (Join-Path $dir ('ext\' + $side + '\__init__.lua')) }
        if ($folder -ne 'VEManager') {
            if ($mod.HasWebUI -isnot [bool] -or -not $mod.HasWebUI) { throw ('HasWebUI must be true: ' + $folder) }
            Assert-StandVuic (Join-Path $dir 'ui.vuic')
        } elseif ($mod.HasWebUI -eq $true) { Assert-StandVuic (Join-Path $dir 'ui.vuic') }
        $mods[$folder] = $mod
    }
    Assert-StandFile (Join-Path $Profile.ModsRoot 'VU-BattleRoyale\ext\shared\__init__.lua')
    foreach ($required in @('VEManager', 'VU-BattleRoyale-Compass')) {
        if (-not $mods['VU-BattleRoyale'].Dependencies.$required) { throw ('VU-BattleRoyale must declare dependency: ' + $required) }
    }
    foreach ($folder in $folders) {
        $deps = $mods[$folder].Dependencies
        if ($null -eq $deps) { continue }
        if ($deps -isnot [pscustomobject]) { throw ('Invalid Dependencies: ' + $folder) }
        foreach ($dep in $deps.PSObject.Properties) {
            if ($dep.Name -eq 'veniceext') { $version = '1.12.0' }
            elseif ($folder -eq 'VU-BattleRoyale' -and $dep.Name -in @('VEManager', 'VU-BattleRoyale-Compass')) { $version = $mods[$dep.Name].Version }
            else { throw ('Unsupported/cyclic dependency in stand profile: ' + $folder + ' -> ' + $dep.Name) }
            if ($dep.Value -isnot [string] -or -not (Test-StandVersion $version $dep.Value)) { throw ('Dependency mismatch: ' + $folder + ' -> ' + $dep.Name + ' ' + $dep.Value) }
        }
    }
    Assert-StandUiAssets $Profile.UiAssets
}

function Assert-StandConfigBackup([string]$Admin, [string]$Directory) {
    $manifest = Read-StandJson (Join-Path $Directory 'backup.json')
    if ($manifest.admin -ne [IO.Path]::GetFullPath($Admin)) { throw 'Config backup belongs to another Admin directory' }
    foreach ($name in @('Startup.txt', 'MapList.txt', 'ModList.txt')) {
        $path = Join-Path $Directory $name
        if (-not [IO.File]::Exists($path) -or -not $manifest.hashes.$name -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256 -ErrorAction Stop).Hash -ne $manifest.hashes.$name) { throw ('Missing/damaged config backup: ' + $path) }
    }
}

function Get-StandConfigBackup([string]$Admin, $Previous) {
    $path = [string]$Previous.savedConfigBackup
    if (-not $path) { $path = [string]$Previous.funBotsBackup }
    if ($path) { Assert-StandConfigBackup $Admin $path; return $path }
    $startup = Join-Path $Admin 'Startup.txt'
    $marked = [IO.File]::Exists($startup) -and [IO.File]::ReadAllText($startup).Contains('# EVBots stand profile: Royale')
    if ($marked -or ($Previous.modProfile -eq 'Royale' -and -not $Previous.isolatedServer)) { throw 'Royale Admin has no recorded config backup; refusing to replace it' }
    foreach ($name in @('Startup.txt', 'MapList.txt', 'ModList.txt')) {
        if (-not [IO.File]::Exists((Join-Path $Admin $name))) { throw ('Missing Admin config: ' + $name) }
    }
    return ''
}

function Backup-StandSavedConfig([string]$Admin, [string]$Parent) {
    $dir = Join-Path $Parent ('royale\stand-config-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Path $dir -ErrorAction Stop | Out-Null
    $hashes = @{}
    foreach ($name in @('Startup.txt', 'MapList.txt', 'ModList.txt')) {
        $to = Join-Path $dir $name
        Copy-Item -LiteralPath (Join-Path $Admin $name) -Destination $to -ErrorAction Stop
        $hashes[$name] = (Get-FileHash -LiteralPath $to -Algorithm SHA256 -ErrorAction Stop).Hash
    }
    [IO.File]::WriteAllText((Join-Path $dir 'backup.json'), (@{admin=[IO.Path]::GetFullPath($Admin);hashes=$hashes} | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    Assert-StandConfigBackup $Admin $dir
    return $dir
}

function Write-StandUtf8Lines([string]$Path, [string[]]$Lines) {
    $bom = $false
    if ([IO.File]::Exists($Path)) {
        $raw = [IO.File]::ReadAllBytes($Path)
        $bom = $raw.Length -ge 3 -and $raw[0] -eq 239 -and $raw[1] -eq 187 -and $raw[2] -eq 191
    }
    [IO.File]::WriteAllLines($Path, $Lines, (New-Object Text.UTF8Encoding($bom)))
}

function Write-StandProfileConfig($Profile, [string]$Admin, [string]$SavedConfigBackup, [int]$Tickets) {
    if ($Profile.Name -eq 'Royale') {
        Write-StandUtf8Lines (Join-Path $Admin 'Startup.txt') @(
            '# EVBots stand profile: Royale', 'vars.serverName EVBots', 'vars.maxPlayers 64',
            'admin.password change-this-rcon-password', 'vars.idleTimeout 0', 'vars.autoBalance false', 'vars.unlockMode all',
            'vars.gameModeCounter 100', 'vars.ctfRoundTimeModifier 100')
        Write-StandUtf8Lines (Join-Path $Admin 'MapList.txt') @('XP5_003 ConquestLarge0 1')
    } else {
        if ($SavedConfigBackup) {
            Assert-StandConfigBackup $Admin $SavedConfigBackup
            foreach ($name in @('Startup.txt', 'MapList.txt', 'ModList.txt')) {
                Copy-Item -LiteralPath (Join-Path $SavedConfigBackup $name) -Destination (Join-Path $Admin $name) -Force -ErrorAction Stop
            }
        }
        $startup = Join-Path $Admin 'Startup.txt'
        $lines = New-Object 'Collections.Generic.List[string]'
        foreach ($line in [IO.File]::ReadAllLines($startup, [Text.Encoding]::UTF8)) {
            if ($line -notmatch '^\s*vars\.(roundTimeLimit|gameModeCounter|ctfRoundTimeModifier)(\s|$)') { $lines.Add($line) }
        }
        $lines.Add('vars.gameModeCounter ' + $Tickets); $lines.Add('vars.ctfRoundTimeModifier 100')
        Write-StandUtf8Lines $startup $lines.ToArray()
    }
    $modList = @($Profile.ActiveMods -split '[;,]' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    Write-StandUtf8Lines (Join-Path $Admin 'ModList.txt') $modList
}

function Get-StandLuaResult([string[]]$Lines, [bool]$RequireLevel = $false) {
    if ($Lines -match 'LUA-MODE FAILED|LEVEL-MODE FAILED') { return 'failed' }
    if ($Lines -match 'VEH #') { return 'crash' }
    if ($RequireLevel -and -not ($Lines -match 'LEVEL-MODE READY')) { return $null }
    if ($Lines -match 'LUA-MODE READY') { return 'ready' }
    return $null
}

function Get-StandJoinProfile($State, [string]$Name, [string]$ModsRoot, [string]$UiAssets, [bool]$NoClientDll) {
    if ($State.modProfile) {
        if (-not $State.ready -or -not $State.profileConfigured) { throw 'The saved stand run did not reach readiness; start it successfully before joining' }
        if ($Name -and $Name -ne $State.modProfile) { throw 'Requested client profile differs from the running stand profile' }
        if ($ModsRoot -and [IO.Path]::GetFullPath($ModsRoot).TrimEnd('\') -ne $State.modsRoot) { throw 'Client ModsRoot must match the saved local stand ModsRoot' }
        $Name = [string]$State.modProfile
        $ModsRoot = [string]$State.modsRoot
        if (-not $UiAssets) { $UiAssets = [string]$State.uiAssets }
    } elseif (-not $Name) { $Name = 'Standard' }
    if ($Name -eq 'FunBots') { $Name = 'Standard' }
    if ($Name -eq 'Royale' -and -not $State.modProfile) { throw 'Royale join requires a saved Royale stand run' }
    $profile = Get-StandModProfile $Name $ModsRoot $UiAssets
    if ($profile.Name -eq 'Royale' -and $NoClientDll) { throw 'Royale requires the client DLL and client mods' }
    return $profile
}

function Get-StandProfileEnvironment($Profile, [bool]$Client, [bool]$Tests) {
    $flags = @{ EVBOTS_ACTIVE_MODS = $Profile.ActiveMods }
    if ($Client) {
        $flags.EVBOTS_CLIENT_MODS = $Profile.ModsRoot
        if ($Profile.Name -eq 'Royale') { $flags.EVBOTS_UI_ASSETS = $Profile.UiAssets }
    } else {
        $flags.EVBOTS_MODS = $Profile.ModsRoot
        $flags.EVBOTS_TESTS = if ($Tests -and $Profile.Name -eq 'Standard') { '1' } else { '0' }
    }
    if ($Profile.Name -eq 'Royale') {
        # Royale is a different mod (no fun-bots); keep logs quiet and disable bot/bridge probes.
        # 16.11.2026: verbose снова 1 — без него не видно, подключились ли фазы EngineRes на клиенте
        # и применился ли патч checksum на сервере (диагностика загрузки XP5_003).
        $flags.EVBOTS_LOG_VERBOSE = '1'
        $flags.EVBOTS_SPM_MGR = '0'
        # Keep the existing native player update hook; this does not enable bots.
        $flags.EVBOTS_SPM_TICK = '1'
        $flags.EVBOTS_SPM_SOLDIER = '0'
        $flags.EVBOTS_SPM_PROBE = '0'
        # 16.11: old bypass misses three comparisons and jumps one byte before the
        # success block. Keep the native content check until resource parity is fixed.
        if (-not $Client) { $flags.EVBOTS_SKIP_LEVEL_CHECKSUM = '0' }
        # EVBOTS_LEVEL_NAME/GAMEMODE принудительно диспатчили Level:Loaded вне потока кадра движка ->
        # Blueprint/EntityManager:GetIterator падал в VU-BattleRoyale (Blueprint требует frame thread).
        # Оставляем движку штатный путь готовности уровня.
    }
    return $flags
}

function Prepare-StandRoyaleServer([string]$ServerDirectory, [string]$DataDirectory) {
    $launcher = 'D:\Games-cache\royale\package-launcher\EV Launcher.exe'
    $target = if ($ServerDirectory) { [IO.Path]::GetFullPath($ServerDirectory).TrimEnd('\') } else { 'D:\Games-cache\royale\server-runtime-v3' }
    $data = if ($DataDirectory) { [IO.Path]::GetFullPath($DataDirectory).TrimEnd('\') } else { 'D:\Games-cache\royale\server-assets-csharp-v3\Data' }
    Assert-StandFile (Join-Path $data 'layout.toc')
    # Client patch manifests conflict with the dedicated Linux Data manifest.
    # Check before reusing the runtime: parseManifest asserts before level load.
    $clientPatch = Join-Path $target 'Update\Patch\Data\initfs_Win32'
    if (Test-Path -LiteralPath $clientPatch) {
        throw ('Royale server contains client Update resources: ' + (Join-Path $target 'Update') + '. Move this Update entry out of the dedicated server directory before starting.')
    }
    $dataLink = Join-Path $target 'Data'
    # Reuse existing runtime and (re)point Data junction to requested data when possible.
    # This supports switching between full overlay and parity data without rebuilding runtime.
    if ((Test-Path (Join-Path $target 'bf3_server_Final.exe')) -and (Test-Path (Join-Path $target 'Instance\Admin'))) {
        $item = Get-Item $dataLink -Force -ErrorAction SilentlyContinue
        $cur = if ($item -and $item.Target) { @($item.Target)[0] } else { '' }
        $want = [IO.Path]::GetFullPath($data).TrimEnd('\')
        if ($cur -and ([IO.Path]::GetFullPath($cur).TrimEnd('\') -ieq $want)) {
            return [pscustomobject]@{ ServerDir = $target; InstanceDir = (Join-Path $target 'Instance') }
        }
        if (Test-Path $dataLink) { cmd /c rmdir "$dataLink" 2>&1 | Out-Null }
        if (Test-Path $want) {
            cmd /c mklink /J "$dataLink" "$want" 2>&1 | Out-Null
            $item = Get-Item $dataLink -Force -ErrorAction SilentlyContinue
            $cur = if ($item -and $item.Target) { @($item.Target)[0] } else { '' }
            if ($cur -and ([IO.Path]::GetFullPath($cur).TrimEnd('\') -ieq $want)) {
                return [pscustomobject]@{ ServerDir = $target; InstanceDir = (Join-Path $target 'Instance') }
            }
        }
    }
    Assert-StandFile $launcher
    $resultPath = Join-Path $RunDir ('royale-layout-' + (Get-Date -Format 'HHmmss-fff') + '.json')
    $arguments = @('--prepare-royale-server', $Game, $Instance, $target, $data, $resultPath)
    foreach ($argument in $arguments) { if ($argument.Contains('"')) { throw 'Invalid quote in layout argument' } }
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $launcher
    $startInfo.Arguments = (($arguments | ForEach-Object { '"' + $_.TrimEnd('\') + '"' }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $process = [Diagnostics.Process]::Start($startInfo)
    try {
        if (-not $process.WaitForExit(30000)) { throw 'Royale layout preparation is still running; no server was launched' }
        $result = Read-StandJson $resultPath
        if ($process.ExitCode -ne 0 -or $result.error) { throw ('Royale layout refused: ' + $result.error) }
        if ($result.ServerDir -ne $target -or $result.InstanceDir -ne (Join-Path $target 'Instance')) { throw 'Unexpected isolated Royale paths' }
        return $result
    } finally { $process.Dispose() }
}

function Prepare-StandRoyaleClient([string]$TargetDirectory) {
    $target = if ($TargetDirectory) { [IO.Path]::GetFullPath($TargetDirectory).TrimEnd('\') } else { 'D:\Games-cache\royale\client-runtime' }
    $src = $ClientGame
    if (-not (Test-Path (Join-Path $src 'bf3.exe'))) { throw 'Client BF3 not found: ' + $src }
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    # Link/copy top-level files and dirs except Data (Data handled separately).
    foreach ($item in Get-ChildItem -LiteralPath $src -Force) {
        if ($item.Name -ieq 'Data') { continue }
        $dest = Join-Path $target $item.Name
        if (Test-Path $dest) { continue }
        if ($item.PSIsContainer) {
            cmd /c mklink /J "$dest" "$($item.FullName)" 2>&1 | Out-Null
            if (-not (Test-Path $dest)) { Copy-Item -LiteralPath $item.FullName -Destination $dest -Recurse -Force }
        } else {
            cmd /c mklink /H "$dest" "$($item.FullName)" 2>&1 | Out-Null
            if (-not (Test-Path $dest)) { Copy-Item -LiteralPath $item.FullName -Destination $dest -Force }
        }
    }
    # Default: junction Data to stock BF3 Data (read-only view). Caller can replace with custom prepared Data.
    $data = Join-Path $target 'Data'
    if (-not (Test-Path $data)) {
        cmd /c mklink /J "$data" (Join-Path $src 'Data') 2>&1 | Out-Null
        if (-not (Test-Path $data)) { throw 'Failed to prepare client Data link: ' + $data }
    }
    return $target
}
