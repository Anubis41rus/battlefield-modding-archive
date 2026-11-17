# EVBots stand: fully autonomous run - ONLY when the user said they are away (memory: feedback-autonomous-runs).
# Closes the client, restarts the stand (start.ps1), clicks Deploy until the tests start, waits for the results.
# Without -UserAway it refuses: the client may be the human's game. ASCII only (see common.ps1).
#   cycle.ps1 -UserAway [-DeployMin 6] [-PlayMin 0] [-ExtraEnv "K=V,..."]   PlayMin: keep the human spawned N more minutes after TESTS-DONE
# Output: trash\<date>\run\cycle-HHmmss.log. From an elevated session start it via explorer.exe run\cycle.cmd.
param([switch]$UserAway, [int]$DeployMin = 6, [int]$PlayMin = 0, [string]$ExtraEnv = '', [switch]$KeepUp)
. "$PSScriptRoot\common.ps1"
# 18.10 (build 185): minimize the server/backend console windows and bring the game window to the front before a click.
Add-Type -Namespace EVBotsWin -Name U32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
'@
function Show-Client {
    foreach ($n in @('bf3_server_Final', 'EV Backend', 'evbots-loader', 'activation-loader')) {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } |
            ForEach-Object { [void][EVBotsWin.U32]::ShowWindow($_.MainWindowHandle, 6) }
    }
    $g = Get-Process -Name 'bf3' -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($g) {
        [EVBotsWin.U32]::keybd_event(0x12, 0, 0, [System.UIntPtr]::Zero); [EVBotsWin.U32]::keybd_event(0x12, 0, 2, [System.UIntPtr]::Zero)
        [void][EVBotsWin.U32]::ShowWindow($g.MainWindowHandle, 9); [void][EVBotsWin.U32]::SetForegroundWindow($g.MainWindowHandle)
    }
}
$transcript = $RunDir + '\cycle-' + (Get-Date).ToString('HHmmss') + '.log'
Start-Transcript -Path $transcript -Force | Out-Null
try {
    if (-not $UserAway) { Write-Step 'STOP: cycle.ps1 closes the client and clicks in it. Use -UserAway only when the user is away.'; return }
    $c = Get-Proc 'bf3'
    if ($c) {
        Stop-Process -Id $c.Id -Force
        [void](Wait-For { -not (Get-Proc 'bf3') } 15 500)
    }
    $startArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ($PSScriptRoot + '\start.ps1'), '-Force')
    if ($ExtraEnv -ne '') { $startArgs += @('-ExtraEnv', $ExtraEnv) }
    & powershell.exe @startArgs
    if ($LASTEXITCODE -ne 0) { Write-Step ('start.ps1 failed, code ' + $LASTEXITCODE); return }
    $st = Get-RunState
    $started = Wait-For {
        if ((Read-LogSince ([long]$st.offset)) -match 'PlayerManager: TEST ') { return $true }
        Show-Client  # 18.10: the server console in front blocked the click (tests never started on XP5_002)
        & um win drive --proc bf3 'focus' 'click 1110 646' 2>$null | Out-Null  # fails while another window is in front: retried
        return $null
    } ($DeployMin * 60) 10000
    Write-Step ('deploy loop done, tests started=' + [bool]$started)
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ($PSScriptRoot + '\tests.ps1')
    if ($PlayMin -gt 0) {
        # Keep the human in the game: click Deploy only when the human has no body (the 20 s "body=true|false" line of
        # the mod log). Clicking while alive fired the weapon (user, run 174).
        $until = (Get-Date).AddMinutes($PlayMin)
        while ((Get-Date) -lt $until -and (Get-Proc 'bf3_server_Final')) {
            $last = @((Read-LogSince ([long]$st.offset)) | Where-Object { $_ -match '=(true|false) \S+A4=' }) | Select-Object -Last 1
            if (-not $last -or $last -match '=false \S+A4=') {
                Show-Client
                & um win drive --proc bf3 'focus' 'click 1110 646' 2>$null | Out-Null
            }
            Start-Sleep -Seconds 15
        }
        Write-Step ('play time done (' + $PlayMin + ' min)')
    }
} finally {
    # 18.10 (build 185): a full cycle ends clean - client closed, server and backend stopped (user: last time the server
    # stayed up with the client closed). -KeepUp keeps everything running.
    if ($UserAway -and -not $KeepUp) {
        $c2 = Get-Proc 'bf3'
        if ($c2) { Stop-Process -Id $c2.Id -Force; [void](Wait-For { -not (Get-Proc 'bf3') } 15 500) }
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ($PSScriptRoot + '\stop.ps1') -Force
    }
    Write-Step 'CYCLE-DONE'
    Stop-Transcript | Out-Null
}
