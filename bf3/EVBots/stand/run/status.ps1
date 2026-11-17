# EVBots stand: what is running and how the current run goes (from this run's part of EVBots.log).
#   status.ps1            summary
#   status.ps1 -Errors 20 show the last 20 ERR lines
# ASCII only (see common.ps1).
param([int]$Errors = 5)
. "$PSScriptRoot\common.ps1"

foreach ($n in 'bf3_server_Final', 'EV Backend', 'bf3') {
    $p = Get-Proc $n
    if ($p) {
        $up = [int]((Get-Date) - $p.StartTime).TotalSeconds
        Write-Output ('{0,-17} PID {1,-6} up {2,5} s  CPU {3,6} s  mem {4,5} MB' -f $n, $p.Id, $up, [int]$p.CPU, [int]($p.WorkingSet64 / 1MB))
    } else {
        Write-Output ('{0,-17} not running' -f $n)
    }
}

$j = Get-Json '/servers'
if ($j -and $j.servers -and $j.servers.Count -gt 0) {
    $s = $j.servers[0]
    Write-Output ('backend: server "' + $s.name + '" gameId=' + $s.gameId + ' map=' + $s.map + ' ' + $s.mode + ' players=' + $s.players + '/' + $s.maxPlayers + ' finalized=' + $s.finalized)
} else {
    Write-Output 'backend: no server registered (or backend down)'
}

$st = Get-RunState
if (-not $st) { Write-Output ('no run state (' + $StateFile + '): start the stand with run\start.ps1'); exit 0 }
if ($st.isolatedServer -and $st.instanceDir) {
    $Instance = [string]$st.instanceDir
    Write-Output ('isolated server: ' + $st.serverDir)
}
Write-Output ('run started ' + $st.started + ', DLL md5 ' + $st.dll + ', tests=' + $st.tests)
$lines = @(Read-LogSince ([long]$st.offset))
$veh = @($lines | Where-Object { $_ -match 'VEH #' })
$err = @($lines | Where-Object { $_ -match '\] ERR ' })
$pass = @($lines | Where-Object { $_ -match ' TEST \S+ PASS' }).Count
$fail = @($lines | Where-Object { $_ -match ' TEST \S+ FAIL' })
Write-Output ('log of this run: ' + $lines.Count + ' lines, VEH ' + $veh.Count + ', ERR ' + $err.Count + ', TEST PASS ' + $pass + ' FAIL ' + $fail.Count + $(if ($lines -match 'TESTS-DONE') { ' (TESTS-DONE)' } else { '' }))
$lines | Where-Object { $_ -match 'LUA-MODE READY' } | Select-Object -First 1 | ForEach-Object { Write-Output ('  ' + $_) }
$lines | Where-Object { $_ -match 'heartbeat ticks' } | Select-Object -Last 1 | ForEach-Object { Write-Output ('  ' + $_) }
$lines | Where-Object { $_ -match 'SpmProbe: update this=' } | Select-Object -Last 1 | ForEach-Object { Write-Output ('  engine players: ' + $_) }
$veh | Select-Object -First 3 | ForEach-Object { Write-Output ('  ' + $_) }
$fail | Select-Object -Last 5 | ForEach-Object { Write-Output ('  ' + $_) }
$err | Select-Object -Last $Errors | ForEach-Object { Write-Output ('  ' + $_) }

# Engine's own log (rotated on every server start, times in UTC): asserts and crash with the engine call context.
# Run 173: the crash cause (DataBus assert on a soldier) was only here, not in EVBots.log.
$rt = Get-ChildItem ($Instance + '\Logs\RuntimeLog_*_server.log') -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if ($rt) {
    $rtLines = @(Get-Content $rt.FullName -Encoding UTF8 | Where-Object { $_ -match '": (Assert|Crash|Error):' })
    Write-Output ('engine RuntimeLog ' + $rt.Name + ': Assert/Crash/Error ' + $rtLines.Count)
    $rtLines | Select-Object -Last 5 | ForEach-Object { Write-Output ('  ' + $_) }
    $dumps = @(Get-ChildItem ($Instance + '\Logs\CrashDump_*.dmp') -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $rt.CreationTime })
    if ($dumps.Count -gt 0) { Write-Output ('  crash dump: ' + $dumps[-1].FullName) }
}
