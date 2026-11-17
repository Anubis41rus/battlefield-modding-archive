# EVBots stand: BF3 client joins the local server directly (State_ClaimReservation), no menus.
# Never touches a running client - the human may be playing. ASCII only (see common.ps1).
#   join.ps1                  login KOTuk1 (or $env:EVBOTS_LOGIN)
#   join.ps1 -Login NAME
#   join.ps1 -NoClientDll     client without EVBots.dll (activation.dll only, as before build 215)
# 23.10 (build 215, K5): the client also gets EVBots.dll (client mode: client Lua, engine frame tick, engine messages)
# and the mods folder via EVBOTS_CLIENT_MODS (until K4 the client takes ext\client from the local Admin\Mods).
param([string]$Login = $(if ($env:EVBOTS_LOGIN) { $env:EVBOTS_LOGIN } else { 'KOTuk1' }),
      [switch]$NoClientDll,
      [ValidateSet('Standard', 'FunBots', 'Royale')][string]$ModProfile,
      [string]$ModsRoot = '', [string]$UiAssets = '',
      [string]$ClientGameDir = '',
      [switch]$VerboseClient,
      [switch]$ChecksumDiagnostics)
. "$PSScriptRoot\common.ps1"

$running = Get-StandRunningClient
if ($running) { Write-Step ('STOP: client already running (PID ' + $running.Id + '), not touching it'); exit 2 }
try {
    $state = Get-RunState
    $profile = Get-StandJoinProfile $state $ModProfile $ModsRoot $UiAssets ([bool]$NoClientDll)
    Assert-StandProfileFiles $profile
} catch { Write-Step ('ERROR: ' + $_.Exception.Message); exit 4 }

# The game id comes from the live backend once the server registered there (an old log may hold a stale id:
# 13.10 the client went to gameid=3 while the new backend had id=1 and exited silently).
$srv = Wait-For {
    if ($state.modProfile -and (Get-StandLuaResult (Read-LogSince ([long]$state.offset))) -in @('failed', 'crash')) { return 'failed' }
    $j = Get-Json '/servers'
    if ($j -and $j.servers -and $j.servers.Count -gt 0 -and $j.servers[0].finalized) { return $j.servers[0] }
    return $null
} 60
if ($srv -eq 'failed') { Write-Step 'ERROR: saved stand run has failed'; exit 1 }
if (-not $srv) { Write-Step 'ERROR: backend has no finalized server in 60 s'; exit 1 }
$gid = [string]$srv.gameId

# personaref of a REGISTERED account (unknown personas are rejected by the backend).
$accs = Get-Json '/accounts'
$hit = $null
if ($accs) { $hit = $accs.accounts | Where-Object { $_.login -eq $Login } | Select-Object -First 1 }
if (-not $hit) { Write-Step ('ERROR: account ' + $Login + ' not found in ' + $Http + '/accounts'); exit 1 }
$persona = [string]$hit.personaref

# The client needs the same EA-domain redirection as the server, otherwise it looks for real EA servers
# and exits ~37 s after activation.
$env:BF3ACT_DNSREDIR = 'gosredirector.ea.com,gosredirector.online.ea.com,gosredirector.scert.ea.com,gosredirector.stest.ea.com'
$env:BF3ACT_DNSREDIR_IP = '127.0.0.1'
$env:BF3ACT_DRM = '0'
# Server env inherited from start.ps1 (BF3ACT_MODE=dedicated, EVBOTS_*) must not reach the client.
Remove-Item Env:BF3ACT_MODE -ErrorAction SilentlyContinue
Remove-Item Env:BF3ACT_LOG -ErrorAction SilentlyContinue
$freqParts = $env:EVBOTS_FREQ_CLIENT_PARTS   # 26.10 (238-4): группы правок частоты клиента — для поиска «неверной лицензии»
Get-ChildItem Env: | Where-Object { $_.Name -like 'EVBOTS_*' } | ForEach-Object { Remove-Item ('Env:' + $_.Name) }
if ($freqParts) { $env:EVBOTS_FREQ_CLIENT_PARTS = $freqParts; Write-Step ('client: EVBOTS_FREQ_CLIENT_PARTS=' + $freqParts) }

$target = $ServerAddr
# Quotes must be backslash-escaped exactly as in BF3NativeBots\join.bat, otherwise the argument splits.
$params = '<data putinsquad=\"false\" gameid=\"' + $gid + '\" personaref=\"' + $persona + '\" levelmode=\"mp\" serveraddr=\"' + $target + '\"/>'
Write-Step ('join: login=' + $Login + ' personaref=' + $persona + ' gameid=' + $gid + ' server=' + $target)

$dllArgs = @()
if (-not $NoClientDll) {
    $profileEnv = Get-StandProfileEnvironment $profile $true $false
    foreach ($key in $profileEnv.Keys) { Set-Item -Path ('Env:' + $key) -Value $profileEnv[$key] }
    if ($profile.Name -ne 'Royale' -or $VerboseClient) {
        $env:EVBOTS_LOG_VERBOSE = '1'   # full client log on the stand (quiet mode drops hook / first-frame / subscription lines)
    } else {
        $env:EVBOTS_LOG_VERBOSE = '0'
    }
    # K4 (build 219): NetEvents channel (ev-core.dll, like Harmony in VU); the client greets the server with its soldier
    # name so the server can map the connection to the player (VU knows it from the Zeus login).
    $env:EVBOTS_PLAYER_NAME = $(if ($hit.name) { [string]$hit.name } else { $Login })
    # 26.10 (build 238): the client runs at the server frequency (like VU -high60/-high120), see run\frequency.txt
    $env:EVBOTS_FREQUENCY = $Frequency
    $dllArgs = @()
    if (Test-Path $CoreDll) { $dllArgs += @('--dll', $CoreDll) }
    $dllArgs += @('--dll', $ActDll)
    Write-Step ('client: ' + (($dllArgs | Where-Object { $_ -ne '--dll' } | ForEach-Object { Split-Path $_ -Leaf }) -join ', ') +
        ' after activation.dll, player ' + $env:EVBOTS_PLAYER_NAME + ', mods ' + $env:EVBOTS_CLIENT_MODS)
} else {
    Write-Step 'client: activation.dll only (-NoClientDll)'
}
$clientGameArgs = @(
    '--', '-webMode', 'MP', '-Origin_NoAppFocus', '-onlineEnvironment', 'prod',
    '-loginToken', '0', '-AuthToken', '0',
    '-requestState', 'State_ClaimReservation',
    '-requestStateParams', $params)
$clientGame = if ($ClientGameDir) { [IO.Path]::GetFullPath($ClientGameDir).TrimEnd('\') } else { $ClientGame }
if ($ClientGameDir) { Write-Step ('client game dir: ' + $clientGame) }
$cliArgs = @(
    '--client', '--game', $clientGame, '--drm-strict',
    '--license-artifact', ($Kit + '\license\71067.dlf')) + $dllArgs + $clientGameArgs + @(
    '-Online.BlazeLogLevel', '2', '-Online.DirtysockLogLevel', '2',
    '-windowed', '-w', '1280', '-h', '720'
)
# Run the loader directly, NOT via Start-Process (that gave no activation log and no client).
if ($ChecksumDiagnostics) {
    # Retail client has no Level.OutputChecksumDebugLog console variable.
    $env:EVBOTS_CHECKSUM_TRACE = '1'
    $cliArgs += @('-Game.LogFileEnable', 'true')
}
if (Get-StandRunningClient) { Write-Step 'STOP: client appeared while preparing join'; exit 2 }
$out = $RunDir + '\join.out'
& $Loader @cliArgs 2>&1 | Out-File -FilePath $out -Encoding utf8
$cli = Wait-For { Get-Proc 'bf3' } 30
if ($cli) { Write-Step ('client started, PID ' + $cli.Id + ' (loader output: ' + $out + ')') }
else { Write-Step ('ERROR: client did not start, see ' + $out); exit 1 }
