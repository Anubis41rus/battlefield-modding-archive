# EVBots stand: (re)start backend + server, deploy the fresh DLL, wait until Lua is ready, join the client.
# The human presses "Deploy" in the game (rule 19). Autonomous runs with autoclick: cycle.ps1.
#   start.ps1                 deploy build\EVBots.dll if changed, tests on, join client
#   start.ps1 -NoTests        without EVBOTS_TESTS
#   start.ps1 -NoJoin         server only
#   start.ps1 -NoDeploy       keep the DLL that is in activation\
#   start.ps1 -Force          legacy cycle.ps1 argument; a running client is always protected
#   start.ps1 -ModProfile Royale [-ModsRoot PATH] [-UiAssets PATH] [-NoJoin]
#   start.ps1 -ExtraEnv "EVBOTS_VU_CREATE=ab,EVBOTS_X=1"   extra server env flags for this run (experiments)
#   start.ps1 -StandTickets 100                            tickets % written to Startup.txt (default 30)
# Output is also saved to trash\<date>\run\start-HHmmss.log. ASCII only (see common.ps1).
param(
    [switch]$NoTests,
    [switch]$NoJoin,
    [switch]$NoDeploy,
    [switch]$ChecksumDiagnostics,
    [switch]$Force,
    [string]$ExtraEnv = '',
    [int]$ReadySec = 150,
    [int]$StandTickets = 30,      # vars.gameModeCounter, % (Standard only)
    [ValidateSet('Standard', 'FunBots', 'Royale')][string]$ModProfile = 'Standard',
    [string]$RoyaleServerDirectory = '',
    [string]$RoyaleDataDirectory = '',
    [string]$ModsRoot = '',
    [string]$UiAssets = ''
)
. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Stop'
$transcript = $RunDir + '\start-' + (Get-Date).ToString('HHmmss') + '.log'
Start-Transcript -Path $transcript -Force | Out-Null
$exit = 0
try {
    # 1. Do not switch/deploy under any running client, even without a server.
    $cli = Get-StandRunningClient
    if ($cli) {
        Write-Step ('STOP: client ' + $cli.ProcessName + ' is running (PID ' + $cli.Id + '). Close it first; -Force does not bypass this guard.')
        $exit = 2; return
    }
    $profile = Get-StandModProfile $ModProfile $ModsRoot $UiAssets
    Assert-StandProfileFiles $profile
    $editorialFile = Join-Path (Split-Path $Game -Parent) 'htdocs\easo\editorial\Battlefield\3\config\pc\server.xml'
    foreach ($path in @($Loader, ($Kit + '\backend\EV Backend.exe'), ($Game + '\bf3_server_Final.exe'))) { Assert-StandFile $path }
    $deployDll = if (-not $NoDeploy -and [IO.File]::Exists($BuildDll)) { $BuildDll } else { $ActDll }
    Assert-StandFile $deployDll
    $deployLua = if (-not $NoDeploy -and [IO.File]::Exists((Join-Path (Split-Path $BuildDll -Parent) 'lua.dll'))) {
        Join-Path (Split-Path $BuildDll -Parent) 'lua.dll'
    } else { Join-Path (Split-Path $ActDll -Parent) 'lua.dll' }
    Assert-StandFile $deployLua
    if (-not $NoJoin) {
        foreach ($path in @(($ClientGame + '\bf3.exe'), ($Kit + '\activation\activation.dll'), ($Kit + '\license\71067.dlf'))) { Assert-StandFile $path }
    }
    $admin = Join-Path $Instance 'Admin'
    $previous = Get-RunState
    $savedConfigBackup = Get-StandConfigBackup $admin $previous
    $testsEnabled = -not $NoTests -and $profile.Name -eq 'Standard'
    Write-Step ('profile ' + $profile.Name + ', roots ' + $profile.ActiveMods + ', mods ' + $profile.ModsRoot)
    if ($profile.Name -eq 'Royale') { Write-Step 'Royale is experimental; bot tests are disabled. File checks do not prove a complete match works.' }

    # 2. Stop the old stand (also releases the lock on EVBots.dll).
    if (Get-StandRunningClient) { Write-Step 'STOP: client appeared during preflight'; $exit = 2; return }
    Write-Step 'stopping old server/backend'
    if (-not (Stop-Stand)) { Write-Step 'ERROR: old server/backend did not exit in 20 s'; $exit = 3; return }

    # 3. Back up only Admin settings, never any mod files. Persist recovery data
    # before the first Admin write so a failed start can be safely retried.
    if ($profile.Name -eq 'Royale') {
        # The old server folder may contain an independent xinput/BotMod proxy.
        # Use an isolated executable directory and independent Admin settings.
        $royaleData = $RoyaleDataDirectory
        $royaleTarget = if ($RoyaleServerDirectory) { $RoyaleServerDirectory } else { 'D:\Games-cache\royale\server-runtime-v3' }
        if (-not $royaleData) {
            # Default to vanilla/parity server data for client/server checksum compatibility (stock Win32 client).
            # Full overlay (Linux-format, needs matching client assets): -RoyaleDataDirectory "D:\Games-cache\royale\server-assets-csharp-v3\Data"
            $royaleData = 'C:\Games\BF-Portable\Battlefield_3_Server\Data'
        }
        # XP5_003 map config must match data mode. COOP_009 bundles require server data that contains them.
        # On vanilla/parity server data, listing COOP superbundles/bundles causes mount failures that can crash the server.
        $xp5 = Join-Path $profile.ModsRoot 'VU-BattleRoyale\ext\Shared\Maps\XP5_003.lua'
        $xp5Full = 'C:\Games\backup\2026-11-12\XP5_003.lua.full-overlay'
        $xp5Parity = 'C:\Games\backup\2026-11-12\XP5_003.lua.parity-no-coop'
        $isOverlay = [IO.Path]::GetFullPath($royaleData).TrimEnd('\') -ieq 'D:\Games-cache\royale\server-assets-csharp-v3\Data'
        if ($env:EVBOTS_ROYALE_XP5_CONFIG -eq 'keep') {
            Write-Step 'XP5_003 config: keep (EVBOTS_ROYALE_XP5_CONFIG=keep)'
        } elseif ($isOverlay -and (Test-Path $xp5Full)) {
            Copy-Item $xp5Full $xp5 -Force
            Write-Step 'XP5_003 config: full-overlay (COOP_009 enabled)'
        } elseif (-not $isOverlay -and (Test-Path $xp5Parity)) {
            Copy-Item $xp5Parity $xp5 -Force
            Write-Step 'XP5_003 config: parity (no COOP_009, checksum-safe with stock client)'
        }
        $layout = Prepare-StandRoyaleServer $royaleTarget $royaleData
        $Game = [string]$layout.ServerDir
        $Instance = [string]$layout.InstanceDir
        $admin = Join-Path $Instance 'Admin'
        $savedConfigBackup = ''
        Write-Step ('isolated Royale server: ' + $Game + ' (Data: ' + $royaleData + ')')
    }
    $runState = @{ offset = (Get-LogLength); started = (Get-Date).ToString('s'); tests = $testsEnabled; env = $ExtraEnv;
        modProfile = $profile.Name; modsRoot = $profile.ModsRoot; uiAssets = $profile.UiAssets;
        activeMods = $profile.ActiveMods; savedConfigBackup = $savedConfigBackup; profileConfigured = $false; ready = $false;
        isolatedServer = ($profile.Name -eq 'Royale'); serverDir = $Game; instanceDir = $Instance }
    Save-RunState $runState
    Write-StandProfileConfig $profile $admin $savedConfigBackup $StandTickets
    $runState.profileConfigured = $true
    if ($profile.Name -eq 'Standard') { $runState.savedConfigBackup = '' }
    Save-RunState $runState
    Write-Step ('Admin configured for ' + $profile.Name)

    # 4. Deploy: backup of the working DLL first (rule 15), then copy and verify.
    $mdBuild = Get-FileMd5 $BuildDll
    $mdAct = Get-FileMd5 $ActDll
    if (-not $NoDeploy -and $mdBuild -ne '' -and $mdBuild -ne $mdAct) {
        New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
        if (Test-Path $ActDll) {
            $bak = $BackupDir + '\evbots-before-' + (Get-Date).ToString('HHmmss') + '.dll'
            Copy-Item $ActDll $bak -Force
            Write-Step ('backup of the working DLL: ' + $bak)
        }
        Copy-Item $BuildDll $ActDll -Force
        $mdAct = Get-FileMd5 $ActDll
        if ($mdAct -ne $mdBuild) { Write-Step 'ERROR: DLL copy failed (locked?)'; $exit = 5; return }
        Write-Step ('deployed new DLL, md5 ' + $mdAct)
    } else {
        Write-Step ('DLL unchanged, md5 ' + $mdAct)
    }
    # 4b. ev-core.dll (23.10): same rule - backup of the working copy, then copy and verify.
    $mdCoreBuild = Get-FileMd5 $CoreBuild
    $mdCore = Get-FileMd5 $CoreDll
    if (-not $NoDeploy -and $mdCoreBuild -ne '' -and $mdCoreBuild -ne $mdCore) {
        New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
        if (Test-Path $CoreDll) { Copy-Item $CoreDll ($BackupDir + '\ev-core-before-' + (Get-Date).ToString('HHmmss') + '.dll') -Force }
        Copy-Item $CoreBuild $CoreDll -Force
        $mdCore = Get-FileMd5 $CoreDll
        if ($mdCore -ne $mdCoreBuild) { Write-Step 'ERROR: ev-core.dll copy failed (locked?)'; $exit = 5; return }
        Write-Step ('deployed ev-core.dll, md5 ' + $mdCore)
    }
    # 4c. lua.dll (build 220): Lua 5.4.2 next to EVBots.dll (EVBots.dll loads it from its own folder), like vubin\lua.dll in VU.
    $luaBuild = Split-Path $BuildDll -Parent | Join-Path -ChildPath 'lua.dll'
    $luaAct = Split-Path $ActDll -Parent | Join-Path -ChildPath 'lua.dll'
    $mdLuaBuild = Get-FileMd5 $luaBuild
    $mdLua = Get-FileMd5 $luaAct
    if (-not $NoDeploy -and $mdLuaBuild -ne '' -and $mdLuaBuild -ne $mdLua) {
        New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null
        if (Test-Path $luaAct) { Copy-Item $luaAct ($BackupDir + '\lua-before-' + (Get-Date).ToString('HHmmss') + '.dll') -Force }
        Copy-Item $luaBuild $luaAct -Force
        if ((Get-FileMd5 $luaAct) -ne $mdLuaBuild) { Write-Step 'ERROR: lua.dll copy failed (locked?)'; $exit = 5; return }
        Write-Step ('deployed lua.dll, md5 ' + $mdLuaBuild)
    }
    $coreArgs = @()
    if (Test-Path $CoreDll) { $coreArgs = @('--dll', 'ev-core.dll') } else { Write-Step 'ev-core.dll not found - server window stays engine-default' }

    # 5. Backend; wait until its HTTP answers.
    $offset = Get-LogLength
    $backendArgs = @(
        'serve', '--redirector-port', ([string]$RedirectorPort), '--blaze-port', ([string]$BlazePort), '--http-port', ([string]$BackendHttpPort),
        '--profiles-dir', ($Kit + '\profiles'), '--progress-dir', ($Kit + '\progress'),
        '--stats-catalog', ($Kit + '\progress\bf3-stats-catalog.json'),
        '--instance', $Instance, '--mods-dir', ('"' + $profile.ModsRoot + '"'),
        '--game-server-address', $ServerAddr, '--advertise-ip', '127.0.0.1',
        '--server-name', 'EVBots', '--evbots-log', $Log
    )
    if (Test-Path -LiteralPath $editorialFile) {
        $backendArgs += @('--editorial-file', ('"' + $editorialFile + '"'))
    }
    # 18.10: лог бэкенда прошлого прогона не затираем (вопрос пользователя про gameId 2 — лог был уже перезаписан)
    if (Test-Path ($RunDir + '\backend.out')) {
        Move-Item ($RunDir + '\backend.out') ($RunDir + '\backend-' + (Get-Date -Format 'HHmmss') + '.out') -Force
    }
    Start-Process -FilePath ($Kit + '\backend\EV Backend.exe') -ArgumentList $backendArgs `
        -WorkingDirectory ($Kit + '\backend') -WindowStyle Hidden `
        -RedirectStandardOutput ($RunDir + '\backend.out') -RedirectStandardError ($RunDir + '\backend.err') | Out-Null
    if (-not (Wait-For { Get-Json '/' } 20)) { Write-Step 'ERROR: backend HTTP did not answer in 20 s'; $exit = 6; return }
    Write-Step ('backend up, PID ' + (Get-Proc 'EV Backend').Id)

    # 6. Server.
    foreach ($k in $ServerEnv.Keys) { Set-Item -Path ('Env:' + $k) -Value $ServerEnv[$k] }
    foreach ($kv in ($ExtraEnv -split ',')) {
        if ($kv -match '^\s*([A-Za-z0-9_]+)=(.*)$') { Set-Item -Path ('Env:' + $Matches[1]) -Value $Matches[2]; Write-Step ('extra env ' + $Matches[1] + '=' + $Matches[2]) }
    }
    # Profile values are authoritative, including over inherited/ExtraEnv flags.
    $profileEnv = Get-StandProfileEnvironment $profile $false $testsEnabled
    foreach ($key in $profileEnv.Keys) { Set-Item -Path ('Env:' + $key) -Value $profileEnv[$key] }
    Remove-Item Env:EVBOTS_CLIENT_MODS, Env:EVBOTS_UI_ASSETS -ErrorAction SilentlyContinue
    $serverGameArgs = @(
        '-serverInstancePath', $Instance,
        '-Server.ServerName', 'EVBots', '-GamePort', ([string]$GamePort),
        '-Network.MaxClientCount', '64', '-RemoteAdminPort', ('127.0.0.1:' + $RemoteAdminPort))
    if ($profile.Name -eq 'Royale') {
        $serverGameArgs += @('-CpuTiming.Enable', 'false')
    }
    if ($ChecksumDiagnostics) {
        # Output-only diagnostic. FullNameChecksumLog would change the hash input.
        $serverGameArgs += @('-Level.OutputChecksumDebugLog', 'true', '-Game.LogFileEnable', 'true')
    }
    $serverArgs = @(
        '--dedicated', '--game', ('"' + $Game + '"'),
        '--exe', 'bf3_server_Final.exe') + $coreArgs + @('--dll', 'EVBots.dll',
        '--log', ($RunDir + '\server-loader.out'), '--wait', '--') + $serverGameArgs + @(
        '+admin.password', 'change-this-rcon-password', '-PingSite', 'ams', '-Region', 'EU',
        '-Login', 'bf3_1', '-Password', 'password',
        '-Server.ActiveLicenses', 'premium_xp4,xp2_ks,xp3_ks,xp4_ks,xp5_ks,ea_nfs12,le,premium,pwp')
    # No stdout redirect for the long-lived server: PowerShell would wait for the handles to close.
    Start-Process -FilePath $Loader -ArgumentList $serverArgs -WorkingDirectory $Kit -WindowStyle Hidden | Out-Null
    $started = Get-Date
    $runState.offset = $offset; $runState.started = $started.ToString('s'); $runState.dll = $mdAct
    Save-RunState $runState

    # 7. Wait for Lua (instead of a blind 40 s): ready / crash / timeout.
    Write-Step ('server starting, waiting up to ' + $ReadySec + ' s for LUA-MODE READY')
    $result = Wait-For {
        $lines = Read-LogSince $offset
        $status = Get-StandLuaResult $lines ($profile.Name -eq 'Royale')
        if ($status) { return $status }
        if (((Get-Date) - $started).TotalSeconds -gt 15 -and -not (Get-Proc 'bf3_server_Final')) { return 'exited' }
        return $null
    } $ReadySec
    $lines = Read-LogSince $offset
    $lines | Where-Object { $_ -match 'EVBots loaded|EngineRes: .* \d+/\d+ |LUA-MODE READY|LUA-MODE FAILED|VEH #|ERR ' } |
        Select-Object -First 12 | ForEach-Object { Write-Output ('    ' + $_) }
    if ($result -ne 'ready') {
        Write-Step ('ERROR: server not ready: ' + $(if ($result) { $result } else { 'timeout' }))
        $exit = 7; return
    }
    $runState.ready = $true
    Save-RunState $runState
    Write-Step ('server ready in ' + [int]((Get-Date) - $started).TotalSeconds + ' s, PID ' + (Get-Proc 'bf3_server_Final').Id)

    # 8. Client.
    if ($NoJoin) { Write-Step 'no join (-NoJoin)'; return }
    $joinOptions = @()
    if ($ChecksumDiagnostics) { $joinOptions += @('-ChecksumDiagnostics', '-VerboseClient') }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ($PSScriptRoot + '\join.ps1') @joinOptions
    if ($LASTEXITCODE -ne 0) { $exit = $LASTEXITCODE; Write-Step 'ERROR: client join failed'; return }
    Write-Step 'done: wait for the client to load and press Deploy. Status: run\status.ps1, tests: run\tests.ps1'
} catch {
    Write-Step ('ERROR: ' + $_.Exception.Message)
    $exit = 4
} finally {
    Stop-Transcript | Out-Null
    exit $exit
}
