# EVBots stand: stop server and backend. Refuses while a client is running (a human may be playing)
# unless -Force. The client itself is never killed here. ASCII only (see common.ps1).
param([switch]$Force)
. "$PSScriptRoot\common.ps1"
$cli = Get-StandRunningClient
if ($cli -and -not $Force) {
    Write-Step ('STOP: client ' + $cli.ProcessName + ' is running (PID ' + $cli.Id + ') - a human may be playing. Close it or use -Force.')
    exit 2
}
if (Stop-Stand) { Write-Step 'server and backend stopped' } else { Write-Step 'ERROR: server/backend still running after 20 s'; exit 3 }
