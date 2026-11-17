# 26.10 (238-4): как cycle-keep, но БЕЗ перезапуска сервера — только вход клиента к уже поднятому стенду (пользователь ушёл
# и попросил «стенд не перезапускай, просто меня запусти»). Автоклик «Дислокации», tests.ps1, потом PlayMin минут в игре
# (дислокация снова — только когда тела нет, как cycle.ps1). Только при -UserAway. Стенд остаётся поднятым.
param([switch]$UserAway, [int]$DeployMin = 6, [int]$PlayMin = 20)
. "$PSScriptRoot\common.ps1"
Add-Type -Namespace EVBotsWin -Name U32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
'@
function Show-Client {
    foreach ($n in @('bf3_server_Final', 'EV Backend', 'evbots-loader')) {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } |
            ForEach-Object { [void][EVBotsWin.U32]::ShowWindow($_.MainWindowHandle, 6) }
    }
    $g = Get-Process -Name 'bf3' -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($g) {
        [EVBotsWin.U32]::keybd_event(0x12, 0, 0, [System.UIntPtr]::Zero); [EVBotsWin.U32]::keybd_event(0x12, 0, 2, [System.UIntPtr]::Zero)
        [void][EVBotsWin.U32]::ShowWindow($g.MainWindowHandle, 9); [void][EVBotsWin.U32]::SetForegroundWindow($g.MainWindowHandle)
    }
}
$transcript = $RunDir + '\play-join-' + (Get-Date).ToString('HHmmss') + '.log'
Start-Transcript -Path $transcript -Force | Out-Null
try {
    if (-not $UserAway) { Write-Step 'STOP: play-join.ps1 clicks in the client. Use -UserAway only when the user is away.'; return }
    if (-not (Get-Proc 'bf3_server_Final')) { Write-Step 'STOP: server is not running'; return }
    if (Get-Proc 'bf3') { Write-Step 'STOP: client already running'; return }
    $offset = Get-LogLength
    $st = Get-RunState
    # tests.ps1/status.ps1 читают журнал с offset из состояния — сдвигаем его на этот вход, остальное как у start.ps1
    Save-RunState @{ offset = $offset; started = (Get-Date).ToString('s'); dll = $(if ($st) { $st.dll } else { '' });
                     tests = $(if ($st) { $st.tests } else { $true }); env = $(if ($st) { $st.env } else { '' }) }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ($PSScriptRoot + '\join.ps1')
    $started = Wait-For {
        if ((Read-LogSince $offset) -match 'PlayerManager: TEST ') { return $true }
        Show-Client
        & um win drive --proc bf3 'focus' 'click 1110 646' 2>$null | Out-Null
        return $null
    } ($DeployMin * 60) 10000
    Write-Step ('deploy loop done, tests started=' + [bool]$started)
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ($PSScriptRoot + '\tests.ps1')
    $until = (Get-Date).AddMinutes($PlayMin)
    while ((Get-Date) -lt $until -and (Get-Proc 'bf3_server_Final') -and (Get-Proc 'bf3')) {
        $last = @((Read-LogSince $offset) | Where-Object { $_ -match '=(true|false) \S+A4=' }) | Select-Object -Last 1
        if (-not $last -or $last -match '=false \S+A4=') {
            Show-Client
            & um win drive --proc bf3 'focus' 'click 1110 646' 2>$null | Out-Null
        }
        Start-Sleep -Seconds 15
    }
    Write-Step ('play time done (' + $PlayMin + ' min)')
} finally {
    Write-Step 'PLAY-JOIN-DONE'
    Stop-Transcript | Out-Null
}
